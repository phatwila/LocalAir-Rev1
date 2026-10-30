#include "auto_engine_v3.h"

#include "../sys/config_store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t PM_VALID_MS = 3000;
constexpr uint32_t PM_DEGRADED_MS = 10000;
constexpr uint32_t PM_FAULT_MS = 20000;
constexpr uint32_t GAS_VALID_MS = 2000;
constexpr uint32_t GAS_DEGRADED_MS = 5000;
constexpr uint32_t GAS_FAULT_MS = 15000;
constexpr uint32_t RH_VALID_MS = 3000;
constexpr uint32_t RH_DEGRADED_MS = 10000;
constexpr uint32_t RH_FAULT_MS = 20000;
constexpr uint32_t GAS_WARMUP_MS = 15000;
constexpr uint32_t DOMINANCE_PERSIST_MS = 10000;
constexpr uint32_t MODE_RECOVER_EXIT_MS = 60000;
constexpr uint32_t MODE_PURGE_EXIT_MS = 20000;
constexpr uint32_t VENT_PERSIST_MS = 30000;
constexpr uint32_t DOWNSHIFT_DWELL_MS = 15000;

constexpr float PM_RISE_GAIN = 0.01f;
constexpr float VOC_RISE_GAIN = 0.01f;
constexpr float PM_RISE_CAP = 0.15f;
constexpr float VOC_RISE_CAP = 0.15f;
constexpr float LANE_IDLE_THRESHOLD = 0.12f;
constexpr float LANE_ELEVATED_THRESHOLD = 0.30f;
constexpr float PM_PURGE_THRESHOLD = 0.65f;
constexpr float GAS_PURGE_THRESHOLD = 0.70f;
constexpr float RECOVER_EXIT_THRESHOLD = 0.20f;
constexpr float VENT_ACTIVE_THRESHOLD = 0.15f;
constexpr float VENT_HIGH_THRESHOLD = 0.30f;
constexpr float FAULTSAFE_FALLBACK_TARGET_PCT = 40.0f;

constexpr float PM_ANCHOR_X[] = {0.0f, 9.0f, 35.0f, 55.0f, 125.0f};
constexpr float PM_ANCHOR_Y[] = {0.0f, 0.15f, 0.45f, 0.70f, 1.00f};
constexpr float GAS_ANCHOR_X[] = {0.0f, 120.0f, 250.0f, 450.0f, 900.0f};
constexpr float GAS_ANCHOR_Y[] = {0.0f, 0.20f, 0.45f, 0.75f, 1.00f};
constexpr float VENT_ANCHOR_X[] = {500.0f, 800.0f, 1000.0f, 1500.0f, 2000.0f, 2500.0f};
constexpr float VENT_ANCHOR_Y[] = {0.0f, 0.15f, 0.30f, 0.55f, 0.80f, 1.00f};

static float clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static float clampPct(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 100.0f) return 100.0f;
    return value;
}

static float ewma(float prev, float sample, float alpha) {
    if (!isfinite(prev)) return sample;
    return ((1.0f - alpha) * prev) + (alpha * sample);
}

static float elapsedAlpha(float alpha_1s, uint32_t elapsed_ms) {
    return 1.0f - powf(1.0f - alpha_1s, (float)elapsed_ms / 1000.0f);
}

static AutoTrustLevel trustFromAge(uint32_t age_ms,
                                   uint32_t valid_ms,
                                   uint32_t degraded_ms,
                                   uint32_t fault_ms,
                                   bool force_fault = false) {
    if (force_fault) return AutoTrustLevel::Fault;
    // age 0 = sampled this cycle (freshest); "never seen" is UINT32_MAX from the caller.
    if (age_ms <= valid_ms) return AutoTrustLevel::Valid;
    if (age_ms <= degraded_ms) return AutoTrustLevel::Degraded;
    if (age_ms <= fault_ms) return AutoTrustLevel::Stale;
    return AutoTrustLevel::Fault;
}

