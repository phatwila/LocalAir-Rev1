#pragma once
#include "application.h"
#include <stdint.h>
#include "rpm_sample.h"

// Telemetry-only background thread: samples RPM every 5s.
// Uses the RPM calibration table from ach_model to compute expected RPM.
class RpmMonitor {
public:
    static constexpr uint32_t POLL_INTERVAL_MS = 5000;
    static constexpr float    ENTER_TOLERANCE  = 0.14f;
    static constexpr float    EXIT_TOLERANCE   = 0.10f;
    // Keep the absolute deadband below the low-speed shortfall we now expect.
    static constexpr uint16_t ABS_DEADBAND_RPM = 100;
    static constexpr uint8_t  ENTER_STREAK     = 2;
    static constexpr uint8_t  EXIT_STREAK      = 2;

    RpmMonitor();

    void stop() { Lock lock(_mutex); _running = false; }
    void setTarget(uint8_t speed) { Lock lock(_mutex); _target = speed; }
    void record(RpmSample sample);
    RpmSample snapshot() const { Lock lock(_mutex); return _sample; }

    // Call from a dedicated thread. Exits when stop() clears _running.
    void run(RpmSample (*get_rpm_fn)());

    static bool classifyMismatch(uint16_t expected, uint16_t actual, bool previous) {
        if (expected == 0 || actual == 0) return previous;

        const uint16_t diff = (actual > expected) ? (actual - expected) : (expected - actual);
        const float rel = (float)diff / (float)expected;
        if (previous) return rel > EXIT_TOLERANCE;
        if (diff <= ABS_DEADBAND_RPM) return false;
        return rel > ENTER_TOLERANCE;
    }

    uint16_t lastRpm() const { return snapshot().currentValue(millis()); }
    bool lastMismatch() const { const auto s = snapshot(); return s.fresh(millis()) && s.mismatch; }

private:
    struct Lock {
        RecursiveMutex& mutex;
        explicit Lock(RecursiveMutex& m) : mutex(m) { mutex.lock(); }
        ~Lock() { mutex.unlock(); }
    };
    mutable RecursiveMutex _mutex;
    RpmSample _sample;
    uint8_t _target;
    bool     _last_mismatch;
    uint8_t  _mismatch_streak;
    uint8_t  _match_streak;
    bool _running;
};
