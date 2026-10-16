#pragma once

#include "application.h"
#include <ctype.h>
#include <errno.h>
#include <climits>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

inline void jsonEscape(const char* in, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    size_t oi = 0;
    for (size_t i = 0; in[i] != '\0' && oi + 1 < out_len; ++i) {
        char c = in[i];
        if ((c == '"' || c == '\\') && oi + 2 < out_len) {
            out[oi++] = '\\';
            out[oi++] = c;
        } else if ((uint8_t)c < 0x20) {
            out[oi++] = ' ';
        } else {
            out[oi++] = c;
        }
    }
    out[oi] = '\0';
}

inline const char* jsonFloatOrNull(float v, char* out, size_t out_len, uint8_t precision) {
    if (!out || out_len == 0) return "null";
    if (!isfinite(v)) {
        strncpy(out, "null", out_len);
        out[out_len - 1] = '\0';
        return out;
    }
    if (precision == 0) snprintf(out, out_len, "%.0f", v);
    else if (precision == 1) snprintf(out, out_len, "%.1f", v);
    else if (precision == 2) snprintf(out, out_len, "%.2f", v);
    else snprintf(out, out_len, "%.*f", precision, v);
    return out;
}

inline bool jsonValueTerminates(const char* p) {
    if (!p) return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return *p == '\0' || *p == ',' || *p == '}';
}

inline bool jsonContainsAlpha(const char* s) {
    if (!s) return false;
    for (; *s; ++s) {
        if (isalpha((unsigned char)*s)) return true;
    }
    return false;
}

inline bool parseJsonDottedQuad(const char* buf, uint32_t* out) {
    if (!buf || !out) return false;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    char extra = '\0';
    int matched = sscanf(buf, " %u.%u.%u.%u %c", &a, &b, &c, &d, &extra);
    if (matched != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    return true;
}

namespace json_detail {
inline void space(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
}
inline int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
inline bool unicode(const char*& p, const char* end, uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
        if (p == end || hex(*p) < 0) return false;
        value = value * 16 + (unsigned)hex(*p++);
    }
    return true;
}
// Decode one string character. Used for validation and escaped-key equality.
inline bool character(const char*& p, const char* end, uint32_t& value) {
    if (p == end || (uint8_t)*p < 0x20 || *p == '"') return false;
    value = (uint8_t)*p++;
    if (value != '\\') {
        if (value < 0x80) return true;
        const unsigned count = value >= 0xc2 && value <= 0xdf ? 1 :
                               value >= 0xe0 && value <= 0xef ? 2 :
                               value >= 0xf0 && value <= 0xf4 ? 3 : 0;
        if (!count) return false;
        value &= (1u << (6 - count)) - 1;
        for (unsigned i = 0; i < count; ++i) {
            if (p == end || ((uint8_t)*p & 0xc0) != 0x80) return false;
            value = (value << 6) | ((uint8_t)*p++ & 0x3f);
        }
        return value >= (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) &&
               value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
    }
    if (p == end) return false;
    switch (*p++) {
        case '"': value = '"'; return true;
        case '\\': value = '\\'; return true;
        case '/': value = '/'; return true;
        case 'b': value = '\b'; return true;
        case 'f': value = '\f'; return true;
        case 'n': value = '\n'; return true;
        case 'r': value = '\r'; return true;
        case 't': value = '\t'; return true;
        case 'u': {
            if (!unicode(p, end, value)) return false;
            if (value >= 0xdc00 && value <= 0xdfff) return false;
            if (value >= 0xd800 && value <= 0xdbff) {
                if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return false;
                p += 2;
                uint32_t low;
                if (!unicode(p, end, low) || low < 0xdc00 || low > 0xdfff) return false;
                value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
            }
            return true;
        }
        default: return false;
    }
}
inline bool string(const char*& p, const char* end) {
    if (p == end || *p++ != '"') return false;
    while (p < end && *p != '"') {
        uint32_t value;
        if (!character(p, end, value)) return false;
    }
    if (p == end) return false;
    ++p;
    return true;
}
inline bool equal(const char* a, const char* ae, const char* b, const char* be) {
    while (a < ae && b < be) {
        uint32_t av, bv;
        if (!character(a, ae, av) || !character(b, be, bv) || av != bv) return false;
    }
    return a == ae && b == be;
}
inline bool value(const char*& p, const char* end, unsigned depth);
inline bool object(const char*& p, const char* end, unsigned depth) {
    if (depth > 8 || p == end || *p++ != '{') return false;
    const char* members = p;
    space(p, end);
    if (p < end && *p == '}') { ++p; return true; }
    while (p < end) {
        const char* key = p;
        if (!string(p, end)) return false;
        const char* key_end = p;
        // Only top-level fields are API inputs. Reject their duplicates,
        // including escaped spellings, without allocating a token table.
        if (depth == 1) {
            const char* prev = members;
            space(prev, end);
            while (prev < key) {
                const char* prev_key = prev;
                if (!string(prev, end)) return false;
                if (equal(prev_key + 1, prev - 1, key + 1, key_end - 1)) return false;
                space(prev, end);
                if (prev == end || *prev++ != ':') return false;
                space(prev, end);
                if (!value(prev, end, depth)) return false;
                space(prev, end);
                if (prev < key && *prev == ',') ++prev;
                space(prev, end);
            }
        }
        space(p, end);
        if (p == end || *p++ != ':') return false;
        space(p, end);
        if (!value(p, end, depth)) return false;
        space(p, end);
        if (p == end) return false;
        if (*p == '}') { ++p; return true; }
        if (*p++ != ',') return false;
        space(p, end);
    }
    return false;
}
inline bool value(const char*& p, const char* end, unsigned depth) {
    if (p == end) return false;
    if (*p == '"') return string(p, end);
    if (*p == '{') return object(p, end, depth + 1);
    if (*p == '[') {
        if (depth >= 8) return false;
        ++p;
        space(p, end);
        if (p < end && *p == ']') { ++p; return true; }
        while (p < end) {
            if (!value(p, end, depth + 1)) return false;
            space(p, end);
            if (p == end) return false;
            if (*p == ']') { ++p; return true; }
            if (*p++ != ',') return false;
            space(p, end);
        }
        return false;
    }
    const char* literal = *p == 't' ? "true" : *p == 'f' ? "false" : *p == 'n' ? "null" : nullptr;
    if (literal) {
        const size_t n = strlen(literal);
        if ((size_t)(end - p) < n || memcmp(p, literal, n) != 0) return false;
        p += n;
        return true;
    }
    if (*p == '-') ++p;
    if (p == end) return false;
    if (*p == '0') ++p;
    else {
        if (*p < '1' || *p > '9') return false;
        do { ++p; } while (p < end && *p >= '0' && *p <= '9');
    }
    if (p < end && *p == '.') {
        ++p;
        if (p == end || *p < '0' || *p > '9') return false;
        do { ++p; } while (p < end && *p >= '0' && *p <= '9');
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        if (p == end || *p < '0' || *p > '9') return false;
        do { ++p; } while (p < end && *p >= '0' && *p <= '9');
    }
    return true;
}
} // namespace json_detail

