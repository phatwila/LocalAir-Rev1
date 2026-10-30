/*
 * LocalAir — Particle P1 Firmware
 * Local replacement firmware for the Aura Air purifier controller.
 *
 * Phase 2: algorithm on-chip with local HTTP REST + SSE + MQTT + mDNS.
 * Direct fan actuation now goes through PWM on P1S0; Serial1 remains passive
 * telemetry/status from the secondary PM2.5 board and other
 * ribbon-connected telemetry sources.
 *
 * Build: DEVICE_OS_DIR=./device-os ./bin/build-aura-os.sh
 * Flash: ./bin/flash-local.sh
 */

#include "application.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "algo/fan_speed.h"
#include "algo/kalman_voc.h"
#include "algo/adaptive_baseline.h"
#include "algo/ach_model.h"
#include "algo/humidity_comp.h"
#include "app/air_controller.h"
#include "app/auto_engine_v3.h"
#include "app/fan_command_router.h"
#include "app/command_supervisor.h"
#include "app/schedule_window.h"
#include "app/sense_burst.h"
#include "hal/daughter_board.h"
#include "hal/sensor_hub.h"
#include "hal/rpm_monitor.h"
#include "app/air_treatment.h"
#include "sys/config_store.h"
#include "sys/event_log.h"
#include "sys/boot_diag.h"
#include "generated/build_version.h"
#include "sys/watchdog.h"
#include "net/http_server.h"
#include "net/mdns_responder.h"
#include "net/mdns_service.h"
#include "net/mqtt_client.h"

SYSTEM_MODE(AUTOMATIC);   // Particle manages WiFi
SYSTEM_THREAD(ENABLED);   // application runs independently of system thread
// Record why the device reset. Device OS writes DCT only when the flag changes.
STARTUP(System.enableFeature(FEATURE_RESET_INFO));

// ── Globals ───────────────────────────────────────────────────────────────────

static Config          g_config;
static AirController   g_ctrl;
static AutoEngineV3    g_auto_v3;
static SensorHub       g_sensors;
static DaughterBoard   g_fan;
static EventLog        g_log;
static CommandSupervisor g_cmd;
static CommandInbox g_command_inbox;
static HttpServer*     g_http = nullptr;
static MdnsResponder   g_mdns;
static MqttClient*     g_mqtt = nullptr;
static bool            g_mqtt_last_enabled = false;
static uint32_t        g_mqtt_last_broker_ip = 0;
static char            g_mqtt_last_host[24] = "";
static uint16_t g_mqtt_last_port = 0;
static char g_mqtt_last_user[sizeof(g_config.mqtt_user)] = "";
static char g_mqtt_last_pass[sizeof(g_config.mqtt_pass)] = "";
static RpmMonitor      g_rpm;
static SenseBurst      g_burst;
static bool            g_startup_seen_sensor_cycle = false;
static uint32_t        g_startup_armed_ms = 0;
static uint32_t        g_startup_fan_on_ms = 0;
static uint32_t        g_startup_first_full_valid_ms = 0;
static uint32_t        g_startup_handoff_ms = 0;
static bool            g_startup_handoff_pending = false;
static uint8_t         g_startup_handoff_result = HttpServer::STARTUP_RESULT_NONE;
static PersistedControlState g_restore_control_state = {
    CONTROL_STATE_AUTO, 0, 16
};
static bool            g_restore_control_state_valid = false;
static bool            g_restore_control_state_applied = false;
static bool            g_force_control_tick = false;
static float           g_rpm_factor_smoothed = 1.0f;
static uint8_t         g_rpm_factor_samples = 0;
static int             g_rpm_factor_x1000_var = 1000;
static int             g_rpm_factor_samples_var = 0;
static int             g_duty_trim_var = 0;
static char            g_fw_version[] = LOCALAIR_FIRMWARE_VERSION;
static int             g_build_number_var = LOCALAIR_BUILD_NUMBER;
static char            g_local_ip[16] = "0.0.0.0";
static char            g_dashboard_url[48] = "http://0.0.0.0/";
static char            g_mdns_host[24] = "pending";
static char            g_mdns_url[40]  = "http://pending.local/";
static bool            g_mdns_started = false;
static bool            g_led_controlled = false;
static bool            g_led_is_on = false;
static AutoMode        g_led_mode = AutoMode::Idle;

// Shared fan state (written by main loop, read by rpm_monitor thread)
static volatile uint8_t  g_fan_speed    = 15;
static int               g_fan_speed_var = 15;
static int               g_fan_percent_var = 52;

// Hysteresis
static uint8_t   g_current_speed = 0xFF; // force first command
static uint32_t  g_last_cmd_ms   = 0;
static constexpr uint8_t  HYSTERESIS    = 2;
static constexpr uint32_t CMD_HOLDOFF   = 8000;

// History ring buffer for /api/history endpoint (200 entries)
struct HistoryEntry { uint32_t ts; float voc; float pm25; uint8_t speed; };
static HistoryEntry g_history[200];
static uint8_t      g_history_head  = 0;
static uint8_t      g_history_count = 0;

// ── Fan control helpers ───────────────────────────────────────────────────────

// Returns the last known speed, falling back to g_fan_speed before first command.
static inline uint8_t prevSpeed() {
    return (g_current_speed == 0xFF) ? g_fan_speed : g_current_speed;
}

static void syncFanMirrors(uint8_t speed) {
    g_fan_speed = speed;
    g_rpm.setTarget(speed);
    g_fan_speed_var = speed;
    g_fan_percent_var = fanPercentFromSpeed(speed, 1, 16);
}

static bool trustIsValid(AutoTrustLevel trust) {
    return trust == AutoTrustLevel::Valid || trust == AutoTrustLevel::Degraded;
}

static void syncAutoV3Telemetry(const AutoEngineV3::Result& v3) {
    if (!g_http) return;
    g_http->auto_v3.auto_owner = HttpServer::OWNER_AUTO;
    g_http->auto_v3.auto_mode = (uint8_t)v3.mode;
    g_http->auto_v3.dominant_lane = v3.dominant_lane;
    g_http->auto_v3.target_airflow_pct = (uint8_t)lroundf(v3.target_airflow_pct);
    g_http->auto_v3.applied_airflow_pct = (uint8_t)lroundf(v3.applied_airflow_pct);
    g_http->auto_v3.quantized_step = v3.quantized_step;
    g_http->auto_v3.sensor_voc_valid = trustIsValid(v3.trust.gas);
    g_http->auto_v3.sensor_eco2_valid = trustIsValid(v3.trust.eco2);
    g_http->auto_v3.sensor_pm_valid = trustIsValid(v3.trust.pm);
    g_http->auto_v3.sensor_rh_valid = trustIsValid(v3.trust.temp_rh);
    g_http->auto_v3.sensor_full_valid =
        trustIsValid(v3.trust.pm) && trustIsValid(v3.trust.gas) &&
        trustIsValid(v3.trust.eco2) && trustIsValid(v3.trust.temp_rh);
    strncpy(g_http->auto_v3.reason, v3.reason, sizeof(g_http->auto_v3.reason) - 1);
    g_http->auto_v3.reason[sizeof(g_http->auto_v3.reason) - 1] = '\0';
}

static void syncRpmFactorTelemetry() {
    g_rpm_factor_x1000_var = (int)lroundf(g_ctrl.ach().rpmFactor() * 1000.0f);
    g_rpm_factor_samples_var = (int)g_rpm_factor_samples;
}

enum class LedEffect : uint8_t {
    Off = 0,
    Flow,
    Blink,
    Breath,
    Rainbow,
};

enum class LedStatus : uint8_t {
    Off = 0,
    Startup,
    NotReady,
    ManualOverride,
    FaultSafe,
    Idle,
    Maintain,
    Recover,
    ElevatedGas,
    PurgeGas,
    ElevatedPM,
    PurgePM,
};

struct LedPattern {
    LedEffect effect;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint32_t period_ms;
    uint32_t on_ms;
    uint8_t min_level;
    uint8_t max_level;
};

static inline uint8_t scale8(uint8_t value, uint8_t scale) {
    return (uint8_t)(((uint16_t)value * (uint16_t)scale) / 255U);
}

