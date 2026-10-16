#include "daughter_board.h"
#include "../algo/ach_model.h"
#include "duty_trim_policy.h"
#include <string.h>

namespace {
volatile uint32_t g_tach_pulse_count = 0;
}

static uint8_t fanPwmDutyForSpeed(uint8_t step) {
    if (step < DaughterBoard::MIN_SPEED) step = DaughterBoard::MIN_SPEED;
    if (step > DaughterBoard::MAX_SPEED) step = DaughterBoard::MAX_SPEED;

    // Map logical step (1-16) → calibrated PWM register → OEM analogWrite value.
    // STEP_TO_REG[] is 0-indexed; step 1 → index 0.
    const uint8_t reg = DaughterBoard::STEP_TO_REG[step - 1];
    // OEM PWM range: analogWrite 4-50 for registers 1-32.
    const uint32_t oem_val = 4 + (uint32_t)(reg - 1) * 46 / 31;
    return (uint8_t)oem_val;
}

DaughterBoard::DaughterBoard()
    : _commanded_speed(DaughterBoard::IDLE_SPEED), _last_cmd_ms(0),
      _readiness_state_since_ms(0),
      _tach_last_sample_ms(0), _tach_last_sample_count(0),
      _trim_sample_ms(0), _trim_sample_count(0), _trim_accumulator(0.0f),
      _duty_trim(0), _trim_holdoff_ms(0), _last_trim_adjust_ms(0),
      _last_trim_attempt_ms(0),
      _serial_ready(false), _actuation_ready(false), _actuation_proven(false),
      _tach_reader_ready(false),
      _module_present_assumed(true), _readiness_state(READINESS_UNKNOWN),
      _uart_capture_busy(false), _last_tx_len(0), _last_rx_len(0), _last_tx_ms(0)
{}

void DaughterBoard::_tachPulseISR() {
    ++g_tach_pulse_count;
}

void DaughterBoard::_setReadinessState(ReadinessState state, uint32_t now_ms) {
    if (_readiness_state == state) return;
    _readiness_state = state;
    _readiness_state_since_ms = now_ms;
}

const char* DaughterBoard::readinessStateName() const {
    switch (_readiness_state) {
        case READINESS_BOOT_MISSING: return "boot_missing";
        case READINESS_BOOT_SETTLING: return "boot_settling";
        case READINESS_BOOT_READY: return "boot_ready";
        case READINESS_RUNTIME_LOST: return "runtime_lost";
        case READINESS_UNKNOWN:
        default:
            return "unknown";
    }
}

void DaughterBoard::_applyPwmDuty(uint8_t speed) {
    int16_t duty = (int16_t)fanPwmDutyForSpeed(speed) + _duty_trim;
    if (duty < DUTY_MIN) duty = DUTY_MIN;
    if (duty > DUTY_MAX) duty = DUTY_MAX;
    pinMode(AURA_FAN_PWM_PIN, OUTPUT);
    analogWrite(AURA_FAN_PWM_PIN, (uint8_t)duty);
}

void DaughterBoard::begin() {
    const uint32_t now_ms = millis();
    Serial1.begin(9600);
    _serial_ready = true;
    _actuation_ready = false;
    _actuation_proven = false;
    _tach_reader_ready = false;
    _uart_capture_busy = false;
    _tach_last_sample_ms = now_ms;
    _tach_last_sample_count = 0;
    _trim_sample_ms = now_ms;
    _trim_sample_count = 0;
    g_tach_pulse_count = 0;
    _last_tx_len = 0;
    _last_rx_len = 0;
    _last_tx_ms = 0;
    _trim_accumulator = 0.0f;
    _duty_trim = 0;
    _trim_holdoff_ms = 0;
    _trim_holdoff_active = false;
    _last_trim_adjust_ms = 0;
    _last_trim_attempt_ms = 0;
#if defined(AURA_DIRECT_FAN_PWM) && AURA_DIRECT_FAN_PWM
    // Drive the fan at full startup speed immediately so the room-sniff spin
    // runs during the 15s boot-settle window. _actuation_ready is not yet set
    // but the PWM hardware is live from this point forward.
    _commanded_speed = MIN_SPEED;
    _applyPwmDuty(_commanded_speed);
    _last_cmd_ms = now_ms;
#endif
    _initTachReader();
    clearUartLog();
    _setReadinessState(_module_present_assumed ? READINESS_BOOT_SETTLING : READINESS_BOOT_MISSING, now_ms);
}

void DaughterBoard::_initTachReader() {
#ifdef AURA_TACH_PIN
    pinMode(AURA_TACH_PIN, INPUT_PULLUP);
    _tach_reader_ready = attachInterrupt(AURA_TACH_PIN, DaughterBoard::_tachPulseISR, AURA_TACH_INTERRUPT_MODE);
#else
    _tach_reader_ready = false;
#endif
}

