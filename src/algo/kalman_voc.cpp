#include "kalman_voc.h"
#include <string.h>

KalmanVOC::KalmanVOC(float init_voc) {
    _x[0] = init_voc; _x[1] = 0.0f;
    _cov[0] = 500.0f; _cov[1] = 0.0f;
    _cov[2] = 0.0f;   _cov[3] = 100.0f;
}

void KalmanVOC::update(float z, float* level_out, float* vel_out) {
    // ── Predict ──────────────────────────────────────────────────────────────
    float x0 = _x[0] + _x[1];
    float x1 = _x[1];

    float p00 = _cov[0] + _cov[1] + _cov[2] + _cov[3] + Q_LEVEL;
    float p01 = _cov[1] + _cov[3];
    float p10 = _cov[2] + _cov[3];
    float p11 = _cov[3] + Q_VEL;

    _x[0] = x0; _x[1] = x1;
    _cov[0] = p00; _cov[1] = p01; _cov[2] = p10; _cov[3] = p11;

    // ── Update ───────────────────────────────────────────────────────────────
    float y  = z - _x[0];           // innovation
    float S  = _cov[0] + R_MEAS;      // innovation covariance
    float K0 = _cov[0] / S;           // Kalman gain - level
    float K1 = _cov[2] / S;           // Kalman gain - velocity
    float p00_old = _cov[0];
    float p01_old = _cov[1];
    float p10_old = _cov[2];
    float p11_old = _cov[3];

    _x[0] += K0 * y;
    _x[1] += K1 * y;

    _cov[0] = (1.0f - K0) * p00_old;
    _cov[1] = (1.0f - K0) * p01_old;
    _cov[2] = p10_old - K1 * p00_old;
    _cov[3] = p11_old - K1 * p01_old;

    // SOTA Safety: Prevent numerical covariance collapse under extreme signal drift
    if (_cov[0] < 1.0f) _cov[0] = 1.0f;
    if (_cov[3] < 1.0f) _cov[3] = 1.0f;

    if (_x[0] < 0.0f) _x[0] = 0.0f; // VOC can't be negative

    if (level_out) *level_out = _x[0];
    if (vel_out)   *vel_out   = _x[1];
}

void KalmanVOC::serialize(uint8_t* buf) const {
    memcpy(buf,     &_x[0], 4);
    memcpy(buf + 4, &_x[1], 4);
    memcpy(buf + 8, &_cov[0], 4);  // P00 - enough to hot-start
}

void KalmanVOC::deserialize(const uint8_t* buf) {
    memcpy(&_x[0], buf,     4);
    memcpy(&_x[1], buf + 4, 4);
    memcpy(&_cov[0], buf + 8, 4);
    // Restore reasonable off-diagonal/velocity covariance on warm start
    _cov[1] = 0.0f; _cov[2] = 0.0f; _cov[3] = 50.0f;
}
