#pragma once
#include "rpm_sample.h"
#include "application.h"
#include <stdint.h>

#ifndef AURA_TACH_PIN
#define AURA_TACH_PIN P1S3
#endif
#ifndef AURA_TACH_PULSES_PER_REV
#define AURA_TACH_PULSES_PER_REV 2U
#endif
#ifndef AURA_TACH_INTERRUPT_MODE
#define AURA_TACH_INTERRUPT_MODE FALLING
#endif
#ifndef AURA_DIRECT_FAN_PWM
#define AURA_DIRECT_FAN_PWM 1
#endif
#ifndef AURA_FAN_PWM_PIN
#define AURA_FAN_PWM_PIN P1S0
#endif

#if defined(AURA_DIRECT_FAN_PWM) && AURA_DIRECT_FAN_PWM
    static constexpr const char* BACKEND_NAME = "fan_pwm";
#else
    static constexpr const char* BACKEND_NAME = "fan_disabled";
#endif

// HAL for fan actuation via PWM on P1S0 with Serial1 telemetry/status from the
// secondary PM2.5 board (WS180313138) and other ribbon-connected sources.

class DaughterBoard {
public:
    // Logical step range. Step 1 = loudest (2478 RPM), step 16 = quietest (693 RPM).
    // Each step maps to a calibrated PWM register via STEP_TO_REG[].
    static constexpr uint8_t MIN_SPEED = 1;
    static constexpr uint8_t MAX_SPEED = 16;
    // Idle: step 10 (~40% speed, 1126 RPM) — enough airflow for sensor reads.
    static constexpr uint8_t IDLE_SPEED = 10;
    static constexpr uint32_t BOOT_SETTLE_MS = 15000;

    // PWM duty range (OEM analogWrite 4-38 for registers 1-24).
    static constexpr uint8_t  DUTY_MIN          = 4;
    static constexpr uint8_t  DUTY_MAX          = 38;
    // I-controller trim range: ±8 duty counts covers observed ±29% RPM error.
    static constexpr int8_t   TRIM_MAX          = 8;
    static constexpr int8_t   TRIM_MIN          = -8;
    static constexpr uint16_t TRIM_DEADBAND_RPM = 60;    // no integration below this error
    static constexpr uint32_t TRIM_HOLDOFF_MS   = 4000;  // freeze after speed change
    static constexpr float    TRIM_K_I          = 1.00f; // duty-counts/s per normalized RPM error

    // Step i (0-indexed) → PWM register. Equal-CADR spacing, all steps above JND.
    // Derived from live tach sweep 2026-04-09; registers 25-31 are sub-JND, 32 is
    // non-monotonic — both excluded.
    static constexpr uint8_t STEP_TO_REG[16] = {
        1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 14, 16, 18, 21, 24
    };

    enum ReadinessState : uint8_t {
        READINESS_UNKNOWN = 0,
        READINESS_BOOT_MISSING = 1,
        READINESS_BOOT_SETTLING = 2,
        READINESS_BOOT_READY = 3,
        READINESS_RUNTIME_LOST = 4,
    };

    DaughterBoard();

    void begin();
    void service();

    // Set fan speed (1=loudest/fastest, 16=quietest/slowest).
    // Returns true only once the active backend is ready.
    bool setFanSpeed(uint8_t speed);

    // Poll RPM. Returns 0 if unavailable.
    // Mutates internal sample window — owned by the RPM monitor thread.
    uint16_t getRpm();
    RpmSample readRpmSample();
    // Non-mutating RPM read using a trim-local sample window on the main thread.
    uint16_t getRpmInstant() const;
    uint32_t tachPulseCount() const;

    int8_t dutyTrim() const { return _duty_trim; }

    void        setActuationReady(bool ready) { _actuation_ready = ready; }
    bool        isActuationReady() const { return _actuation_ready; }
    void        setActuationProven(bool proven) { _actuation_proven = proven; }
    bool        isActuationProven() const { return _actuation_proven; }
    void        setModulePresentAssumed(bool present);
    bool        modulePresentAssumed() const { return _module_present_assumed; }
    ReadinessState readinessState() const { return _readiness_state; }
    const char* readinessStateName() const;
    bool        isModuleReady() const { return _readiness_state == READINESS_BOOT_READY; }
    uint32_t    readinessStateSinceMs() const { return _readiness_state_since_ms; }
    bool        tachReaderReady() const { return _tach_reader_ready; }
    const char* backendName() const { return BACKEND_NAME; }
    uint8_t     lastTxLen() const { return _last_tx_len; }
    uint8_t     lastRxLen() const { return _last_rx_len; }
    uint32_t    lastTxMs() const { return _last_tx_ms; }
    bool        lastSendSawReply() const { return _last_rx_len > 0; }

    uint8_t  lastCommandedSpeed() const { return _commanded_speed; }
    uint32_t lastCommandMs()      const { return _last_cmd_ms; }

    void pollPassiveUart();
    void clearUartLog();

    // Fan command sender. Public builds use the verified direct PWM backend.
    bool sendFanSpeedHypothesis(uint8_t speed, const char* note = nullptr);

private:
    static void _tachPulseISR();
    void _applyPwmDuty(uint8_t speed);
    bool _sendFrame(const uint8_t* bytes, uint8_t len, const char* note);
    void _logFrame(const uint8_t* tx, uint8_t tx_len,
                   const uint8_t* rx, uint8_t rx_len,
                   const char* note);
    void _initTachReader();
    void _setReadinessState(ReadinessState state, uint32_t now_ms);

    uint8_t  _commanded_speed;
    uint32_t _last_cmd_ms;
    uint32_t _readiness_state_since_ms;
    uint32_t _tach_last_sample_ms;
    uint32_t _tach_last_sample_count;
    mutable uint32_t _trim_sample_ms;
    mutable uint32_t _trim_sample_count;
    float    _trim_accumulator;
    int8_t   _duty_trim;        // I-controller: signed duty offset applied on every PWM write
    uint32_t _trim_holdoff_ms;  // freeze I-controller integration until this timestamp
    bool _trim_holdoff_active = false;
    uint32_t _last_trim_adjust_ms;
    uint32_t _last_trim_attempt_ms;
    bool     _serial_ready;
    bool     _actuation_ready;
    bool     _actuation_proven;
    bool     _tach_reader_ready;
    bool     _module_present_assumed;
    ReadinessState _readiness_state;
    volatile bool _uart_capture_busy;
    uint8_t  _last_tx_len;
    uint8_t  _last_rx_len;
    uint32_t _last_tx_ms;

};
