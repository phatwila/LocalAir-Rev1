#include "config_store.h"
#include "eeprom_record.h"
#include "../algo/ach_model.h"
#include "../algo/adaptive_baseline.h"
#include "../algo/fan_speed.h"
#include <math.h>
#include <string.h>

namespace {
RecursiveMutex g_config_mutex;

static uint8_t nearestRoomProfile(float room_m3) {
    if (!isfinite(room_m3) || room_m3 <= 0.0f) {
        return ROOM_PROFILE_EVERYDAY;
    }
    if (room_m3 <= 22.0f) return ROOM_PROFILE_SLEEP;
    if (room_m3 <= 35.0f) return ROOM_PROFILE_EVERYDAY;
    return ROOM_PROFILE_OPEN_PLAN;
}

static uint8_t coerceStoredRoomProfile(uint8_t code, float room_m3) {
    if (roomProfileIsPreset(code)) {
        return code;
    }
    switch (code) {
        case 'b':
        case 's':
            return ROOM_PROFILE_SLEEP;
        case 'l':
            return ROOM_PROFILE_EVERYDAY;
        case 'o':
            return ROOM_PROFILE_OPEN_PLAN;
        default:
            return nearestRoomProfile(room_m3);
    }
}
}  // namespace

ConfigStore::Lock::Lock() { g_config_mutex.lock(); }
ConfigStore::Lock::~Lock() { g_config_mutex.unlock(); }

void ConfigStore::defaults(Config* c) {
    c->room_m3          = 30.0f;
    c->room_profile     = ROOM_PROFILE_EVERYDAY;
    c->timezone_offset_hours = DEFAULT_TIMEZONE_OFFSET_HOURS;
    c->cadr_max         = 180.0f;
    c->kalman_x0        = 20.0f;
    c->kalman_x1        = 0.0f;
    c->kalman_p00       = 500.0f;
    c->baseline         = 20.0f;
    c->mqtt_broker_ip   = 0;
    c->mqtt_port        = 1883;
    c->mqtt_enabled     = false;
    c->max_fan_percent     = 100;
    c->min_fan_percent     = 40;
    c->led_enabled      = true;
    c->led_custom_color_enabled = false;
    c->led_color_r      = 255;
    c->led_color_g      = 255;
    c->led_color_b      = 255;
    memset(c->mdns_base_name, 0, sizeof(c->mdns_base_name));
    strncpy(c->mdns_base_name, "localair", sizeof(c->mdns_base_name) - 1);
    memset(c->mqtt_user, 0, sizeof(c->mqtt_user));
    memset(c->mqtt_pass, 0, sizeof(c->mqtt_pass));
    c->schedule_enabled = false;
    c->schedule_start_min = 1320;
    c->schedule_end_min = 420;
    c->schedule_fan_percent = 35;
    c->static_ip_enabled = false;
    c->static_ip = 0;
    c->static_netmask = 0;
    c->static_gateway = 0;
    c->static_dns = 0;
}


// Little-endian accessors over an in-memory config image.
static float imgFloat(const uint8_t* img, uint16_t off) {
    float v;
    memcpy(&v, img + off, 4);
    return v;
}

static uint32_t imgU32(const uint8_t* img, uint16_t off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)img[off + i] << (8 * i);
    return v;
}

static uint16_t imgU16(const uint8_t* img, uint16_t off) {
    return (uint16_t)img[off] | ((uint16_t)img[off + 1] << 8);
}

static void imgPutFloat(uint8_t* img, uint16_t off, float v) {
    memcpy(img + off, &v, 4);
}

static void imgPutU32(uint8_t* img, uint16_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) img[off + i] = (v >> (8 * i)) & 0xFF;
}

static void imgPutU16(uint8_t* img, uint16_t off, uint16_t v) {
    img[off] = v & 0xFF;
    img[off + 1] = (v >> 8) & 0xFF;
}