inline bool jsonValidateObject(const char* json, size_t length) {
    if (!json || memchr(json, '\0', length)) return false;
    const char* p = json;
    const char* end = json + length;
    json_detail::space(p, end);
    if (!json_detail::object(p, end, 1)) return false;
    json_detail::space(p, end);
    return p == end;
}

inline bool jsonFindKey(const char* json, const char* key, const char** value_out) {
    if (!json || !key || !value_out || !key[0]) return false;
    const size_t length = strlen(json);
    if (!jsonValidateObject(json, length)) return false;
    const char* key_end = key + strlen(key);
    if (key_end - key >= 2 && *key == '"' && key_end[-1] == '"') { ++key; --key_end; }
    const char* p = json;
    const char* end = json + length;
    json_detail::space(p, end);
    ++p;
    json_detail::space(p, end);
    while (p < end && *p != '}') {
        const char* start = p;
        if (!json_detail::string(p, end)) return false;
        const bool matches = json_detail::equal(start + 1, p - 1, key, key_end);
        json_detail::space(p, end);
        ++p; // validated colon
        json_detail::space(p, end);
        if (matches) { *value_out = p; return true; }
        if (!json_detail::value(p, end, 1)) return false;
        json_detail::space(p, end);
        if (p < end && *p == ',') ++p;
        json_detail::space(p, end);
    }
    return false;
}

inline bool jsonFieldPresent(const char* json, const char* key) {
    const char* p = nullptr;
    return jsonFindKey(json, key, &p);
}

