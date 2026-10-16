#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * Tiny, value-only structs for canonical public status and config.
 * Designed to minimize memory footprint and avoid stack/heap risks.
 *
 * These structs define the exact HTTP public contract for
 * /api/status and /api/config, shared with SSE and MQTT.
 *
 * Measured sizes (x86-64 host, Cortex-M3 may differ by padding):
 *   PublicStatusView: ~184 bytes
 *   PublicConfigView: ~64 bytes
 *
 * Hard ceiling: neither struct may exceed 256 bytes.
 */

struct PublicStatusView {
    uint32_t ts;
    char particle_id[25];
    char mdns_host[24];
    char local_ip[16];
    char firmware_version[24];
    uint32_t build_number;

    struct {
        float voc_raw;
        float voc_kalman;
        float voc_velocity;
        float pm25;
        float temp_c;
        float rh_pct;
        float eco2;
        bool voc_fresh;
        bool pm_fresh;
    } sensors;

    struct {
        uint8_t fan_percent;
        uint8_t speed;
        uint8_t manual_command_percent;
        uint8_t v3_target_airflow_pct;
        uint8_t v3_applied_airflow_pct;
        uint8_t v3_quantized_step;
        uint8_t v3_auto_mode;
        uint8_t v3_dominant_lane;
        char v3_reason[32];
        uint16_t rpm_target;
        uint16_t rpm_actual;
        uint32_t rpm_age_ms;
        bool rpm_valid;
        bool rpm_fresh;
        bool manual_mode;
        bool schedule_active;
        uint8_t schedule_target_pct;
        uint8_t startup_state;
        bool startup_active;
        bool fan_ready;
        bool actuation_ready;
        bool pm_telemetry_available;
    } control;
};

struct PublicConfigView {
    uint8_t room_profile;
    float room_m3;
    uint8_t min_fan_percent;
    uint8_t max_fan_percent;
    bool led_enabled;
    bool led_custom_color_enabled;
    uint8_t led_color_r;
    uint8_t led_color_g;
    uint8_t led_color_b;
    char hostname_base[24];
    int8_t timezone_offset_hours;
    bool mqtt_enabled;
    uint16_t mqtt_port;
    char mqtt_broker_ip[16];
    char mqtt_user[24];
    bool mqtt_pass_set;
    bool schedule_enabled;
    uint16_t schedule_start_min;
    uint16_t schedule_end_min;
    uint8_t schedule_fan_percent;
    bool static_ip_enabled;
    uint32_t static_ip;       // packed, first octet in high byte; 0 = unset
    uint32_t static_netmask;
    uint32_t static_gateway;
    uint32_t static_dns;
};

static_assert(sizeof(PublicStatusView) <= 256, "PublicStatusView exceeds 256-byte stack ceiling");
static_assert(sizeof(PublicConfigView) <= 256, "PublicConfigView exceeds 256-byte stack ceiling");

// Transport-neutral name helper for startup_state.
// Mirrors HttpServer::startupStateName() exactly; defined here so
// non-HTTP transports (MQTT, SSE) do not need to depend on HttpServer.
inline const char* publicStartupStateName(uint8_t state) {
    switch (state) {
        case 1: return "boot_spin";
        case 2: return "await_first_valid_sensor";
        case 3: return "auto_active";
        case 4: return "manual_override";
        default: return "idle";
    }
}