static float interpolateAnchors(float x, const float* xs, const float* ys, size_t count) {
    if (count == 0) return 0.0f;
    if (x <= xs[0]) return ys[0];
    for (size_t i = 1; i < count; ++i) {
        if (x <= xs[i]) {
            const float span = xs[i] - xs[i - 1];
            if (span <= 0.0f) return ys[i];
            const float t = (x - xs[i - 1]) / span;
            return ys[i - 1] + (ys[i] - ys[i - 1]) * t;
        }
    }
    return ys[count - 1];
}

static const char* laneName(AutoLane lane) {
    switch (lane) {
        case AutoLane::Particle: return "pm";
        case AutoLane::Gas: return "gas";
        case AutoLane::Ventilation: return "vent";
        case AutoLane::None:
        default: return "none";
    }
}

static const char* modeName(AutoMode mode) {
    switch (mode) {
        case AutoMode::Idle: return "idle";
        case AutoMode::Maintain: return "maintain";
        case AutoMode::ElevatedPM: return "elevated_pm";
        case AutoMode::PurgePM: return "purge_pm";
        case AutoMode::ElevatedGas: return "elevated_gas";
        case AutoMode::PurgeGas: return "purge_gas";
        case AutoMode::Recover: return "recover";
        case AutoMode::FaultSafe:
        default: return "fault_safe";
    }
}

static float modeTargetAch(AutoMode mode, AutoLane dominant) {
    switch (mode) {
        case AutoMode::Idle:
            return 0.0f;
        case AutoMode::Maintain:
            return 2.5f;
        case AutoMode::ElevatedPM:
            return 6.0f;
        case AutoMode::PurgePM:
            return 10.0f;
        case AutoMode::ElevatedGas:
            return 5.0f;
        case AutoMode::PurgeGas:
            return 9.0f;
        case AutoMode::Recover:
            return 3.0f;
        case AutoMode::FaultSafe:
        default:
            return dominant == AutoLane::Particle ? 5.0f : 4.0f;
    }
}

static LaneSeverity severityFromScore(AutoLane lane, float score) {
    if (lane == AutoLane::None) return LaneSeverity::None;
    if (score >= 0.65f) return LaneSeverity::Purge;
    if (score >= 0.30f) return LaneSeverity::Elevated;
    if (score > 0.0f) return LaneSeverity::Low;
    return LaneSeverity::None;
}

static void buildReason(char* out,
                        size_t out_len,
                        AutoMode mode,
                        AutoLane dominant,
                        bool floor_applied,
                        bool faulted) {
    if (!out || out_len == 0) return;
    const char* mode_part = modeName(mode);
    const char* lane_part = laneName(dominant);
    if (faulted) {
        snprintf(out, out_len, "fault_%s", lane_part);
    } else if (floor_applied) {
        snprintf(out, out_len, "%s_%s_floor", mode_part, lane_part);
    } else {
        snprintf(out, out_len, "%s_%s", mode_part, lane_part);
    }
    out[out_len - 1] = '\0';
}

static float laneConfidence(float score, AutoTrustLevel trust) {
    float base = score;
    switch (trust) {
        case AutoTrustLevel::Valid: base += 0.20f; break;
        case AutoTrustLevel::Degraded: base += 0.05f; break;
        case AutoTrustLevel::Stale: base -= 0.30f; break;
        case AutoTrustLevel::Fault: base -= 0.50f; break;
    }
    return clamp01(base);
}

} // namespace

AutoEngineV3::AutoEngineV3() {
    reset();
}

void AutoEngineV3::reset() {
    memset(&_state, 0, sizeof(_state));
    _state.initialized = true;
    _state.mode = AutoMode::Idle;
    _state.dominant_lane = AutoLane::None;
    _state.voc_baseline = 20.0f;
    // NAN-seed the EWMAs so ewma() bootstraps from the first sample instead
    // of blending up from zero for the first minute after boot.
    _state.pm_fast = NAN;   _state.pm_slow = NAN;   _state.pm_prev_fast = NAN;
    _state.voc_fast = NAN;  _state.voc_slow = NAN;  _state.voc_prev_fast = NAN;
    _state.eco2_fast = NAN; _state.eco2_slow = NAN;
    _state.last_target_airflow_pct = 0.0f;
    _state.last_limited_airflow_pct = 0.0f;
    _state.last_quantized_step = 16;
}

