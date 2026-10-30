#pragma once

#include "application.h"
#include <stdint.h>

struct MdnsServiceState {
    IPAddress last_ip;
    uint32_t failed_ms = 0;
    bool retry_pending = false;
    uint8_t failures = 0;
};

// Consecutive failures back off 5 s, 10 s, 20 s, 40 s, then 60 s.
inline uint32_t mdnsServiceRetryMs(uint8_t failures) {
    constexpr uint32_t RETRY_MS = 5000;
    constexpr uint32_t MAX_RETRY_MS = 60000;
    const uint8_t shift = failures > 1 ? (uint8_t)(failures - 1) : 0;
    return shift >= 4 ? MAX_RETRY_MS : RETRY_MS << shift;
}

inline void mdnsServiceFailed(MdnsServiceState& state) {
    state.failed_ms = millis();
    state.retry_pending = true;
    if (state.failures < UINT8_MAX) state.failures++;
}

// Returns true when the published network state should be refreshed.
template <typename Responder>
bool mdnsServiceTick(MdnsServiceState& state, Responder& responder,
                     bool& started, bool wifi_ready, const char* base_name) {
    if (!wifi_ready) {
        const bool changed = started;
        if (started) {
            responder.stop();
            started = false;
        }
        state.retry_pending = false;
        state.failures = 0;
        return changed;
    }
    bool changed = false;
    if (started && !responder.running()) {
        started = false;
        mdnsServiceFailed(state);
        changed = true;
    }
    if (!started) {
        if (state.retry_pending &&
            (uint32_t)(millis() - state.failed_ms) < mdnsServiceRetryMs(state.failures)) return changed;
        if (!responder.begin(base_name)) {
            mdnsServiceFailed(state);
            return changed;
        }
        started = true;
        state.retry_pending = false;
        state.failures = 0;
        state.last_ip = WiFi.localIP();
        changed = true;
    }
    const IPAddress ip = WiFi.localIP();
    if (ip != state.last_ip) {
        if (!responder.restart(base_name)) {
            started = false;
            mdnsServiceFailed(state);
            return true;
        }
        state.last_ip = ip;
        changed = true;
    }
    responder.poll();
    if (!responder.running()) {
        started = false;
        mdnsServiceFailed(state);
        changed = true;
    }
    return changed;
}
