#pragma once
#include <stdint.h>

struct DutyTrimDecision {
    float  next_accumulator;
    int8_t next_trim;
    bool   adjusted;
    bool   sample_consumed;
};

class DutyTrimPolicy {
public:
    static DutyTrimDecision update(float accumulator,
                                   int8_t current_trim,
                                   uint16_t rpm_actual,
                                   uint16_t rpm_expected,
                                   int8_t trim_min,
                                   int8_t trim_max,
                                   uint16_t deadband_rpm,
                                   float k_i);
};
