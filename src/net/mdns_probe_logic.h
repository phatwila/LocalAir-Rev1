#pragma once

#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>

enum MdnsConflictMode : uint8_t {
    MDNS_CONFLICT_MODE_PROBE = 0,
    MDNS_CONFLICT_MODE_RUNTIME = 1,
};

inline uint32_t mdnsPackIpv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c << 8) | (uint32_t)d;
}

inline int mdnsCompareIpv4(uint32_t lhs, uint32_t rhs) {
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}

inline bool mdnsFormatCandidateName(char* out, size_t out_len,
                                    const char* base_name, uint16_t suffix) {
    if (!out || out_len == 0 || !base_name || !base_name[0]) return false;
    if (suffix == 0) {
        strncpy(out, base_name, out_len - 1);
        out[out_len - 1] = '\0';
    } else {
        snprintf(out, out_len, "%s-%u", base_name, (unsigned)suffix);
    }
    const size_t len = strlen(out);
    return len > 0 && len < out_len;
}

inline bool mdnsCiEqual(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}

inline bool mdnsNameMatchesCandidate(const char* name, const char* candidate) {
    if (!name || !candidate) return false;
    if (strcasecmp(name, candidate) == 0) return true;
    char fqdn[96];
    snprintf(fqdn, sizeof(fqdn), "%s.local", candidate);
    return strcasecmp(name, fqdn) == 0;
}

inline bool mdnsMatchLabelText(const uint8_t* pkt, uint16_t pkt_len,
                               uint16_t offset, const char* text, uint8_t len) {
    if (!pkt || !text) return false;
    if ((uint16_t)(offset + 1 + len) > pkt_len) return false;
    if (pkt[offset] != len) return false;
    for (uint8_t i = 0; i < len; ++i) {
        if (!mdnsCiEqual((char)pkt[offset + 1 + i], text[i])) return false;
    }
    return true;
}

inline bool mdnsNameEqualsCandidate(const uint8_t* pkt, uint16_t pkt_len,
                                    uint16_t offset, const char* candidate) {
    if (!pkt || !candidate) return false;
    const uint8_t len = (uint8_t)strlen(candidate);
    if (!mdnsMatchLabelText(pkt, pkt_len, offset, candidate, len)) return false;
    offset = (uint16_t)(offset + 1 + len);
    if (!mdnsMatchLabelText(pkt, pkt_len, offset, "local", 5)) return false;
    offset = (uint16_t)(offset + 6);
    return offset < pkt_len && pkt[offset] == 0;
}

inline bool mdnsReadName(const uint8_t* pkt, uint16_t pkt_len, uint16_t offset,
                         char* out, size_t out_len, uint16_t* out_next) {
    if (!pkt || !out || out_len == 0 || !out_next) return false;
    size_t pos = 0;
    uint16_t cur = offset;
    uint16_t next = offset;
    bool jumped = false;
    uint8_t jumps = 0;

    while (cur < pkt_len) {
        uint8_t len = pkt[cur];
        if (len == 0) {
            if (!jumped) next = (uint16_t)(cur + 1);
            out[pos] = '\0';
            *out_next = next;
            return true;
        }
        if ((len & 0xC0) == 0xC0) {
            if ((uint16_t)(cur + 1) >= pkt_len || jumps++ > 8) return false;
            uint16_t ptr = (uint16_t)(((len & 0x3F) << 8) | pkt[cur + 1]);
            if (ptr >= pkt_len) return false;
            if (!jumped) next = (uint16_t)(cur + 2);
            cur = ptr;
            jumped = true;
            continue;
        }
        if ((len & 0xC0) != 0 || (uint16_t)(cur + 1 + len) > pkt_len) return false;
        if (pos != 0) {
            if (pos + 1 >= out_len) return false;
            out[pos++] = '.';
        }
        if (pos + len >= out_len) return false;
        for (uint8_t i = 0; i < len; ++i) {
            out[pos++] = (char)pkt[cur + 1 + i];
        }
        cur = (uint16_t)(cur + 1 + len);
        if (!jumped) next = cur;
    }

    return false;
}