static bool fixupConfig(Config* c) {
    bool fixed = false;
    // Sanity-check critical float fields — corrupt flash can produce NaN/Inf/zero
    if (c->room_m3    <= 0.0f || c->room_m3    > 2000.0f) { c->room_m3    = 30.0f; fixed = true; }
    if (c->cadr_max   < AchModel::CADR_MAX_SANE_MIN || c->cadr_max   > AchModel::CADR_MAX_SANE_MAX) {
        c->cadr_max = AchModel::CADR_MAX_DEFAULT;
        fixed = true;
    }
    if (c->baseline   < 1.0f || c->baseline  > 5000.0f)   { c->baseline   = 20.0f; fixed = true; }
    if (c->kalman_x0  < 0.0f || c->kalman_x0 > 5000.0f)   { c->kalman_x0  = 20.0f; fixed = true; }
    if (c->kalman_p00 <= 0.0f || c->kalman_p00 > 1e6f)    { c->kalman_p00 = 500.0f; fixed = true; }

    // Legacy builds used a CO2-like VOC scale around 400.
    // Migrate those hot-start values to the SGP30 TVOC scale.
    if (c->baseline >= 300.0f && c->kalman_x0 >= 300.0f) {
        c->baseline = 20.0f;
        c->kalman_x0 = 20.0f;
        c->kalman_x1 = 0.0f;
        c->kalman_p00 = 500.0f;
        fixed = true;
    }

    const uint8_t coerced_profile = coerceStoredRoomProfile(c->room_profile, c->room_m3);
    if (coerced_profile != c->room_profile) {
        c->room_profile = coerced_profile;
        fixed = true;
    }
    return fixed;
}

static void resetScheduleDefaults(Config* c) {
    c->schedule_enabled = false;
    c->schedule_start_min = 1320;
    c->schedule_end_min = 420;
    c->schedule_fan_percent = 35;
}

static bool scheduleConfigIsValid(const Config* c) {
    if (!c) return false;
    if (c->schedule_start_min > 1439) return false;
    if (c->schedule_end_min > 1439) return false;
    if (c->schedule_fan_percent > 100) return false;
    if (c->schedule_start_min == c->schedule_end_min) return false;
    return true;
}

static void resetStaticIpDefaults(Config* c) {
    c->static_ip_enabled = false;
    c->static_ip = 0;
    c->static_netmask = 0;
    c->static_gateway = 0;
    c->static_dns = 0;
}

static bool staticIpConfigIsValid(const Config* c) {
    if (!c) return false;
    if (!c->static_ip_enabled) return true;
    // Enabled pin requires at least an address and netmask
    if (c->static_ip == 0 || c->static_netmask == 0) return false;
    return true;
}

static bool decodeLegacyV1(const uint8_t* img, Config* c) {
    if (ConfigStore::crc32(img, EEPROM_CRC_OFF_V1) != imgU32(img, EEPROM_CRC_OFF_V1)) return false;

    ConfigStore::defaults(c);
    c->room_m3          = imgFloat(img, 0x004);
    c->timezone_offset_hours = DEFAULT_TIMEZONE_OFFSET_HOURS;
    c->cadr_max         = imgFloat(img, 0x010);
    c->kalman_x0        = imgFloat(img, 0x018);
    c->kalman_x1        = imgFloat(img, 0x01C);
    c->kalman_p00       = 500.0f;
    c->baseline         = imgFloat(img, 0x020);
    memcpy(&c->mqtt_broker_ip, img + 0x028, 4);
    c->mqtt_port = imgU16(img, 0x02C);
    if (c->mqtt_port == 0) c->mqtt_port = 1883;
    c->mqtt_enabled = c->mqtt_broker_ip != 0;
    memcpy(c->mqtt_user, img + 0x038, 32);
    memcpy(c->mqtt_pass, img + 0x058, 32);
    c->mqtt_user[31] = c->mqtt_pass[31] = '\0';

    c->room_profile = coerceStoredRoomProfile(c->room_profile, c->room_m3);
    fixupConfig(c);
    return true;
}