void DaughterBoard::service() {
    if (!_serial_ready) return;

    const uint32_t now_ms = millis();
    if (!_module_present_assumed) {
        if (_readiness_state == READINESS_BOOT_READY) {
            _setReadinessState(READINESS_RUNTIME_LOST, now_ms);
        } else if (_readiness_state != READINESS_BOOT_MISSING) {
            _setReadinessState(READINESS_BOOT_MISSING, now_ms);
        }
        return;
    }

    if (_readiness_state == READINESS_UNKNOWN ||
        _readiness_state == READINESS_BOOT_MISSING ||
        _readiness_state == READINESS_RUNTIME_LOST) {
        _setReadinessState(READINESS_BOOT_SETTLING, now_ms);
    }

    if (_readiness_state == READINESS_BOOT_SETTLING &&
        (now_ms - _readiness_state_since_ms) >= BOOT_SETTLE_MS) {
        _setReadinessState(READINESS_BOOT_READY, now_ms);
    }

    if (_readiness_state == READINESS_BOOT_READY && !_actuation_ready) {
        _actuation_ready = true;
    }

#if defined(AURA_DIRECT_FAN_PWM) && AURA_DIRECT_FAN_PWM
    // I-controller: trim duty to close RPM error. Runs every 2s when tach is live.
    if (_trim_holdoff_active && (int32_t)(now_ms - _trim_holdoff_ms) >= 0) _trim_holdoff_active = false;
    if (_actuation_ready && _tach_reader_ready && !_trim_holdoff_active &&
        (_last_trim_attempt_ms == 0 || (now_ms - _last_trim_attempt_ms) >= 2000)) {
        _last_trim_attempt_ms = now_ms;
        const uint16_t rpm_actual   = getRpmInstant();
        const uint16_t rpm_expected = AchModel::expectedRpm(_commanded_speed);
        const DutyTrimDecision trim = DutyTrimPolicy::update(_trim_accumulator,
                                                             _duty_trim,
                                                             rpm_actual,
                                                             rpm_expected,
                                                             TRIM_MIN,
                                                             TRIM_MAX,
                                                             TRIM_DEADBAND_RPM,
                                                             TRIM_K_I);
        if (trim.sample_consumed) {
            _trim_accumulator = trim.next_accumulator;
            _duty_trim = trim.next_trim;
            if (trim.adjusted) {
                _last_trim_adjust_ms = now_ms;
            }
        }
    }

    // OEM refreshes PWM every 2s to keep the secondary board alive.
    // Without this, the board goes cold after ~50s.
    if (_actuation_ready && (now_ms - _last_cmd_ms) >= 2000) {
        _applyPwmDuty(_commanded_speed);
        _last_cmd_ms = now_ms;
    }
#endif
}

void DaughterBoard::setModulePresentAssumed(bool present) {
    if (_module_present_assumed == present) return;
    _module_present_assumed = present;

    const uint32_t now_ms = millis();
    if (!present) {
        _actuation_ready = false;
        _actuation_proven = false;
        _setReadinessState(_readiness_state == READINESS_BOOT_READY ? READINESS_RUNTIME_LOST
                                                                    : READINESS_BOOT_MISSING,
                           now_ms);
        return;
    }

    _setReadinessState(READINESS_BOOT_SETTLING, now_ms);
}

bool DaughterBoard::setFanSpeed(uint8_t speed) {
    if (speed < MIN_SPEED) speed = MIN_SPEED;
    if (speed > MAX_SPEED) speed = MAX_SPEED;

    if (!isModuleReady() || !_actuation_ready || !_serial_ready) {
        return false;
    }

    const float prev_trim_accumulator = _trim_accumulator;
    const int8_t prev_trim = _duty_trim;
    const uint32_t prev_holdoff = _trim_holdoff_ms;
    const bool prev_holdoff_active = _trim_holdoff_active;
    const uint32_t prev_last_trim_adjust = _last_trim_adjust_ms;
    const uint32_t prev_last_trim_attempt = _last_trim_attempt_ms;
    const uint32_t now_ms = millis();
    _trim_accumulator = 0.0f;
    _duty_trim = 0;
    _trim_holdoff_ms = now_ms + TRIM_HOLDOFF_MS;
    _trim_holdoff_active = true;
    _last_trim_adjust_ms = now_ms;
    _last_trim_attempt_ms = now_ms;
    if (!sendFanSpeedHypothesis(speed, "setFanSpeed")) {
        _trim_accumulator = prev_trim_accumulator;
        _duty_trim = prev_trim;
        _trim_holdoff_ms = prev_holdoff;
        _trim_holdoff_active = prev_holdoff_active;
        _last_trim_adjust_ms = prev_last_trim_adjust;
        _last_trim_attempt_ms = prev_last_trim_attempt;
        return false;
    }
    return true;
}

