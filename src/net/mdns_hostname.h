#pragma once

#include <stddef.h>

static constexpr size_t MDNS_HOSTNAME_MAX_LEN = 23;

inline bool mdnsBaseNameIsValid(const char* s) {
    if (!s || !s[0]) return false;

    size_t len = 0;
    bool previous_hyphen = false;
    for (; s[len]; ++len) {
        if (len >= MDNS_HOSTNAME_MAX_LEN) return false;

        const char c = s[len];
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        const bool hyphen = c == '-';
        if (!lower && !digit && !hyphen) return false;
        if (hyphen && (len == 0 || previous_hyphen)) return false;
        previous_hyphen = hyphen;
    }

    if (len == 0 || s[len - 1] == '-') return false;
    return true;
}
