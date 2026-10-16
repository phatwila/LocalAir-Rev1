#include "sensor_hub.h"
#include "../algo/humidity_comp.h"
#include <math.h>

SensorHub::SensorHub()
    : _sgp30_ready(false), _sht31_ready(false), _pm_ready(false),
      _humidity_kind(HumiditySensorKind::Unknown),
      _sgp30_last_init_attempt_ms(0),
      _pm_bytes_seen(0), _pm_valid_frames(0),
      _pm_checksum_failures(0), _pm_last_valid_ms(0),
      _pm_cached_pm25(NAN), _pm_has_cached(false),
      _last_voc_ppb(0.0f), _last_eco2_ppm(400.0f),
      _last_temp_c(25.0f), _last_rh_pct(50.0f),
      _sgp30_has_cached(false), _sht31_has_cached(false),
      _sgp30_last_sample_ms(0), _sgp30_last_valid_ms(0), _humidity_last_valid_ms(0),
      _pm_raw_head(0), _pm_raw_count(0)
{
    memset(_pm_raw, 0, sizeof(_pm_raw));
}

bool SensorHub::begin() {
    Wire.begin();
    // Serial1 is initialized by DaughterBoard::begin() at 9600 baud before this runs.

    _sgp30_ready = _sgp30Init();
    _sht31_ready = true; // humidity lane auto-detects on first read
    _humidity_kind = HumiditySensorKind::Unknown;
    _pm_ready    = false; // PM telemetry becomes available after a valid frame is parsed
    _sgp30_last_init_attempt_ms = millis();
    if (_sgp30_ready) _sgp30_init_age.sample(_sgp30_last_init_attempt_ms);
    _sgp30_last_sample_ms = 0;
    _sgp30_last_valid_ms = 0;
    _humidity_last_valid_ms = 0;
    return _sgp30_ready;
}

bool SensorHub::read(SensorReading* out, SensorCycleStatus* status) {
    if (!out) return false;
    if (status) {
        status->sgp30_valid = false;
        status->sht31_valid = false;
        status->pm_valid = false;
        status->full_valid = false;
    }
    const uint32_t now = millis();
    _pm_age.tick(now);
    _gas_age.tick(now);
    _humidity_age.tick(now);
    _sgp30_init_age.tick(now);

    float temp_c = 25.0f, rh_pct = 50.0f;
    bool humidity_ok = false;
    if (_humidity_kind == HumiditySensorKind::HDC1080) {
        humidity_ok = _hdc1080Measure(&temp_c, &rh_pct);
    } else if (_humidity_kind == HumiditySensorKind::SHT31) {
        humidity_ok = _sht31Measure(&temp_c, &rh_pct);
    } else {
        if (_hdc1080Measure(&temp_c, &rh_pct)) {
            _humidity_kind = HumiditySensorKind::HDC1080;
            humidity_ok = true;
        } else if (_sht31Measure(&temp_c, &rh_pct)) {
            _humidity_kind = HumiditySensorKind::SHT31;
            humidity_ok = true;
        }
    }
    if (humidity_ok) {
        _last_temp_c = temp_c;
        _last_rh_pct = rh_pct;
        _sht31_has_cached = true;
        _humidity_last_valid_ms = now;
        _humidity_age.sample(millis());
    } else if (_sht31_has_cached) {
        temp_c = _last_temp_c;
        rh_pct = _last_rh_pct;
    }
    out->temp_c = temp_c;
    out->rh_pct = rh_pct;

    // Update SGP30 humidity compensation each cycle
    if (humidity_ok || _sht31_has_cached) {
        setHumidityCompensation(temp_c, rh_pct);
    }

    const uint32_t observed_ms = millis();
    _pm_age.tick(observed_ms);
    _gas_age.tick(observed_ms);
    _humidity_age.tick(observed_ms);
    _sgp30_init_age.tick(observed_ms);
    const bool pm_fresh = _pmIsFresh(observed_ms);
    const bool sgp30_ok = _sgp30IsFresh(observed_ms);
    out->voc_ppb  = _sgp30_has_cached ? _last_voc_ppb : 0.0f;
    out->eco2_ppm = _sgp30_has_cached ? _last_eco2_ppm : 400.0f;

    // PM values are cached by servicePm() which runs every loop iteration.
    // Just consume the latest cached reading here.
    out->pm25 = _pm_has_cached ? _pm_cached_pm25 : NAN;
    _pm_ready = pm_fresh;

    if (status) {
        status->sgp30_valid = sgp30_ok;
        status->sht31_valid = humidity_ok;
        status->pm_valid = pm_fresh;
        status->full_valid = status->sgp30_valid && status->sht31_valid && status->pm_valid;
    }

    return sgp30_ok || pm_fresh || humidity_ok;
}