inline bool mdnsSkipQuestion(const uint8_t* pkt, uint16_t pkt_len, uint16_t* offset_io,
                             char* out_name, size_t out_name_len, uint16_t* out_qtype) {
    if (!pkt || !offset_io || !out_name || out_name_len == 0 || !out_qtype) return false;
    uint16_t offset = *offset_io;
    uint16_t next = 0;
    if (!mdnsReadName(pkt, pkt_len, offset, out_name, out_name_len, &next)) return false;
    if ((uint16_t)(next + 4) > pkt_len) return false;
    *out_qtype = (uint16_t)(((uint16_t)pkt[next] << 8) | pkt[next + 1]);
    *offset_io = (uint16_t)(next + 4);
    return true;
}

inline bool mdnsReadRecord(const uint8_t* pkt, uint16_t pkt_len, uint16_t* offset_io,
                           char* out_name, size_t out_name_len,
                           uint16_t* out_type, uint16_t* out_class,
                           const uint8_t** out_rdata, uint16_t* out_rdlen) {
    if (!pkt || !offset_io || !out_name || out_name_len == 0 ||
        !out_type || !out_class || !out_rdata || !out_rdlen) {
        return false;
    }
    uint16_t offset = *offset_io;
    uint16_t next = 0;
    if (!mdnsReadName(pkt, pkt_len, offset, out_name, out_name_len, &next)) return false;
    if ((uint16_t)(next + 10) > pkt_len) return false;
    *out_type = (uint16_t)(((uint16_t)pkt[next] << 8) | pkt[next + 1]);
    *out_class = (uint16_t)(((uint16_t)pkt[next + 2] << 8) | pkt[next + 3]);
    *out_rdlen = (uint16_t)(((uint16_t)pkt[next + 8] << 8) | pkt[next + 9]);
    next = (uint16_t)(next + 10);
    if ((uint16_t)(next + *out_rdlen) > pkt_len) return false;
    *out_rdata = pkt + next;
    *offset_io = (uint16_t)(next + *out_rdlen);
    return true;
}

inline bool mdnsPacketConflicts(const uint8_t* pkt, uint16_t pkt_len,
                                const char* candidate, uint32_t our_ip,
                                MdnsConflictMode mode) {
    if (!pkt || pkt_len < 12 || !candidate) return false;

    const bool is_response = (pkt[2] & 0x80) != 0;
    const uint16_t qdcount = (uint16_t)(((uint16_t)pkt[4] << 8) | pkt[5]);
    const uint16_t ancount = (uint16_t)(((uint16_t)pkt[6] << 8) | pkt[7]);
    const uint16_t nscount = (uint16_t)(((uint16_t)pkt[8] << 8) | pkt[9]);
    const uint16_t arcount = (uint16_t)(((uint16_t)pkt[10] << 8) | pkt[11]);

    uint16_t offset = 12;
    for (uint16_t i = 0; i < qdcount; ++i) {
        char name[96];
        uint16_t qtype = 0;
        if (!mdnsSkipQuestion(pkt, pkt_len, &offset, name, sizeof(name), &qtype)) return false;
        (void)name;
        (void)qtype;
    }

    uint32_t peer_ip = 0;
    const uint16_t rr_total = (uint16_t)(ancount + nscount + arcount);
    for (uint16_t i = 0; i < rr_total; ++i) {
        char name[96];
        uint16_t type = 0;
        uint16_t rrclass = 0;
        const uint8_t* rdata = nullptr;
        uint16_t rdlen = 0;
        if (!mdnsReadRecord(pkt, pkt_len, &offset, name, sizeof(name), &type, &rrclass,
                            &rdata, &rdlen)) {
            return false;
        }
        if (!mdnsNameMatchesCandidate(name, candidate)) continue;
        if (type != 1 || (rrclass & 0x7FFF) != 1 || rdlen != 4) continue;
        peer_ip = mdnsPackIpv4(rdata[0], rdata[1], rdata[2], rdata[3]);
        if (mode == MDNS_CONFLICT_MODE_RUNTIME) {
            return peer_ip != our_ip;
        }
        if (is_response) {
            return peer_ip != our_ip;
        }
        const int cmp = mdnsCompareIpv4(peer_ip, our_ip);
        return cmp <= 0;
    }

    if (mode == MDNS_CONFLICT_MODE_RUNTIME) return false;
    return false;
}
