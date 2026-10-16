#pragma once
#include <stdint.h>

// Fan speed step convention: higher step number = slower fan = quieter.
//   step 1  = loudest (2478 RPM, PWM reg 1)
//   step 16 = quietest (693 RPM, PWM reg 24)
//
// Steps are logical — DaughterBoard maps each step to its calibrated PWM
// register via STEP_TO_REG[]. Steps are chosen so each increment carries a
// perceptible and roughly equal CADR change (~8.6 m³/h).
//
// These helpers encode that convention once so every callsite expresses
// intent (floor/ceiling in airflow terms) rather than raw register direction.

// Ensure fan runs at least as fast as min_speed (airflow floor).
// If speed is currently slower (higher register value), speed up to min_speed.
inline uint8_t fanFloor(uint8_t speed, uint8_t min_speed) {
    return (speed > min_speed) ? min_speed : speed;
}

// Ensure fan runs no faster than max_speed (airflow ceiling).
// If speed is currently faster (lower register value), slow down to max_speed.
inline uint8_t fanCeiling(uint8_t speed, uint8_t max_speed) {
    return (speed < max_speed) ? max_speed : speed;
}

// Is fan 'a' running faster (louder, more airflow) than fan 'b'?
inline bool fanFasterThan(uint8_t a, uint8_t b) { return a < b; }

// Is fan 'a' running slower (quieter, less airflow) than fan 'b'?
inline bool fanSlowerThan(uint8_t a, uint8_t b) { return a > b; }

// Convert a 0 to 100 percent fan request into a raw speed register.
// 0% maps to the slowest speed, 100% maps to the fastest speed.
inline uint8_t fanSpeedFromPercent(uint8_t percent, uint8_t min_speed, uint8_t max_speed) {
    if (min_speed >= max_speed) return min_speed;
    if (percent == 0) return max_speed;
    if (percent >= 100) return min_speed;

    uint16_t span = (uint16_t)(max_speed - min_speed);
    uint16_t offset = (uint16_t)(((uint32_t)percent * span + 50u) / 100u);
    uint8_t speed = (uint8_t)(max_speed - offset);
    if (speed < min_speed) speed = min_speed;
    if (speed > max_speed) speed = max_speed;
    return speed;
}

// Convert a raw speed register into a 0 to 100 percent fan value.
// The slowest register becomes 0%, the fastest register becomes 100%.
inline uint8_t fanPercentFromSpeed(uint8_t speed, uint8_t min_speed, uint8_t max_speed) {
    if (min_speed >= max_speed) return 0;
    if (speed == 0) return 0;
    if (speed <= min_speed) return 100;
    if (speed >= max_speed) return 0;

    uint16_t span = (uint16_t)(max_speed - min_speed);
    uint16_t offset = (uint16_t)(max_speed - speed);
    return (uint8_t)(((uint32_t)offset * 100u + (span / 2u)) / span);
}

// Return 0 when no applied step satisfies both inclusive percent bounds.
// Scan slower steps first so equal distances choose less airflow.
inline uint8_t fanStepWithinPercentBounds(float target, uint8_t floor_pct, uint8_t cap_pct) {
    if (floor_pct > cap_pct || cap_pct > 100) return 0;
    uint8_t best_step = 0;
    float best_distance = 101.0f;
    for (int step = 16; step >= 1; --step) {
        const uint8_t mapped = fanPercentFromSpeed((uint8_t)step, 1, 16);
        if (mapped < floor_pct || mapped > cap_pct) continue;
        const float distance = mapped >= target ? mapped - target : target - mapped;
        if (distance < best_distance) {
            best_distance = distance;
            best_step = (uint8_t)step;
        }
    }
    return best_step;
}