// Decodes a config image. Returns false for a bad magic or CRC; the caller
// picks the next source. needs_save reports repairs that should persist.
static bool decodeImage(const uint8_t* img, Config* c, bool* needs_save) {
    *needs_save = false;
    const uint32_t magic = imgU32(img, EEPROM_BASE);
    if (magic == CONFIG_MAGIC_V1) {
        if (!decodeLegacyV1(img, c)) return false;
        *needs_save = true;
        return true;
    }
    if (magic != CONFIG_MAGIC) return false;

    ConfigStore::defaults(c);
    c->room_m3          = imgFloat(img, 0x004);
    c->room_profile     = img[0x033];
    c->cadr_max         = imgFloat(img, 0x010);
    c->kalman_x0        = imgFloat(img, 0x018);
    c->kalman_x1        = imgFloat(img, 0x01C);
    c->kalman_p00       = imgFloat(img, 0x020);
    c->baseline         = imgFloat(img, 0x024);
    memcpy(&c->mqtt_broker_ip, img + 0x02C, 4);
    c->mqtt_port    = imgU16(img, 0x030);
    c->mqtt_enabled = img[0x032] != 0;
    const uint8_t layout_version = img[EEPROM_LAYOUT_OFF];
    c->max_fan_percent = (layout_version >= 3)
        ? img[EEPROM_MAX_FAN_PERCENT_OFF]
        : 100;
    if (c->max_fan_percent == 0 || c->max_fan_percent > 100) c->max_fan_percent = 100;
    c->min_fan_percent = (layout_version >= 4)
        ? img[EEPROM_MIN_FAN_PERCENT_OFF]
        : 40;
    if (c->min_fan_percent < 40 || c->min_fan_percent > 100) c->min_fan_percent = 40;
    if (layout_version >= 5) {
        c->led_enabled = img[EEPROM_LED_ENABLED_OFF] != 0;
        c->led_custom_color_enabled = img[EEPROM_LED_CUSTOM_COLOR_ENABLED_OFF] != 0;
        c->led_color_r = img[EEPROM_LED_COLOR_OFF + 0];
        c->led_color_g = img[EEPROM_LED_COLOR_OFF + 1];
        c->led_color_b = img[EEPROM_LED_COLOR_OFF + 2];
    }
    if (layout_version >= 6) {
        memcpy(c->mdns_base_name, img + EEPROM_MDNS_BASE_OFF, 24);
        c->mdns_base_name[23] = '\0';
    }
    if (layout_version >= 7) {
        c->schedule_enabled = img[EEPROM_SCHEDULE_OFF] != 0;
        c->schedule_start_min = imgU16(img, EEPROM_SCHEDULE_OFF + 1);
        c->schedule_end_min = imgU16(img, EEPROM_SCHEDULE_OFF + 3);
        c->schedule_fan_percent = img[EEPROM_SCHEDULE_OFF + 5];
    }
    if (layout_version >= 8) {
        c->static_ip_enabled = img[EEPROM_STATIC_IP_OFF] != 0;
        c->static_ip      = imgU32(img, EEPROM_STATIC_IP_OFF + 1);
        c->static_netmask = imgU32(img, EEPROM_STATIC_IP_OFF + 5);
        c->static_gateway = imgU32(img, EEPROM_STATIC_IP_OFF + 9);
        c->static_dns     = imgU32(img, EEPROM_STATIC_IP_OFF + 13);
    }

    memcpy(c->mqtt_user, img + 0x034, 32);
    memcpy(c->mqtt_pass, img + 0x054, 32);
    c->mqtt_user[31] = c->mqtt_pass[31] = '\0';

    bool schema_ok = true;
    if (imgU32(img, EEPROM_SCHEMA_OFF) != CONFIG_SCHEMA_MARKER) {
        schema_ok = false;
        c->timezone_offset_hours = DEFAULT_TIMEZONE_OFFSET_HOURS;
    } else {
        c->timezone_offset_hours = (int8_t)img[0x00F];
        if (c->timezone_offset_hours < -12 || c->timezone_offset_hours > 14) {
            c->timezone_offset_hours = DEFAULT_TIMEZONE_OFFSET_HOURS;
            schema_ok = false;
        }
    }

    // Validate the image's inner CRC
    const uint16_t crc_off = layout_version >= 5
        ? EEPROM_CRC_OFF
        : EEPROM_CRC_OFF_V4;
    if (ConfigStore::crc32(img, crc_off) != imgU32(img, crc_off)) return false;

    bool profile_fixed = fixupConfig(c);
    bool fan_bounds_fixed = false;
    if (c->min_fan_percent >= c->max_fan_percent ||
        fanStepWithinPercentBounds(0.0f, c->min_fan_percent, c->max_fan_percent) == 0) {
        const uint8_t old_floor = c->min_fan_percent;
        const uint8_t old_cap = c->max_fan_percent;
        uint8_t repaired_floor = 0;
        for (uint8_t step = 1; step <= 16; ++step) {
            const uint8_t mapped = fanPercentFromSpeed(step, 1, 16);
            if (mapped >= 40 && mapped < c->max_fan_percent && mapped > repaired_floor) {
                repaired_floor = mapped;
            }
        }
        if (repaired_floor == 0) {
            c->min_fan_percent = 40;
            c->max_fan_percent = 100;
        } else {
            c->min_fan_percent = repaired_floor;
        }
        fan_bounds_fixed = true;
        Serial.printlnf("  Repaired Auto bounds: %u-%u to %u-%u",
                        old_floor, old_cap, c->min_fan_percent, c->max_fan_percent);
    }
    bool hostname_fixed = false;
    if (layout_version < 6 || !mdnsBaseNameIsValid(c->mdns_base_name)) {
        memset(c->mdns_base_name, 0, sizeof(c->mdns_base_name));
        strncpy(c->mdns_base_name, "localair", sizeof(c->mdns_base_name) - 1);
        hostname_fixed = true;
    }
    bool schedule_fixed = false;
    if (layout_version < 7 || !scheduleConfigIsValid(c)) {
        resetScheduleDefaults(c);
        schedule_fixed = true;
    }
    bool static_ip_fixed = false;
    if (layout_version < 8 || !staticIpConfigIsValid(c)) {
        resetStaticIpDefaults(c);
        static_ip_fixed = true;
    }

    *needs_save = !schema_ok || profile_fixed || fan_bounds_fixed || hostname_fixed ||
                  schedule_fixed || static_ip_fixed;
    return true;
}