RpmSample DaughterBoard::readRpmSample() {
    return readTachWindow(millis(), tachPulseCount(), _tach_reader_ready,
                          _tach_last_sample_ms, _tach_last_sample_count, AURA_TACH_PULSES_PER_REV);
}

uint16_t DaughterBoard::getRpm() {
    const auto sample = readRpmSample();
    return sample.valid ? sample.value : 0;
}

uint16_t DaughterBoard::getRpmInstant() const {
    if (!_tach_reader_ready) return 0;
    const uint32_t now_ms = millis();
    const uint32_t pulse_count = tachPulseCount();
    const uint32_t delta   = pulse_count - _trim_sample_count;
    const uint32_t elapsed = now_ms - _trim_sample_ms;
    if (elapsed == 0) return 0;
    if (elapsed > 10000) {
        _trim_sample_ms = now_ms;
        _trim_sample_count = pulse_count;
        return 0;
    }
    if (delta == 0) {
        _trim_sample_ms = now_ms;
        _trim_sample_count = pulse_count;
        return 0;
    }
    _trim_sample_ms = now_ms;
    _trim_sample_count = pulse_count;
    uint64_t rpm = ((uint64_t)delta * 60000UL) / elapsed;
    rpm /= AURA_TACH_PULSES_PER_REV;
    if (rpm > 65535U) rpm = 65535U;
    return (uint16_t)rpm;
}

uint32_t DaughterBoard::tachPulseCount() const {
    uint32_t pulse_count;
    noInterrupts();
    pulse_count = g_tach_pulse_count;
    interrupts();
    return pulse_count;
}

bool DaughterBoard::_sendFrame(const uint8_t* bytes, uint8_t len, const char* note) {
    if (!_serial_ready || !bytes || len == 0) return false;
    if (len > 16) return false;

    _uart_capture_busy = true;
    while (Serial1.available()) Serial1.read();
    Serial1.write(bytes, len);
    Serial1.flush();

    uint8_t rx[16] = {0};
    uint8_t rx_len = 0;
    uint32_t deadline = millis() + 150;
    while (millis() < deadline && rx_len < sizeof(rx)) {
        while (Serial1.available() && rx_len < sizeof(rx)) {
            rx[rx_len++] = (uint8_t)Serial1.read();
        }
        if (rx_len < sizeof(rx)) delay(1);
    }

    _last_tx_len = len;
    _last_rx_len = rx_len;
    _last_tx_ms = millis();
    _uart_capture_busy = false;
    _logFrame(bytes, len, rx, rx_len, note);
    return true;
}

void DaughterBoard::_logFrame(const uint8_t* tx, uint8_t tx_len,
                              const uint8_t* rx, uint8_t rx_len,
                              const char* note) {
    (void)tx; (void)tx_len; (void)rx; (void)rx_len; (void)note;
}

bool DaughterBoard::sendFanSpeedHypothesis(uint8_t speed, const char* note) {
    if (!_serial_ready) return false;
    if (!isModuleReady()) return false;
    if (!_actuation_ready) return false;

#if defined(AURA_DIRECT_FAN_PWM) && AURA_DIRECT_FAN_PWM
    _applyPwmDuty(speed);
    _commanded_speed = speed;
    _last_cmd_ms = millis();
    _logFrame(nullptr, 0, nullptr, 0, note ? note : "fan_speed_pwm");
    return true;
#elif defined(AURA_ENABLE_LAB_UART_FAN) && AURA_ENABLE_LAB_UART_FAN
    // Lab-only UART experiment. Not part of the public release runtime.

    auto fanFrameChecksum = [](const uint8_t* bytes, uint8_t len) -> uint8_t {
        uint16_t sum = 0;
        if (!bytes || len < 3) return 0;
        for (uint8_t i = 1; i + 1 < len; ++i) {
            sum = (uint16_t)(sum + bytes[i]);
        }
        return (uint8_t)(0u - (sum & 0xFF));
    };

    uint8_t frame[9] = {0};
    if (speed < MIN_SPEED) speed = MIN_SPEED;
    if (speed > MAX_SPEED) speed = MAX_SPEED;
    // Translate logical step → calibrated register for the OEM frame byte.
    const uint8_t reg = STEP_TO_REG[speed - 1];
    frame[0] = 0xFF;
    frame[1] = (uint8_t)(reg - 1);
    frame[2] = 0x00;
    frame[3] = 0x00;
    frame[4] = 0x00;
    frame[5] = 0x00;
    frame[6] = 0x01;
    frame[7] = 0x00;
    frame[8] = fanFrameChecksum(frame, sizeof(frame));
    if (!_sendFrame(frame, sizeof(frame), note ? note : "fan_speed_hypothesis")) {
        return false;
    }
    _commanded_speed = speed;
    _last_cmd_ms = millis();
    return true;
#else
    (void)speed;
    (void)note;
    return false;
#endif
}

void DaughterBoard::clearUartLog() {}

void DaughterBoard::pollPassiveUart() {}
