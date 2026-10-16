#pragma once

#include "application.h"
#include "json_utils.h"
#include "../sys/public_views.h"

template <typename Out>
inline void renderPublicStatusJson(Out& out, const PublicStatusView& v) {
    char entry[256];
    char f0_buf[16];
    char f1_buf[16];
    char f2_buf[16];
    char f3_buf[16];

    out.print("{");
    snprintf(entry, sizeof(entry), "\"ts\":%lu,\"particle_id\":\"%s\",\"mdns_host\":\"%s\",\"local_ip\":\"%s\",\"firmware_version\":\"%s\",\"build_number\":%lu,",
             (unsigned long)v.ts, v.particle_id, v.mdns_host, v.local_ip, v.firmware_version, (unsigned long)v.build_number);
    out.print(entry);

    const char* f0_json = jsonFloatOrNull(v.sensors.voc_raw, f0_buf, sizeof(f0_buf), 0);
    const char* f1_json = jsonFloatOrNull(v.sensors.voc_kalman, f1_buf, sizeof(f1_buf), 0);
    const char* f2_json = jsonFloatOrNull(v.sensors.voc_velocity, f2_buf, sizeof(f2_buf), 1);
    const char* f3_json = jsonFloatOrNull(v.sensors.pm25, f3_buf, sizeof(f3_buf), 1);
    snprintf(entry, sizeof(entry), "\"sensors\":{\"voc_raw\":%s,\"voc_kalman\":%s,\"voc_velocity\":%s,\"pm25\":%s,",
             f0_json, f1_json, f2_json, f3_json);
    out.print(entry);

    f0_json = jsonFloatOrNull(v.sensors.temp_c, f0_buf, sizeof(f0_buf), 1);
    f1_json = jsonFloatOrNull(v.sensors.rh_pct, f1_buf, sizeof(f1_buf), 0);
    f2_json = jsonFloatOrNull(v.sensors.eco2, f2_buf, sizeof(f2_buf), 0);
    snprintf(entry, sizeof(entry), "\"temp_c\":%s,\"rh_pct\":%s,\"eco2\":%s,\"voc_fresh\":%s,\"pm_fresh\":%s},",
             f0_json, f1_json, f2_json,
             v.sensors.voc_fresh ? "true" : "false", v.sensors.pm_fresh ? "true" : "false");
    out.print(entry);

    snprintf(entry, sizeof(entry), "\"control\":{\"fan_percent\":%u,\"speed\":%u,\"manual_command_percent\":%u,",
             v.control.fan_percent, v.control.speed, v.control.manual_command_percent);
    out.print(entry);

    snprintf(entry, sizeof(entry), "\"v3_target_airflow_pct\":%u,\"v3_applied_airflow_pct\":%u,\"v3_quantized_step\":%u,",
             v.control.v3_target_airflow_pct, v.control.v3_applied_airflow_pct, v.control.v3_quantized_step);
    out.print(entry);

    snprintf(entry, sizeof(entry), "\"v3_auto_mode\":%u,\"v3_dominant_lane\":%u,\"v3_reason\":\"%s\",",
             v.control.v3_auto_mode, v.control.v3_dominant_lane, v.control.v3_reason);
    out.print(entry);

    const char* mode = v.control.manual_mode ? "manual" :
                       (v.control.schedule_active ? "schedule" : "auto");
    snprintf(entry, sizeof(entry), "\"rpm_target\":%u,\"rpm_actual\":%u,\"mode\":\"%s\",",
             v.control.rpm_target, v.control.rpm_actual, mode);
    out.print(entry);

    snprintf(entry, sizeof(entry), "\"rpm_valid\":%s,\"rpm_fresh\":%s,\"rpm_age_ms\":",
             v.control.rpm_valid ? "true" : "false", v.control.rpm_fresh ? "true" : "false");
    out.print(entry);
    if (v.control.rpm_age_ms == UINT32_MAX) out.print("null,");
    else { snprintf(entry, sizeof(entry), "%lu,", (unsigned long)v.control.rpm_age_ms); out.print(entry); }

    snprintf(entry, sizeof(entry), "\"startup_state\":\"%s\",",
             publicStartupStateName(v.control.startup_state));
    out.print(entry);

    snprintf(entry, sizeof(entry), "\"startup_active\":%s,\"fan_ready\":%s,\"actuation_ready\":%s,\"pm_telemetry_available\":%s,",
             v.control.startup_active ? "true" : "false",
             v.control.fan_ready ? "true" : "false",
             v.control.actuation_ready ? "true" : "false",
             v.control.pm_telemetry_available ? "true" : "false");
    out.print(entry);

    snprintf(entry, sizeof(entry), "\"schedule_active\":%s,\"schedule_target_pct\":%u}}",
             v.control.schedule_active ? "true" : "false",
             v.control.schedule_target_pct);
    out.print(entry);
}
