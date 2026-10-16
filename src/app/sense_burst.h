#pragma once
#include <stdint.h>

// Boot spin helper: keep the fan at startup target until we have the first
// full sensor cycle or a timeout forces auto handoff.
class SenseBurst {
public:
    static constexpr uint8_t  STARTUP_SPEED      = 1;
    static constexpr uint32_t STARTUP_MIN_SPIN_MS = 20000;
    static constexpr uint32_t STARTUP_TIMEOUT_MS = 60000;

    SenseBurst();

    void start(uint32_t now_ms);
    void completeBySensor(uint32_t now_ms);
    void completeByTimeout(uint32_t now_ms);
    void cancel();
    bool isActive() const { return _active; }
    bool isComplete() const { return _complete; }
    bool completedBySensor() const { return _completed_by_sensor; }
    bool timedOut() const { return _timed_out; }
    uint32_t startMs() const { return _start_ms; }
    uint32_t elapsedMs(uint32_t now_ms) const;

private:
    bool     _active;
    bool     _complete;
    bool     _completed_by_sensor;
    bool     _timed_out;
    uint32_t _start_ms;
};