void ConfigStore::encodeImage(const Config* c, uint8_t* img) {
    memset(img, 0, CONFIG_IMAGE_LEN);
    imgPutU32(img, EEPROM_BASE, CONFIG_MAGIC);
    imgPutFloat(img, 0x004, c->room_m3);
    img[0x00F] = (uint8_t)c->timezone_offset_hours;
    imgPutFloat(img, 0x010, c->cadr_max);
    imgPutFloat(img, EEPROM_RESERVED_F32_OFF, 0.0f);
    imgPutFloat(img, 0x018, c->kalman_x0);
    imgPutFloat(img, 0x01C, c->kalman_x1);
    imgPutFloat(img, 0x020, c->kalman_p00);
    imgPutFloat(img, 0x024, c->baseline);
    // mqtt_broker_ip occupies 0x02C-0x02F (4 bytes).
    memcpy(img + 0x02C, &c->mqtt_broker_ip, 4);
    imgPutU16(img, 0x030, c->mqtt_port);
    img[0x032] = c->mqtt_enabled ? 1 : 0;
    img[0x033] = c->room_profile;
    memcpy(img + 0x034, c->mqtt_user, 32);
    memcpy(img + 0x054, c->mqtt_pass, 32);
    imgPutU32(img, EEPROM_SCHEMA_OFF, CONFIG_SCHEMA_MARKER);
    img[EEPROM_LAYOUT_OFF] = CONFIG_LAYOUT_VERSION;
    img[EEPROM_MAX_FAN_PERCENT_OFF] = c->max_fan_percent;
    img[EEPROM_MIN_FAN_PERCENT_OFF] = c->min_fan_percent;
    img[EEPROM_LED_ENABLED_OFF] = c->led_enabled ? 1 : 0;
    img[EEPROM_LED_CUSTOM_COLOR_ENABLED_OFF] = c->led_custom_color_enabled ? 1 : 0;
    img[EEPROM_LED_COLOR_OFF + 0] = c->led_color_r;
    img[EEPROM_LED_COLOR_OFF + 1] = c->led_color_g;
    img[EEPROM_LED_COLOR_OFF + 2] = c->led_color_b;
    imgPutU32(img, EEPROM_CRC_OFF, crc32(img, EEPROM_CRC_OFF));
    memcpy(img + EEPROM_MDNS_BASE_OFF, c->mdns_base_name, 24);
    img[EEPROM_SCHEDULE_OFF] = c->schedule_enabled ? 1 : 0;
    imgPutU16(img, EEPROM_SCHEDULE_OFF + 1, c->schedule_start_min);
    imgPutU16(img, EEPROM_SCHEDULE_OFF + 3, c->schedule_end_min);
    img[EEPROM_SCHEDULE_OFF + 5] = c->schedule_fan_percent;
    img[EEPROM_STATIC_IP_OFF] = c->static_ip_enabled ? 1 : 0;
    imgPutU32(img, EEPROM_STATIC_IP_OFF + 1, c->static_ip);
    imgPutU32(img, EEPROM_STATIC_IP_OFF + 5, c->static_netmask);
    imgPutU32(img, EEPROM_STATIC_IP_OFF + 9, c->static_gateway);
    imgPutU32(img, EEPROM_STATIC_IP_OFF + 13, c->static_dns);
}

