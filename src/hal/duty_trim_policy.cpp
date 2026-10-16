#include "duty_trim_policy.h"

DutyTrimDecision DutyTrimPolicy::update(float accumulator,
                                        int8_t current_trim,
                                        uint16_t rpm_actual,
                                        uint16_t rpm_expected,
                                        int8_t trim_min,
                                        int8_t trim_max,
                                        uint16_t deadband_rpm,
                                        float k_i) {
    DutyTrimDecision result{accumulator, current_trim, false, false};
    if (rpm_actual == 0 || rpm_expected == 0) return result;

    const int16_t err = (int16_t)rpm_expected - (int16_t)rpm_actual;
    if (err <= (int16_t)deadband_rpm && err >= -(int16_t)deadband_rpm) {
        result.next_accumulator = 0.0f;
        result.sample_consumed = true;
        return result;
    }

    // Higher PWM duty slows this fan, so overspeed needs positive trim.
    const float delta = k_i * ((float)rpm_actual - (float)rpm_expected) / (float)rpm_expected * 2.0f;
    float next_accumulator = accumulator + delta;
    int8_t trim_step = 0;
    while (next_accumulator >= 1.0f) {
        ++trim_step;
        next_accumulator -= 1.0f;
    }
    while (next_accumulator <= -1.0f) {
        --trim_step;
        next_accumulator += 1.0f;
    }

    int8_t next_trim = (int8_t)(current_trim + trim_step);
    if (next_trim < trim_min) next_trim = trim_min;
    if (next_trim > trim_max) next_trim = trim_max;
    if (next_trim == trim_max && next_accumulator > 0.0f) next_accumulator = 0.0f;
    if (next_trim == trim_min && next_accumulator < 0.0f) next_accumulator = 0.0f;
    result.next_accumulator = next_accumulator;
    result.next_trim = next_trim;
    result.adjusted = (next_trim != current_trim);
    result.sample_consumed = true;
    return result;
}
