#pragma once

#include <stdint.h>
#include <stdio.h>

struct HttpDebugView {
    uint8_t sse_active;
    uint8_t sse_max;
    uint32_t sse_accepted;
    uint32_t sse_denied;
    uint32_t sse_reaped;
    uint32_t sse_heartbeats;
    uint32_t sse_broadcasts;
    uint32_t last_sse_heartbeat_ms;
    uint32_t last_sse_broadcast_ms;
};

template <typename Out>
inline void renderHttpDebugJson(Out& out, const HttpDebugView& v) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"sse\":{\"active\":%u,\"max\":%u,\"accepted\":%lu,"
             "\"denied\":%lu,\"reaped\":%lu,\"heartbeats\":%lu,"
             "\"broadcasts\":%lu,\"last_heartbeat_ms\":%lu,"
             "\"last_broadcast_ms\":%lu}}",
             (unsigned)v.sse_active,
             (unsigned)v.sse_max,
             (unsigned long)v.sse_accepted,
             (unsigned long)v.sse_denied,
             (unsigned long)v.sse_reaped,
             (unsigned long)v.sse_heartbeats,
             (unsigned long)v.sse_broadcasts,
             (unsigned long)v.last_sse_heartbeat_ms,
             (unsigned long)v.last_sse_broadcast_ms);
    out.print(buf);
}