namespace {
struct SlotProbe {
    bool     valid;
    bool     torn;   // slot magic present but record failed its CRC
    uint32_t seq;
};

SlotProbe probeSlot(uint16_t off, uint8_t* scratch) {
    EepromRecordMeta meta{};
    SlotProbe p{};
    p.valid = EepromRecord::read(off, CONFIG_SLOT_MAGIC, scratch, CONFIG_IMAGE_LEN, &meta) &&
              meta.len == CONFIG_IMAGE_LEN;
    p.torn = !p.valid && EepromRecord::readU32(off) == CONFIG_SLOT_MAGIC;
    p.seq = p.valid ? meta.seq : 0;
    return p;
}

bool estimatorIsSane(const PersistedEstimator& e) {
    if (!isfinite(e.kalman_x0) || !isfinite(e.kalman_x1) ||
        !isfinite(e.kalman_p00) || !isfinite(e.baseline)) return false;
    if (e.baseline < 1.0f || e.baseline > 5000.0f) return false;
    if (e.kalman_x0 < 0.0f || e.kalman_x0 > 5000.0f) return false;
    if (e.kalman_p00 <= 0.0f || e.kalman_p00 > 1e6f) return false;
    return true;
}
}  // namespace

bool ConfigStore::load(Config* c, ConfigLoadResult* result) {
    Lock lock;
    uint8_t img[CONFIG_IMAGE_LEN];
    const SlotProbe a = probeSlot(CONFIG_SLOT_A_OFF, img);
    const SlotProbe b = probeSlot(CONFIG_SLOT_B_OFF, img);

    // Newest valid slot first, then the other valid slot.
    uint16_t order[2];
    int count = 0;
    if (a.valid && b.valid) {
        order[count++] = a.seq >= b.seq ? CONFIG_SLOT_A_OFF : CONFIG_SLOT_B_OFF;
        order[count++] = a.seq >= b.seq ? CONFIG_SLOT_B_OFF : CONFIG_SLOT_A_OFF;
    } else if (a.valid) {
        order[count++] = CONFIG_SLOT_A_OFF;
    } else if (b.valid) {
        order[count++] = CONFIG_SLOT_B_OFF;
    }

    ConfigLoadResult r = CONFIG_LOAD_DEFAULTS;
    bool needs_save = false;
    bool loaded = false;
    for (int i = 0; i < count && !loaded; i++) {
        if (EepromRecord::read(order[i], CONFIG_SLOT_MAGIC, img, CONFIG_IMAGE_LEN, nullptr) &&
            decodeImage(img, c, &needs_save)) {
            loaded = true;
            r = (a.torn || b.torn || i > 0) ? CONFIG_LOAD_RECOVERED : CONFIG_LOAD_OK;
        }
    }
    if (!loaded) {
        for (uint16_t i = 0; i < CONFIG_IMAGE_LEN; i++) img[i] = EEPROM.read(EEPROM_BASE + i);
        if (decodeImage(img, c, &needs_save)) {
            loaded = true;
            r = CONFIG_LOAD_LEGACY;
            needs_save = true;  // migrate into slot A
        }
    }
    if (!loaded) {
        defaults(c);
        r = CONFIG_LOAD_DEFAULTS;
        needs_save = true;
    }

    PersistedEstimator est;
    if (loadEstimator(&est)) {
        c->kalman_x0  = est.kalman_x0;
        c->kalman_x1  = est.kalman_x1;
        c->kalman_p00 = est.kalman_p00;
        c->baseline   = est.baseline;
    }

    if (needs_save) save(c);
    if (result) *result = r;
    return r != CONFIG_LOAD_DEFAULTS;
}