AirflowTarget AutoEngineV3::quantizeTarget(float airflow_pct, uint8_t min_speed, uint8_t max_speed) {
    AirflowTarget out{};
    if (airflow_pct < 0.0f) airflow_pct = 0.0f;
    if (airflow_pct > 100.0f) airflow_pct = 100.0f;
    out.target_airflow_pct = airflow_pct;
    out.quantized_step = fanSpeedFromPercent((uint8_t)lroundf(airflow_pct), min_speed, max_speed);
    return out;
}

AutoEngineV3::Result AutoEngineV3::computeTarget(const AutoCycleInputs& inputs) {
    Result out{};

    const SensorSnapshot& s = inputs.sensor;
    SensorTrust trust{};
    trust.pm = trustFromAge(s.pm_age_ms, PM_VALID_MS, PM_DEGRADED_MS, PM_FAULT_MS);
    trust.gas = trustFromAge(s.gas_age_ms, GAS_VALID_MS, GAS_DEGRADED_MS, GAS_FAULT_MS,
                             s.sgp30_in_warmup);
    trust.eco2 = trust.gas;
    trust.temp_rh = trustFromAge(s.humidity_age_ms, RH_VALID_MS, RH_DEGRADED_MS, RH_FAULT_MS,
                                 !s.humidity_comp_ok);
    // Payload validity is independent for each pollutant, even when two
    // measurements share a sensor timestamp and warmup interval.
    if (!isfinite(s.pm25) || s.pm25 < 0.0f) trust.pm = AutoTrustLevel::Fault;
    if (!isfinite(s.voc_ppb) || s.voc_ppb < 0.0f) trust.gas = AutoTrustLevel::Fault;
    if (!isfinite(s.eco2_ppm) || s.eco2_ppm < 0.0f) trust.eco2 = AutoTrustLevel::Fault;
    if (!isfinite(s.temp_c) || !isfinite(s.rh_pct) || s.rh_pct < 0.0f || s.rh_pct > 100.0f) {
        trust.temp_rh = AutoTrustLevel::Fault;
    }

    if (!_state.initialized) reset();

    const bool pm_authority = trust.pm != AutoTrustLevel::Stale && trust.pm != AutoTrustLevel::Fault;
    const bool gas_authority = trust.gas != AutoTrustLevel::Stale && trust.gas != AutoTrustLevel::Fault;
    const bool eco2_usable = trust.eco2 == AutoTrustLevel::Valid || trust.eco2 == AutoTrustLevel::Degraded;
    const bool vent_authority = eco2_usable && trust.temp_rh != AutoTrustLevel::Fault;

    const bool new_pm = pm_authority && s.pm_sample_generation != 0 &&
        s.pm_sample_generation != _state.last_pm_generation;
    const bool new_gas = (gas_authority || eco2_usable) && s.gas_sample_generation != 0 &&
        s.gas_sample_generation != _state.last_gas_generation;
    if (new_pm) {
        uint32_t elapsed = _state.last_pm_generation == 0 ? 1000u
            : (uint32_t)(inputs.now_ms - _state.last_pm_sample_ms);
        if (elapsed > PM_FAULT_MS) elapsed = 1000u;
        if (elapsed == 0) elapsed = 1u;
        _state.pm_prev_fast = _state.pm_fast;
        _state.pm_fast = ewma(_state.pm_fast, s.pm25, elapsedAlpha(0.45f, elapsed));
        _state.pm_slow = ewma(_state.pm_slow, s.pm25, elapsedAlpha(0.05f, elapsed));
        const float rise = isfinite(_state.pm_prev_fast)
            ? fmaxf(0.0f, _state.pm_fast - _state.pm_prev_fast) : 0.0f;
        _state.pm_rise_score = fminf(rise * 1000.0f / elapsed * PM_RISE_GAIN, PM_RISE_CAP);
        _state.last_pm_sample_ms = inputs.now_ms;
        _state.last_pm_generation = s.pm_sample_generation;
    }
    if (new_gas) {
        uint32_t elapsed = _state.last_gas_generation == 0 ? 1000u
            : (uint32_t)(inputs.now_ms - _state.last_gas_sample_ms);
        if (elapsed > GAS_FAULT_MS) elapsed = 1000u;
        if (elapsed == 0) elapsed = 1u;
        _state.voc_prev_fast = _state.voc_fast;
        if (gas_authority) {
            _state.voc_fast = ewma(_state.voc_fast, s.voc_ppb, elapsedAlpha(0.35f, elapsed));
            _state.voc_slow = ewma(_state.voc_slow, s.voc_ppb, elapsedAlpha(0.02f, elapsed));
            const float rise = isfinite(_state.voc_prev_fast)
                ? fmaxf(0.0f, _state.voc_fast - _state.voc_prev_fast) : 0.0f;
            _state.voc_rise_score = fminf(rise * 1000.0f / elapsed * VOC_RISE_GAIN, VOC_RISE_CAP);
        }
        if (eco2_usable) {
            _state.eco2_fast = ewma(_state.eco2_fast, s.eco2_ppm, elapsedAlpha(0.20f, elapsed));
            _state.eco2_slow = ewma(_state.eco2_slow, s.eco2_ppm, elapsedAlpha(0.03f, elapsed));
        }
        _state.last_gas_sample_ms = inputs.now_ms;
        _state.last_gas_generation = s.gas_sample_generation;
    }

    // NAN-safe views: a sensor never seen contributes zero score, not the top anchor.
    const float pm_fast_v = isfinite(_state.pm_fast) ? _state.pm_fast : 0.0f;
    const float voc_fast_v = isfinite(_state.voc_fast) ? _state.voc_fast : 0.0f;
    const float eco2_fast_v = isfinite(_state.eco2_fast) ? _state.eco2_fast : 0.0f;

    const float pm_level = interpolateAnchors(clamp01(pm_fast_v / 125.0f) * 125.0f,
                                              PM_ANCHOR_X, PM_ANCHOR_Y,
                                              sizeof(PM_ANCHOR_X) / sizeof(PM_ANCHOR_X[0]));
    const float pm_score = clamp01(pm_level + _state.pm_rise_score);

    // Room clean-air floor: prefer the live adaptive baseline from the slow
    // estimator lane; fall back to the reset default when it is absent.
    const float voc_baseline = (isfinite(s.voc_baseline_ppb) && s.voc_baseline_ppb > 0.0f)
        ? s.voc_baseline_ppb
        : fmaxf(0.0f, _state.voc_baseline);
    const float voc_excess = fmaxf(0.0f, voc_fast_v - voc_baseline - 80.0f);
    const float gas_level = interpolateAnchors(voc_excess, GAS_ANCHOR_X, GAS_ANCHOR_Y,
                                               sizeof(GAS_ANCHOR_X) / sizeof(GAS_ANCHOR_X[0]));
    const float gas_score = clamp01(gas_level + _state.voc_rise_score);

    if (vent_authority && eco2_fast_v >= 1000.0f) {
        if (_state.vent_high_since_ms == 0) {
            _state.vent_high_since_ms = inputs.now_ms;
        }
    } else {
        _state.vent_high_since_ms = 0;
    }
    const float vent_level = interpolateAnchors(eco2_fast_v, VENT_ANCHOR_X, VENT_ANCHOR_Y,
                                                sizeof(VENT_ANCHOR_X) / sizeof(VENT_ANCHOR_X[0]));
    const bool vent_persistent = _state.vent_high_since_ms != 0 &&
        (uint32_t)(inputs.now_ms - _state.vent_high_since_ms) >= VENT_PERSIST_MS;
    const float vent_score = vent_persistent ? vent_level : fminf(vent_level, VENT_ACTIVE_THRESHOLD);

    auto fillLane = [](LaneState* lane,
                       AutoLane lane_id,
                       AutoTrustLevel trust_level,
                       float score,
                       bool authority) {
        if (!lane) return;
        lane->lane = lane_id;
        lane->trust = trust_level;
        lane->score = clamp01(score);
        lane->severity = severityFromScore(lane_id, lane->score);
        lane->confidence = laneConfidence(lane->score, trust_level);
        lane->authority_enabled = authority;
        lane->reason_bits = 0;
        if (trust_level == AutoTrustLevel::Degraded) lane->reason_bits |= 1u << 0;
        if (trust_level == AutoTrustLevel::Stale) lane->reason_bits |= 1u << 1;
        if (trust_level == AutoTrustLevel::Fault) lane->reason_bits |= 1u << 2;
        if (!authority) lane->reason_bits |= 1u << 3;
    };

    fillLane(&out.pm_lane, AutoLane::Particle, trust.pm, pm_score, pm_authority);
    fillLane(&out.gas_lane, AutoLane::Gas, trust.gas, gas_score, gas_authority);
    const AutoTrustLevel vent_trust = (uint8_t)trust.eco2 > (uint8_t)trust.temp_rh
        ? trust.eco2 : trust.temp_rh;
    fillLane(&out.vent_lane, AutoLane::Ventilation, vent_trust, vent_score, vent_authority);
    out.trust = trust;

    struct Candidate {
        AutoLane lane;
        float score;
        bool authority;
    };
    Candidate top{AutoLane::None, 0.0f, false};
    const LaneState* lanes[] = {&out.pm_lane, &out.gas_lane, &out.vent_lane};
    for (const LaneState* lane : lanes) {
        if (!lane->authority_enabled) continue;
        if (lane->score > top.score) {
            top.lane = lane->lane;
            top.score = lane->score;
            top.authority = true;
        }
    }

    const LaneState* current_lane_state = nullptr;
    switch (_state.dominant_lane) {
        case AutoLane::Particle: current_lane_state = &out.pm_lane; break;
        case AutoLane::Gas: current_lane_state = &out.gas_lane; break;
        case AutoLane::Ventilation: current_lane_state = &out.vent_lane; break;
        case AutoLane::None:
        default: break;
    }
    const float current_score = current_lane_state && current_lane_state->authority_enabled
        ? current_lane_state->score : -1.0f;

    AutoLane dominant = AutoLane::None;
    AutoLane secondary = AutoLane::None;
    const auto clearDominantCandidate = [this]() {
        _state.dominant_candidate = AutoLane::None;
        _state.dominant_candidate_since_ms = 0;
    };

    if (top.authority && top.score >= LANE_IDLE_THRESHOLD) {
        if (_state.dominant_lane == AutoLane::None || current_score < 0.0f) {
            dominant = top.lane;
            clearDominantCandidate();
        } else if (top.lane == _state.dominant_lane) {
            dominant = _state.dominant_lane;
            clearDominantCandidate();
        } else {
            const float gap = top.score - current_score;
            if (gap >= 0.10f) {
                dominant = top.lane;
                clearDominantCandidate();
            } else {
                if (_state.dominant_candidate != top.lane) {
                    _state.dominant_candidate = top.lane;
                    _state.dominant_candidate_since_ms = inputs.now_ms;
                }
                if ((uint32_t)(inputs.now_ms - _state.dominant_candidate_since_ms) >= DOMINANCE_PERSIST_MS) {
                    dominant = top.lane;
                    clearDominantCandidate();
                } else {
                    dominant = _state.dominant_lane;
                }
            }
        }
    } else {
        clearDominantCandidate();
    }

    const LaneState* dominant_state = nullptr;
    switch (dominant) {
        case AutoLane::Particle: dominant_state = &out.pm_lane; break;
        case AutoLane::Gas: dominant_state = &out.gas_lane; break;
        case AutoLane::Ventilation: dominant_state = &out.vent_lane; break;
        case AutoLane::None:
        default: break;
    }

    if (dominant == AutoLane::None) {
        secondary = top.authority ? top.lane : AutoLane::None;
    } else {
        float best_other = -1.0f;
        for (const LaneState* lane : lanes) {
            if (lane->lane == dominant || !lane->authority_enabled) continue;
            if (lane->score > best_other) {
                best_other = lane->score;
                secondary = lane->lane;
            }
        }
    }
    out.dominant_lane = dominant;
    out.secondary_lane = secondary;

    AutoMode proposed_mode = AutoMode::Idle;
    float dominant_score = dominant_state ? dominant_state->score : 0.0f;
    const float gas_score_visible = out.gas_lane.authority_enabled ? out.gas_lane.score : 0.0f;
    const float pm_score_visible = out.pm_lane.authority_enabled ? out.pm_lane.score : 0.0f;
    const float vent_score_visible = out.vent_lane.authority_enabled ? out.vent_lane.score : 0.0f;

    if (!top.authority && !pm_authority && !gas_authority && !vent_authority) {
        proposed_mode = AutoMode::FaultSafe;
    } else if (dominant == AutoLane::Particle) {
        if (dominant_score >= PM_PURGE_THRESHOLD) proposed_mode = AutoMode::PurgePM;
        else if (dominant_score >= LANE_ELEVATED_THRESHOLD) proposed_mode = AutoMode::ElevatedPM;
        else if (dominant_score < LANE_IDLE_THRESHOLD && _state.mode != AutoMode::Recover) proposed_mode = AutoMode::Idle;
        else proposed_mode = AutoMode::Recover;
    } else if (dominant == AutoLane::Gas) {
        if (dominant_score >= GAS_PURGE_THRESHOLD) proposed_mode = AutoMode::PurgeGas;
        else if (dominant_score >= LANE_ELEVATED_THRESHOLD) proposed_mode = AutoMode::ElevatedGas;
        else if (dominant_score < LANE_IDLE_THRESHOLD && _state.mode != AutoMode::Recover) proposed_mode = AutoMode::Idle;
        else proposed_mode = AutoMode::Recover;
    } else if (dominant == AutoLane::Ventilation) {
        proposed_mode = (dominant_score >= VENT_ACTIVE_THRESHOLD) ? AutoMode::Maintain : AutoMode::Idle;
    } else {
        const float max_lane = fmaxf(pm_score_visible, fmaxf(gas_score_visible, vent_score_visible));
        if (max_lane < LANE_IDLE_THRESHOLD) {
            proposed_mode = (_state.mode == AutoMode::PurgePM || _state.mode == AutoMode::PurgeGas ||
                             _state.mode == AutoMode::ElevatedPM || _state.mode == AutoMode::ElevatedGas)
                                ? AutoMode::Recover
                                : AutoMode::Idle;
        } else {
            proposed_mode = AutoMode::Maintain;
        }
    }

    if (_state.mode == AutoMode::PurgePM && dominant == AutoLane::Particle &&
        dominant_score >= PM_PURGE_THRESHOLD) {
        proposed_mode = AutoMode::PurgePM;
    } else if (_state.mode == AutoMode::PurgeGas && dominant == AutoLane::Gas &&
               dominant_score >= GAS_PURGE_THRESHOLD) {
        proposed_mode = AutoMode::PurgeGas;
    } else if (_state.mode == AutoMode::Recover) {
        const float max_lane = fmaxf(pm_score_visible, fmaxf(gas_score_visible, vent_score_visible));
        if (max_lane < LANE_IDLE_THRESHOLD) {
            if (_state.last_mode_change_ms != 0 &&
                (uint32_t)(inputs.now_ms - _state.last_mode_change_ms) >= MODE_RECOVER_EXIT_MS) {
                proposed_mode = AutoMode::Idle;
            } else {
                proposed_mode = AutoMode::Recover;
            }
        }
    }

    const bool severe_entry = proposed_mode == AutoMode::PurgePM || proposed_mode == AutoMode::PurgeGas;
    if (proposed_mode != _state.mode) {
        if (severe_entry ||
            _state.last_mode_change_ms == 0 ||
            (uint32_t)(inputs.now_ms - _state.last_mode_change_ms) >= DOMINANCE_PERSIST_MS) {
            _state.mode = proposed_mode;
            _state.last_mode_change_ms = inputs.now_ms;
            _state.mode_candidate = AutoMode::Idle;
            _state.mode_candidate_since_ms = 0;
        } else {
            if (_state.mode_candidate != proposed_mode) {
                _state.mode_candidate = proposed_mode;
                _state.mode_candidate_since_ms = inputs.now_ms;
            } else if ((uint32_t)(inputs.now_ms - _state.mode_candidate_since_ms) >= DOMINANCE_PERSIST_MS) {
                _state.mode = proposed_mode;
                _state.last_mode_change_ms = inputs.now_ms;
                _state.mode_candidate = AutoMode::Idle;
                _state.mode_candidate_since_ms = 0;
            }
        }
    } else {
        _state.mode_candidate = AutoMode::Idle;
        _state.mode_candidate_since_ms = 0;
    }

    if (_state.mode == AutoMode::FaultSafe && top.authority) {
        if (top.lane == AutoLane::Particle && top.score >= LANE_ELEVATED_THRESHOLD) {
            _state.mode = top.score >= PM_PURGE_THRESHOLD ? AutoMode::PurgePM : AutoMode::ElevatedPM;
            _state.last_mode_change_ms = inputs.now_ms;
        } else if (top.lane == AutoLane::Gas && top.score >= LANE_ELEVATED_THRESHOLD) {
            _state.mode = top.score >= GAS_PURGE_THRESHOLD ? AutoMode::PurgeGas : AutoMode::ElevatedGas;
            _state.last_mode_change_ms = inputs.now_ms;
        } else if (top.lane == AutoLane::Ventilation && top.score >= VENT_ACTIVE_THRESHOLD) {
            _state.mode = AutoMode::Maintain;
            _state.last_mode_change_ms = inputs.now_ms;
        }
    }

    if (_state.mode == AutoMode::PurgePM || _state.mode == AutoMode::PurgeGas) {
        _state.last_purge_ms = inputs.now_ms;
    }

    const float room_m3 = (isfinite(inputs.learned_room_m3) && inputs.learned_room_m3 > 0.0f)
        ? inputs.learned_room_m3
        : inputs.room_m3;
    const float cadr_m3h = inputs.effective_cadr_m3h;

    const bool plant_valid = isfinite(room_m3) && room_m3 > 0.0f && isfinite(cadr_m3h) && cadr_m3h > 0.0f;
    if (!plant_valid) {
        _state.mode = AutoMode::FaultSafe;
        _state.last_mode_change_ms = inputs.now_ms;
    }
    float requested_ach = modeTargetAch(_state.mode, dominant);
    if (_state.mode == AutoMode::FaultSafe) {
        requested_ach = 0.0f;
    }
    float target_pct = 0.0f;
    bool faulted = false;
    if (plant_valid && requested_ach > 0.0f) {
        const float required_cadr = room_m3 * requested_ach;
        target_pct = 100.0f * required_cadr / cadr_m3h;
    } else if (_state.mode == AutoMode::FaultSafe) {
        target_pct = FAULTSAFE_FALLBACK_TARGET_PCT;
        faulted = true;
    } else if (_state.mode == AutoMode::Idle) {
        // Idle demands no airflow; the user min-fan floor still applies below.
        target_pct = 0.0f;
    } else {
        target_pct = 45.0f;
        faulted = true;
    }

    target_pct = clampPct(target_pct);
    out.target_airflow_pct = target_pct;

    float limited_pct = target_pct;
    const bool user_floor_applied = inputs.min_fan_percent > 0 && limited_pct < inputs.min_fan_percent;
    const bool user_cap_applied = inputs.max_fan_percent > 0 && limited_pct > inputs.max_fan_percent;
    if (user_floor_applied) limited_pct = inputs.min_fan_percent;
    if (user_cap_applied) limited_pct = inputs.max_fan_percent;

    const float prev_pct = isfinite(inputs.current_airflow_pct)
        ? clampPct(inputs.current_airflow_pct)
        : _state.last_limited_airflow_pct;
    const float rate_prev_pct = _state.rate_limit_initialized
        ? clampPct(_state.last_limited_airflow_pct)
        : prev_pct;
    const uint32_t call_elapsed_ms = _state.rate_limit_initialized
        ? (uint32_t)(inputs.now_ms - _state.last_compute_ms) : 1000u;
    const float slew_seconds = (float)(call_elapsed_ms > 1000u ? 1000u : call_elapsed_ms) / 1000.0f;
    const bool rising = limited_pct > rate_prev_pct;
    const bool falling = limited_pct < rate_prev_pct;
    if (rising) {
        const float slew = (_state.mode == AutoMode::PurgePM || _state.mode == AutoMode::PurgeGas) ? 20.0f : 10.0f;
        limited_pct = fminf(limited_pct, rate_prev_pct + slew * slew_seconds);
        _state.last_rate_limit_ms = inputs.now_ms;
        _state.last_downshift_ms = 0;
        _state.last_downshift_valid = false;
    } else if (falling) {
        const uint32_t since_rate = _state.last_rate_limit_ms == 0 ? UINT32_MAX
                                                                  : (uint32_t)(inputs.now_ms - _state.last_rate_limit_ms);
        const float slew = (_state.mode == AutoMode::Recover) ? 3.0f : 5.0f;
        if (since_rate < 3000u) {
            limited_pct = rate_prev_pct;
        } else {
            const float candidate_pct = fmaxf(limited_pct, rate_prev_pct - slew * slew_seconds);
            const uint8_t prev_step = _state.rate_limit_initialized && _state.last_quantized_step >= 1 && _state.last_quantized_step <= 16
                ? _state.last_quantized_step
                : AutoEngineV3::quantizeTarget(rate_prev_pct, 1, 16).quantized_step;
            const uint8_t candidate_step = fanStepWithinPercentBounds(candidate_pct, inputs.min_fan_percent, inputs.max_fan_percent > 0 ? inputs.max_fan_percent : 100);
            if (candidate_step > prev_step) {
                const uint32_t since_downshift = !_state.last_downshift_valid ? UINT32_MAX
                                                                               : (uint32_t)(inputs.now_ms - _state.last_downshift_ms);
                if (since_downshift >= DOWNSHIFT_DWELL_MS) {
                    limited_pct = candidate_pct;
                    _state.last_downshift_ms = inputs.now_ms;
                    _state.last_downshift_valid = true;
                } else {
                    limited_pct = rate_prev_pct;
                }
            } else {
                limited_pct = candidate_pct;
            }
        }
    } else {
        _state.last_downshift_ms = 0;
        _state.last_downshift_valid = false;
    }

    const bool floor_applied = user_floor_applied;
    out.limited_airflow_pct = clampPct(limited_pct);
    if (_state.mode == AutoMode::FaultSafe) {
        // FaultSafe bypasses slew, but still honors valid user bounds.
        float fallback_pct = clampPct(out.target_airflow_pct);
        if (inputs.min_fan_percent > 0 && fallback_pct < inputs.min_fan_percent) {
            fallback_pct = (float)inputs.min_fan_percent;
        }
        if (inputs.max_fan_percent > 0 && fallback_pct > inputs.max_fan_percent) {
            fallback_pct = (float)inputs.max_fan_percent;
        }
        out.limited_airflow_pct = clampPct(fallback_pct);
        _state.last_rate_limit_ms = inputs.now_ms;
    }
    // A newly changed bound takes effect before the next slew allowance.
    if (inputs.min_fan_percent > 0 && out.limited_airflow_pct < inputs.min_fan_percent) {
        out.limited_airflow_pct = inputs.min_fan_percent;
    }
    if (inputs.max_fan_percent > 0 && out.limited_airflow_pct > inputs.max_fan_percent) {
        out.limited_airflow_pct = inputs.max_fan_percent;
    }
    const uint8_t floor_pct = inputs.min_fan_percent;
    const uint8_t cap_pct = inputs.max_fan_percent > 0 ? inputs.max_fan_percent : 100;
    out.quantized_step = fanStepWithinPercentBounds(out.limited_airflow_pct, floor_pct, cap_pct);
    if (out.quantized_step == 0) {
        // ConfigStore and /api/config prevent this; preserve a valid fallback.
        out.quantized_step = fanStepWithinPercentBounds(40.0f, 40, 100);
    }
    out.applied_airflow_pct = fanPercentFromSpeed(out.quantized_step, 1, 16);
    out.user_cap_applied = user_cap_applied;
    out.user_floor_applied = floor_applied;

    buildReason(out.reason, sizeof(out.reason), _state.mode, dominant,
                out.user_floor_applied, faulted);

    _state.dominant_lane = dominant;
    _state.last_target_airflow_pct = out.target_airflow_pct;
    _state.last_limited_airflow_pct = out.limited_airflow_pct;
    _state.last_quantized_step = out.quantized_step;
    _state.last_compute_ms = inputs.now_ms;
    _state.rate_limit_initialized = true;
    out.mode = _state.mode;
    return out;
}