void SensorHub::servicePm() {
    const uint32_t now_ms = millis();
    _pm_age.tick(now_ms);
    _gas_age.tick(now_ms);
    _humidity_age.tick(now_ms);
    _sgp30_init_age.tick(now_ms);
    _serviceSgp30(now_ms);

    // Drain all available Serial1 bytes and parse frames.
    // Called every main loop iteration to avoid missing frames.
    float pm25 = NAN;
    if (_pmRead(&pm25)) {
        _pm_cached_pm25 = pm25;
        _pm_has_cached = true;
        _pm_age.sample(millis());
    }
    const uint32_t observed_ms = millis();
    _pm_age.tick(observed_ms);
    _gas_age.tick(observed_ms);
    _humidity_age.tick(observed_ms);
    _sgp30_init_age.tick(observed_ms);
    _pm_ready = _pmIsFresh(observed_ms);
}

void SensorHub::setHumidityCompensation(float temp_c, float rh_pct) {
    uint16_t word = HumidityComp::sgp30HumidityWord(temp_c, rh_pct);
    _sgp30SetHumidity(word);
}

// ── SGP30 raw I2C ─────────────────────────────────────────────────────────────

bool SensorHub::_sgp30Init() {
    Wire.beginTransmission(SGP30_ADDR);
    Wire.write(0x20); Wire.write(0x03); // Init_air_quality command
    return (Wire.endTransmission() == 0);
}

bool SensorHub::_sgp30Measure(uint16_t* tvoc, uint16_t* eco2) {
    Wire.beginTransmission(SGP30_ADDR);
    Wire.write(0x20); Wire.write(0x08); // Measure_air_quality
    if (Wire.endTransmission() != 0) return false;

    delay(12); // SGP30 needs 12ms to compute

    if (Wire.requestFrom((uint8_t)SGP30_ADDR, (uint8_t)6) != 6) return false;
    uint8_t buf[6];
    for (uint8_t i = 0; i < 6; i++) buf[i] = Wire.read();

    // Verify CRCs
    if (_sgp30Crc(buf, 2) != buf[2]) return false;
    if (_sgp30Crc(buf + 3, 2) != buf[5]) return false;

    if (eco2)  *eco2  = ((uint16_t)buf[0] << 8) | buf[1];
    if (tvoc)  *tvoc  = ((uint16_t)buf[3] << 8) | buf[4];
    return true;
}

bool SensorHub::_sgp30SetHumidity(uint16_t word) {
    uint8_t data[2] = { (uint8_t)(word >> 8), (uint8_t)(word & 0xFF) };
    Wire.beginTransmission(SGP30_ADDR);
    Wire.write(0x20); Wire.write(0x61); // Set_humidity
    Wire.write(data[0]); Wire.write(data[1]);
    Wire.write(_sgp30Crc(data, 2));
    return (Wire.endTransmission() == 0);
}

uint8_t SensorHub::_sgp30Crc(uint8_t* data, uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

// ── Humidity lane ─────────────────────────────────────────────────────────────

bool SensorHub::_hdc1080Measure(float* temp_c, float* rh_pct) {
    // Historical firmware strings point to HDC1080 on the humidity lane.
    // The HDC1080 uses 7-bit I2C address 0x40 and returns temperature then RH.
    // Configure 14-bit T/RH mode before each one-shot read so the probe works
    // even if the sensor woke up in its default state.
    Wire.beginTransmission(HDC1080_ADDR);
    Wire.write(0x02);
    Wire.write(0x10);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;

    Wire.beginTransmission(HDC1080_ADDR);
    Wire.write(0x00); // trigger temperature + humidity conversion
    if (Wire.endTransmission() != 0) return false;

    delay(15);

    if (Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)4) != 4) return false;
    uint8_t buf[4];
    for (uint8_t i = 0; i < 4; i++) buf[i] = Wire.read();

    uint16_t raw_t  = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_rh = ((uint16_t)buf[2] << 8) | buf[3];

    if (temp_c)  *temp_c  = -40.0f + 165.0f * ((float)raw_t  / 65536.0f);
    if (rh_pct)  *rh_pct  = 100.0f           * ((float)raw_rh / 65536.0f);
    return true;
}