void ConfigStore::save(const Config* c) {
    Lock lock;
    uint8_t img[CONFIG_IMAGE_LEN];
    const SlotProbe a = probeSlot(CONFIG_SLOT_A_OFF, img);
    const SlotProbe b = probeSlot(CONFIG_SLOT_B_OFF, img);

    // Overwrite the older or invalid slot so the newest valid one survives.
    uint16_t target = CONFIG_SLOT_A_OFF;
    uint32_t seq = 1;
    if (a.valid && b.valid) {
        target = a.seq <= b.seq ? CONFIG_SLOT_A_OFF : CONFIG_SLOT_B_OFF;
        seq = (a.seq > b.seq ? a.seq : b.seq) + 1;
    } else if (a.valid) {
        target = CONFIG_SLOT_B_OFF;
        seq = a.seq + 1;
    } else if (b.valid) {
        target = CONFIG_SLOT_A_OFF;
        seq = b.seq + 1;
    }

    encodeImage(c, img);
    EepromRecord::write(target, CONFIG_SLOT_MAGIC, CONFIG_SLOT_VERSION, seq, img, CONFIG_IMAGE_LEN);
}

bool ConfigStore::loadEstimator(PersistedEstimator* est) {
    Lock lock;
    uint8_t payload[sizeof(PersistedEstimator)];
    EepromRecordMeta meta{};
    if (!EepromRecord::read(EEPROM_ESTIMATOR_OFF, ESTIMATOR_MAGIC, payload, sizeof(payload), &meta)) {
        return false;
    }
    if (meta.version != ESTIMATOR_VERSION || meta.len != sizeof(payload)) return false;
    PersistedEstimator e;
    memcpy(&e, payload, sizeof(e));
    if (!estimatorIsSane(e)) return false;
    if (est) *est = e;
    return true;
}

