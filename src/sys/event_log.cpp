#include "event_log.h"
#include "application.h"
#include <string.h>

EventLog::EventLog() : _head(0), _count(0) {
    memset(_buf, 0, sizeof(_buf));
}

void EventLog::push(AuraEventType type, uint8_t speed, uint16_t rpm,
                    float voc, float pm25, const char* msg)
{
    LogEntry& e = _buf[_head];
    e.ts     = Time.now();
    e.type   = type;
    e.speed  = speed;
    e.rpm    = rpm;
    e.voc    = voc;
    e.pm25   = pm25;
    memset(e.msg, 0, sizeof(e.msg));
    if (msg) strncpy(e.msg, msg, sizeof(e.msg) - 1);

    _head = (_head + 1) % CAPACITY;
    if (_count < CAPACITY) _count++;
}

const LogEntry& EventLog::entry(uint8_t i) const {
    // i=0 → newest
    if (_count == 0) {
        return _buf[0];
    }
    uint8_t idx = (_head + CAPACITY - 1 - (i % _count)) % CAPACITY;
    return _buf[idx];
}
