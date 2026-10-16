#pragma once
#include "application.h"
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "../net/mdns_hostname.h"

// EEPROM-backed config. P1 emulated EEPROM holds 2047 bytes.
//
// Region map:
//   0x000-0x0BF  legacy config image, read-only since b56 (migration source
//                and last-resort fallback; old firmware still reads it)
//   0x100-0x10F  control-state record, separate CRC block
//   0x120-0x13E  estimator record (Kalman + baseline), EepromRecord
//   0x300-0x3CE  config slot A, EepromRecord wrapping a config image
//   0x400-0x4CE  config slot B, EepromRecord wrapping a config image
//
// Config slots alternate by sequence number, so one slot always holds the
// last complete save. Each slot payload is a config image in the layout
// below, which is also the legacy layout at 0x000.
//
// Config image layout:
//   0x000  4  magic (0xAEA10002)
//   0x004  4  room_m3 (float)
//   0x008  4  reserved_unused
//   0x00C-0x00E  3  reserved_unused
//   0x00F  1  timezone_utc_offset_hours
//   0x010  4  cadr_max (float)
//   0x014  4  reserved_unused
//   0x018  4  kalman_x0 (float)
//   0x01C  4  kalman_x1 (float)
//   0x020  4  kalman_p00 (float)
//   0x024  4  baseline (float)
//   0x028  4  reserved_unused
//   0x02C  4  mqtt_broker_ip (uint32 packed)
//   0x030  2  mqtt_port
//   0x032  1  mqtt_enabled
//   0x033  1  room_profile (small/sleep/everyday/open_plan/c; legacy aliases accepted)
//   0x034 32  mqtt_username (null-term)
//   0x054 32  mqtt_password (null-term)
//   0x074  4  schema marker (0x41555241 = "AURA")
//   0x078-0x079  reserved (was uvc_enabled, sterionizer_enabled — always-on, not configurable)
//   0x07A  1  reserved_unused
//   0x07B  1  layout_version
//   0x07C-0x07F  CRC32 for layout v2-v4
//   0x081  1  max_fan_percent
//   0x082  1  min_fan_percent
//   0x083  1  led_enabled
//   0x084  1  led_custom_color_enabled
//   0x085  1  led_color_r
//   0x086  1  led_color_g
//   0x087  1  led_color_b
//   0x088-0x08B  CRC32 for layout v5+
//   0x08C-0x0A3  mdns_base_name (24 bytes, null-term)
//   0x0A4  1  schedule_enabled
//   0x0A5  2  schedule_start_min (uint16 LE)
//   0x0A7  2  schedule_end_min (uint16 LE)
//   0x0A9  1  schedule_fan_percent
//   0x0AA  1  static_ip_enabled
//   0x0AB  4  static_ip (uint32 packed)
//   0x0AF  4  static_netmask (uint32 packed)
//   0x0B3  4  static_gateway (uint32 packed)
//   0x0B7  4  static_dns (uint32 packed)

static constexpr uint32_t CONFIG_MAGIC_V1 = 0xAEA10001;
static constexpr uint32_t CONFIG_MAGIC   = 0xAEA10002;
static constexpr uint16_t EEPROM_BASE    = 0x000;
static constexpr uint16_t EEPROM_CRC_OFF_V1 = 0x078;
static constexpr uint16_t EEPROM_CRC_OFF_V4 = 0x07C; // v4 includes schema + air-treatment fields
static constexpr uint16_t EEPROM_CRC_OFF = 0x088;    // v5 adds LED controls
static constexpr uint16_t EEPROM_SCHEMA_OFF = 0x074;
static constexpr uint32_t CONFIG_SCHEMA_MARKER = 0x41555241; // "AURA"
static constexpr uint16_t EEPROM_RESERVED_F32_OFF = 0x014;
static constexpr int8_t   DEFAULT_TIMEZONE_OFFSET_HOURS = -5;
static constexpr uint16_t EEPROM_LAYOUT_OFF          = 0x07B;
static constexpr uint16_t EEPROM_MAX_FAN_PERCENT_OFF = 0x081;
static constexpr uint16_t EEPROM_MIN_FAN_PERCENT_OFF = 0x082;
static constexpr uint16_t EEPROM_LED_ENABLED_OFF = 0x083;
static constexpr uint16_t EEPROM_LED_CUSTOM_COLOR_ENABLED_OFF = 0x084;
static constexpr uint16_t EEPROM_LED_COLOR_OFF = 0x085;
static constexpr uint16_t EEPROM_MDNS_BASE_OFF = 0x08C;
static constexpr uint16_t EEPROM_SCHEDULE_OFF = 0x0A4;
static constexpr uint16_t EEPROM_STATIC_IP_OFF = 0x0AA;
static constexpr uint8_t  CONFIG_LAYOUT_VERSION      = 8; // Bump to add static IP pin
static constexpr uint16_t EEPROM_CONTROL_STATE_OFF   = 0x100;
static constexpr uint16_t EEPROM_CONTROL_STATE_CRC_OFF = 0x10C;
static constexpr uint32_t CONTROL_STATE_MAGIC        = 0x4354524C; // "CTRL"
static constexpr uint8_t  CONTROL_STATE_VERSION      = 1;
static constexpr uint16_t CONFIG_IMAGE_LEN           = 0x0C0;
static constexpr uint16_t CONFIG_SLOT_A_OFF          = 0x300;
static constexpr uint16_t CONFIG_SLOT_B_OFF          = 0x400;
static constexpr uint32_t CONFIG_SLOT_MAGIC          = 0x53474643; // "CFGS"
static constexpr uint8_t  CONFIG_SLOT_VERSION        = 1;
static constexpr uint16_t EEPROM_ESTIMATOR_OFF       = 0x120;
static constexpr uint32_t ESTIMATOR_MAGIC            = 0x4D545345; // "ESTM"
static constexpr uint8_t  ESTIMATOR_VERSION          = 1;

