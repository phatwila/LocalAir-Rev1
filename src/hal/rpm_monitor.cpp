#include "rpm_monitor.h"
#include "../algo/ach_model.h"

RpmMonitor::RpmMonitor()
    : _target(1), _last_mismatch(false), _mismatch_streak(0), _match_streak(0), _running(true) {}

void RpmMonitor::record(RpmSample sample) {
    Lock lock(_mutex);
    sample.sequence = _sample.sequence + 1;
    if (!sample.sequence) ++sample.sequence;
    if (!sample.valid) {
        _last_mismatch = false;
        _mismatch_streak = _match_streak = 0;
    } else {
        const uint16_t expected = AchModel::expectedRpm(_target);
        const bool mismatch = sample.value == 0 ? expected > 0
            : classifyMismatch(expected, sample.value, _last_mismatch);
        if (mismatch) {
            if (_mismatch_streak < 255) ++_mismatch_streak;
            _match_streak = 0;
            if (_mismatch_streak >= ENTER_STREAK) _last_mismatch = true;
        } else {
            if (_match_streak < 255) ++_match_streak;
            _mismatch_streak = 0;
            if (_match_streak >= EXIT_STREAK) _last_mismatch = false;
        }
    }
    sample.mismatch = _last_mismatch;
    _sample = sample;
}

void RpmMonitor::run(RpmSample (*get_rpm_fn)()) {
    while (true) {
        { Lock lock(_mutex); if (!_running) return; }
        delay(POLL_INTERVAL_MS);
        record(get_rpm_fn());
    }
}
