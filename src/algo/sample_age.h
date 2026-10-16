#pragma once

#include <stdint.h>

// Advance on every service pass. A faulted age cannot become fresh through
// millis() rollover; only a new valid sample resets it.
class SampleAge {
public:
    SampleAge() : _last_tick_ms(0), _age_ms(0), _started(false), _seen(false), _generation(0) {}

    void tick(uint32_t now_ms) {
        if (_started && _seen) {
            const uint32_t elapsed = now_ms - _last_tick_ms;
            const uint32_t max_add = UINT32_MAX - _age_ms;
            _age_ms += elapsed > max_add ? max_add : elapsed;
        }
        _last_tick_ms = now_ms;
        _started = true;
    }

    void sample(uint32_t now_ms) {
        tick(now_ms);
        _seen = true;
        _age_ms = 0;
        ++_generation;
        if (_generation == 0) ++_generation;
    }

    uint32_t ageMs() const { return _seen ? _age_ms : UINT32_MAX; }
    uint32_t generation() const { return _generation; }

private:
    uint32_t _last_tick_ms;
    uint32_t _age_ms;
    bool _started;
    bool _seen;
    uint32_t _generation;
};
