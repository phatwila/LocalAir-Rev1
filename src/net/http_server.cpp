#include "http_server.h"
#include "json_utils.h"
#include "mdns_hostname.h"
#include "mqtt_input.h"
#include "mqtt_client.h"
#include "public_status_renderer.h"
#include "http_debug_renderer.h"
#include "http_request.h"
#include "../sys/boot_diag.h"
#include "../algo/fan_speed.h"
#include "generated/build_version.h"
#include "../web/dashboard.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void normalizeRequestPath(char* path, size_t path_len) {
    if (!path || path_len == 0) return;
    if (strncmp(path, "http://", 7) != 0 && strncmp(path, "https://", 8) != 0) return;

    char* scheme_end = strstr(path, "://");
    if (!scheme_end) return;

    char* slash = strchr(scheme_end + 3, '/');
    if (!slash) {
        strncpy(path, "/", path_len - 1);
        path[path_len - 1] = '\0';
        return;
    }

    size_t n = strlen(slash);
    if (n >= path_len) n = path_len - 1;
    memmove(path, slash, n);
    path[n] = '\0';
}

static void formatIpAddress(uint32_t ip, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >> 8) & 0xFF),
             (unsigned)(ip & 0xFF));
}

static void formatMqttBrokerDisplay(uint32_t ip, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (ip == 0) { out[0] = '\0'; return; }
    formatIpAddress(ip, out, out_len);
}

static uint32_t packIpAddress(const IPAddress& ip) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

static bool backendUsesDirectPwm(const DaughterBoard* fan) {
    return fan && strcmp(fan->backendName(), "fan_pwm") == 0;
}

static bool actuationPathReady(const DaughterBoard* fan, bool actuation_ready) {
    return fan && fan->isModuleReady() && actuation_ready && fan->isActuationReady();
}

static bool actuationPathLive(const DaughterBoard* fan,
                              bool actuation_ready,
                              bool actuation_proven) {
    if (!actuationPathReady(fan, actuation_ready)) return false;
    if (backendUsesDirectPwm(fan)) return true;
    return actuation_proven && fan->isActuationProven();
}

const char* HttpServer::startupStateName(uint8_t state) {
    switch (state) {
        case STARTUP_STATE_BOOT_SPIN: return "boot_spin";
        case STARTUP_STATE_AWAIT_FIRST_VALID_SENSOR: return "await_first_valid_sensor";
        case STARTUP_STATE_AUTO_ACTIVE: return "auto_active";
        case STARTUP_STATE_MANUAL_OVERRIDE: return "manual_override";
        case STARTUP_STATE_IDLE:
        default: return "idle";
    }
}

const char* HttpServer::startupResultName(uint8_t result) {
    switch (result) {
        case STARTUP_RESULT_SENSOR: return "sensor";
        case STARTUP_RESULT_TIMEOUT: return "timeout";
        case STARTUP_RESULT_MANUAL: return "manual";
        case STARTUP_RESULT_NONE:
        default: return "none";
    }
}

HttpServer::HttpServer(AirController* ctrl, EventLog* log, Config* config,
                       DaughterBoard* fan, CommandInbox* inbox, MqttClient* mqtt)
    : has_new_state(false), override_speed(0), override_percent(0), override_active(false),
      override_source(SRC_UNKNOWN), rpm_actual(0), rpm_expected(0), rpm_mismatch(false), rpm_factor(1.0f), duty_trim(0),
      startup_burst_active(false), startup_state_code(STARTUP_STATE_IDLE),
      startup_result_code(STARTUP_RESULT_NONE), startup_armed_ms(0), startup_fan_on_ms(0),
      startup_started_ms(0), startup_first_full_valid_ms(0), startup_handoff_ms(0), startup_timeout_ms(60000),
      startup_sensor_sgp30_valid(false), startup_sensor_sht31_valid(false),
      startup_sensor_pm_valid(false), startup_sensor_full_valid(false),
      schedule_active(false), schedule_target_pct(0),
      command_owner(OWNER_AUTO),
      last_command_source(SRC_UNKNOWN), last_command_ts(0),
      actuation_backend("unknown"), module_readiness("unknown"),
      actuation_ready(false), actuation_proven(false), module_present_assumed(true),
      tach_reader_ready(false), tach_pulse_count(0),
      last_uart_tx_len(0), last_uart_rx_len(0), last_uart_tx_ms(0),
      pm_telemetry_available(false), pm_bytes_seen(0), pm_valid_frames(0),
      pm_checksum_failures(0), pm_last_valid_ms(0), pm_raw_count(0),
      voc_fresh(false), pm_fresh(false), voc_last_valid_ms(0),
      humidity_last_valid_ms(0), pm_snapshot_age_ms(0), humidity_snapshot_age_ms(0),
      sensor_snapshot_age_ms(0), last_fast_control_ms(0), last_slow_estimator_ms(0),
      auto_v3{},
      mdns_host("pending"), local_ip("0.0.0.0"),
      history_buf(nullptr), history_head(nullptr), history_count(nullptr),
      _server(PORT), _ctrl(ctrl), _log(log), _config(config), _fan(fan), _inbox(inbox), _mqtt(mqtt),
      _hostname_change_callback(nullptr),
      _static_ip_change_callback(nullptr)
{
    memset(&latest_state,   0, sizeof(latest_state));
    memset(&latest_sensors, 0, sizeof(latest_sensors));
    memset(&auto_v3, 0, sizeof(auto_v3));
    memset(pm_raw_hex, 0, sizeof(pm_raw_hex));
    memset(_cmd_audit, 0, sizeof(_cmd_audit));
    _cmd_audit_head = 0;
    _cmd_audit_count = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        _sse_active[i] = false;
        _sse_last_heartbeat_ms[i] = 0;
    }
    _sse_accepted = 0;
    _sse_denied = 0;
    _sse_reaped = 0;
    _sse_heartbeats = 0;
    _sse_broadcasts = 0;
    _last_sse_heartbeat_ms = 0;
    _last_sse_broadcast_ms = 0;
}

void HttpServer::setHostnameChangeCallback(HostnameChangeCallback callback) {
    _hostname_change_callback = callback;
}

void HttpServer::setStaticIpChangeCallback(StaticIpChangeCallback callback) {
    _static_ip_change_callback = callback;
}

void HttpServer::run() {
    _server.begin();
    while (true) {
        serviceSse();

        // Broadcast SSE to connected clients
        if (takeCommandUpdate() || has_new_state) {
            broadcastSse();
            has_new_state = false;
        }

        TCPClient client = _server.available();
        if (!client) { delay(10); continue; }

        // Check if this looks like a SSE request before reading full headers
        // (handled inside handleClient)
        handleClient(client);
    }
}