static inline float clampUnit(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static inline uint8_t lerp8(uint8_t from, uint8_t to, float t) {
    const float clamped_t = clampUnit(t);
    const float delta = (float)to - (float)from;
    return (uint8_t)lroundf((float)from + (delta * clamped_t));
}

static void computeFlowColor(uint32_t now,
                             uint8_t applied_fan_percent,
                             uint8_t base_red,
                             uint8_t base_green,
                             uint8_t base_blue,
                             uint8_t min_level,
                             uint8_t max_level,
                             uint8_t* red,
                             uint8_t* green,
                             uint8_t* blue) {
    if (!red || !green || !blue) return;

    float fan_norm = (float)applied_fan_percent / 100.0f;
    if (fan_norm < 0.0f) fan_norm = 0.0f;
    if (fan_norm > 1.0f) fan_norm = 1.0f;

    const float two_pi = 6.28318530718f;
    const float t_seconds = (float)now * 0.001f;
    const float base_period_ms = 5600.0f - (fan_norm * 3600.0f);
    const float base_phase = fmodf((float)now, base_period_ms) / base_period_ms;
    const float primary_wave = 0.5f + (0.5f * sinf((base_phase * two_pi) - 1.57079632679f));

    const float drift_rate_hz = 0.07f + (0.33f * fan_norm);
    const float drift_inner = sinf((t_seconds * 0.37f) + 0.8f);
    const float drift = 0.12f * sinf((t_seconds * drift_rate_hz * two_pi) + (1.3f * drift_inner));
    const float blend = clampUnit(primary_wave + drift);

    const uint8_t highlight_r = lerp8(base_red, 255, 0.30f);
    const uint8_t highlight_g = lerp8(base_green, 255, 0.30f);
    const uint8_t highlight_b = lerp8(base_blue, 255, 0.30f);

    const uint8_t blended_r = lerp8(base_red, highlight_r, blend);
    const uint8_t blended_g = lerp8(base_green, highlight_g, blend);
    const uint8_t blended_b = lerp8(base_blue, highlight_b, blend);

    uint8_t local_min = min_level;
    uint8_t local_max = max_level;
    if (local_max < local_min) {
        const uint8_t temp = local_min;
        local_min = local_max;
        local_max = temp;
    }
    if (local_min < 24U) local_min = 24U;
    if (local_max > 230U) local_max = 230U;
    if (local_max < local_min) local_max = local_min;

    const float brightness_wave = 0.75f + (0.25f * (0.5f + 0.5f * sinf((t_seconds * 0.85f) + blend)));
    const uint8_t level = (uint8_t)lroundf((float)local_min +
                                            ((float)(local_max - local_min) * brightness_wave));

    *red = scale8(blended_r, level);
    *green = scale8(blended_g, level);
    *blue = scale8(blended_b, level);
}

static void rainbowColor(uint32_t now, uint32_t period_ms,
                         uint8_t* red, uint8_t* green, uint8_t* blue) {
    if (!red || !green || !blue) return;
    if (period_ms == 0) period_ms = 2400;
    const uint32_t phase = now % period_ms;
    const uint32_t wheel = (phase * 1536UL) / period_ms;
    const uint8_t step = (uint8_t)(wheel & 0xFF);
    switch (wheel >> 8) {
        case 0: *red = 255; *green = step; *blue = 0; break;
        case 1: *red = (uint8_t)(255 - step); *green = 255; *blue = 0; break;
        case 2: *red = 0; *green = 255; *blue = step; break;
        case 3: *red = 0; *green = (uint8_t)(255 - step); *blue = 255; break;
        case 4: *red = step; *green = 0; *blue = 255; break;
        default: *red = 255; *green = 0; *blue = (uint8_t)(255 - step); break;
    }
}

static LedStatus ledStatusFor(AutoMode mode,
                              bool manual_override,
                              bool startup_active,
                              bool fan_ready,
                              bool actuation_ready) {
    if (startup_active) return LedStatus::Startup;
    if (!fan_ready || !actuation_ready) return LedStatus::NotReady;
    if (manual_override) return LedStatus::ManualOverride;

    switch (mode) {
        case AutoMode::FaultSafe:
            return LedStatus::FaultSafe;
        case AutoMode::PurgeGas:
            return LedStatus::PurgeGas;
        case AutoMode::ElevatedGas:
            return LedStatus::ElevatedGas;
        case AutoMode::PurgePM:
            return LedStatus::PurgePM;
        case AutoMode::ElevatedPM:
            return LedStatus::ElevatedPM;
        case AutoMode::Recover:
            return LedStatus::Recover;
        case AutoMode::Maintain:
            return LedStatus::Maintain;
        case AutoMode::Idle:
        default:
            return LedStatus::Idle;
    }
}

static LedPattern ledPatternFor(LedStatus status, bool led_enabled) {
    if (!led_enabled || status == LedStatus::Off) return {LedEffect::Off, 0, 0, 0, 0, 0, 0, 0};

    switch (status) {
        case LedStatus::Startup:
            return {LedEffect::Rainbow, 0, 0, 0, 2400, 0, 0, 255};
        case LedStatus::NotReady:
            return {LedEffect::Blink, 255, 32, 32, 420, 170, 0, 255};
        case LedStatus::FaultSafe:
            return {LedEffect::Blink, 255, 144, 0, 640, 220, 0, 255};
        case LedStatus::ManualOverride:
            return {LedEffect::Flow, 216, 72, 255, 0, 0, 40, 210};
        case LedStatus::Recover:
            return {LedEffect::Flow, 88, 242, 232, 0, 0, 34, 220};
        case LedStatus::Maintain:
            return {LedEffect::Flow, 176, 236, 255, 0, 0, 34, 208};
        case LedStatus::Idle:
            return {LedEffect::Flow, 238, 250, 255, 0, 0, 28, 196};
        case LedStatus::ElevatedGas:
            return {LedEffect::Flow, 255, 168, 58, 0, 0, 36, 238};
        case LedStatus::PurgeGas:
            return {LedEffect::Flow, 255, 118, 22, 0, 0, 46, 248};
        case LedStatus::ElevatedPM:
            return {LedEffect::Flow, 76, 188, 255, 0, 0, 36, 238};
        case LedStatus::PurgePM:
            return {LedEffect::Flow, 32, 132, 255, 0, 0, 46, 248};
        case LedStatus::Off:
        default:
            return {LedEffect::Off, 0, 0, 0, 0, 0, 0, 0};
    }
}

static void serviceStatusLed(uint32_t now,
                             AutoMode mode,
                             bool manual_override,
                             bool startup_active,
                             bool fan_ready,
                             bool actuation_ready,
                             uint8_t applied_fan_percent) {
    bool force_update = false;
    if (!g_led_controlled) {
        RGB.control(true);
        g_led_controlled = true;
        g_led_is_on = false;
        force_update = true;
    }

    const LedStatus status = ledStatusFor(mode, manual_override, startup_active,
                                          fan_ready, actuation_ready);
    const LedPattern pattern = ledPatternFor(status, g_config.led_enabled);
    const bool use_custom_color =
        g_config.led_enabled &&
        g_config.led_custom_color_enabled &&
        pattern.effect != LedEffect::Off &&
        status != LedStatus::Startup &&
        status != LedStatus::NotReady &&
        status != LedStatus::FaultSafe &&
        fan_ready &&
        actuation_ready;
    uint8_t source_red = pattern.red;
    uint8_t source_green = pattern.green;
    uint8_t source_blue = pattern.blue;
    if (use_custom_color) {
        source_red = g_config.led_color_r;
        source_green = g_config.led_color_g;
        source_blue = g_config.led_color_b;
    }
    uint8_t red = source_red;
    uint8_t green = source_green;
    uint8_t blue = source_blue;
    bool should_on = true;

    switch (pattern.effect) {
        case LedEffect::Off:
            red = green = blue = 0;
            should_on = false;
            break;
        case LedEffect::Blink:
            should_on = pattern.period_ms == 0 || ((now % pattern.period_ms) < pattern.on_ms);
            if (!should_on) {
                red = green = blue = 0;
            }
            break;
        case LedEffect::Flow:
            computeFlowColor(now, applied_fan_percent,
                             source_red, source_green, source_blue,
                             pattern.min_level, pattern.max_level,
                             &red, &green, &blue);
            break;
        case LedEffect::Breath: {
            const uint32_t cycle = pattern.period_ms == 0 ? 2400 : pattern.period_ms;
            const float phase = (float)(now % cycle) / (float)cycle;
            const float breath = 0.5f + 0.5f * sinf(phase * 6.28318530718f - 1.57079632679f);
            const float min_scale = (float)pattern.min_level / 255.0f;
            const float max_scale = (float)pattern.max_level / 255.0f;
            const float scale = min_scale + ((max_scale - min_scale) * breath);
            red = (uint8_t)lroundf((float)source_red * scale);
            green = (uint8_t)lroundf((float)source_green * scale);
            blue = (uint8_t)lroundf((float)source_blue * scale);
            break;
        }
        case LedEffect::Rainbow:
            red = 255;
            green = 0;
            blue = 0;
            rainbowColor(now, pattern.period_ms, &red, &green, &blue);
            red = scale8(red, pattern.max_level);
            green = scale8(green, pattern.max_level);
            blue = scale8(blue, pattern.max_level);
            break;
    }

    if (force_update || should_on != g_led_is_on || red != 0 || green != 0 || blue != 0) {
        RGB.color(red, green, blue);
        g_led_is_on = should_on;
    }
}

static bool parseStrictInt(const String& args, int* out) {
    if (!out) return false;
    const char* s = args.c_str();
    if (!s || *s == '\0') return false;
    char* end = nullptr;
    long value = strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    if (value < INT32_MIN || value > INT32_MAX) return false;
    *out = (int)value;
    return true;
}

static bool doSetFan(uint8_t speed) {
    if (!g_fan.setFanSpeed(speed)) {
        return false;
    }
    syncFanMirrors(speed);
    return true;
}

static bool writeFanSpeed(uint8_t speed, void*) { return doSetFan(speed); }

static FanApplicationResult applyFanTarget(uint8_t target, bool needs_send, uint32_t now,
                                           uint8_t source = HttpServer::SRC_UNKNOWN,
                                           uint8_t owner = HttpServer::OWNER_AUTO, bool bind_command = true) {
    const CommandSupervisor::ApplyToken token = g_cmd.applicationToken(target, source, owner, bind_command);
    return applySupervisedFan(g_cmd, token, needs_send, now, writeFanSpeed, nullptr);
}

static RpmSample doGetRpm() {
    return g_fan.readRpmSample();
}

static void updateNetworkInfo() {
    IPAddress ip = WiFi.localIP();
    snprintf(g_local_ip, sizeof(g_local_ip), "%u.%u.%u.%u",
             (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
    if (g_mdns_started && g_mdns.hostname() && g_mdns.hostname()[0]) {
        strncpy(g_mdns_host, g_mdns.hostname(), sizeof(g_mdns_host) - 1);
        g_mdns_host[sizeof(g_mdns_host) - 1] = '\0';
        snprintf(g_mdns_url,      sizeof(g_mdns_url),      "http://%s.local/", g_mdns_host);
        strncpy(g_dashboard_url, g_mdns_url, sizeof(g_dashboard_url) - 1);
        g_dashboard_url[sizeof(g_dashboard_url) - 1] = '\0';
    } else {
        strncpy(g_mdns_host, "pending", sizeof(g_mdns_host) - 1);
        strncpy(g_mdns_url,  "http://pending.local/", sizeof(g_mdns_url) - 1);
        snprintf(g_dashboard_url, sizeof(g_dashboard_url), "http://%s/", g_local_ip);
    }
}

static void applyAirTreatmentState() {
    applyAirTreatmentOutputs();
}

static void syncCommandMirrors() {
    if (!g_http) return;
    g_http->override_active = g_cmd.manualOverrideDesired();
    g_http->override_speed = g_cmd.manualOverrideDesired() ? g_cmd.desiredSpeed() : g_cmd.effectiveSpeed();
    g_http->override_source = g_cmd.desiredSource();
    g_http->command_owner = g_cmd.effectiveOwner();
    if (g_cmd.effectiveSource() != HttpServer::SRC_UNKNOWN) {
        g_http->last_command_source = g_cmd.effectiveSource();
    }
    CommandSupervisor::StatusSnapshot snapshot{};
    g_cmd.snapshot(millis(), &snapshot);
    g_http->publishCommandState(snapshot, g_fan_speed);
}

static int enqueueCloudFanSpeed(uint8_t speed, uint8_t intent_percent = 255) {
    const uint8_t percent = intent_percent <= 100
        ? intent_percent : fanPercentFromSpeed(speed, 1, 16);
    return g_command_inbox.enqueue({CommandInbox::MANUAL, HttpServer::SRC_CLOUD, speed, percent},
                                   millis(), false) ? 1 : -1;
}

// The application loop alone accepts queued external commands. Cloud callbacks
// can reenter during delay(), but only enqueue immutable requests.
static void drainControlRequests() {
    for (uint8_t count = 0; count < CommandInbox::CAPACITY; ++count) {
        CommandInbox::Request request{};
        CommandInbox::Ticket ticket{};
        if (!g_command_inbox.claim(millis(), &request, &ticket)) break;
        const bool manual_before = g_cmd.manualOverrideDesired();
        const uint8_t previous = prevSpeed();
        const bool manual = request.kind == CommandInbox::MANUAL;
        const CommandSupervisor::CommandRecord command = acceptControlRequest(g_cmd, request, millis());
        g_burst.cancel();
        g_startup_handoff_pending = false;
        g_startup_handoff_result = manual ? HttpServer::STARTUP_RESULT_MANUAL : HttpServer::STARTUP_RESULT_NONE;
        g_restore_control_state_applied = true;
        if (g_http) {
            g_http->override_percent = manual ? request.percent : 0;
            g_http->startup_burst_active = false;
            g_http->startup_state_code = manual
                ? HttpServer::STARTUP_STATE_MANUAL_OVERRIDE : HttpServer::STARTUP_STATE_AUTO_ACTIVE;
            g_http->startup_result_code = g_startup_handoff_result;
            g_http->last_command_source = request.source;
            g_http->last_command_ts = Time.now();
            g_http->recordCommandAudit(command, previous, manual_before, manual, false,
                                       manual ? "manual accepted" : "auto accepted");
        }
        g_force_control_tick = true;
        syncCommandMirrors();
        g_command_inbox.complete(ticket, command);
    }
}

static bool restoreManualControlAfterStartup(uint32_t now, uint8_t prev,
                                             uint8_t startup_result) {
    if (g_restore_control_state_applied) return false;
    g_restore_control_state_applied = true;
    if (!g_restore_control_state_valid ||
        g_restore_control_state.mode != CONTROL_STATE_MANUAL) {
        return false;
    }

    uint8_t percent = g_restore_control_state.manual_percent;
    if (percent > 100) percent = 100;
    const uint8_t speed = fanSpeedFromPercent(percent, 1, 16);
    const CommandSupervisor::CommandRecord cmd =
        g_cmd.submitManualFan(HttpServer::SRC_RESTORE, HttpServer::OWNER_MANUAL, speed, now);
    g_startup_handoff_pending = false;
    g_startup_handoff_result = HttpServer::STARTUP_RESULT_MANUAL;
    g_force_control_tick = true;
    syncCommandMirrors();
    if (g_http) {
        g_http->override_speed = speed;
        g_http->override_percent = percent;
        g_http->override_active = true;
        g_http->override_source = HttpServer::SRC_RESTORE;
        g_http->startup_burst_active = false;
        g_http->startup_state_code = HttpServer::STARTUP_STATE_MANUAL_OVERRIDE;
        g_http->startup_result_code = startup_result;
        g_http->command_owner = HttpServer::OWNER_MANUAL;
        g_http->last_command_source = HttpServer::SRC_RESTORE;
        g_http->last_command_ts = Time.now();
        g_http->recordCommandAudit(cmd, prev, false, true, false, "restore manual");
    }
    return true;
}

static void serviceMdns() {
    static MdnsServiceState state;
    const bool changed = mdnsServiceTick(state, g_mdns, g_mdns_started,
                                         WiFi.ready(), g_config.mdns_base_name);
    if (changed || (g_mdns_started && strcmp(g_mdns_host, g_mdns.hostname()) != 0)) {
        updateNetworkInfo();
        if (g_mdns_started) {
            Serial.printlnf("  mDNS hostname: %s.local", g_mdns.hostname());
        } else {
            Serial.println("  mDNS down, retrying");
        }
    }
}

static void applyStaticIpConfig(bool enabled) {
    if (enabled) {
        const uint32_t ip   = g_config.static_ip;
        const uint32_t mask = g_config.static_netmask;
        const uint32_t gw   = g_config.static_gateway;
        const uint32_t dns  = g_config.static_dns;
        if (ip == 0 || mask == 0) return;
        IPAddress ip_addr((ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        IPAddress mask_addr((mask >> 24) & 0xFF, (mask >> 16) & 0xFF, (mask >> 8) & 0xFF, mask & 0xFF);
        IPAddress gw_addr((gw >> 24) & 0xFF, (gw >> 16) & 0xFF, (gw >> 8) & 0xFF, gw & 0xFF);
        IPAddress dns_addr((dns >> 24) & 0xFF, (dns >> 16) & 0xFF, (dns >> 8) & 0xFF, dns & 0xFF);
        WiFi.setStaticIP(ip_addr, mask_addr, gw_addr, dns_addr);
        WiFi.useStaticIP();
        Serial.printlnf("  static IP pinned: %u.%u.%u.%u",
                        (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
                        (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF));
    } else {
        WiFi.useDynamicIP();
        Serial.println("  static IP unpinned: DHCP on next connect");
    }
}

// Device OS keeps its own static-IP copy. After a config recovery the two
// can disagree, so re-apply the saved pin once if the live address differs.
// A disabled pin is left alone so a lost config cannot unpin a working IP.
static void reconcileStaticIpOnce() {
    static bool s_done = false;
    if (s_done || !WiFi.ready()) return;
    s_done = true;
    if (!g_config.static_ip_enabled || g_config.static_ip == 0) return;
    const IPAddress ip = WiFi.localIP();
    const uint32_t live = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                          ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
    if (live == g_config.static_ip) return;
    Serial.println("  static IP differs from saved pin: re-applying");
    applyStaticIpConfig(true);
}

static bool restartMdnsHostname(const char* base_name) {
    if (!base_name || !base_name[0] || !WiFi.ready()) return false;
    bool ok = false;
    if (g_mdns_started) {
        ok = g_mdns.restart(base_name);
    } else {
        ok = g_mdns.begin(base_name);
        g_mdns_started = ok;
    }
    if (ok) {
        updateNetworkInfo();
        if (g_mqtt && g_config.mqtt_enabled) {
            g_mqtt->begin(g_mdns_host);
            strncpy(g_mqtt_last_host, g_mdns_host, sizeof(g_mqtt_last_host) - 1);
            g_mqtt_last_host[sizeof(g_mqtt_last_host) - 1] = '\0';
            g_mqtt_last_enabled = true;
            g_mqtt_last_broker_ip = g_config.mqtt_broker_ip;
        g_mqtt_last_port = g_config.mqtt_port;
        memcpy(g_mqtt_last_user, g_config.mqtt_user, sizeof(g_mqtt_last_user));
        memcpy(g_mqtt_last_pass, g_config.mqtt_pass, sizeof(g_mqtt_last_pass));
        }
        Serial.printlnf("  mDNS hostname: %s.local", g_mdns.hostname());
    }
    return ok;
}

static void syncActuationStatus() {
    if (!g_http) return;
    g_http->actuation_backend = g_fan.backendName();
    g_http->module_readiness = g_fan.readinessStateName();
    g_http->module_present_assumed = g_fan.modulePresentAssumed();
    g_http->actuation_ready = g_fan.isActuationReady();
    g_http->actuation_proven = g_fan.isActuationProven();
    g_http->tach_reader_ready = g_fan.tachReaderReady();
    g_http->tach_pulse_count = g_fan.tachPulseCount();
    g_http->last_uart_tx_len = g_fan.lastTxLen();
    g_http->last_uart_rx_len = g_fan.lastRxLen();
    g_http->last_uart_tx_ms = g_fan.lastTxMs();
    g_http->pm_telemetry_available = g_sensors.pmReady();
    g_http->pm_bytes_seen = g_sensors.pmBytesSeen();
    g_http->pm_valid_frames = g_sensors.pmValidFrames();
    g_http->pm_checksum_failures = g_sensors.pmChecksumFailures();
    g_http->pm_last_valid_ms = g_sensors.pmLastValidMs();
    g_http->pm_raw_count = g_sensors.pmRawCount();
    g_sensors.pmRawHex(g_http->pm_raw_hex, sizeof(g_http->pm_raw_hex));
    // Sensor freshness telemetry
    const uint32_t now_snap = millis();
    g_http->voc_fresh = !g_sensors.sgp30InWarmup(now_snap) &&
        g_sensors.sgp30AgeMs() <= SensorHub::SGP30_FRESH_WINDOW_MS;
    g_http->pm_fresh = g_sensors.pmAgeMs() <= SensorHub::PM_FRESH_WINDOW_MS;
    g_http->voc_last_valid_ms = g_sensors.sgp30LastValidMs();
    g_http->humidity_last_valid_ms = g_sensors.humidityLastValidMs();
    g_http->sensor_snapshot_age_ms = g_sensors.sgp30Generation() == 0 ? 0 : g_sensors.sgp30AgeMs();
    g_http->pm_snapshot_age_ms = g_sensors.pmGeneration() == 0 ? 0 : g_sensors.pmAgeMs();
    g_http->humidity_snapshot_age_ms = g_sensors.humidityGeneration() == 0 ? 0 : g_sensors.humidityAgeMs();
}

// ── Particle cloud function handlers ─────────────────────────────────────────

int cloudSetFanSpeed(String args) {
    int spd = 0;
    if (!parseStrictInt(args, &spd)) return -1;
    if (spd < 0 || spd > 32) return -1;
    return enqueueCloudFanSpeed((uint8_t)spd);
}

int cloudSetFanPercent(String args) {
    int pct = 0;
    if (!parseStrictInt(args, &pct)) return -1;
    if (pct < 0 || pct > 100) return -1;
    const uint8_t speed = fanSpeedFromPercent((uint8_t)pct, 1, 16);
    return enqueueCloudFanSpeed(speed, (uint8_t)pct);
}

int cloudFanAuto(String) {
    return g_command_inbox.enqueue({CommandInbox::AUTO, HttpServer::SRC_CLOUD, 0, 0},
                                   millis(), false) ? 1 : -1;
}

// ── RPM thread ────────────────────────────────────────────────────────────────

static Thread* g_rpm_thread = nullptr;

static void rpmThreadFn(void*) {
    g_rpm.run(doGetRpm);
}

// ── HTTP thread ───────────────────────────────────────────────────────────────

static Thread* g_http_thread = nullptr;

static void httpThreadFn(void*) {
    if (g_http) g_http->run();
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  LocalAir starting");
    Serial.println("========================================");

    // Load config. The store falls back slot -> legacy -> defaults and
    // persists defaults only when nothing stored was usable.
    ConfigLoadResult config_load = CONFIG_LOAD_DEFAULTS;
    ConfigStore::load(&g_config, &config_load);
    BootDiag::capture(config_load);
    Serial.printlnf("  Config: %s  reset reason: %d", configLoadResultName(config_load),
                    BootDiag::resetReason());
    g_restore_control_state_valid = ConfigStore::loadControlState(&g_restore_control_state);
    if (!g_restore_control_state_valid) {
        g_restore_control_state.mode = CONTROL_STATE_AUTO;
        g_restore_control_state.manual_percent = 0;
        g_restore_control_state.manual_speed = fanSpeedFromPercent(0, 1, 16);
    }
    g_restore_control_state_applied = false;

    Time.zone(g_config.timezone_offset_hours);

    // Apply config to controller
    g_ctrl.configure(g_config.room_m3, g_config.room_profile,
                     g_config.max_fan_percent,
                     g_config.min_fan_percent);

    // Restore Kalman hot-start state. Baseline history warms up after boot.
    uint8_t kb[12];
    memcpy(kb,     &g_config.kalman_x0,  4);
    memcpy(kb + 4, &g_config.kalman_x1,  4);
    memcpy(kb + 8, &g_config.kalman_p00, 4); // restore covariance for hot-start
    g_ctrl.kalman().deserialize(kb);

    // Init sensors
    if (!g_sensors.begin()) {
        Serial.println("  WARN: SGP30 init failed — check I2C wiring");
    }
    g_fan.begin();
    g_cmd.setEffectiveBaseline(g_fan_speed, HttpServer::OWNER_AUTO, HttpServer::SRC_STARTUP_IDLE);

    // Register Particle cloud function
    Particle.function("setFanSpeed", cloudSetFanSpeed);
    Particle.function("setFanPercent", cloudSetFanPercent);
    Particle.function("fanAuto", cloudFanAuto);
    Particle.variable("fanSpeed",    g_fan_speed_var);
    Particle.variable("fanPercent",  g_fan_percent_var);
    Particle.variable("rpmFactX1000", g_rpm_factor_x1000_var);
    Particle.variable("rpmSamps", g_rpm_factor_samples_var);
    Particle.variable("dutyTrim", g_duty_trim_var);
    Particle.variable("FW_VER",      g_fw_version);
    Particle.variable("BUILD_NUM",   g_build_number_var);
    Particle.variable("localIP",     g_local_ip);
    Particle.variable("mdnsHost",    g_mdns_host);
    Particle.variable("mdnsURL",     g_mdns_url);
    Particle.variable("dashboardURL", g_dashboard_url);

    // Startup: immediately spin at the fastest startup target and hold until
    // the first full sensor report or a timeout hands control to auto.
    g_startup_armed_ms = millis();
    g_startup_fan_on_ms = 0;
    g_startup_seen_sensor_cycle = false;
    g_startup_first_full_valid_ms = 0;
    g_startup_handoff_ms = 0;
    g_startup_handoff_pending = false;
    g_startup_handoff_result = HttpServer::STARTUP_RESULT_NONE;
    g_force_control_tick = false;
    g_cmd.submitStartupSpin(HttpServer::SRC_STARTUP_BURST, HttpServer::OWNER_STARTUP_BURST,
                            SenseBurst::STARTUP_SPEED,
                            millis() + SenseBurst::STARTUP_TIMEOUT_MS, millis());
    // The PWM is already firing at STARTUP_SPEED from daughter_board.begin().
    // Start the burst timer unconditionally so the 20s sniff window runs even
    // while _actuation_ready is still false during the 15s boot-settle period.
    {
        const uint32_t fan_on_ms = millis();
        g_startup_fan_on_ms = fan_on_ms;
        g_burst.start(fan_on_ms);
    }
    bool startup_spin_sent = false;
    if (g_fan.isActuationReady()) {
        startup_spin_sent = applyFanTarget(SenseBurst::STARTUP_SPEED, true, millis()).succeeded;
    }
    g_current_speed = SenseBurst::STARTUP_SPEED;
    g_last_cmd_ms = millis();
    syncFanMirrors(g_current_speed);
    {
        char boot_msg[20];
        BootDiag::formatEventMsg(boot_msg, sizeof(boot_msg));
        g_log.push(AuraEventType::REBOOT, SenseBurst::STARTUP_SPEED, 0, 0, 0, boot_msg);
    }
    g_log.push(AuraEventType::SENSE_BURST, SenseBurst::STARTUP_SPEED, 0, 0, 0, "startup spin");
    applyAirTreatmentState();
    serviceStatusLed(millis(), g_led_mode, false, true,
                     g_fan.isModuleReady(), g_fan.isActuationReady(),
                     (uint8_t)g_fan_percent_var);

    // MQTT
    g_mqtt = new MqttClient(&g_config, &g_command_inbox);
    serviceMdns();
    if (g_config.mqtt_enabled && strcmp(g_mdns_host, "pending") != 0) {
        g_mqtt->begin(g_mdns_host);
        strncpy(g_mqtt_last_host, g_mdns_host, sizeof(g_mqtt_last_host) - 1);
        g_mqtt_last_host[sizeof(g_mqtt_last_host) - 1] = '\0';
        g_mqtt_last_enabled = true;
        g_mqtt_last_broker_ip = g_config.mqtt_broker_ip;
        g_mqtt_last_port = g_config.mqtt_port;
        memcpy(g_mqtt_last_user, g_config.mqtt_user, sizeof(g_mqtt_last_user));
        memcpy(g_mqtt_last_pass, g_config.mqtt_pass, sizeof(g_mqtt_last_pass));
    }

    // HTTP server
    g_http = new HttpServer(&g_ctrl, &g_log, &g_config, &g_fan, &g_command_inbox, g_mqtt);
    g_http->setRpmMonitor(&g_rpm);
    g_http->override_active = false;
    g_http->override_speed = g_current_speed;
    g_http->startup_burst_active = true;
    g_http->startup_state_code = HttpServer::STARTUP_STATE_BOOT_SPIN;
    g_http->startup_result_code = HttpServer::STARTUP_RESULT_NONE;
    g_http->startup_armed_ms = g_startup_armed_ms;
    g_http->startup_fan_on_ms = g_startup_fan_on_ms;
    g_http->startup_started_ms = g_startup_armed_ms;
    g_http->startup_first_full_valid_ms = 0;
    g_http->startup_handoff_ms = 0;
    g_http->startup_timeout_ms = SenseBurst::STARTUP_TIMEOUT_MS;
    g_http->startup_sensor_sgp30_valid = false;
    g_http->startup_sensor_sht31_valid = false;
    g_http->startup_sensor_pm_valid = false;
    g_http->startup_sensor_full_valid = false;
    g_http->mdns_host = g_mdns_host;
    g_http->local_ip = g_local_ip;
    g_http->setHostnameChangeCallback(restartMdnsHostname);
    g_http->setStaticIpChangeCallback(applyStaticIpConfig);
    g_http->command_owner = HttpServer::OWNER_STARTUP_BURST;
    g_http->last_command_source = HttpServer::SRC_STARTUP_BURST;
    g_http->last_command_ts = Time.now();
    // Wire history ring buffer so /api/history returns real data
    g_http->history_buf   = reinterpret_cast<HttpServer::HistEntry*>(g_history);
    g_http->history_head  = &g_history_head;
    g_http->history_count = &g_history_count;
    syncActuationStatus();
    g_http->recordCommandAudit(SenseBurst::STARTUP_SPEED, SenseBurst::STARTUP_SPEED,
                               HttpServer::SRC_STARTUP_BURST, HttpServer::OWNER_STARTUP_BURST,
                               false, false, startup_spin_sent, startup_spin_sent ? "boot spin" : "boot spin pending");
    {
        CommandSupervisor::CommandRecord startup_cmd = g_cmd.lastCommand();
        if (startup_cmd.command_id != 0) {
            g_http->recordCommandAudit(startup_cmd, SenseBurst::STARTUP_SPEED,
                                       false, false, startup_spin_sent, "startup spin");
        }
    }
    g_http_thread = new Thread("http", httpThreadFn, nullptr, OS_THREAD_PRIORITY_DEFAULT, 4096);
    syncRpmFactorTelemetry();

    // RPM feedback thread
    g_rpm_thread = new Thread("rpm", rpmThreadFn, nullptr, OS_THREAD_PRIORITY_DEFAULT, 2048);

    // Hardware watchdog
    Watchdog::begin();

    updateNetworkInfo();
    syncCommandMirrors();

    Serial.printlnf("  Room: %.0f m³  preset: %s  floor: spd%d",
                    g_config.room_m3, roomProfileKey(g_config.room_profile),
                    g_ctrl.ach().speedFloor());
    Serial.println("  Ready.\n");
}

// ── Main loop ─────────────────────────────────────────────────────────────────

static uint32_t g_last_sense_ms        = 0;
static uint32_t g_last_fast_control_ms = 0;
static uint32_t g_last_save_ms         = 0;
static uint32_t g_last_mqtt_ms         = 0;
// Slow estimator lane: Kalman, baseline, room decay, room model.
static constexpr uint32_t SENSE_INTERVAL         = 20000; // 20s
// Fast control lane: target recompute using latest sensor snapshot.
static constexpr uint32_t FAST_CONTROL_INTERVAL  = 1000;  // 1s
static constexpr uint32_t SAVE_INTERVAL          = 300000; // 5 min
static constexpr uint32_t MQTT_INTERVAL          = 20000;

struct CommandPublication {
    ~CommandPublication() { syncCommandMirrors(); }
};

static void serviceMqttTransport() {
    if (g_mqtt) {
        if (g_config.mqtt_enabled) {
            if (strcmp(g_mdns_host, "pending") != 0 &&
                (!g_mqtt_last_enabled ||
                 g_mqtt_last_broker_ip != g_config.mqtt_broker_ip ||
                 g_mqtt_last_port != g_config.mqtt_port ||
                 strcmp(g_mqtt_last_user, g_config.mqtt_user) != 0 ||
                 strcmp(g_mqtt_last_pass, g_config.mqtt_pass) != 0 ||
                 strcmp(g_mqtt_last_host, g_mdns_host) != 0)) {
                g_mqtt->begin(g_mdns_host);
                strncpy(g_mqtt_last_host, g_mdns_host, sizeof(g_mqtt_last_host) - 1);
                g_mqtt_last_host[sizeof(g_mqtt_last_host) - 1] = '\0';
                g_mqtt_last_enabled = true;
                g_mqtt_last_broker_ip = g_config.mqtt_broker_ip;
        g_mqtt_last_port = g_config.mqtt_port;
        memcpy(g_mqtt_last_user, g_config.mqtt_user, sizeof(g_mqtt_last_user));
        memcpy(g_mqtt_last_pass, g_config.mqtt_pass, sizeof(g_mqtt_last_pass));
            }
        } else {
            g_mqtt_last_enabled = false;
            g_mqtt_last_broker_ip = 0;
            g_mqtt_last_host[0] = '\0';
        }
    }
    if (g_mqtt) g_mqtt->loop();
}

void loop() {
    CommandPublication command_publication;
    Watchdog::feed();
    drainControlRequests();
    g_fan.service();
    serviceMdns();
    reconcileStaticIpOnce();
    serviceMqttTransport();
    g_sensors.servicePm();
    g_fan.pollPassiveUart();
    syncActuationStatus();

    uint32_t now = millis();
    bool force_control_now = g_force_control_tick;
    if (force_control_now) {
        g_force_control_tick = false;
    }
    if ((now % 15000) < 50) {
        updateNetworkInfo();
    }
    syncCommandMirrors();

    // Verify auto_clear before the apply timeout can race the same loop.
    if (!g_cmd.manualOverrideDesired() && g_cmd.hasFanCommand()) {
        CommandSupervisor::CommandRecord active_cmd{};
        g_cmd.currentCommand(&active_cmd);
        if (active_cmd.command_type == CommandSupervisor::CMD_AUTO_CLEAR &&
            active_cmd.state == CommandSupervisor::STATE_ACCEPTED) {
            const uint8_t prev = prevSpeed();
            const FanApplicationResult applied = applyFanTarget(g_fan_speed, false, now,
                                                                 active_cmd.source, HttpServer::OWNER_AUTO);
            if (g_http) {
                g_http->recordCommandAudit(applied.command, prev,
                                           false, false, true,
                                           "auto clear immediate");
            }
            syncCommandMirrors();
            g_force_control_tick = true;
            force_control_now = true;
        }
    }

    CommandSupervisor::CommandRecord command_result{};
    if (g_cmd.checkFanApplyTimeout(now, &command_result)) {
        g_force_control_tick = true;
        force_control_now = true;
        if (command_result.command_type == CommandSupervisor::CMD_STARTUP_SPIN) {
            g_burst.cancel();
            g_startup_handoff_pending = false;
            g_startup_handoff_result = HttpServer::STARTUP_RESULT_TIMEOUT;
            g_startup_handoff_ms = now;
            if (g_http) {
                g_http->startup_burst_active = false;
                g_http->startup_state_code = HttpServer::STARTUP_STATE_AUTO_ACTIVE;
                g_http->startup_result_code = HttpServer::STARTUP_RESULT_TIMEOUT;
                g_http->startup_handoff_ms = g_startup_handoff_ms;
                g_http->command_owner = HttpServer::OWNER_AUTO;
                g_http->last_command_source = HttpServer::SRC_STARTUP_BURST;
                g_http->last_command_ts = Time.now();
                g_http->has_new_state = true;
            }
        }
        g_log.push(AuraEventType::COMMAND_TIMEOUT, g_fan_speed, g_rpm.lastRpm(), 0, 0, "apply timeout");
        if (g_http) {
            g_http->recordCommandAudit(command_result,
                                       prevSpeed(),
                                       command_result.command_type == CommandSupervisor::CMD_MANUAL_FAN,
                                       false, false, "command timeout");
        }
        syncCommandMirrors();
    }
    if (g_cmd.hasActiveFanCommand()) {
        CommandSupervisor::CommandRecord active_cmd{};
        g_cmd.currentCommand(&active_cmd);
        const bool backend_required =
            active_cmd.command_type == CommandSupervisor::CMD_MANUAL_FAN ||
            active_cmd.command_type == CommandSupervisor::CMD_STARTUP_SPIN ||
            active_cmd.command_type == CommandSupervisor::CMD_ASSIST_BOOST;
        if (backend_required &&
            (active_cmd.state == CommandSupervisor::STATE_APPLIED ||
             active_cmd.state == CommandSupervisor::STATE_VERIFIED) &&
            (!g_fan.isModuleReady() || !g_fan.isActuationReady())) {
            const CommandSupervisor::CommandRecord rolled =
                g_cmd.rollbackFanToAuto(now, "backend unavailable");
            g_log.push(AuraEventType::COMMAND_ROLLBACK, g_fan_speed, g_rpm.lastRpm(), 0, 0, "rollback");
            if (g_http) {
                g_http->recordCommandAudit(rolled,
                                           prevSpeed(),
                                           active_cmd.command_type == CommandSupervisor::CMD_MANUAL_FAN,
                                           false, false, "command rollback");
            }
            syncCommandMirrors();
            g_force_control_tick = true;
            force_control_now = true;
        }
    }

    static bool s_was_manual = false;
    if (persistAutoAfterManualEnds(g_cmd.manualOverrideDesired(), &s_was_manual)) {
        Serial.println("  control: manual ended without request, saved auto");
    }

    // If the backend has been armed after a fail-closed period, mark the
    // previously requested speed as unsent so the next control pass applies it.
    if (g_fan.isModuleReady() && g_fan.isActuationReady() && g_fan.lastCommandMs() == 0 && g_current_speed != 0xFF) {
        g_current_speed = 0xFF;
    }

    // Apply admitted manual requests without waiting for the sensor cadence.
    const bool mode_before = g_cmd.manualOverrideActive();
    if (g_fan.isModuleReady() && g_fan.isActuationReady() && mode_before) {
        const CommandSupervisor::CommandRecord command = g_cmd.fanCommand();
        const uint8_t manual_target = (uint8_t)command.requested_value;
        const bool needs_send = g_fan.lastCommandMs() == 0 || manual_target != g_fan_speed;
        const bool pending = command.state == CommandSupervisor::STATE_ACCEPTED ||
                             command.state == CommandSupervisor::STATE_APPLIED;
        if (g_startup_handoff_pending) {
            g_startup_handoff_pending = false;
            g_startup_handoff_result = HttpServer::STARTUP_RESULT_MANUAL;
            if (g_http) {
                g_http->startup_burst_active = false;
                g_http->startup_state_code = HttpServer::STARTUP_STATE_MANUAL_OVERRIDE;
                g_http->startup_result_code = HttpServer::STARTUP_RESULT_MANUAL;
            }
        }
        if (needs_send || pending) {
            const uint8_t prev = prevSpeed();
            const FanApplicationResult result = applyFanTarget(manual_target, needs_send, now);
            if (result.succeeded) {
                g_current_speed = manual_target;
                if (needs_send) g_last_cmd_ms = now;
            }
            if (!result.matched || !result.succeeded) g_force_control_tick = true;
            syncCommandMirrors();
            if (g_http) {
                g_http->recordCommandAudit(result.command, prev, true,
                                           g_cmd.manualOverrideDesired(), result.succeeded,
                                           result.succeeded ? (needs_send ? "manual apply" : "already at target")
                                                            : "manual apply failed");
            }
        }
    }
    // Force SSE broadcast if mode changed this iteration
    if (g_http && g_cmd.manualOverrideActive() != mode_before) {
        g_http->has_new_state = true;
    }

    if (g_cmd.startupSpinActive() && g_fan.isModuleReady() && g_fan.isActuationReady()) {
        bool pending_hardware_send = (g_fan.lastCommandMs() == 0);
        bool not_yet_verified = (g_cmd.fanCommand().state < CommandSupervisor::STATE_VERIFIED);
        if (g_cmd.startupSpinActive() && (pending_hardware_send || not_yet_verified || g_fan_speed != SenseBurst::STARTUP_SPEED)) {
            uint8_t prev = prevSpeed();
            const FanApplicationResult startup_result = applyFanTarget(SenseBurst::STARTUP_SPEED, true, now);
            if (startup_result.succeeded) {
                if (g_startup_fan_on_ms == 0) {
                    g_startup_fan_on_ms = now;
                    g_burst.start(now);
                }
                const CommandSupervisor::CommandRecord applied = startup_result.command;
                g_current_speed = SenseBurst::STARTUP_SPEED;
                g_last_cmd_ms   = now;
                syncCommandMirrors();
                if (g_http) {
                    g_http->command_owner = HttpServer::OWNER_STARTUP_BURST;
                    g_http->last_command_source = HttpServer::SRC_STARTUP_BURST;
                    g_http->last_command_ts = Time.now();
                    g_http->recordCommandAudit(applied, prev,
                                               false, false, true, "startup spin apply");
                }
            }
        }
    }

    SensorReading reading{};
    SensorCycleStatus sense_status{};
    bool sense_ok = g_sensors.read(&reading, &sense_status);
    if (sense_ok) {
        g_startup_seen_sensor_cycle = true;
    }
    if (sense_status.full_valid && g_startup_first_full_valid_ms == 0) {
        g_startup_first_full_valid_ms = now;
        if (g_http) {
            g_http->startup_first_full_valid_ms = g_startup_first_full_valid_ms;
        }
    }

    ControlState cs{};
    bool have_control = false;
    AutoEngineV3::Result v3{};
    bool have_v3 = false;

    // Slow lane: update Kalman, baseline, and room decay every 20s.
    // This runs on the same cadence as before, gated by sense_ok and SENSE_INTERVAL.
    const bool slow_tick = sense_ok && ((now - g_last_sense_ms) >= SENSE_INTERVAL || force_control_now);
    if (slow_tick) {
        SensorReading slow_reading = reading;
        if (!sense_status.pm_valid) {
            slow_reading.pm25 = NAN;
        }
        if (!sense_status.sgp30_valid) {
            slow_reading.voc_ppb = NAN;
            slow_reading.eco2_ppm = NAN;
        }
    g_ctrl.updateSlowEstimators(slow_reading, now, g_fan_speed, sense_status.sgp30_valid);
    }

    // Fast lane: recompute Auto target every 1s using latest sensor snapshot.
    // PM validity is passed per-cycle so stale PM is treated as absent.
    const bool fast_tick = force_control_now ||
        (now - g_last_fast_control_ms) >= FAST_CONTROL_INTERVAL;
    if (fast_tick) {
        g_last_fast_control_ms = now;
        AutoCycleInputs inputs{};
        inputs.sensor.voc_ppb = reading.voc_ppb;
        inputs.sensor.eco2_ppm = reading.eco2_ppm;
        inputs.sensor.pm25 = reading.pm25;
        inputs.sensor.temp_c = reading.temp_c;
        inputs.sensor.rh_pct = reading.rh_pct;
        inputs.sensor.sample_ms = millis();
        inputs.sensor.pm_age_ms = g_sensors.pmAgeMs();
        inputs.sensor.gas_age_ms = g_sensors.sgp30AgeMs();
        inputs.sensor.humidity_age_ms = g_sensors.humidityAgeMs();
        inputs.sensor.pm_sample_generation = g_sensors.pmGeneration();
        inputs.sensor.gas_sample_generation = g_sensors.sgp30Generation();
        inputs.sensor.sgp30_in_warmup = g_sensors.sgp30InWarmup(inputs.sensor.sample_ms);
        inputs.sensor.humidity_comp_ok = sense_status.sht31_valid &&
            inputs.sensor.humidity_age_ms <= SensorHub::PM_FRESH_WINDOW_MS;
        inputs.sensor.voc_baseline_ppb = g_ctrl.slowStateValid() ? g_ctrl.baseline().get() : 0.0f;
        inputs.room_m3 = g_config.room_m3;
        inputs.learned_room_m3 = NAN;
        inputs.effective_cadr_m3h = g_ctrl.ach().effectiveCadrMax();
        inputs.current_airflow_pct = (float)g_fan_percent_var;
        inputs.current_step = prevSpeed();
        inputs.min_fan_percent = g_config.min_fan_percent;
        inputs.max_fan_percent = g_config.max_fan_percent;
        inputs.now_ms = now;
        v3 = g_auto_v3.computeTarget(inputs);
        have_v3 = true;
        g_led_mode = v3.mode;
        cs.target_speed = v3.quantized_step;
        cs.voc_adj = reading.voc_ppb;
        cs.voc_kalman = g_ctrl.slowStateValid() ? g_ctrl.vocKalman() : NAN;
        cs.voc_velocity = g_ctrl.slowStateValid() ? g_ctrl.vocVelocity() : NAN;
        strncpy(cs.clamp_reason, v3.reason, sizeof(cs.clamp_reason) - 1);
        cs.clamp_reason[sizeof(cs.clamp_reason) - 1] = '\0';
        syncAutoV3Telemetry(v3);
        have_control = true;
        if (g_http) g_http->last_fast_control_ms = now;
    }
    if (slow_tick && g_http) {
        g_http->last_slow_estimator_ms = now;
    }

    if (g_http) {
        g_http->startup_burst_active =
            g_cmd.startupSpinActive() || g_burst.isActive() || g_startup_handoff_pending;
        g_http->startup_armed_ms = g_startup_armed_ms;
        g_http->startup_fan_on_ms = g_startup_fan_on_ms;
        g_http->startup_started_ms = g_startup_armed_ms;
        g_http->startup_first_full_valid_ms = g_startup_first_full_valid_ms;
        g_http->startup_handoff_ms = g_startup_handoff_ms;
        g_http->startup_timeout_ms = SenseBurst::STARTUP_TIMEOUT_MS;
        g_http->startup_sensor_sgp30_valid = sense_status.sgp30_valid;
        g_http->startup_sensor_sht31_valid = sense_status.sht31_valid;
        g_http->startup_sensor_pm_valid = sense_status.pm_valid;
        g_http->startup_sensor_full_valid = sense_status.full_valid;
    }

    if (g_burst.isActive()) {
        if (g_cmd.manualOverrideDesired()) {
            uint8_t prev = prevSpeed();
            g_burst.cancel();
            g_startup_first_full_valid_ms = 0;
            g_startup_handoff_pending = false;
            g_startup_handoff_result = HttpServer::STARTUP_RESULT_MANUAL;
            g_startup_handoff_ms = 0;
            g_http->startup_burst_active = false;
            g_http->startup_state_code = HttpServer::STARTUP_STATE_MANUAL_OVERRIDE;
            g_http->startup_result_code = HttpServer::STARTUP_RESULT_MANUAL;
            g_http->startup_handoff_ms = 0;
            g_http->recordCommandAudit(prev, prev,
                                       HttpServer::SRC_STARTUP_BURST, HttpServer::OWNER_MANUAL,
                                       false, true, false, "startup preempted");
        } else if (g_burst.elapsedMs(now) >= SenseBurst::STARTUP_TIMEOUT_MS) {
            uint8_t prev = prevSpeed();
            g_burst.completeByTimeout(now);
            g_startup_handoff_result = HttpServer::STARTUP_RESULT_TIMEOUT;
            if (restoreManualControlAfterStartup(now, prev, g_startup_handoff_result)) {
                force_control_now = true;
                g_last_cmd_ms = now - CMD_HOLDOFF;
                return;
            }
            g_startup_handoff_pending = true;
            const CommandSupervisor::CommandRecord clear_cmd =
                g_cmd.submitAutoClear(HttpServer::SRC_STARTUP_BURST, HttpServer::OWNER_AUTO,
                                      now, "startup timeout");
            g_restore_control_state_applied = true;
            force_control_now = true;
            g_force_control_tick = false;
            g_http->startup_state_code = HttpServer::STARTUP_STATE_AUTO_ACTIVE;
            g_http->startup_result_code = HttpServer::STARTUP_RESULT_TIMEOUT;
            g_http->command_owner = HttpServer::OWNER_AUTO;
            g_http->last_command_source = HttpServer::SRC_STARTUP_BURST;
            g_http->last_command_ts = Time.now();
            g_last_cmd_ms = now - CMD_HOLDOFF;
            g_http->recordCommandAudit(clear_cmd, prev,
                                       false, false, false, "startup timeout");
        } else if (g_startup_first_full_valid_ms != 0 &&
                   g_burst.elapsedMs(now) >= SenseBurst::STARTUP_MIN_SPIN_MS) {
            uint8_t prev = prevSpeed();
            g_burst.completeBySensor(now);
            g_startup_handoff_result = HttpServer::STARTUP_RESULT_SENSOR;
            if (restoreManualControlAfterStartup(now, prev, g_startup_handoff_result)) {
                force_control_now = true;
                g_last_cmd_ms = now - CMD_HOLDOFF;
                return;
            }
            g_startup_handoff_pending = true;
            const CommandSupervisor::CommandRecord clear_cmd =
                g_cmd.submitAutoClear(HttpServer::SRC_STARTUP_BURST, HttpServer::OWNER_AUTO,
                                      now, "startup sensor");
            g_restore_control_state_applied = true;
            force_control_now = true;
            g_force_control_tick = false;
            g_http->startup_state_code = HttpServer::STARTUP_STATE_AUTO_ACTIVE;
            g_http->startup_result_code = HttpServer::STARTUP_RESULT_SENSOR;
            g_http->command_owner = HttpServer::OWNER_AUTO;
            g_http->last_command_source = HttpServer::SRC_STARTUP_BURST;
            g_http->last_command_ts = Time.now();
            g_last_cmd_ms = now - CMD_HOLDOFF;
            g_http->recordCommandAudit(clear_cmd, prev,
                                       false, false, false, "startup sensor");
        } else if (g_http) {
            g_http->startup_state_code = g_startup_first_full_valid_ms != 0
                ? HttpServer::STARTUP_STATE_AWAIT_FIRST_VALID_SENSOR
                : HttpServer::STARTUP_STATE_BOOT_SPIN;
            g_http->startup_result_code = HttpServer::STARTUP_RESULT_NONE;
        }
    }

    // If the fan backend only becomes ready after the sensor gate has already
    // tripped, still push the startup spin once while the handoff is pending.
    if (g_startup_handoff_pending && g_fan.isModuleReady() && g_fan.isActuationReady()) {
        bool pending_hardware_send = (g_fan.lastCommandMs() == 0);
        if (g_cmd.startupSpinActive() && pending_hardware_send && g_fan_speed != SenseBurst::STARTUP_SPEED) {
            uint8_t prev = prevSpeed();
            const FanApplicationResult startup_result = applyFanTarget(SenseBurst::STARTUP_SPEED, true, now);
            if (startup_result.succeeded) {
                const CommandSupervisor::CommandRecord applied = startup_result.command;
                g_current_speed = SenseBurst::STARTUP_SPEED;
                syncCommandMirrors();
                if (g_http) {
                    g_http->command_owner = HttpServer::OWNER_STARTUP_BURST;
                    g_http->last_command_source = HttpServer::SRC_STARTUP_BURST;
                    g_http->last_command_ts = Time.now();
                    g_http->recordCommandAudit(applied, prev,
                                               false, false, true, "startup spin late");
                }
            }
        }
    }

    uint8_t target = prevSpeed();
    uint8_t source = HttpServer::SRC_UNKNOWN;
    uint8_t owner = HttpServer::OWNER_AUTO;
    const bool time_valid = Time.isValid();
    const uint16_t local_min = time_valid
        ? (uint16_t)(Time.hour() * 60 + Time.minute())
        : 0;
    const bool sched_active = time_valid &&
        scheduleWindowIsActive(g_config.schedule_enabled,
                               g_config.schedule_start_min,
                               g_config.schedule_end_min,
                               local_min);
    const uint8_t sched_speed = sched_active
        ? fanSpeedFromPercent(g_config.schedule_fan_percent, 1, 16)
        : 0;

    const FanSelection selection = selectFanTarget(
        g_cmd.manualOverrideDesired(),
        g_cmd.desiredSpeed(),
        g_burst.isActive() || g_startup_handoff_pending || g_cmd.startupSpinActive(),
        SenseBurst::STARTUP_SPEED,
        sched_active,
        sched_speed,
        (have_control && have_v3) ? &v3 : nullptr,
        prevSpeed());
    target = selection.target_speed;
    switch (selection.reason) {
        case FanSelectionReason::Manual:
            source = g_cmd.desiredSource();
            owner = HttpServer::OWNER_MANUAL;
            break;
        case FanSelectionReason::Startup:
            source = HttpServer::SRC_STARTUP_BURST;
            owner = HttpServer::OWNER_STARTUP_BURST;
            break;
        case FanSelectionReason::Schedule:
            source = HttpServer::SRC_SCHEDULE;
            owner = HttpServer::OWNER_AUTO;
            cs.target_speed = target;
            strncpy(cs.clamp_reason, "schedule", sizeof(cs.clamp_reason) - 1);
            cs.clamp_reason[sizeof(cs.clamp_reason) - 1] = '\0';
            break;
        case FanSelectionReason::Auto:
            source = HttpServer::SRC_CONTROLLER_AUTO;
            owner = HttpServer::OWNER_AUTO;
            cs.target_speed = target;
            strncpy(cs.clamp_reason, v3.reason, sizeof(cs.clamp_reason) - 1);
            cs.clamp_reason[sizeof(cs.clamp_reason) - 1] = '\0';
            break;
        case FanSelectionReason::Fallback:
        default:
            source = HttpServer::SRC_UNKNOWN;
            owner = HttpServer::OWNER_AUTO;
            break;
    }

    if (have_control && g_http) {
        ControlState preview = cs;
        preview.target_speed = target;
        g_http->rpm_actual = g_rpm.lastRpm();
        g_http->rpm_expected = AchModel::expectedRpm(target);
        g_http->rpm_mismatch = g_rpm.lastMismatch();
        g_http->duty_trim = g_fan.dutyTrim();
        g_duty_trim_var = g_fan.dutyTrim();
        g_http->command_owner = owner;
        g_http->schedule_active = sched_active;
        g_http->schedule_target_pct = sched_active ? g_config.schedule_fan_percent : 0;
        syncActuationStatus();
        if (source != HttpServer::SRC_UNKNOWN) g_http->last_command_source = source;
        g_http->latest_state = preview;
        g_http->latest_sensors = reading;
    }

    const bool should_send = shouldSendFanCommand(
        g_fan.isModuleReady(),
        g_fan.isActuationReady(),
        g_current_speed,
        target,
        g_fan.lastCommandMs(),
        target == SenseBurst::STARTUP_SPEED);

    if (fast_tick && should_send) {
        const uint8_t prev = prevSpeed();
        const CommandSupervisor::CommandRecord active = g_cmd.fanCommand();
        const bool bind_command = g_cmd.hasFanCommand() &&
            ((active.command_type == CommandSupervisor::CMD_MANUAL_FAN && owner == HttpServer::OWNER_MANUAL) ||
             (active.command_type == CommandSupervisor::CMD_STARTUP_SPIN && owner == HttpServer::OWNER_STARTUP_BURST) ||
             (active.command_type == CommandSupervisor::CMD_AUTO_CLEAR && owner == HttpServer::OWNER_AUTO));
        const FanApplicationResult result = applyFanTarget(target, true, now, source, owner, bind_command);
        if (result.succeeded) {
            if (g_current_speed != 0xFF && abs((int)target - (int)g_current_speed) >= 4) {
                g_log.push(AuraEventType::SPEED_CHANGE, target, g_rpm.lastRpm(), cs.voc_kalman, reading.pm25);
            }
            g_current_speed = target;
            g_last_cmd_ms = now;
        }
        if (!result.matched && bind_command) g_force_control_tick = true;
        syncCommandMirrors();
        if (g_http) {
            g_http->recordCommandAudit(result.command, prev,
                                       owner == HttpServer::OWNER_MANUAL,
                                       g_cmd.manualOverrideDesired(), result.succeeded,
                                       result.succeeded ? "controller apply" : "command apply failed");
        }
    }

    if (g_startup_handoff_pending && !g_burst.isActive() && !g_cmd.manualOverrideDesired() &&
        (g_fan.lastCommandMs() != 0 || g_startup_handoff_result == HttpServer::STARTUP_RESULT_TIMEOUT)) {
        g_startup_handoff_pending = false;
        g_startup_handoff_ms = now;
        if (g_http) {
            g_http->startup_burst_active = false;
            g_http->startup_state_code = HttpServer::STARTUP_STATE_AUTO_ACTIVE;
            g_http->startup_result_code = g_startup_handoff_result;
            g_http->startup_handoff_ms = g_startup_handoff_ms;
            g_http->command_owner = HttpServer::OWNER_AUTO;
            g_http->last_command_source = HttpServer::SRC_STARTUP_BURST;
            g_http->last_command_ts = Time.now();
            g_http->has_new_state = true;
        }
    } // end fast_tick

    // ── Slow lane gate (20s cadence) ─────────────────────────────────────────
    if (!slow_tick) {

        serviceMdns();
        g_fan.pollPassiveUart();
        serviceStatusLed(now, g_led_mode, g_cmd.manualOverrideActive(),
                         g_burst.isActive() || g_startup_handoff_pending || g_cmd.startupSpinActive(),
                         g_fan.isModuleReady(), g_fan.isActuationReady(),
                         (uint8_t)g_fan_percent_var);
        delay(50);
        return;
    }
    g_last_sense_ms = now;

    if (have_control) {
        // Update HTTP server state for SSE broadcast (slow tick)
        if (g_http) {
            g_http->rpm_actual = g_rpm.lastRpm();
            g_http->rpm_expected = AchModel::expectedRpm(target);
            g_http->rpm_mismatch = g_rpm.lastMismatch();
            // Ignore startup burst and handoff samples.
            const bool rpm_learning_ready =
                !g_http->startup_burst_active &&
                g_http->startup_state_code == HttpServer::STARTUP_STATE_AUTO_ACTIVE &&
                g_http->command_owner == HttpServer::OWNER_AUTO &&
                g_http->last_command_source != HttpServer::SRC_STARTUP_BURST;
            // Smooth live RPM ratio before tightening ACH floor.
            static uint32_t learned_rpm_sequence = 0;
            const RpmSample rpm_sample = g_rpm.snapshot();
            if (rpm_learning_ready && g_http->rpm_expected > 0 &&
                rpm_sample.consumeForLearning(millis(), learned_rpm_sequence)) {
                float observed_factor = (float)rpm_sample.value / (float)g_http->rpm_expected;
                if (observed_factor < 0.5f) observed_factor = 0.5f;
                if (observed_factor > 1.2f) observed_factor = 1.2f;
                if (g_rpm_factor_samples == 0) {
                    g_rpm_factor_smoothed = observed_factor;
                } else {
                    const bool tightening = observed_factor < g_rpm_factor_smoothed;
                    const float alpha = tightening ? 0.10f : 0.25f;
                    g_rpm_factor_smoothed =
                        ((1.0f - alpha) * g_rpm_factor_smoothed) + (alpha * observed_factor);
                }
                if (g_rpm_factor_smoothed < 0.5f) g_rpm_factor_smoothed = 0.5f;
                if (g_rpm_factor_smoothed > 1.2f) g_rpm_factor_smoothed = 1.2f;
                if (g_rpm_factor_samples < 255) g_rpm_factor_samples++;
                if (g_rpm_factor_samples == 1 ||
                    fabsf(g_ctrl.ach().rpmFactor() - g_rpm_factor_smoothed) >= 0.005f) {
                    g_ctrl.ach().setRpmFactor(g_rpm_factor_smoothed);
                }
            }
            g_http->rpm_factor = g_ctrl.ach().rpmFactor();
            syncRpmFactorTelemetry();
            g_http->command_owner = owner;
            g_http->schedule_active = sched_active;
            g_http->schedule_target_pct = sched_active ? g_config.schedule_fan_percent : 0;
            syncActuationStatus();
            if (source != HttpServer::SRC_UNKNOWN) g_http->last_command_source = source;
            g_http->latest_state   = cs;
            g_http->latest_sensors = reading;
            g_http->startup_burst_active = g_burst.isActive();
            g_http->has_new_state  = true;
        }

        // History ring
        {
            HistoryEntry& h = g_history[g_history_head];
            h.ts    = Time.now();
            h.voc   = cs.voc_kalman;
            h.pm25  = reading.pm25;
            h.speed = target;
            g_history_head = (g_history_head + 1) % 200;
            if (g_history_count < 200) g_history_count++;
        }

        // Print dashboard to serial
        Serial.printlnf(
            "VOC=%4.0f→%4.0fadj kal=%4.0f vel=%+5.1f  "
            "PM2.5=%4.1f  T=%.1f°C RH=%.0f%%  "
            "→ spd=%d%s%s",
            reading.voc_ppb, cs.voc_adj, cs.voc_kalman, cs.voc_velocity,
            reading.pm25, reading.temp_c, reading.rh_pct,
            target, "", ""
        );
    }

    // Telemetry publication retains its sensor validity and cadence gates.
    if (g_mqtt && (now - g_last_mqtt_ms) > MQTT_INTERVAL) {
        if (sense_status.full_valid && g_http) {
            PublicStatusView mqtt_view = g_http->buildPublicStatusView();
            g_mqtt->publishState(mqtt_view);
        }
        g_last_mqtt_ms = now;
    }
    serviceStatusLed(now, g_led_mode, g_cmd.manualOverrideActive(),
                     g_burst.isActive() || g_startup_handoff_pending || g_cmd.startupSpinActive(),
                     g_fan.isModuleReady(), g_fan.isActuationReady(),
                     (uint8_t)g_fan_percent_var);

    // Periodic save of estimator state. Config slots are written only on
    // user changes, so a reset here cannot damage config.
    if ((now - g_last_save_ms) > SAVE_INTERVAL) {
        ConfigStore::Lock config_lock;
        g_config.baseline         = g_ctrl.baseline().get();
        // Serialize Kalman state: level, velocity, P[0] covariance
        uint8_t kbuf[12]; g_ctrl.kalman().serialize(kbuf);
        memcpy(&g_config.kalman_x0,  kbuf,     4);
        memcpy(&g_config.kalman_x1,  kbuf + 4, 4);
        memcpy(&g_config.kalman_p00, kbuf + 8, 4);

        const PersistedEstimator est = {
            g_config.kalman_x0, g_config.kalman_x1, g_config.kalman_p00, g_config.baseline,
        };
        ConfigStore::saveEstimator(&est);
        g_last_save_ms = now;
    }
}