bool SensorHub::_sht31Measure(float* temp_c, float* rh_pct) {
    Wire.beginTransmission(SHT31_ADDR);
    Wire.write(0x2C); Wire.write(0x06); // single shot, high repeatability
    if (Wire.endTransmission() != 0) return false;

    delay(15);

    if (Wire.requestFrom((uint8_t)SHT31_ADDR, (uint8_t)6) != 6) return false;
    uint8_t buf[6];
    for (uint8_t i = 0; i < 6; i++) buf[i] = Wire.read();

    if (_sgp30Crc(buf, 2) != buf[2]) return false;
    if (_sgp30Crc(buf + 3, 2) != buf[5]) return false;

    uint16_t raw_t  = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_rh = ((uint16_t)buf[3] << 8) | buf[4];

    if (temp_c)  *temp_c  = -45.0f + 175.0f * ((float)raw_t  / 65535.0f);
    if (rh_pct)  *rh_pct  = 100.0f           * ((float)raw_rh / 65535.0f);
    return true;
}

bool SensorHub::_pmRead(float* pm25) {
    _pm_parser.expire(millis());
    while (Serial1.available()) {
        const uint8_t byte = (uint8_t)Serial1.read();
        ++_pm_bytes_seen;
        _pmCaptureRaw(byte);
        const auto result = _pm_parser.feed(byte, millis(), pm25);
        if (result == PmFrameParser::BadChecksum) ++_pm_checksum_failures;
        if (result != PmFrameParser::Complete) continue;
        ++_pm_valid_frames;
        _pm_last_valid_ms = millis();
        return true;
    }
    return false;
}

void SensorHub::_serviceSgp30(uint32_t now_ms) {
    if (!_sgp30_ready) {
        if ((uint32_t)(now_ms - _sgp30_last_init_attempt_ms) < SGP30_RETRY_INTERVAL_MS) return;
        _sgp30_last_init_attempt_ms = now_ms;
        _sgp30_ready = _sgp30Init();
        if (!_sgp30_ready) return;
        _sgp30_init_age.sample(millis());
        _sgp30_last_sample_ms = 0;
    }
    if (_sgp30_last_sample_ms != 0 &&
        (now_ms - _sgp30_last_sample_ms) < SGP30_MEASURE_INTERVAL_MS) {
        return;
    }

    uint16_t tvoc = 0;
    uint16_t eco2 = 400;
    _sgp30_last_sample_ms = now_ms;
    if (_sgp30Measure(&tvoc, &eco2)) {
        _last_voc_ppb = (float)tvoc;
        _last_eco2_ppm = (float)eco2;
        _sgp30_has_cached = true;
        _sgp30_last_valid_ms = now_ms;
        _gas_age.sample(millis());
    } else if (!sgp30InWarmup(now_ms) && _gas_age.ageMs() > SGP30_WARMUP_MS) {
        // A sustained measurement outage may require a new IAQ initialization.
        _sgp30_ready = false;
        _sgp30_last_init_attempt_ms = now_ms;
    }
}

bool SensorHub::_sgp30IsFresh(uint32_t now_ms) const {
    return _sgp30_ready && !sgp30InWarmup(now_ms) &&
        _gas_age.ageMs() <= SGP30_FRESH_WINDOW_MS;
}

void SensorHub::_pmCaptureRaw(uint8_t b) {
    _pm_raw[_pm_raw_head] = b;
    _pm_raw_head = (uint8_t)((_pm_raw_head + 1) % sizeof(_pm_raw));
    if (_pm_raw_count < sizeof(_pm_raw)) _pm_raw_count++;
}

void SensorHub::pmRawHex(char* out, size_t out_len) const {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (_pm_raw_count == 0) return;

    size_t pos = 0;
    for (uint8_t i = 0; i < _pm_raw_count; ++i) {
        uint8_t idx = (uint8_t)((_pm_raw_head + sizeof(_pm_raw) - _pm_raw_count + i) % sizeof(_pm_raw));
        int n = snprintf(out + pos, out_len - pos, "%s%02X", i ? " " : "", _pm_raw[idx]);
        if (n < 0) break;
        pos += (size_t)n;
        if (pos >= out_len) break;
    }
}

bool SensorHub::_pmIsFresh(uint32_t now_ms) const {
    (void)now_ms;
    return _pm_age.ageMs() <= PM_FRESH_WINDOW_MS;
}