void HttpServer::handleClient(TCPClient& client) {
    // Stream headers: validate every line, keep only framing headers.
    HttpRequestReader reader;
    const uint32_t header_start = millis();
    bool complete_headers = false;
    while ((uint32_t)(millis() - header_start) < 500) {
        if (!client.available()) {
            if (!client.connected()) { requestError(client, 400); return; }
            delay(1); continue;
        }
        const int received = client.read();
        if (received < 0) continue;
        const uint16_t status = reader.feed((char)received);
        if (status == HttpRequestReader::DONE) { complete_headers = true; break; }
        if (status != HttpRequestReader::NEED_MORE) { requestError(client, status); return; }
    }
    if (!complete_headers) { requestError(client, 408); return; }

    // Parse method + path (first line only)
    char method[8] = {0}, path[64] = {0};
    uint16_t body_length = 0;
    bool has_length = false;
    const uint16_t framing_error = parseRequestHeaders(reader.kept(), method, path, &body_length, &has_length);
    if (framing_error) { requestError(client, framing_error); return; }

    normalizeRequestPath(path, sizeof(path));

    // Strip query string from path
    char* q = strchr(path, '?');
    int   qn = 50;
    if (q) {
        sscanf(q + 1, "n=%d", &qn);
        *q = '\0';
    }

    // Route GET
    if (strcmp(method, "GET") == 0) {
        if (body_length) { err400(client); return; }
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            sendDashboard(client);
        } else if (strcmp(path, "/favicon.ico") == 0) {
            client.print("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            client.stop();
        } else if (strcmp(path, "/api/status") == 0) {
            sendStatus(client);
        } else if (strcmp(path, "/api/config") == 0) {
            sendConfig(client);
        } else if (strcmp(path, "/api/history") == 0) {
            sendHistory(client, (uint8_t)(qn < 1 ? 1 : qn > 200 ? 200 : qn));
        } else if (strcmp(path, "/api/events") == 0) {
            sendEvents(client);
        } else if (strcmp(path, "/api/tuning") == 0) {
            sendTuningRegistry(client);
        } else if (strcmp(path, "/api/debug/commands") == 0) {
            sendCommandAudit(client);
        } else if (strcmp(path, "/api/debug/http") == 0) {
            sendHttpDebug(client);
        } else if (strcmp(path, "/api/debug/boot") == 0) {
            sendBootDebug(client);
        } else if (strcmp(path, "/api/debug/actuation") == 0) {
            sendActuation(client);
        } else if (strcmp(path, "/stream") == 0) {
            serviceSse();
            // Find free SSE slot
            for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
                if (!_sse_active[i]) { startSse(client, i); return; }
            }
            _sse_denied++;
            err503(client, "sse slots full");
        } else {
            err400(client);
        }
        return;
    }

    if (strcmp(method, "POST") != 0) { err400(client); return; }
    // Nothing below this gate sees a partial or malformed body.
    char body[512] = {0};
    const uint32_t body_start = millis();
    uint16_t received_body = 0;
    while (received_body < body_length && (uint32_t)(millis() - body_start) < 300) {
        if (client.available()) {
            const int received = client.read();
            if (received >= 0) body[received_body++] = (char)received;
        } else {
            if (!client.connected()) { err400(client); return; }
            delay(1);
        }
    }
    if (received_body != body_length) { requestError(client, 408); return; }
    const bool body_optional = strcmp(path, "/api/fan/auto") == 0 || strcmp(path, "/api/reboot") == 0;
    if ((!body_optional && (!has_length || body_length == 0)) ||
        (body_length && !jsonValidateObject(body, body_length))) { err400(client); return; }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/mqtt/test") == 0) {
            sendMqttTest(client, body, body_length);
        } else if (strcmp(path, "/api/config") == 0) {
            updateConfig(client, body, body_length);
        } else if (strcmp(path, "/api/fan/percentage") == 0 ||
                   strcmp(path, "/api/fan/speed") == 0 ||
                   strcmp(path, "/api/fan/auto") == 0) {
            CommandInbox::Request request{};
            const bool automatic = strcmp(path, "/api/fan/auto") == 0;
            const bool percentage = strcmp(path, "/api/fan/percentage") == 0;
            request.kind = automatic ? CommandInbox::AUTO : CommandInbox::MANUAL;
            request.source = automatic ? SRC_API_AUTO : SRC_API_MANUAL;
            if (!automatic) {
                int number = -1;
                if (percentage) {
                    if (!jsonGetInt(body, "percentage", &number) && !jsonGetInt(body, "value", &number)) {
                        err400(client); return;
                    }
                    if (number < 0) number = 0;
                    if (number > 100) number = 100;
                    request.percent = (uint8_t)number;
                    request.speed = fanSpeedFromPercent(request.percent, 1, 16);
                } else {
                    if (!jsonGetInt(body, "speed", &number) || number < 1 || number > 16) {
                        err400(client); return;
                    }
                    request.speed = (uint8_t)number;
                    request.percent = fanPercentFromSpeed(request.speed, 1, 16);
                }
            }
            CommandInbox::Receipt receipt{};
            if (!awaitCommand(client, request, &receipt)) return;
            char response[192];
            if (automatic) {
                snprintf(response, sizeof(response),
                         "{\"ok\":true,\"accepted\":true,\"command_id\":%lu,\"state\":\"accepted\",\"mode\":\"auto\"}",
                         (unsigned long)receipt.command.command_id);
            } else {
                snprintf(response, sizeof(response),
                         "{\"ok\":true,\"accepted\":true,\"command_id\":%lu,\"state\":\"accepted\","
                         "\"mode\":\"manual\",\"percentage\":%u,\"speed\":%u}",
                         (unsigned long)receipt.command.command_id,
                         (unsigned)receipt.request.percent, (unsigned)receipt.request.speed);
            }
            ok200(client, response);
        } else if (strcmp(path, "/api/reboot") == 0) {
            ok200(client, "{\"ok\":true}");
            delay(200);
            System.reset();
        } else {
            err400(client);
        }
        return;
    }

    err400(client);
}

PublicStatusView HttpServer::buildPublicStatusView() {
    PublicStatusView v{};
    CommandView command{};
    { CommandLock lock(_command_mutex); command = _command_view; }
    v.ts = Time.now();
    strncpy(v.particle_id, System.deviceID().c_str(), sizeof(v.particle_id) - 1);
    v.particle_id[sizeof(v.particle_id) - 1] = '\0';
    strncpy(v.mdns_host, mdns_host ? mdns_host : "pending", sizeof(v.mdns_host) - 1);
    strncpy(v.local_ip, local_ip ? local_ip : "0.0.0.0", sizeof(v.local_ip) - 1);
    strncpy(v.firmware_version, LOCALAIR_FIRMWARE_VERSION, sizeof(v.firmware_version) - 1);
    v.build_number = LOCALAIR_BUILD_NUMBER;

    v.sensors.voc_raw = latest_sensors.voc_ppb;
    v.sensors.voc_kalman = latest_state.voc_kalman;
    v.sensors.voc_velocity = latest_state.voc_velocity;
    v.sensors.pm25 = latest_sensors.pm25;
    v.sensors.temp_c = latest_sensors.temp_c;
    v.sensors.rh_pct = latest_sensors.rh_pct;
    v.sensors.eco2 = latest_sensors.eco2_ppm;
    v.sensors.voc_fresh = voc_fresh;
    v.sensors.pm_fresh = pm_fresh;

    const bool manual_mode = command.state.has_active_command &&
        command.state.active_command.command_type == CommandSupervisor::CMD_MANUAL_FAN;
    v.control.speed = command.physical_speed;
    v.control.fan_percent = fanPercentFromSpeed(v.control.speed, 1, 16);
    v.control.manual_command_percent = manual_mode
        ? command.manual_percent
        : 0;
    
    v.control.v3_target_airflow_pct = auto_v3.target_airflow_pct;
    v.control.v3_applied_airflow_pct = auto_v3.applied_airflow_pct;
    v.control.v3_quantized_step = auto_v3.quantized_step;
    v.control.v3_auto_mode = auto_v3.auto_mode;
    v.control.v3_dominant_lane = (uint8_t)auto_v3.dominant_lane;
    strncpy(v.control.v3_reason, auto_v3.reason, sizeof(v.control.v3_reason) - 1);
    
    v.control.rpm_target = (uint16_t)rpm_expected;
    const RpmSample rpm = _rpm_monitor ? _rpm_monitor->snapshot() : RpmSample{};
    const uint32_t rpm_now = millis();
    v.control.rpm_actual = rpm.currentValue(rpm_now);
    v.control.rpm_valid = rpm.valid;
    v.control.rpm_fresh = rpm.fresh(rpm_now);
    v.control.rpm_age_ms = rpm.age(rpm_now);
    v.control.manual_mode = manual_mode;
    v.control.schedule_active = command.schedule_active;
    v.control.schedule_target_pct = command.schedule_percent;

    v.control.startup_state = command.startup_state;
    v.control.startup_active = command.startup_active;
    v.control.fan_ready = _fan && _fan->isModuleReady() && actuation_ready;
    v.control.actuation_ready = actuation_ready;
    v.control.pm_telemetry_available = pm_telemetry_available;

    return v;
}

