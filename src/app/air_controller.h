#pragma once
#include <stdint.h>
#include "../algo/kalman_voc.h"
#include "../algo/adaptive_baseline.h"
#include "../algo/ach_model.h"
#include "../algo/humidity_comp.h"

struct SensorReading {
    float voc_ppb;
    float eco2_ppm;
    float pm25;
    float temp_c;
    float rh_pct;
};

struct ControlState {
    // Canonical v3 runtime core.
    uint8_t   target_speed;

    // Transitional telemetry and diagnostics.
    float     voc_adj;
    float     voc_kalman;
    float     voc_velocity;
    char      clamp_reason[32];
};

// Slow-estimator infrastructure for LocalAir.
// computeTarget() and all v2 control math have been removed.
// Responsibilities: Kalman VOC, adaptive baseline, and EEPROM persistence.
class AirController {
public:
    static constexpr uint8_t  MIN_SPEED       = 1;
    static constexpr uint8_t  MAX_SPEED       = 16;
    static constexpr uint8_t  SPEED_SENSOR_FLOOR = 10;
    static constexpr uint8_t  IDLE_MIN_PERCENT         = 40;
    static constexpr uint8_t  IDLE_MAX_PERCENT_DEFAULT = 65;

    AirController();

    void configure(float room_m3, uint8_t room_profile,
                   uint8_t max_fan_percent = 100, uint8_t min_fan_percent = 50);

    // Slow lane: update long-memory estimators. Call every ~20s.
    void updateSlowEstimators(const SensorReading& s, uint32_t now_ms,
                              uint8_t commanded_speed = 1, bool gas_valid = true);

    // Expose sub-components for persistence
    KalmanVOC&        kalman()   { return _kalman; }
    AdaptiveBaseline& baseline() { return _baseline; }
    AchModel&         ach()      { return _ach; }
    float vocKalman() const { return _slow.voc_kalman; }
    float vocVelocity() const { return _slow.voc_velocity; }
    bool slowStateValid() const { return _slow.valid; }

private:
    KalmanVOC        _kalman;
    AdaptiveBaseline _baseline;
    AchModel         _ach;
    HumidityComp     _humidity;   // stateless

    // Cached slow-estimator state, updated by updateSlowEstimators().
    struct SlowState {
        float   voc_kalman;
        float   voc_velocity;
        float   baseline;
        bool    valid;
    };
    SlowState _slow;

    uint8_t _room_profile;
    uint8_t _max_fan_percent;
    uint8_t _min_fan_percent;
};
