#pragma once
#include <stdint.h>

// 10th-percentile rolling baseline for SGP30 VOC.
// Learns the room's clean-air floor over time.
// Asymmetric EMA: fast to drop (α=0.20), slow to rise (α=0.02).
// Persisted to external flash by config_store; this class is pure algorithm.
class AdaptiveBaseline {
public:
    static constexpr uint16_t HISTORY_LEN = 200;
    static constexpr float    ALPHA_DOWN  = 0.20f;
    static constexpr float    ALPHA_UP    = 0.02f;
    static constexpr float    MIN_BASE    = 5.0f;
    static constexpr float    MAX_BASE    = 2000.0f;
    static constexpr float    DEFAULT     = 20.0f;

    AdaptiveBaseline();

    // Push one reading, return updated baseline.
    float update(float voc);

    float get() const { return _baseline; }
    void  set(float v); // for restoring from flash

    // Copy internal history out (for persistence) — returns count written.
    uint16_t exportHistory(float* buf, uint16_t maxlen) const;
    void     importHistory(const float* buf, uint16_t count);

private:
    float    _history[HISTORY_LEN];
    uint16_t _head;
    uint16_t _count;
    float    _baseline;

    float percentile10() const;
};