void HttpServer::publishCommandState(const CommandSupervisor::StatusSnapshot& state, uint8_t physical_speed) {
    CommandView view{};
    view.state = state;
    view.physical_speed = physical_speed;
    view.manual_percent = override_percent;
    view.startup_state = startup_state_code;
    view.startup_active = startup_burst_active;
    view.schedule_active = schedule_active;
    view.schedule_percent = schedule_target_pct;
    CommandLock lock(_command_mutex);
    // Age changes alone must not cause an SSE broadcast every loop iteration.
    _command_view.state.pending_command_age_ms = view.state.pending_command_age_ms;
    if (memcmp(&view, &_command_view, sizeof(view)) != 0) _command_dirty = true;
    _command_view = view;
}

bool HttpServer::takeCommandUpdate() {
    CommandLock lock(_command_mutex);
    const bool dirty = _command_dirty;
    _command_dirty = false;
    return dirty;
}

bool HttpServer::awaitCommand(TCPClient& client, const CommandInbox::Request& request,
                              CommandInbox::Receipt* receipt) {
    CommandInbox::Ticket ticket{};
    const uint32_t submitted_at = millis();
    if (!_inbox || !_inbox->enqueue(request, submitted_at, true, &ticket)) {
        err503(client, "command queue unavailable"); return false;
    }
    while (true) {
        const CommandInbox::State state = _inbox->poll(ticket, receipt);
        if (state == CommandInbox::ACCEPTED) {
            _inbox->abandon(ticket);
            return true;
        }
        if (state == CommandInbox::CANCELLED || state == CommandInbox::INVALID) {
            _inbox->abandon(ticket);
            err503(client, "command admission timeout"); return false;
        }
        if (!client.connected()) {
            _inbox->abandon(ticket);
            client.stop(); return false;
        }
        if ((uint32_t)(millis() - submitted_at) >= CommandInbox::HTTP_ADMISSION_TIMEOUT_MS &&
            _inbox->cancel(ticket)) {
            _inbox->abandon(ticket);
            err503(client, "command admission timeout"); return false;
        }
        // A claimed request finishes its local persistence transaction. Never
        // report a rejection which can be followed by a delayed acceptance.
        delay(1);
    }
}

PublicConfigView HttpServer::buildPublicConfigView() {
    PublicConfigView v{};
    ConfigStore::Lock config_lock;
    if (!_config) return v;

    v.room_profile = _config->room_profile;
    v.room_m3 = _config->room_m3;
    v.min_fan_percent = _config->min_fan_percent;
    v.max_fan_percent = _config->max_fan_percent;
    v.led_enabled = _config->led_enabled;
    v.led_custom_color_enabled = _config->led_custom_color_enabled;
    v.led_color_r = _config->led_color_r;
    v.led_color_g = _config->led_color_g;
    v.led_color_b = _config->led_color_b;
    strncpy(v.hostname_base, _config->mdns_base_name, sizeof(v.hostname_base) - 1);
    v.timezone_offset_hours = _config->timezone_offset_hours;
    v.mqtt_enabled = _config->mqtt_enabled;
    v.mqtt_port = _config->mqtt_port;
    formatMqttBrokerDisplay(_config->mqtt_broker_ip, v.mqtt_broker_ip, sizeof(v.mqtt_broker_ip));
    strncpy(v.mqtt_user, _config->mqtt_user, sizeof(v.mqtt_user) - 1);
    v.mqtt_pass_set = _config->mqtt_pass[0] != '\0';
    v.schedule_enabled = _config->schedule_enabled;
    v.schedule_start_min = _config->schedule_start_min;
    v.schedule_end_min = _config->schedule_end_min;
    v.schedule_fan_percent = _config->schedule_fan_percent;
    v.static_ip_enabled = _config->static_ip_enabled;
    v.static_ip = _config->static_ip;
    v.static_netmask = _config->static_netmask;
    v.static_gateway = _config->static_gateway;
    v.static_dns = _config->static_dns;

    return v;
}

void HttpServer::renderPublicStatusStream(TCPClient& client, const PublicStatusView& view) {
    renderPublicStatusJson(client, view);
}

void HttpServer::renderPublicConfigStream(TCPClient& client, const PublicConfigView& view) {
    char entry[256];
    char mqtt_user[32];
    char hostname_base[32];
    jsonEscape(view.mqtt_user, mqtt_user, sizeof(mqtt_user));
    jsonEscape(view.hostname_base, hostname_base, sizeof(hostname_base));

    client.print("{");
    snprintf(entry, sizeof(entry), "\"room_profile\":\"%s\",\"room_m3\":%.1f,\"max_fan_percent\":%u,\"min_fan_percent\":%u,",
             roomProfileKey(view.room_profile), (double)view.room_m3, view.max_fan_percent, view.min_fan_percent);
    client.print(entry);

    snprintf(entry, sizeof(entry), "\"led_enabled\":%s,\"led_custom_color_enabled\":%s,\"led_color_r\":%u,\"led_color_g\":%u,\"led_color_b\":%u,",
             view.led_enabled ? "true" : "false", view.led_custom_color_enabled ? "true" : "false",
             view.led_color_r, view.led_color_g, view.led_color_b);
    client.print(entry);

    snprintf(entry, sizeof(entry), "\"hostname_base\":\"%s\",\"timezone_offset_hours\":%d,\"mqtt_enabled\":%s,\"mqtt_port\":%u,\"mqtt_broker_ip\":\"%s\",",
             hostname_base, (int)view.timezone_offset_hours, view.mqtt_enabled ? "true" : "false", view.mqtt_port, view.mqtt_broker_ip);
    client.print(entry);

    snprintf(entry, sizeof(entry), "\"mqtt_user\":\"%s\",\"mqtt_pass_set\":%s,",
             mqtt_user, view.mqtt_pass_set ? "true" : "false");
    client.print(entry);

    snprintf(entry, sizeof(entry), "\"schedule_enabled\":%s,\"schedule_start_min\":%u,\"schedule_end_min\":%u,\"schedule_fan_percent\":%u,",
             view.schedule_enabled ? "true" : "false",
             view.schedule_start_min,
             view.schedule_end_min,
             view.schedule_fan_percent);
    client.print(entry);

    char static_ip[16], static_netmask[16], static_gateway[16], static_dns[16];
    formatMqttBrokerDisplay(view.static_ip, static_ip, sizeof(static_ip));
    formatMqttBrokerDisplay(view.static_netmask, static_netmask, sizeof(static_netmask));
    formatMqttBrokerDisplay(view.static_gateway, static_gateway, sizeof(static_gateway));
    formatMqttBrokerDisplay(view.static_dns, static_dns, sizeof(static_dns));
    snprintf(entry, sizeof(entry), "\"static_ip_enabled\":%s,\"static_ip\":\"%s\",\"static_netmask\":\"%s\",\"static_gateway\":\"%s\",\"static_dns\":\"%s\"}",
             view.static_ip_enabled ? "true" : "false",
             static_ip, static_netmask, static_gateway, static_dns);
    client.print(entry);
}

