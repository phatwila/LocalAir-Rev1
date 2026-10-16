#include "http_request.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool isTokenChar(char c) {
    return isalnum((unsigned char)c) || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static bool isFieldValueCtl(char c) {
    return ((uint8_t)c < 0x20 && c != '\t') || (uint8_t)c == 0x7f;
}

static bool headerNameEquals(const char* begin, const char* end, const char* name) {
    while (begin < end && *name) {
        if (tolower((unsigned char)*begin++) != tolower((unsigned char)*name++)) return false;
    }
    return begin == end && *name == '\0';
}

void HttpRequestReader::reset() {
    _kept[0] = '\0';
    _kept_len = 0;
    _total = 0;
    _phase = REQUEST_LINE;
    _pending_cr = false;
    _keep_line = false;
    _name_len = 0;
    _name_long = false;
}

bool HttpRequestReader::append(char c) {
    if (_kept_len >= KEPT_CAP - 1) return false;
    _kept[_kept_len++] = c;
    _kept[_kept_len] = '\0';
    return true;
}

bool HttpRequestReader::appendText(const char* s, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        if (!append(s[i])) return false;
    }
    return true;
}

// Called on CRLF. Closes the current line and readies the next one.
uint16_t HttpRequestReader::endLine() {
    switch (_phase) {
        case REQUEST_LINE:
            if (_kept_len == 0) return 400;
            if (!appendText("\r\n", 2)) return 431;
            break;
        case HEADER_NAME:
            if (_name_len != 0 || _name_long) return 400;  // field line without a colon
            if (!appendText("\r\n", 2)) return 431;
            _phase = FINISHED;
            return DONE;
        case HEADER_VALUE:
            if (_keep_line && !appendText("\r\n", 2)) return 431;
            break;
        case FINISHED:
            return DONE;
    }
    _phase = HEADER_NAME;
    _name_len = 0;
    _name_long = false;
    _keep_line = false;
    return NEED_MORE;
}

uint16_t HttpRequestReader::feed(char c) {
    if (_phase == FINISHED) return DONE;
    if (c == '\0') return 400;
    if (++_total > MAX_HEADER_BYTES) return 431;

    if (_pending_cr) {
        _pending_cr = false;
        if (c != '\n') return 400;  // bare CR
        return endLine();
    }
    if (c == '\r') {
        _pending_cr = true;
        return NEED_MORE;
    }
    if (c == '\n') return 400;  // bare LF

    switch (_phase) {
        case REQUEST_LINE:
            // The parser validates the request line after DONE.
            return append(c) ? NEED_MORE : 431;

        case HEADER_NAME:
            if (c == ':') {
                if (_name_len == 0 && !_name_long) return 400;
                _keep_line = !_name_long &&
                    (headerNameEquals(_name, _name + _name_len, "Content-Length") ||
                     headerNameEquals(_name, _name + _name_len, "Transfer-Encoding"));
                if (_keep_line && (!appendText(_name, _name_len) || !append(':'))) return 431;
                _phase = HEADER_VALUE;
                return NEED_MORE;
            }
            if (!isTokenChar(c)) return 400;
            if (_name_len < NAME_CAP) {
                _name[_name_len++] = c;
            } else {
                _name_long = true;  // too long to be a kept header
            }
            return NEED_MORE;

        case HEADER_VALUE:
            if (isFieldValueCtl(c)) return 400;
            if (_keep_line && !append(c)) return 431;
            return NEED_MORE;

        case FINISHED:
            return DONE;
    }
    return 400;
}

// Header lines have already been received in full. Parse framing before routing.
uint16_t parseRequestHeaders(char* headers, char* method, char* path,
                             uint16_t* body_length, bool* has_length) {
    char* line_end = strstr(headers, "\r\n");
    if (!line_end) return 400;
    *line_end = '\0';
    char version[16] = {}, extra = 0;
    if (sscanf(headers, "%7s %63s %15s %c", method, path, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)) return 400;
    *body_length = 0;
    *has_length = false;
    uint32_t parsed_length = 0;
    for (char* line = line_end + 2; *line != '\r'; line = line_end + 2) {
        line_end = strstr(line, "\r\n");
        if (!line_end) return 400;
        const char* colon = (const char*)memchr(line, ':', (size_t)(line_end - line));
        if (!colon || colon == line) return 400;
        for (const char* p = line; p < colon; ++p) {
            if (!isTokenChar(*p)) return 400;
        }
        for (const char* p = colon + 1; p < line_end; ++p) {
            if (isFieldValueCtl(*p)) return 400;
        }
        if (headerNameEquals(line, colon, "Transfer-Encoding")) return 400;
        if (!headerNameEquals(line, colon, "Content-Length")) continue;
        if (*has_length) return 400;
        *has_length = true;
        const char* p = colon + 1;
        while (p < line_end && (*p == ' ' || *p == '\t')) ++p;
        if (p == line_end || *p < '0' || *p > '9') return 400;
        while (p < line_end && *p >= '0' && *p <= '9') {
            const uint32_t digit = (unsigned)(*p++ - '0');
            if (parsed_length > (UINT32_MAX - digit) / 10) return 400;
            parsed_length = parsed_length * 10 + digit;
        }
        while (p < line_end && (*p == ' ' || *p == '\t')) ++p;
        if (p != line_end) return 400;
    }
    if (parsed_length > 511) return 413;
    *body_length = (uint16_t)parsed_length;
    return 0;
}
