#pragma once

#include <stdint.h>

#include "../algo/fan_speed.h"

enum class AutoTrustLevel : uint8_t {
    Valid = 0,
    Degraded = 1,
    Stale = 2,
    Fault = 3,
};

enum class AutoLane : uint8_t {
    None = 0,
    Particle = 1,
    Gas = 2,
    Ventilation = 3,
};

enum class AutoMode : uint8_t {
    Idle = 0,
    Maintain = 1,
    ElevatedPM = 2,
    PurgePM = 3,
    ElevatedGas = 4,
    PurgeGas = 5,
    Recover = 6,
    FaultSafe = 7,
};

enum class LaneSeverity : uint8_t {
    None = 0,
    Low = 1,
    Elevated = 2,
    Purge = 3,
};

struct SensorSnapshot {
    float voc_ppb;
    float eco2_ppm;
    float pm25;
    float temp_c;
    float rh_pct;
    uint32_t sample_ms;
    uint32_t pm_age_ms;
    uint32_t gas_age_ms;
    uint32_t humidity_age_ms;
    uint32_t pm_sample_generation;
    uint32_t gas_sample_generation;
    bool sgp30_in_warmup;
    bool humidity_comp_ok;
    // Live adaptive clean-air baseline (ppb). 0/NAN = not available yet.
    float voc_baseline_ppb;
};

struct AutoCycleInputs {
    SensorSnapshot sensor;
    float room_m3;
    float learned_room_m3;
    float effective_cadr_m3h;
    float current_airflow_pct;
    uint8_t current_step;
    uint8_t min_fan_percent;
    uint8_t max_fan_percent;
    uint32_t now_ms;
};

struct SensorTrust {
    AutoTrustLevel pm;
    AutoTrustLevel gas;
    AutoTrustLevel temp_rh;
    AutoTrustLevel eco2;
};

struct LaneState {
    AutoLane lane;
    AutoTrustLevel trust;
    LaneSeverity severity;
    float score;
    float confidence;
    bool authority_enabled;
    uint32_t reason_bits;
};

struct AirflowTarget {
    float target_airflow_pct;
    uint8_t quantized_step;
};

struct AutoV3Telemetry {
    uint8_t auto_owner;
    uint8_t auto_mode;
    AutoLane dominant_lane;
    uint8_t target_airflow_pct;
    uint8_t applied_airflow_pct;
    uint8_t quantized_step;
    bool sensor_voc_valid;
    bool sensor_eco2_valid;
    bool sensor_pm_valid;
    bool sensor_rh_valid;
    bool sensor_full_valid;
    char reason[32];
};

class AutoEngineV3 {
public:
    struct Result {
        SensorTrust trust;
        LaneState pm_lane;
        LaneState gas_lane;
        LaneState vent_lane;
        AutoLane dominant_lane;
        AutoLane secondary_lane;
        AutoMode mode;
        float target_airflow_pct;
        float limited_airflow_pct;
        uint8_t quantized_step;
        float applied_airflow_pct;
        bool user_cap_applied;
        bool user_floor_applied;
        char reason[32];
    };

    AutoEngineV3();

    void reset();

    Result computeTarget(const AutoCycleInputs& inputs);

    static AirflowTarget quantizeTarget(float airflow_pct, uint8_t min_speed = 1,
                                       uint8_t max_speed = 16);

private:
    struct InternalState {
        bool initialized;
        AutoMode mode;
        AutoLane dominant_lane;
        AutoLane dominant_candidate;
        uint32_t dominant_candidate_since_ms;
        AutoMode mode_candidate;
        uint32_t mode_candidate_since_ms;
        uint32_t last_mode_change_ms;
        float pm_fast;
        float pm_slow;
        float pm_prev_fast;
        float voc_fast;
        float voc_slow;
        float voc_prev_fast;
        float eco2_fast;
        float eco2_slow;
        float voc_baseline;
        uint32_t last_purge_ms;
        uint32_t vent_high_since_ms;
        float last_target_airflow_pct;
        float last_limited_airflow_pct;
        uint8_t last_quantized_step;
        uint32_t last_rate_limit_ms;
        uint32_t last_compute_ms;
        uint32_t last_pm_sample_ms;
        uint32_t last_gas_sample_ms;
        uint32_t last_pm_generation;
        uint32_t last_gas_generation;
        float pm_rise_score;
        float voc_rise_score;
        bool rate_limit_initialized;
        uint32_t last_downshift_ms;
    };

    InternalState _state;
};
