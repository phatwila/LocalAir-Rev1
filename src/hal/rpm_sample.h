#pragma once
#include <stdint.h>

struct RpmSample {
    static constexpr uint32_t MAX_AGE_MS = 10000;
    uint16_t value = 0;
    bool valid = false;
    bool mismatch = false;
    uint32_t sampled_at_ms = 0;
    uint32_t sequence = 0;
    uint32_t age(uint32_t now) const { return sequence ? now - sampled_at_ms : UINT32_MAX; }
    bool fresh(uint32_t now) const { return valid && age(now) <= MAX_AGE_MS; }
    uint16_t currentValue(uint32_t now) const { return fresh(now) ? value : 0; }
    bool consumeForLearning(uint32_t now, uint32_t& previous_sequence) const {
        if (!fresh(now) || !value || sequence == previous_sequence) return false;
        previous_sequence = sequence;
        return true;
    }
};

// A completed pulse-free window is valid zero feedback, not proof of a stall.
inline RpmSample readTachWindow(uint32_t now, uint32_t pulses, bool ready,
                               uint32_t& previous_ms, uint32_t& previous_pulses,
                               uint32_t pulses_per_revolution) {
    RpmSample sample{};
    sample.sampled_at_ms = now;
    const uint32_t elapsed = now - previous_ms;
    if (!elapsed) return sample;
    const uint32_t count = pulses - previous_pulses;
    previous_ms = now;
    previous_pulses = pulses;
    if (!ready || elapsed > RpmSample::MAX_AGE_MS || !pulses_per_revolution) return sample;
    const uint64_t rpm = (uint64_t)count * 60000u / elapsed / pulses_per_revolution;
    sample.valid = true;
    sample.value = rpm > 65535u ? 65535u : (uint16_t)rpm;
    return sample;
}
