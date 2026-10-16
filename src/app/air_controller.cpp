// AirController: slow-estimator infrastructure for LocalAir.
// computeTarget() and all v2 control math have been removed.
// Live responsibilities:
//   updateSlowEstimators() -- Kalman VOC and adaptive baseline.
//   baseline() / kalman() / ach() accessors -- EEPROM persistence and RPM factor.
#include "air_controller.h"
#include "../algo/fan_speed.h"
#include "../sys/config_store.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

AirController::AirController()
    : _kalman(20.0f), _ach(30.0f, 4.0f), _room_profile(0),
      _max_fan_percent(100), _min_fan_percent(40)
{
    _slow.voc_kalman   = 0.0f;
    _slow.voc_velocity = 0.0f;
    _slow.baseline     = 20.0f;
    _slow.valid        = false;
}

void AirController::configure(float room_m3, uint8_t room_profile,
                               uint8_t max_fan_percent,
                               uint8_t min_fan_percent)
{
    _max_fan_percent = max_fan_percent;
    _min_fan_percent = min_fan_percent;
    _ach.setRoom(room_m3);
    _room_profile = room_profile;
    if (_room_profile == 0) _room_profile = ROOM_PROFILE_EVERYDAY;
    const float profile_ach = (_room_profile == ROOM_PROFILE_SLEEP) ? 2.5f
                          : (_room_profile == ROOM_PROFILE_OPEN_PLAN) ? 5.0f
                          : 4.0f;
    _ach.setTargetAch(profile_ach);
}

void AirController::updateSlowEstimators(const SensorReading& s, uint32_t now_ms,
                                          uint8_t commanded_speed, bool gas_valid)
{
    (void)s.pm25;
    (void)now_ms;
    (void)commanded_speed;

    if (!gas_valid || !isfinite(s.voc_ppb) || !isfinite(s.eco2_ppm)) {
        return;
    }

    float voc_adj = s.voc_ppb;

    // Kalman update (slow -- 20s cadence keeps filter stable)
    float voc_k, vel_k;
    _kalman.update(voc_adj, &voc_k, &vel_k);
    if (voc_k < 0.0f) voc_k = 0.0f;

    // Adaptive baseline (slow)
    float base = _baseline.update(voc_adj);

    // Cache for any caller that still reads slow state
    _slow.voc_kalman   = voc_k;
    _slow.voc_velocity = vel_k;
    _slow.baseline     = base;
    _slow.valid        = true;
}
