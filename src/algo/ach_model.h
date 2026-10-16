#pragma once
#include <stdint.h>
#include <math.h>

// RPM calibration table — full sweep measured 2026-04-09 via live tach at
// each PWM register. speed 1 = loudest (2478 RPM), speed 24 = last
// perceptible step (693 RPM). Registers 25-31 produce imperceptible deltas
// (<JND). Register 32 is non-monotonic (motor instability at low PWM).
// Usable hardware range: registers 1-24.
struct SpeedRpm { uint8_t speed; uint16_t rpm; };
static constexpr SpeedRpm RPM_TABLE[] = {
    {1,2478},{2,2376},{3,2232},{4,2066},{5,1956},{6,1848},{7,1770},{8,1692},
    {9,1610},{10,1500},{11,1416},{12,1314},{13,1239},{14,1170},{15,1126},
    {16,1046},{17,1014},{18,942},{19,897},{20,838},{21,804},{22,756},
    {23,723},{24,693}
};
static constexpr uint8_t RPM_TABLE_LEN = 24;

// ACH-based fan speed floor.
// Fan law: airflow ∝ RPM  →  CADR(speed) = CADR_max × RPM(speed)/RPM_max
//
class AchModel {
public:
    static constexpr float  CADR_MAX_DEFAULT = 180.0f; // m³/h at speed 1, factory estimate
    static constexpr float  CADR_MAX_SANE_MIN = 60.0f;
    static constexpr float  CADR_MAX_SANE_MAX = 500.0f;
    // Logical step range — matches DaughterBoard::MIN_SPEED / MAX_SPEED.
    static constexpr uint8_t MIN_SPEED  = 1;
    static constexpr uint8_t MAX_SPEED  = 16;
    // Step 10 = PWM register 11 = ~1416 RPM — minimum for reliable PM sensor reads.
    static constexpr uint8_t SPEED_SENSOR_FLOOR = 10;

    explicit AchModel(float room_m3 = 30.0f, float ach_target = 4.0f,
                      float cadr_max = CADR_MAX_DEFAULT,
                      float rpm_factor = 1.0f);

    // Returns highest speed# (quietest) that still meets target ACH.
    // Remember: higher number = slower fan = less airflow.
    uint8_t  speedFloor() const { return _floor; }

    // Current CADR estimate (m³/h) at a given speed.
    float cadrAt(uint8_t speed) const;

    // Effective maximum CADR after live RPM correction.
    float effectiveCadrMax() const { return _cadr_max * _rpm_factor; }

    // CADR required to hold the configured ACH target.
    float requiredCadr() const { return (_room_m3 > 0.0f) ? _room_m3 * _ach_target : 0.0f; }

    // Positive means the target is reachable at the current factor.
    float cadrHeadroom() const { return effectiveCadrMax() - requiredCadr(); }

    bool targetAchReachable() const { return cadrHeadroom() >= 0.0f; }

    // Actual ACH at a given speed for this room.
    float achAt(uint8_t speed) const {
        return (_room_m3 > 0.0f) ? cadrAt(speed) / _room_m3 : 0.0f;
    }

    void setRoom(float m3)       { _room_m3 = m3; _recalcFloor(); }
    void setTargetAch(float ach) { _ach_target = ach; _recalcFloor(); }
    void setCadrMax(float c)     { _cadr_max = c;  _recalcFloor(); }
    // Live RPM correction: telemetry and diagnostics only.
    void  setRpmFactor(float factor);
    float rpmFactor() const { return _rpm_factor; }

    float roomM3()    const { return _room_m3; }
    float targetAch() const { return _ach_target; }
    float cadrMax()   const { return _cadr_max; }
    static uint16_t expectedRpm(uint8_t speed);

private:
    float   _room_m3;
    float   _ach_target;
    float   _cadr_max;
    float   _rpm_factor;  // actual/expected RPM ratio, clamped [0.5, 1.2]
    uint8_t _floor;

    void _recalcFloor();
};
