#include "ach_model.h"
#include "../hal/daughter_board.h"
#include <math.h>
#include <string.h>

uint16_t AchModel::expectedRpm(uint8_t step) {
    // Translate logical step (1-16) → calibrated PWM register, then look up RPM.
    if (step < MIN_SPEED) step = MIN_SPEED;
    if (step > MAX_SPEED) step = MAX_SPEED;
    const uint8_t reg = DaughterBoard::STEP_TO_REG[step - 1];
    // RPM_TABLE is indexed by register — exact match always present for regs 1-24.
    for (uint8_t i = 0; i < RPM_TABLE_LEN; i++)
        if (RPM_TABLE[i].speed == reg) return RPM_TABLE[i].rpm;
    // Fallback interpolation (should not be reached with the full 1-24 table).
    uint8_t  lo_s = 1,  hi_s = 24;
    uint16_t lo_r = RPM_TABLE[0].rpm, hi_r = RPM_TABLE[RPM_TABLE_LEN - 1].rpm;
    for (uint8_t i = 0; i < RPM_TABLE_LEN; i++) {
        if (RPM_TABLE[i].speed <= reg) { lo_s = RPM_TABLE[i].speed; lo_r = RPM_TABLE[i].rpm; }
        if (RPM_TABLE[i].speed >= reg && RPM_TABLE[i].speed < hi_s) {
            hi_s = RPM_TABLE[i].speed; hi_r = RPM_TABLE[i].rpm;
        }
    }
    if (lo_s == hi_s) return lo_r;
    return (uint16_t)(lo_r + (float)(reg - lo_s) / (hi_s - lo_s) * (float)(hi_r - lo_r));
}

AchModel::AchModel(float room_m3, float ach_target, float cadr_max, float rpm_factor)
    : _room_m3(room_m3), _ach_target(ach_target), _cadr_max(cadr_max),
      _rpm_factor(rpm_factor), _floor(SPEED_SENSOR_FLOOR)
{
    if (_rpm_factor < 0.5f) _rpm_factor = 0.5f;
    if (_rpm_factor > 1.2f) _rpm_factor = 1.2f;
    _recalcFloor();
}

float AchModel::cadrAt(uint8_t speed) const {
    float rpm_max = (float)expectedRpm(MIN_SPEED);
    float rpm     = (float)expectedRpm(speed);
    return effectiveCadrMax() * (rpm / rpm_max);
}

void AchModel::setRpmFactor(float factor) {
    if (factor < 0.5f) factor = 0.5f;
    if (factor > 1.2f) factor = 1.2f;
    _rpm_factor = factor;
}

void AchModel::_recalcFloor() {
    const float needed = requiredCadr();
    const float nominal_max = _cadr_max;
    if (needed <= 0.0f || nominal_max <= 0.0f) {
        _floor = MIN_SPEED;
        return;
    }
    if (nominal_max < needed) {
        _floor = MIN_SPEED;
        return;
    }

    // Walk from slowest to fastest, find highest speed# that meets needed CADR
    _floor = MIN_SPEED;
    for (int s = (int)MAX_SPEED; s >= (int)MIN_SPEED; s--) {
        float rpm_max = (float)expectedRpm(MIN_SPEED);
        float rpm     = (float)expectedRpm((uint8_t)s);
        float cadr    = nominal_max * (rpm / rpm_max);
        if (cadr >= needed) {
            _floor = (uint8_t)s;
            break;
        }
    }
    if (_floor > SPEED_SENSOR_FLOOR) _floor = SPEED_SENSOR_FLOOR; // never slower than sensor minimum
}