void HttpServer::sendStatus(TCPClient& client) {
    PublicStatusView view = buildPublicStatusView();
    jsonHeader(client, 200, "application/json");
    renderPublicStatusStream(client, view);
    client.stop();
}

void HttpServer::sendConfig(TCPClient& client) {
    PublicConfigView view = buildPublicConfigView();
    jsonHeader(client, 200, "application/json");
    renderPublicConfigStream(client, view);
    client.stop();
}

void HttpServer::updateConfig(TCPClient& client, const char* body, uint16_t) {
    char room_profile_buf[24];
    const bool room_profile_present = jsonFieldPresent(body, "room_profile");
    if (room_profile_present && !jsonGetString(body, "room_profile",
                                              room_profile_buf, sizeof(room_profile_buf))) {
        err400(client);
        return;
    }
    uint8_t room_profile = room_profile_present
        ? roomProfileCodeFromKey(room_profile_buf, ROOM_PROFILE_UNKNOWN)
        : ROOM_PROFILE_EVERYDAY;
    ConfigStore::Lock config_lock;
    Config next = *_config;
    float float_value = 0.0f;
    int int_value = 0;
    bool bool_value = false;

    const bool room_m3_present = jsonFieldPresent(body, "room_m3");
    if (room_m3_present) {
        if (!jsonGetFloat(body, "room_m3", &float_value)) { err400(client); return; }
        next.room_m3 = float_value;
    }
    if (jsonFieldPresent(body, "max_fan_percent")) {
        int int_val = 100;
        if (!jsonGetInt(body, "max_fan_percent", &int_val) || int_val > 100 || int_val <= 0) { err400(client); return; }
        next.max_fan_percent = (uint8_t)int_val;
    }
    if (jsonFieldPresent(body, "min_fan_percent")) {
        int int_val = 50;
        if (!jsonGetInt(body, "min_fan_percent", &int_val) || int_val < 40 || int_val > 100) { err400(client); return; }
        next.min_fan_percent = (uint8_t)int_val;
    }
    if (jsonFieldPresent(body, "led_enabled")) {
        if (!jsonGetBool(body, "led_enabled", &bool_value)) { err400(client); return; }
        next.led_enabled = bool_value;
    }
    if (jsonFieldPresent(body, "led_custom_color_enabled")) {
        if (!jsonGetBool(body, "led_custom_color_enabled", &bool_value)) { err400(client); return; }
        next.led_custom_color_enabled = bool_value;
    }
    if (jsonFieldPresent(body, "led_color_r")) {
        if (!jsonGetInt(body, "led_color_r", &int_value)) { err400(client); return; }
        if (int_value < 0) int_value = 0;
        if (int_value > 255) int_value = 255;
        next.led_color_r = (uint8_t)int_value;
    }
    if (jsonFieldPresent(body, "led_color_g")) {
        if (!jsonGetInt(body, "led_color_g", &int_value)) { err400(client); return; }
        if (int_value < 0) int_value = 0;
        if (int_value > 255) int_value = 255;
        next.led_color_g = (uint8_t)int_value;
    }
    if (jsonFieldPresent(body, "led_color_b")) {
        if (!jsonGetInt(body, "led_color_b", &int_value)) { err400(client); return; }
        if (int_value < 0) int_value = 0;
        if (int_value > 255) int_value = 255;
        next.led_color_b = (uint8_t)int_value;
    }
    if (next.min_fan_percent >= next.max_fan_percent ||
        fanStepWithinPercentBounds(0.0f, next.min_fan_percent, next.max_fan_percent) == 0) {
        err400(client);
        return;
    }
    bool mqtt_broker_clear = false;
    if (jsonFieldPresent(body, "mqtt_broker_clear")) {
        if (!jsonGetBool(body, "mqtt_broker_clear", &bool_value)) { err400(client); return; }
        mqtt_broker_clear = bool_value;
    }
    const ParsedMqttBrokerField mqtt_broker = parseMqttBrokerField(body, "mqtt_broker_ip");
    if (!mqtt_broker.valid) {
        err400(client);
        return;
    }
    if (mqtt_broker.present) {
        next.mqtt_broker_ip = (mqtt_broker.clear || mqtt_broker_clear) ? 0 : mqtt_broker.ip;
    } else if (mqtt_broker_clear) {
        next.mqtt_broker_ip = 0;
    }
    if (jsonFieldPresent(body, "mqtt_port")) {
        if (!jsonGetInt(body, "mqtt_port", &int_value) || int_value < 1 || int_value > 65535) { err400(client); return; }
        next.mqtt_port = (uint16_t)int_value;
    }
    if (jsonFieldPresent(body, "mqtt_enabled")) {
        if (!jsonGetBool(body, "mqtt_enabled", &bool_value)) { err400(client); return; }
        next.mqtt_enabled = bool_value;
    }
    char mqtt_user_buf[sizeof(next.mqtt_user)];
    bool mqtt_user_clear = false;
    if (jsonFieldPresent(body, "mqtt_user_clear")) {
        if (!jsonGetBool(body, "mqtt_user_clear", &bool_value)) { err400(client); return; }
        mqtt_user_clear = bool_value;
    }
    if (jsonGetString(body, "mqtt_user", mqtt_user_buf, sizeof(mqtt_user_buf))) {
        strncpy(next.mqtt_user, mqtt_user_buf, sizeof(next.mqtt_user) - 1);
        next.mqtt_user[sizeof(next.mqtt_user) - 1] = '\0';
    } else if (jsonFieldPresent(body, "mqtt_user")) {
        err400(client);
        return;
    }
    char mqtt_pass_buf[sizeof(next.mqtt_pass)];
    bool mqtt_pass_clear = false;
    if (jsonFieldPresent(body, "mqtt_pass_clear")) {
        if (!jsonGetBool(body, "mqtt_pass_clear", &bool_value)) { err400(client); return; }
        mqtt_pass_clear = bool_value;
    }
    if (jsonGetString(body, "mqtt_pass", mqtt_pass_buf, sizeof(mqtt_pass_buf))) {
        strncpy(next.mqtt_pass, mqtt_pass_buf, sizeof(next.mqtt_pass) - 1);
        next.mqtt_pass[sizeof(next.mqtt_pass) - 1] = '\0';
    } else if (jsonFieldPresent(body, "mqtt_pass")) {
        err400(client);
        return;
    }
    if (mqtt_user_clear) {
        next.mqtt_user[0] = '\0';
    }
    if (mqtt_pass_clear) {
        next.mqtt_pass[0] = '\0';
    }
    if (jsonFieldPresent(body, "timezone_offset_hours")) {
        if (!jsonGetInt(body, "timezone_offset_hours", &int_value)) { err400(client); return; }
        if (int_value < -12) int_value = -12;
        if (int_value > 14) int_value = 14;
        next.timezone_offset_hours = (int8_t)int_value;
    }
    if (jsonFieldPresent(body, "schedule_enabled")) {
        if (!jsonGetBool(body, "schedule_enabled", &bool_value)) { err400(client); return; }
        next.schedule_enabled = bool_value;
    }
    if (jsonFieldPresent(body, "schedule_start_min")) {
        if (!jsonGetInt(body, "schedule_start_min", &int_value) ||
            int_value < 0 || int_value > 1439) { err400(client); return; }
        next.schedule_start_min = (uint16_t)int_value;
    }
    if (jsonFieldPresent(body, "schedule_end_min")) {
        if (!jsonGetInt(body, "schedule_end_min", &int_value) ||
            int_value < 0 || int_value > 1439) { err400(client); return; }
        next.schedule_end_min = (uint16_t)int_value;
    }
    if (jsonFieldPresent(body, "schedule_fan_percent")) {
        if (!jsonGetInt(body, "schedule_fan_percent", &int_value) ||
            int_value < 0 || int_value > 100) { err400(client); return; }
        next.schedule_fan_percent = (uint8_t)int_value;
    }
    if (next.schedule_enabled && next.schedule_start_min == next.schedule_end_min) {
        err400(client);
        return;
    }
    if (jsonFieldPresent(body, "static_ip_enabled")) {
        if (!jsonGetBool(body, "static_ip_enabled", &bool_value)) { err400(client); return; }
        next.static_ip_enabled = bool_value;
    }
    struct { const char* key; uint32_t* dst; } static_ip_fields[] = {
        {"static_ip",      &next.static_ip},
        {"static_netmask", &next.static_netmask},
        {"static_gateway", &next.static_gateway},
        {"static_dns",     &next.static_dns},
    };
    for (const auto& field : static_ip_fields) {
        if (!jsonFieldPresent(body, field.key)) continue;
        char ip_buf[20];
        if (!jsonGetString(body, field.key, ip_buf, sizeof(ip_buf))) { err400(client); return; }
        if (ip_buf[0] == '\0') { *field.dst = 0; continue; }
        uint32_t parsed = 0;
        if (!parseJsonDottedQuad(ip_buf, &parsed)) { err400(client); return; }
        *field.dst = parsed;
    }
    if (next.static_ip_enabled) {
        // Pin: no explicit address means capture the current DHCP lease
        if (next.static_ip == 0 || next.static_netmask == 0) {
            next.static_ip      = packIpAddress(WiFi.localIP());
            next.static_netmask = packIpAddress(WiFi.subnetMask());
            next.static_gateway = packIpAddress(WiFi.gatewayIP());
            next.static_dns     = packIpAddress(WiFi.dnsServerIP());
        }
        if (next.static_dns == 0) next.static_dns = next.static_gateway;
        if (next.static_ip == 0 || next.static_netmask == 0) { err400(client); return; }
    }
    const bool static_ip_apply_needed =
        (next.static_ip_enabled != _config->static_ip_enabled) ||
        (next.static_ip_enabled && (next.static_ip != _config->static_ip ||
                                    next.static_netmask != _config->static_netmask ||
                                    next.static_gateway != _config->static_gateway ||
                                    next.static_dns != _config->static_dns));

    bool hostname_changed = false;
    char hostname_base_buf[sizeof(next.mdns_base_name)];
    if (jsonGetString(body, "hostname_base", hostname_base_buf, sizeof(hostname_base_buf))) {
        if (!mdnsBaseNameIsValid(hostname_base_buf)) { err400(client); return; }
        hostname_changed = strcmp(next.mdns_base_name, hostname_base_buf) != 0;
        memset(next.mdns_base_name, 0, sizeof(next.mdns_base_name));
        strncpy(next.mdns_base_name, hostname_base_buf, sizeof(next.mdns_base_name) - 1);
    } else if (jsonFieldPresent(body, "hostname_base")) {
        err400(client);
        return;
    }

    if (room_profile_present && !roomProfileIsPreset(room_profile)) {
        err400(client);
        return;
    }
    if (!room_profile_present) {
        room_profile = next.room_profile;
    }
    next.room_profile = room_profile;

    *_config = next;

    _ctrl->configure(_config->room_m3, _config->room_profile,
                     _config->max_fan_percent,
                     _config->min_fan_percent);
    ConfigStore::save(_config);
    Time.zone(_config->timezone_offset_hours);
    if (hostname_changed && _hostname_change_callback) {
        _hostname_change_callback(_config->mdns_base_name);
    }

    // Echo back saved config so the UI can verify without a roundtrip
    PublicConfigView view = buildPublicConfigView();
    jsonHeader(client, 200, "application/json");
    renderPublicConfigStream(client, view);
    client.stop();

    // Store Device OS addressing after the response. On pinned P1 Device OS,
    // these setters take effect on the next reconnect; they do not force one.
    if (static_ip_apply_needed && _static_ip_change_callback) {
        _static_ip_change_callback(_config->static_ip_enabled);
    }
}