enum ConfigLoadResult : uint8_t {
    CONFIG_LOAD_OK = 0,        // newest config slot loaded
    CONFIG_LOAD_RECOVERED,     // a torn slot was skipped; older slot loaded
    CONFIG_LOAD_LEGACY,        // no valid slot; legacy image migrated
    CONFIG_LOAD_DEFAULTS,      // nothing valid; defaults saved
};

inline const char* configLoadResultName(ConfigLoadResult r) {
    switch (r) {
        case CONFIG_LOAD_OK:        return "ok";
        case CONFIG_LOAD_RECOVERED: return "recovered";
        case CONFIG_LOAD_LEGACY:    return "legacy";
        case CONFIG_LOAD_DEFAULTS:  return "defaults";
    }
    return "unknown";
}

enum ControlStateMode : uint8_t {
    CONTROL_STATE_AUTO = 0,
    CONTROL_STATE_MANUAL = 1,
};

static constexpr uint8_t ROOM_PROFILE_UNKNOWN  = 0x00;
static constexpr uint8_t ROOM_PROFILE_SLEEP    = 's';
static constexpr uint8_t ROOM_PROFILE_EVERYDAY = 'l';
static constexpr uint8_t ROOM_PROFILE_OPEN_PLAN = 'o';

const char* roomProfileKey(uint8_t code);
uint8_t     roomProfileCodeFromKey(const char* key, uint8_t fallback = ROOM_PROFILE_EVERYDAY);
bool        roomProfileIsPreset(uint8_t code);

inline const char* roomProfileKey(uint8_t code) {
    switch (code) {
        case ROOM_PROFILE_SLEEP:    return "sleep";
        case ROOM_PROFILE_EVERYDAY: return "everyday";
        case ROOM_PROFILE_OPEN_PLAN:return "open_plan";
        default:                    return "everyday";
    }
}

inline uint8_t roomProfileCodeFromKey(const char* key, uint8_t fallback) {
    if (!key || !key[0]) return fallback;
    if (!strcmp(key, "sleep"))    return ROOM_PROFILE_SLEEP;
    if (!strcmp(key, "everyday")) return ROOM_PROFILE_EVERYDAY;
    if (!strcmp(key, "open_plan")) return ROOM_PROFILE_OPEN_PLAN;
    return fallback;
}

inline bool roomProfileIsPreset(uint8_t code) {
    return code == ROOM_PROFILE_SLEEP ||
           code == ROOM_PROFILE_EVERYDAY ||
           code == ROOM_PROFILE_OPEN_PLAN;
}

struct Config {
    float    room_m3;
    uint8_t  room_profile;
    int8_t   timezone_offset_hours;
    float    cadr_max;
    float    kalman_x0;
    float    kalman_x1;
    float    kalman_p00; // P[0] covariance — hot-start warm confidence
    float    baseline;
    uint32_t mqtt_broker_ip;
    uint16_t mqtt_port;
    bool     mqtt_enabled;
    uint8_t  max_fan_percent;
    uint8_t  min_fan_percent;
    bool     led_enabled;
    bool     led_custom_color_enabled;
    uint8_t  led_color_r;
    uint8_t  led_color_g;
    uint8_t  led_color_b;
    char     mdns_base_name[24];
    char     mqtt_user[32];
    char     mqtt_pass[32];
    bool     schedule_enabled;
    uint16_t schedule_start_min;
    uint16_t schedule_end_min;
    uint8_t  schedule_fan_percent;
    bool     static_ip_enabled;
    uint32_t static_ip;       // packed like mqtt_broker_ip (first octet in high byte)
    uint32_t static_netmask;
    uint32_t static_gateway;
    uint32_t static_dns;
};

struct PersistedEstimator {
    float kalman_x0;
    float kalman_x1;
    float kalman_p00;
    float baseline;
};

struct PersistedControlState {
    uint8_t mode;
    uint8_t manual_percent;
    uint8_t manual_speed;
};

class ConfigStore {
public:
    class Lock {
    public:
        Lock();
        ~Lock();
    };

    static void     defaults(Config* c);
    // Returns false only when no stored config was usable and defaults
    // were saved. Estimator values overlay the Kalman/baseline fields.
    static bool     load(Config* c, ConfigLoadResult* result = nullptr);
    // Writes the older config slot. Never touches the legacy image.
    static void     save(const Config* c);
    // Serializes c into a CONFIG_IMAGE_LEN-byte image with its inner CRC.
    static void     encodeImage(const Config* c, uint8_t* img);
    static uint32_t crc32(const uint8_t* data, uint16_t len);
    static bool     loadControlState(PersistedControlState* state);
    static void     saveControlState(const PersistedControlState* state);
    // Periodic hot-start state, kept apart so config bytes stay unchanged.
    static bool     loadEstimator(PersistedEstimator* est);
    static void     saveEstimator(const PersistedEstimator* est);
};
