#pragma once
#include "pm_frame_parser.h"
#include "../algo/sample_age.h"
#include "application.h"
#include <stdint.h>
#include "../app/air_controller.h"  // for SensorReading

struct SensorCycleStatus {
    bool sgp30_valid;
    bool sht31_valid;
    bool pm_valid;
    bool full_valid;
};

// Reads all sensors:
//   SGP30    @ I2C 0x58 — TVOC + eCO2
//   Humidity lane @ I2C 0x40 / 0x44 / 0x45 — HDC1080 or SHT31 family
//   Serial1  @ 9600 baud — WS180313138 proprietary PM2.5 stream
//     Frame: 0xFF 0x18 + 5 data bytes + 1 checksum byte = 9 bytes total.
//     PM2.5 (µg/m³): byte 4.
//
// Historical firmware strings point at HDC1080, while the newer sensor lane
// can also be spoken to as SHT31. We auto-detect the humidity family at run time.
class SensorHub {
public:
    static constexpr uint8_t SGP30_ADDR  = 0x58;
    static constexpr uint8_t HDC1080_ADDR = 0x40;
    static constexpr uint8_t SHT31_ADDR   = 0x44; // may be 0x45 — check ADDR pin
    static constexpr uint32_t SGP30_WARMUP_MS = 15000; // 15s before readings valid
    static constexpr uint32_t SGP30_MEASURE_INTERVAL_MS = 1000; // datasheet IAQ cadence
    static constexpr uint32_t SGP30_RETRY_INTERVAL_MS = 30000;
    static constexpr uint32_t SGP30_FRESH_WINDOW_MS = 5000;
    static constexpr uint32_t PM_FRESH_WINDOW_MS = 15000; // PM frame freshness window

    SensorHub();

    // Call once in setup().
    bool begin();

    // Returns true when at least one sensor lane is usable.
    bool read(SensorReading* out, SensorCycleStatus* status = nullptr);

    // Call frequently (every loop iteration) to drain Serial1 PM frames and
    // keep SGP30 IAQ sampling running at 1 Hz.
    // Caches the latest valid PM2.5 reading for the next read() call.
    void servicePm();

    // Feed current T+RH to SGP30 humidity compensation command.
    void setHumidityCompensation(float temp_c, float rh_pct);

    bool sgp30Ready() const { return _sgp30_ready; }
    bool sht31Ready() const { return _sht31_ready; }
    // True when the most recent sensor cycle received a fresh PM frame.
    bool pmReady()    const { return _pm_ready; }
    bool sgp30HasCached() const { return _sgp30_has_cached; }
    float vocPpb() const { return _last_voc_ppb; }
    float eco2Ppm() const { return _last_eco2_ppm; }
    uint32_t sgp30LastValidMs() const { return _sgp30_last_valid_ms; }
    uint32_t humidityLastValidMs() const { return _humidity_last_valid_ms; }
    uint32_t sgp30AgeMs() const { return _gas_age.ageMs(); }
    uint32_t humidityAgeMs() const { return _humidity_age.ageMs(); }
    uint32_t pmAgeMs() const { return _pm_age.ageMs(); }
    uint32_t sgp30Generation() const { return _gas_age.generation(); }
    uint32_t pmGeneration() const { return _pm_age.generation(); }
    uint32_t humidityGeneration() const { return _humidity_age.generation(); }
    bool sgp30InWarmup(uint32_t now_ms) const {
        (void)now_ms;
        return _sgp30_ready && _sgp30_init_age.ageMs() < SGP30_WARMUP_MS;
    }
    bool pmHasCached() const { return _pm_has_cached; }
    float pm25() const { return _pm_cached_pm25; }
    uint32_t pmBytesSeen() const { return _pm_bytes_seen; }
    uint32_t pmValidFrames() const { return _pm_valid_frames; }
    uint32_t pmChecksumFailures() const { return _pm_checksum_failures; }
    uint32_t pmLastValidMs() const { return _pm_last_valid_ms; }
    uint8_t pmRawCount() const { return _pm_raw_count; }
    void pmRawHex(char* out, size_t out_len) const;

private:
    enum class HumiditySensorKind : uint8_t {
        Unknown = 0,
        HDC1080 = 1,
        SHT31   = 2,
    };

    bool     _sgp30_ready;
    bool     _sht31_ready;
    bool     _pm_ready;
    HumiditySensorKind _humidity_kind;
    uint32_t _sgp30_last_init_attempt_ms;
    uint32_t _pm_bytes_seen;
    uint32_t _pm_valid_frames;
    uint32_t _pm_checksum_failures;
    uint32_t _pm_last_valid_ms;
    float    _pm_cached_pm25;
    bool     _pm_has_cached;
    float    _last_voc_ppb;
    float    _last_eco2_ppm;
    float    _last_temp_c;
    float    _last_rh_pct;
    bool     _sgp30_has_cached;
    bool     _sht31_has_cached;
    uint32_t _sgp30_last_sample_ms;
    uint32_t _sgp30_last_valid_ms;
    uint32_t _humidity_last_valid_ms;
    uint8_t  _pm_raw[64];
    uint8_t  _pm_raw_head;
    uint8_t  _pm_raw_count;
    SampleAge _pm_age;
    SampleAge _gas_age;
    SampleAge _humidity_age;
    SampleAge _sgp30_init_age;

    // SGP30 raw commands
    bool     _sgp30Init();
    bool     _sgp30Measure(uint16_t* tvoc, uint16_t* eco2);
    bool     _sgp30SetHumidity(uint16_t word);
    uint8_t  _sgp30Crc(uint8_t* data, uint8_t len);

    // Humidity lane: HDC1080 or SHT31 auto-detect
    bool     _hdc1080Measure(float* temp_c, float* rh_pct);
    bool     _sht31Measure(float* temp_c, float* rh_pct);

    // Serial1 PM telemetry parser (WS180313138 0xFF 0x18 9-byte frames)
    bool     _pmRead(float* pm25);
    PmFrameParser _pm_parser;
    void     _pmCaptureRaw(uint8_t b);

    void     _serviceSgp30(uint32_t now_ms);
    bool     _sgp30IsFresh(uint32_t now_ms) const;
    bool     _pmIsFresh(uint32_t now_ms) const;
};