void ConfigStore::saveEstimator(const PersistedEstimator* est) {
    if (!est) return;
    Lock lock;
    uint8_t payload[sizeof(PersistedEstimator)];
    memcpy(payload, est, sizeof(payload));
    // A constant seq keeps those bytes unchanged, so emulation skips them.
    EepromRecord::write(EEPROM_ESTIMATOR_OFF, ESTIMATOR_MAGIC, ESTIMATOR_VERSION, 0,
                        payload, sizeof(payload));
}

bool ConfigStore::loadControlState(PersistedControlState* state) {
    Lock lock;
    if (state) {
        state->mode = CONTROL_STATE_AUTO;
        state->manual_percent = 0;
        state->manual_speed = fanSpeedFromPercent(0, 1, 16);
    }

    const uint16_t off = EEPROM_CONTROL_STATE_OFF;
    if (EepromRecord::readU32(off) != CONTROL_STATE_MAGIC) return false;

    uint8_t buf[EEPROM_CONTROL_STATE_CRC_OFF - EEPROM_CONTROL_STATE_OFF];
    for (uint16_t i = 0; i < sizeof(buf); i++) buf[i] = EEPROM.read(off + i);
    const uint32_t stored_crc = EepromRecord::readU32(EEPROM_CONTROL_STATE_CRC_OFF);
    if (crc32(buf, sizeof(buf)) != stored_crc) return false;

    const uint8_t version = EEPROM.read(off + 4);
    const uint8_t mode = EEPROM.read(off + 5);
    const uint8_t percent = EEPROM.read(off + 6);
    const uint8_t speed = EEPROM.read(off + 7);
    if (version != CONTROL_STATE_VERSION) return false;
    if (mode != CONTROL_STATE_AUTO && mode != CONTROL_STATE_MANUAL) return false;
    if (percent > 100) return false;

    const uint8_t derived_speed = fanSpeedFromPercent(percent, 1, 16);
    if (mode == CONTROL_STATE_MANUAL && speed != derived_speed) return false;

    if (state) {
        state->mode = mode;
        state->manual_percent = mode == CONTROL_STATE_MANUAL ? percent : 0;
        state->manual_speed = mode == CONTROL_STATE_MANUAL ? derived_speed : fanSpeedFromPercent(0, 1, 16);
    }
    return true;
}

void ConfigStore::saveControlState(const PersistedControlState* state) {
    if (!state) return;
    Lock lock;

    uint8_t mode = state->mode == CONTROL_STATE_MANUAL ? CONTROL_STATE_MANUAL : CONTROL_STATE_AUTO;
    uint8_t percent = state->manual_percent > 100 ? 100 : state->manual_percent;
    if (mode == CONTROL_STATE_AUTO) percent = 0;
    const uint8_t speed = fanSpeedFromPercent(percent, 1, 16);

    const uint16_t off = EEPROM_CONTROL_STATE_OFF;
    EepromRecord::writeU32(off, CONTROL_STATE_MAGIC);
    EEPROM.write(off + 4, CONTROL_STATE_VERSION);
    EEPROM.write(off + 5, mode);
    EEPROM.write(off + 6, percent);
    EEPROM.write(off + 7, speed);
    for (uint16_t addr = off + 8; addr < EEPROM_CONTROL_STATE_CRC_OFF; addr++) {
        EEPROM.write(addr, 0);
    }

    uint8_t buf[EEPROM_CONTROL_STATE_CRC_OFF - EEPROM_CONTROL_STATE_OFF];
    for (uint16_t i = 0; i < sizeof(buf); i++) buf[i] = EEPROM.read(off + i);
    EepromRecord::writeU32(EEPROM_CONTROL_STATE_CRC_OFF, crc32(buf, sizeof(buf)));
}

uint32_t ConfigStore::crc32(const uint8_t* data, uint16_t len) {
    return EepromRecord::crc32(data, len);
}