inline bool jsonGetString(const char* json, const char* key, char* out, size_t out_len) {
    if (!out || out_len == 0) return false;
    const char* p = nullptr;
    if (!jsonFindKey(json, key, &p)) return false;
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    const char* end = json + strlen(json);
    while (*p && *p != '"') {
        uint32_t value;
        if (!json_detail::character(p, end, value) || value == 0) return false;
        const unsigned bytes = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
        if (n + bytes >= out_len) return false;
        if (bytes == 1) out[n++] = (char)value;
        else {
            out[n++] = (char)((bytes == 2 ? 0xc0 : bytes == 3 ? 0xe0 : 0xf0) |
                              (value >> (6 * (bytes - 1))));
            for (unsigned i = bytes - 1; i > 0; --i)
                out[n++] = (char)(0x80 | ((value >> (6 * (i - 1))) & 0x3f));
        }
    }
    if (*p != '"') return false;
    out[n] = '\0';
    return jsonValueTerminates(p + 1);
}

inline bool jsonGetInt(const char* json, const char* key, int* out) {
    if (!out) return false;
    const char* p = nullptr;
    if (!jsonFindKey(json, key, &p)) return false;
    bool quoted = false;
    if (*p == '"') {
        quoted = true;
        p++;
    }
    errno = 0;
    char* end = nullptr;
    long value = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || value < INT_MIN || value > INT_MAX) return false;
    if (quoted) {
        if (*end != '"') return false;
        end++;
    }
    if (!jsonValueTerminates(end)) return false;
    *out = (int)value;
    return true;
}

inline bool jsonGetFloat(const char* json, const char* key, float* out) {
    if (!out) return false;
    const char* p = nullptr;
    if (!jsonFindKey(json, key, &p)) return false;
    bool quoted = false;
    if (*p == '"') {
        quoted = true;
        p++;
    }
    char* end = nullptr;
    float value = strtof(p, &end);
    if (end == p) return false;
    if (!isfinite(value)) return false;
    if (quoted) {
        if (*end != '"') return false;
        end++;
    }
    if (!jsonValueTerminates(end)) return false;
    *out = value;
    return true;
}

inline bool jsonGetBool(const char* json, const char* key, bool* out) {
    if (!out) return false;
    const char* p = nullptr;
    if (!jsonFindKey(json, key, &p)) return false;
    bool quoted = false;
    if (*p == '"') {
        quoted = true;
        p++;
    }
    if (strncmp(p, "true", 4) == 0) {
        p += 4;
        if (quoted) {
            if (*p != '"') return false;
            p++;
        }
        if (!jsonValueTerminates(p)) return false;
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        p += 5;
        if (quoted) {
            if (*p != '"') return false;
            p++;
        }
        if (!jsonValueTerminates(p)) return false;
        *out = false;
        return true;
    }
    if (*p == '1' && (!quoted || p[1] == '"' || jsonValueTerminates(p + 1))) {
        p++;
        if (quoted) {
            if (*p != '"') return false;
            p++;
        }
        if (!jsonValueTerminates(p)) return false;
        *out = true;
        return true;
    }
    if (*p == '0' && (!quoted || p[1] == '"' || jsonValueTerminates(p + 1))) {
        p++;
        if (quoted) {
            if (*p != '"') return false;
            p++;
        }
        if (!jsonValueTerminates(p)) return false;
        *out = false;
        return true;
    }
    return false;
}

inline bool jsonStringFieldPresent(const char* body, const char* key) {
    const char* p = nullptr;
    return jsonFindKey(body, key, &p) && *p == '"';
}

inline bool parseJsonIpValue(const char* body, const char* key, uint32_t* out) {
    if (!body || !key || !out) return false;
    char buf[32];
    if (!jsonGetString(body, key, buf, sizeof(buf))) return false;
    return parseJsonDottedQuad(buf, out);
}

inline bool parseJsonBrokerValue(const char* body, const char* key, uint32_t* out) {
    if (parseJsonIpValue(body, key, out)) return true;
    if (!body || !key || !out) return false;
    char buf[64];
    if (!jsonGetString(body, key, buf, sizeof(buf))) return false;
    if (!jsonContainsAlpha(buf)) return false;
    IPAddress resolved = WiFi.resolve(buf);
    if (!resolved) return false;
    *out = ((uint32_t)resolved[0] << 24) | ((uint32_t)resolved[1] << 16) |
           ((uint32_t)resolved[2] << 8) | (uint32_t)resolved[3];
    return true;
}