void HttpServer::sendMqttTest(TCPClient& client, const char* body, uint16_t) {
    ConfigStore::Lock config_lock;
    Config probe = _config ? *_config : Config{};
    probe.mqtt_enabled = true;

    bool bool_value = false;
    int int_value = 0;
    bool mqtt_broker_clear = false;
    if (jsonFieldPresent(body, "mqtt_broker_clear")) {
        if (!jsonGetBool(body, "mqtt_broker_clear", &bool_value)) { err400(client); return; }
        mqtt_broker_clear = bool_value;
    }
    const ParsedMqttBrokerField mqtt_broker = parseMqttBrokerField(body, "mqtt_broker_ip");
    if (!mqtt_broker.valid) {
        ok200(client, "{\"ok\":false,\"connected\":false,\"message\":\"Invalid MQTT broker host/IP\"}");
        return;
    }
    if (mqtt_broker.present) {
        probe.mqtt_broker_ip = mqtt_broker.clear ? 0 : mqtt_broker.ip;
    } else if (mqtt_broker_clear) {
        probe.mqtt_broker_ip = 0;
    }
    if (probe.mqtt_broker_ip == 0) {
        ok200(client, "{\"ok\":false,\"connected\":false,\"message\":\"MQTT broker host/IP is required\"}");
        return;
    }

    int_value = probe.mqtt_port ? probe.mqtt_port : 1883;
    if (jsonFieldPresent(body, "mqtt_port")) {
        if (!jsonGetInt(body, "mqtt_port", &int_value) || int_value < 1 || int_value > 65535) {
            ok200(client, "{\"ok\":false,\"connected\":false,\"message\":\"Invalid MQTT port\"}");
            return;
        }
    }
    probe.mqtt_port = (uint16_t)int_value;

    char mqtt_user_buf[sizeof(probe.mqtt_user)];
    bool mqtt_user_clear = false;
    if (jsonFieldPresent(body, "mqtt_user_clear")) {
        if (!jsonGetBool(body, "mqtt_user_clear", &bool_value)) { ok200(client, "{\"ok\":false,\"connected\":false,\"message\":\"Invalid MQTT username clear flag\"}"); return; }
        mqtt_user_clear = bool_value;
    }
    if (jsonGetString(body, "mqtt_user", mqtt_user_buf, sizeof(mqtt_user_buf))) {
        strncpy(probe.mqtt_user, mqtt_user_buf, sizeof(probe.mqtt_user) - 1);
        probe.mqtt_user[sizeof(probe.mqtt_user) - 1] = '\0';
    } else if (jsonFieldPresent(body, "mqtt_user")) {
        ok200(client, "{\"ok\":false,\"connected\":false,\"message\":\"Invalid MQTT username\"}");
        return;
    }

    char mqtt_pass_buf[sizeof(probe.mqtt_pass)];
    bool mqtt_pass_clear = false;
    if (jsonFieldPresent(body, "mqtt_pass_clear")) {
        if (!jsonGetBool(body, "mqtt_pass_clear", &bool_value)) { ok200(client, "{\"ok\":false,\"connected\":false,\"message\":\"Invalid MQTT password clear flag\"}"); return; }
        mqtt_pass_clear = bool_value;
    }
    if (jsonGetString(body, "mqtt_pass", mqtt_pass_buf, sizeof(mqtt_pass_buf))) {
        strncpy(probe.mqtt_pass, mqtt_pass_buf, sizeof(probe.mqtt_pass) - 1);
        probe.mqtt_pass[sizeof(probe.mqtt_pass) - 1] = '\0';
    } else if (jsonFieldPresent(body, "mqtt_pass")) {
        ok200(client, "{\"ok\":false,\"connected\":false,\"message\":\"Invalid MQTT password\"}");
        return;
    }
    if (mqtt_broker_clear) {
        probe.mqtt_broker_ip = 0;
    }
    if (mqtt_user_clear) {
        probe.mqtt_user[0] = '\0';
    }
    if (mqtt_pass_clear) {
        probe.mqtt_pass[0] = '\0';
    }

    char msg[96];
    bool ok = _mqtt ? _mqtt->testConnection(probe, msg, sizeof(msg)) : false;
    if (!_mqtt) {
        strncpy(msg, "MQTT client unavailable", sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = '\0';
    }

    char broker[16];
    formatMqttBrokerDisplay(probe.mqtt_broker_ip, broker, sizeof(broker));
    char msg_json[128];
    jsonEscape(msg, msg_json, sizeof(msg_json));
    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"ok\":%s,\"connected\":%s,\"broker\":\"%s:%d\",\"message\":\"%s\"}",
        ok ? "true" : "false",
        ok ? "true" : "false",
        broker,
        probe.mqtt_port,
        msg_json);
    ok200(client, resp);
}

