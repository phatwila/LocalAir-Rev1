#include "adaptive_baseline.h"
#include <string.h>
#include <math.h>

AdaptiveBaseline::AdaptiveBaseline()
    : _head(0), _count(0), _baseline(DEFAULT)
{
    memset(_history, 0, sizeof(_history));
}

float AdaptiveBaseline::update(float voc) {
    _history[_head] = voc;
    _head = (_head + 1) % HISTORY_LEN;
    if (_count < HISTORY_LEN) _count++;

    if (_count >= 10) {
        float raw = percentile10();
        float alpha = (raw < _baseline) ? ALPHA_DOWN : ALPHA_UP;
        _baseline = alpha * raw + (1.0f - alpha) * _baseline;
        if (_baseline < MIN_BASE) _baseline = MIN_BASE;
        if (_baseline > MAX_BASE) _baseline = MAX_BASE;
    }
    return _baseline;
}

void AdaptiveBaseline::set(float v) {
    if (v >= MIN_BASE && v <= MAX_BASE) _baseline = v;
}

uint16_t AdaptiveBaseline::exportHistory(float* buf, uint16_t maxlen) const {
    uint16_t n = (_count < maxlen) ? _count : maxlen;
    // Export in chronological order
    for (uint16_t i = 0; i < n; i++) {
        uint16_t idx = (_head + HISTORY_LEN - _count + i) % HISTORY_LEN;
        buf[i] = _history[idx];
    }
    return n;
}

void AdaptiveBaseline::importHistory(const float* buf, uint16_t count) {
    _head = 0; _count = 0;
    uint16_t n = (count < HISTORY_LEN) ? count : HISTORY_LEN;
    memcpy(_history, buf, n * sizeof(float));
    _head  = n % HISTORY_LEN;
    _count = n;
}

float AdaptiveBaseline::percentile10() const {
    // Insertion-sort copy to find 10th percentile without heap allocation.
    // Works on up to 200 floats — fast enough on Cortex-M3 at 120 MHz.
    float sorted[HISTORY_LEN];
    memcpy(sorted, _history, _count * sizeof(float));
    for (uint16_t i = 1; i < _count; i++) {
        float key = sorted[i];
        int16_t j = (int16_t)i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; j--; }
        sorted[j+1] = key;
    }
    uint16_t idx = _count / 10;
    if (idx >= _count) idx = _count - 1;
    return sorted[idx];
}
