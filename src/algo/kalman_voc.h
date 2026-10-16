#pragma once
#include <stdint.h>

// 2-state Kalman filter: tracks [VOC level, VOC velocity].
// Constant-velocity model: level[k] = level[k-1] + vel[k-1]
// Two-state VOC estimator tuned for LocalAir runtime cadence.
// Tuned on live SGP30 data during bench validation.
class KalmanVOC {
public:
    static constexpr float Q_LEVEL = 225.0f; // process noise — level (15 ppb σ)
    static constexpr float Q_VEL   = 100.0f; // process noise — velocity (10 ppb/sample σ)
    static constexpr float R_MEAS  = 225.0f; // measurement noise — SGP30 ±15 ppb → 15²

    explicit KalmanVOC(float init_voc = 20.0f);

    // Feed one measurement, returns filtered level and velocity.
    void update(float z, float* level_out, float* vel_out);

    float level()    const { return _x[0]; }
    float velocity() const { return _x[1]; }

    // Project forward n_intervals (predict where VOC will be).
    float predict(float n_intervals) const {
        return _x[0] + _x[1] * n_intervals;
    }

    // Persist/restore across power cycles (12 bytes).
    void   serialize(uint8_t* buf8) const;
    void deserialize(const uint8_t* buf8);

private:
    float _x[2]; // state [level, velocity]
    float _cov[4]; // covariance (row-major 2x2)
};