void HttpServer::sendHistory(TCPClient& client, uint8_t n) {
    if (!history_buf || !history_head || !history_count || *history_count == 0) {
        ok200(client, "[]");
        return;
    }
    uint8_t total = *history_count;
    if (n > total) n = total;

    // Stream JSON array without a single large stack buffer
    jsonHeader(client, 200, "application/json");
    client.print("[");
    for (uint8_t i = 0; i < n; i++) {
        // Walk backwards from head — newest first
        uint8_t idx = (*history_head + 200 - 1 - i) % 200;
        const HistEntry& e = history_buf[idx];
        char pm25_buf[16];
        const char* pm25_json = jsonFloatOrNull(e.pm25, pm25_buf, sizeof(pm25_buf), 1);
        const uint8_t fan_percent = fanPercentFromSpeed(e.speed, 1, 16);
        char entry[112];
        snprintf(entry, sizeof(entry),
            "%s{\"ts\":%lu,\"voc\":%.0f,\"pm25\":%s,\"spd\":%d,\"fan_percent\":%u}",
            i > 0 ? "," : "", (unsigned long)e.ts, e.voc, pm25_json, e.speed, (unsigned)fan_percent);
        client.print(entry);
    }
    client.print("]");
    client.stop();
}

void HttpServer::sendTuningRegistry(TCPClient& client) {
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "{"
          "\"registry\":["
            "{\"name\":\"timezone_offset_hours\",\"classification\":\"policy\",\"auto_apply\":false},"
            "{\"name\":\"mqtt_enabled\",\"classification\":\"policy\",\"auto_apply\":false},"
            "{\"name\":\"mqtt_port\",\"classification\":\"policy\",\"auto_apply\":false},"
            "{\"name\":\"HYSTERESIS\",\"classification\":\"offline-fixed\",\"auto_apply\":false},"
            "{\"name\":\"CMD_HOLDOFF\",\"classification\":\"offline-fixed\",\"auto_apply\":false},"
            "{\"name\":\"PM25_BONUS\",\"classification\":\"offline-fixed\",\"auto_apply\":false}"
          "]"
        "}");
    ok200(client, buf);
}

void HttpServer::sendCommandAudit(TCPClient& client) {
    uint8_t limit;
    {
        CommandLock lock(_command_mutex);
        limit = _cmd_audit_count > 8 ? 8 : _cmd_audit_count;
        for (uint8_t i = 0; i < limit; ++i) {
            const uint8_t idx = (_cmd_audit_head + CMD_AUDIT_CAPACITY - 1 - i) % CMD_AUDIT_CAPACITY;
            _audit_response[i] = _cmd_audit[idx];
        }
    }
    jsonHeader(client, 200, "application/json");
    client.print("[");
    for (uint8_t i = 0; i < limit; i++) {
        const CommandAuditEntry& e = _audit_response[i];
        const uint8_t requested_percent = fanPercentFromSpeed(e.requested_speed, 1, 16);
        const uint8_t previous_percent = fanPercentFromSpeed(e.previous_speed, 1, 16);
        const uint8_t effective_percent = fanPercentFromSpeed(
            e.effective_value > 0 ? (uint8_t)e.effective_value : 0, 1, 16);
        char escaped_reason[48];
        char escaped_failure[48];
        char escaped_cleared[32];
        jsonEscape(e.reason, escaped_reason, sizeof(escaped_reason));
        jsonEscape(e.failure_reason, escaped_failure, sizeof(escaped_failure));
        jsonEscape(e.cleared_by, escaped_cleared, sizeof(escaped_cleared));
        char entry[768];
        snprintf(entry, sizeof(entry),
            "%s{\"ts\":%lu,\"command_id\":%lu,\"requested_speed\":%u,\"previous_speed\":%u,"
            "\"requested_percent\":%u,\"previous_percent\":%u,"
            "\"requested_value\":%d,"
            "\"source\":\"%s\",\"command_owner\":\"%s\","
            "\"command_type\":\"%s\",\"state\":\"%s\","
            "\"mode_before\":\"%s\",\"mode_after\":\"%s\","
            "\"outcome\":\"%s\",\"reason\":\"%s\","
            "\"failure_reason\":\"%s\",\"cleared_by\":\"%s\","
            "\"expires_at_ms\":%lu,\"effective_value\":%d,\"effective_percent\":%u}",
            i > 0 ? "," : "",
            (unsigned long)e.ts,
            (unsigned long)e.command_id,
            e.requested_speed,
            e.previous_speed,
            (unsigned)requested_percent,
            (unsigned)previous_percent,
            (int)e.requested_value,
            sourceName(e.source),
            ownerName(e.owner),
            CommandSupervisor::commandTypeName(e.command_type),
            CommandSupervisor::commandStateName(e.state),
            e.mode_before_manual ? "manual" : "auto",
            e.mode_after_manual ? "manual" : "auto",
            e.sent ? "sent" : "skipped",
            escaped_reason,
            escaped_failure,
            escaped_cleared,
            (unsigned long)e.expires_at_ms,
            (int)e.effective_value,
            (unsigned)effective_percent
        );
        client.print(entry);
    }
    client.print("]");
    client.stop();
}

void HttpServer::sendEvents(TCPClient& client) {
    static const char* type_names[] = {
        "SPEED_CHANGE","JERK_ALERT","RPM_MISMATCH","SENSE_BURST",
        "CADR_CALIBRATED","ROOM_ESTIMATED","RESERVED_6","REBOOT",
        "WIFI_CONNECT","MQTT_CONNECT","ASSIST_TIMEOUT","COMMAND_TIMEOUT",
        "COMMAND_ROLLBACK"
    };
    // Stream events as JSON array — build incrementally to avoid big stack buf
    jsonHeader(client, 200, "application/json");
    client.print("[");
    uint8_t n = _log->count();
    for (uint8_t i = 0; i < n && i < 64; i++) {
        const LogEntry& e = _log->entry(i);
        char escaped_msg[64];
        jsonEscape(e.msg, escaped_msg, sizeof(escaped_msg));
        char pm25_buf[16];
        const char* pm25_json = jsonFloatOrNull(e.pm25, pm25_buf, sizeof(pm25_buf), 1);
        char entry[184];
        const char* tname = (uint8_t)e.type < (sizeof(type_names) / sizeof(type_names[0]))
                                ? type_names[(uint8_t)e.type]
                                : "UNKNOWN";
        const uint8_t fan_percent = fanPercentFromSpeed(e.speed, 1, 16);
        snprintf(entry, sizeof(entry),
            "%s{\"ts\":%lu,\"type\":\"%s\",\"speed\":%d,\"fan_percent\":%u,\"rpm\":%u,\"voc\":%.0f,\"pm25\":%s,\"msg\":\"%s\"}",
            i > 0 ? "," : "", (unsigned long)e.ts, tname, e.speed, (unsigned)fan_percent, (unsigned)e.rpm, e.voc, pm25_json, escaped_msg);
        client.print(entry);
    }
    client.print("]");
    client.stop();
}

void HttpServer::sendActuation(TCPClient& client) {
    const bool module_ready = _fan && _fan->isModuleReady();
    const bool ready_now = actuationPathReady(_fan, actuation_ready);
    const bool live_now = actuationPathLive(_fan, actuation_ready, actuation_proven);
    const char* control_transport = !module_ready ? "interlock-blocked"
                                 : (live_now ? "live"
                                 : (ready_now ? "timer-ready" : "unproven"));
    const char* control_blocker = !module_ready ? "module-readiness-gate"
                               : (!ready_now ? "transport-not-ready"
                               : (!live_now ? "transport-unproven" : "none"));
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "{"
          "\"backend\":\"%s\","
          "\"readiness_state\":\"%s\","
          "\"module_present_assumed\":%s,"
          "\"ready\":%s,"
          "\"proven\":%s,"
          "\"control_transport\":\"%s\","
          "\"control_blocker\":\"%s\","
          "\"pm_telemetry_available\":%s,"
          "\"pm_bytes_seen\":%lu,"
          "\"pm_valid_frames\":%lu,"
          "\"pm_checksum_failures\":%lu,"
          "\"pm_last_valid_ms\":%lu,"
          "\"pm_raw_count\":%u,"
          "\"pm_raw_hex\":\"%s\","
          "\"tach_reader_ready\":%s,"
          "\"tach_pulse_count\":%lu,"
          "\"last_uart_tx_len\":%u,"
          "\"last_uart_rx_len\":%u,"
          "\"last_uart_tx_ms\":%lu,"
          "\"last_uart_saw_reply\":%s,"
          "\"last_commanded_speed\":%u,"
          "\"last_commanded_percent\":%u,"
          "\"last_command_ms\":%lu,"
          "\"duty_trim\":%d,"
          "\"rpm_actual\":%u,"
          "\"rpm_expected\":%u,"
          "\"v3_pm_valid\":%s,"
          "\"v3_gas_valid\":%s,"
          "\"v3_rh_valid\":%s"
        "}",
        actuation_backend ? actuation_backend : "unknown",
        module_readiness ? module_readiness : "unknown",
        module_present_assumed ? "true" : "false",
        ready_now ? "true" : "false",
        live_now ? "true" : "false",
        control_transport,
        control_blocker,
        pm_telemetry_available ? "true" : "false",
        (unsigned long)pm_bytes_seen,
        (unsigned long)pm_valid_frames,
        (unsigned long)pm_checksum_failures,
        (unsigned long)pm_last_valid_ms,
        (unsigned)pm_raw_count,
        pm_raw_hex,
        tach_reader_ready ? "true" : "false",
        (unsigned long)tach_pulse_count,
        (unsigned)last_uart_tx_len,
        (unsigned)last_uart_rx_len,
        (unsigned long)last_uart_tx_ms,
        last_uart_rx_len > 0 ? "true" : "false",
        _fan ? _fan->lastCommandedSpeed() : 0,
        (unsigned)fanPercentFromSpeed(_fan ? _fan->lastCommandedSpeed() : 0, 1, 16),
        (unsigned long)(_fan ? _fan->lastCommandMs() : 0),
        (int)duty_trim,
        (unsigned)buildPublicStatusView().control.rpm_actual,
        (unsigned)rpm_expected,
        auto_v3.sensor_pm_valid ? "true" : "false",
        auto_v3.sensor_voc_valid ? "true" : "false",
        auto_v3.sensor_rh_valid ? "true" : "false");
    ok200(client, buf);
}

void HttpServer::sendHttpDebug(TCPClient& client) {
    HttpDebugView view{};
    view.sse_active = activeSseCount();
    view.sse_max = MAX_CLIENTS;
    view.sse_accepted = _sse_accepted;
    view.sse_denied = _sse_denied;
    view.sse_reaped = _sse_reaped;
    view.sse_heartbeats = _sse_heartbeats;
    view.sse_broadcasts = _sse_broadcasts;
    view.last_sse_heartbeat_ms = _last_sse_heartbeat_ms;
    view.last_sse_broadcast_ms = _last_sse_broadcast_ms;

    struct ClientOut {
        TCPClient& client;
        void print(const char* s) { client.print(s); }
    } out{client};

    client.print("HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Connection: close\r\n\r\n");
    renderHttpDebugJson(out, view);
    client.stop();
}

void HttpServer::sendBootDebug(TCPClient& client) {
    char buf[192];
    BootDiag::renderJson(buf, sizeof(buf), millis() / 1000);
    ok200(client, buf);
}

void HttpServer::recordCommandAudit(uint8_t requested_speed, uint8_t previous_speed,
                                    uint8_t source, uint8_t owner,
                                    bool mode_before_manual, bool mode_after_manual,
                                    bool sent, const char* reason) {
    CommandLock lock(_command_mutex);
    CommandAuditEntry& e = _cmd_audit[_cmd_audit_head];
    e.ts = Time.now();
    e.command_id = 0;
    e.expires_at_ms = 0;
    e.requested_speed = requested_speed;
    e.previous_speed = previous_speed;
    e.requested_value = requested_speed;
    e.source = source;
    e.owner = owner;
    e.command_type = CommandSupervisor::CMD_NONE;
    e.state = sent ? CommandSupervisor::STATE_VERIFIED : CommandSupervisor::STATE_FAILED;
    e.mode_before_manual = mode_before_manual;
    e.mode_after_manual = mode_after_manual;
    e.sent = sent;
    e.effective_value = requested_speed;
    memset(e.failure_reason, 0, sizeof(e.failure_reason));
    memset(e.cleared_by, 0, sizeof(e.cleared_by));
    if (reason && reason[0]) {
        strncpy(e.reason, reason, sizeof(e.reason) - 1);
        e.reason[sizeof(e.reason) - 1] = '\0';
    } else {
        strcpy(e.reason, "command");
    }
    _cmd_audit_head = (_cmd_audit_head + 1) % CMD_AUDIT_CAPACITY;
    if (_cmd_audit_count < CMD_AUDIT_CAPACITY) _cmd_audit_count++;
    if (sent) {
        command_owner = owner;
        last_command_source = source;
        last_command_ts = e.ts;
    }
}

void HttpServer::recordCommandAudit(const CommandSupervisor::CommandRecord& cmd,
                                    uint8_t previous_speed,
                                    bool mode_before_manual, bool mode_after_manual,
                                    bool sent, const char* reason) {
    CommandLock lock(_command_mutex);
    CommandAuditEntry& e = _cmd_audit[_cmd_audit_head];
    e.ts = Time.now();
    e.command_id = cmd.command_id;
    e.expires_at_ms = cmd.expires_at_ms;
    e.requested_speed = cmd.requested_value >= 0 ? (uint8_t)cmd.requested_value : 0;
    e.previous_speed = previous_speed;
    e.requested_value = cmd.requested_value;
    e.source = cmd.source;
    e.owner = cmd.owner;
    e.command_type = cmd.command_type;
    e.state = cmd.state;
    e.mode_before_manual = mode_before_manual;
    e.mode_after_manual = mode_after_manual;
    e.sent = sent;
    e.effective_value = cmd.effective_value;
    memset(e.reason, 0, sizeof(e.reason));
    memset(e.failure_reason, 0, sizeof(e.failure_reason));
    memset(e.cleared_by, 0, sizeof(e.cleared_by));
    if (reason && reason[0]) {
        strncpy(e.reason, reason, sizeof(e.reason) - 1);
        e.reason[sizeof(e.reason) - 1] = '\0';
    } else {
        strncpy(e.reason, "command", sizeof(e.reason) - 1);
    }
    if (cmd.failure_reason[0]) {
        strncpy(e.failure_reason, cmd.failure_reason, sizeof(e.failure_reason) - 1);
    }
    if (cmd.cleared_by[0]) {
        strncpy(e.cleared_by, cmd.cleared_by, sizeof(e.cleared_by) - 1);
    }
    _cmd_audit_head = (_cmd_audit_head + 1) % CMD_AUDIT_CAPACITY;
    if (_cmd_audit_count < CMD_AUDIT_CAPACITY) _cmd_audit_count++;
    if (sent) {
        command_owner = cmd.owner;
        last_command_source = cmd.source;
        last_command_ts = e.ts;
    }
}

// One elapsed-time budget covers both headers and the compressed body. Device
// OS 3.3.1 can cast a negative socket error to size_t, so reject oversized returns.
static bool writeDashboardBytes(TCPClient& client, const uint8_t* data, size_t length,
                                uint32_t started_ms) {
    static constexpr uint32_t SEND_BUDGET_MS = 3000;
    size_t sent = 0;
    while (sent < length) {
        const uint32_t elapsed = millis() - started_ms;
        if (elapsed >= SEND_BUDGET_MS || !client.connected()) return false;
        const size_t remaining = length - sent;
        const size_t chunk = remaining > 256 ? 256 : remaining;
        const size_t written = client.write(data + sent, chunk, SEND_BUDGET_MS - elapsed);
        if (written == 0 || written > chunk) return false;
        sent += written;
    }
    return true;
}

void HttpServer::sendDashboard(TCPClient& client) {
    // dashboard.h provides the prebuilt dashboard HTML payload.
    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Encoding: gzip\r\n"
        "Vary: Accept-Encoding\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        (int)DASHBOARD_HTML_GZ_LEN);
    const uint32_t started_ms = millis();
    if (writeDashboardBytes(client, reinterpret_cast<const uint8_t*>(header),
                            strlen(header), started_ms)) {
        writeDashboardBytes(client, DASHBOARD_HTML_GZ, DASHBOARD_HTML_GZ_LEN, started_ms);
    }
    client.stop();
}

void HttpServer::startSse(TCPClient& client, uint8_t slot) {
    client.print(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n"
        "retry:3000\n\n"
    );
    _sse_clients[slot] = client;
    _sse_active[slot]  = true;
    _sse_last_heartbeat_ms[slot] = millis();
    _sse_accepted++;
}

uint8_t HttpServer::reapSse() {
    uint8_t reaped = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!_sse_active[i]) continue;
        if (_sse_clients[i].connected()) continue;
        _sse_active[i] = false;
        _sse_last_heartbeat_ms[i] = 0;
        reaped++;
    }
    _sse_reaped += reaped;
    return reaped;
}

uint8_t HttpServer::activeSseCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (_sse_active[i]) count++;
    }
    return count;
}

void HttpServer::serviceSse() {
    reapSse();
    const uint32_t now = millis();
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!_sse_active[i]) continue;
        if ((uint32_t)(now - _sse_last_heartbeat_ms[i]) < 10000UL) continue;
        _sse_clients[i].print(":hb\n\n");
        _sse_last_heartbeat_ms[i] = now;
        _last_sse_heartbeat_ms = now;
        _sse_heartbeats++;
        if (!_sse_clients[i].connected()) {
            _sse_active[i] = false;
            _sse_last_heartbeat_ms[i] = 0;
            _sse_reaped++;
        }
    }
}

void HttpServer::broadcastSse() {
    reapSse();
    PublicStatusView view = buildPublicStatusView();
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!_sse_active[i]) continue;
        if (!_sse_clients[i].connected()) { _sse_active[i] = false; continue; }
        _sse_clients[i].print("data:");
        renderPublicStatusStream(_sse_clients[i], view);
        _sse_clients[i].print("\n\n");
        _last_sse_broadcast_ms = millis();
        _sse_last_heartbeat_ms[i] = _last_sse_broadcast_ms;
        _sse_broadcasts++;
    }
}

const char* HttpServer::sourceName(uint8_t source) {
    switch (source) {
        case SRC_CONTROLLER_AUTO: return "controller_auto";
        case SRC_API_MANUAL: return "api_manual";
        case SRC_API_AUTO: return "api_auto";
        case SRC_CLOUD: return "particle_cloud";
        case SRC_MQTT: return "mqtt";
        case SRC_STARTUP_BURST: return "startup_burst";
        case SRC_STARTUP_IDLE: return "startup_idle";
        case SRC_RESTORE: return "restore";
        case SRC_SCHEDULE: return "schedule";
        default: return "unknown";
    }
}

const char* HttpServer::ownerName(uint8_t owner) {
    switch (owner) {
        case OWNER_MANUAL: return "manual";
        case OWNER_STARTUP_BURST: return "startup_burst";
        case OWNER_EXTERNAL: return "external";
        default: return "auto";
    }
}

const char* HttpServer::commandTypeName(uint8_t type) {
    return CommandSupervisor::commandTypeName(type);
}

const char* HttpServer::commandStateName(uint8_t state) {
    return CommandSupervisor::commandStateName(state);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

int HttpServer::parseJsonInt(const char* body, const char* key, int def) {
    int v = def;
    return jsonGetInt(body, key, &v) ? v : def;
}

bool HttpServer::parseJsonBool(const char* body, const char* key, bool def) {
    bool v = def;
    return jsonGetBool(body, key, &v) ? v : def;
}

void HttpServer::jsonHeader(TCPClient& c, uint16_t code, const char* ctype, int clen) {
    char h[128];
    const char* phrase = (code == 200) ? "OK" :
                         (code == 400) ? "Bad Request" :
                         (code == 408) ? "Request Timeout" :
                         (code == 413) ? "Payload Too Large" :
                         (code == 431) ? "Request Header Fields Too Large" :
                         (code == 503) ? "Service Unavailable" : "";
    if (clen >= 0)
        snprintf(h, sizeof(h),
            "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            code, phrase, ctype, clen);
    else
        snprintf(h, sizeof(h),
            "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
            code, phrase, ctype);
    c.print(h);
}

void HttpServer::ok200(TCPClient& c, const char* json_body) {
    int blen = strlen(json_body);
    jsonHeader(c, 200, "application/json", blen);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(json_body);
    int sent = 0;
    while (sent < blen) {
        int chunk = blen - sent;
        if (chunk > 256) chunk = 256;
        c.write(p + sent, chunk);
        sent += chunk;
    }
    c.stop();
}

void HttpServer::err400(TCPClient& c) {
    c.print("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    c.stop();
}

void HttpServer::requestError(TCPClient& c, uint16_t code) {
    jsonHeader(c, code, "application/json", 0);
    c.stop();
}

void HttpServer::err503(TCPClient& c, const char* reason) {
    char escaped[96];
    jsonEscape(reason ? reason : "service unavailable", escaped, sizeof(escaped));
    char body[168];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    int blen = strlen(body);
    jsonHeader(c, 503, "application/json", blen);
    c.print(body);
    c.stop();
}
