#pragma once
#include "application.h"
#include <stdint.h>
#include "../app/air_controller.h"
#include "../app/auto_engine_v3.h"
#include "../app/command_supervisor.h"
#include "../app/command_inbox.h"
#include "../sys/event_log.h"
#include "../sys/config_store.h"
#include "../sys/public_views.h"
#include "../hal/daughter_board.h"
#include "../hal/rpm_monitor.h"

class MqttClient;

// Local REST API on port 80.
// Runs in its own thread via Thread.
// Routes:
//   GET  /                 → embedded dashboard HTML
//   GET  /api/status       → public status JSON
//   GET  /api/config       → full config JSON
//   POST /api/config       → update config (JSON body)
//   POST /api/fan/speed    → {"speed":N}  manual override
//   POST /api/fan/percentage → {"percentage":N} manual override
//   POST /api/fan/auto     → return to auto mode
//   GET  /api/history      → last N readings (?n=50)
//   POST /api/mqtt/test    → probe broker connection
//   GET  /api/events       → last 64 log entries
//   GET  /api/debug/http   → HTTP/SSE connection counters
//   GET  /api/debug/boot   → last reset reason and config load source
//   GET  /api/debug/actuation → live actuator state
//   POST /api/reboot       → safe reboot
//   GET  /stream           → SSE live feed (text/event-stream)

class HttpServer {
public:
    static constexpr uint16_t PORT = 80;
    static constexpr uint16_t MAX_CLIENTS = 4;
    static constexpr uint8_t CMD_AUDIT_CAPACITY = 64;
    typedef bool (*HostnameChangeCallback)(const char* base_name);
    typedef void (*StaticIpChangeCallback)(bool enabled);

    enum CommandSourceCode : uint8_t {
        SRC_UNKNOWN = 0,
        SRC_CONTROLLER_AUTO = 1,
        SRC_API_MANUAL = 2,
        SRC_API_AUTO = 3,
        SRC_MQTT = 4,
        SRC_CLOUD = 5,
        SRC_STARTUP_BURST = 6,
        SRC_STARTUP_IDLE = 7,
        // 8 reserved (was SRC_CALIBRATION — unused)
        SRC_RESTORE = 9,
        // 10 reserved (was SRC_API_CONFIG — unused)
        SRC_SCHEDULE = 11,
    };

    enum CommandOwnerCode : uint8_t {
        OWNER_AUTO = 0,
        OWNER_MANUAL = 1,
        OWNER_STARTUP_BURST = 2,
        OWNER_EXTERNAL = 3,
    };

    enum StartupStateCode : uint8_t {
        STARTUP_STATE_IDLE = 0,
        STARTUP_STATE_BOOT_SPIN = 1,
        STARTUP_STATE_AWAIT_FIRST_VALID_SENSOR = 2,
        STARTUP_STATE_AUTO_ACTIVE = 3,
        STARTUP_STATE_MANUAL_OVERRIDE = 4,
    };

    enum StartupResultCode : uint8_t {
        STARTUP_RESULT_NONE = 0,
        STARTUP_RESULT_SENSOR = 1,
        STARTUP_RESULT_TIMEOUT = 2,
        STARTUP_RESULT_MANUAL = 3,
    };

    HttpServer(AirController* ctrl, EventLog* log, Config* config, DaughterBoard* fan,
               CommandInbox* inbox, MqttClient* mqtt);

    // Call from dedicated thread — runs forever.
    void run();
    void setHostnameChangeCallback(HostnameChangeCallback callback);
    void setStaticIpChangeCallback(StaticIpChangeCallback callback);

    // Used by SSE and history: written by main loop each cycle.
    volatile bool        has_new_state;
    ControlState         latest_state;
    SensorReading        latest_sensors;
    volatile uint8_t     override_speed;
    volatile uint8_t     override_percent;
    volatile bool        override_active;
    volatile uint8_t     override_source;
    volatile uint16_t    rpm_actual;
    volatile uint16_t    rpm_expected;
    volatile bool        rpm_mismatch;
    volatile float       rpm_factor;
    volatile int8_t      duty_trim;
    volatile bool        startup_burst_active;
    volatile uint8_t     startup_state_code;
    volatile uint8_t     startup_result_code;
    volatile uint32_t    startup_armed_ms;
    volatile uint32_t    startup_fan_on_ms;
    volatile uint32_t    startup_started_ms;
    volatile uint32_t    startup_first_full_valid_ms;
    volatile uint32_t    startup_handoff_ms;
    volatile uint32_t    startup_timeout_ms;
    volatile bool        startup_sensor_sgp30_valid;
    volatile bool        startup_sensor_sht31_valid;
    volatile bool        startup_sensor_pm_valid;
    volatile bool        startup_sensor_full_valid;
    volatile bool        schedule_active;
    volatile uint8_t     schedule_target_pct;
    volatile uint8_t     command_owner;
    volatile uint8_t     last_command_source;
    volatile uint32_t    last_command_ts;
    const char*          actuation_backend;
    const char*          module_readiness;
    volatile bool        actuation_ready;
    volatile bool        actuation_proven;
    volatile bool        module_present_assumed;
    volatile bool        tach_reader_ready;
    volatile uint32_t    tach_pulse_count;
    volatile uint8_t     last_uart_tx_len;
    volatile uint8_t     last_uart_rx_len;
    volatile uint32_t    last_uart_tx_ms;
    volatile bool        pm_telemetry_available;
    volatile uint32_t    pm_bytes_seen;
    volatile uint32_t    pm_valid_frames;
    volatile uint32_t    pm_checksum_failures;
    volatile uint32_t    pm_last_valid_ms;
    volatile uint8_t     pm_raw_count;
    char                 pm_raw_hex[192];
    // Sensor freshness telemetry
    volatile bool        voc_fresh;
    volatile bool        pm_fresh;
    volatile uint32_t    voc_last_valid_ms;
    volatile uint32_t    humidity_last_valid_ms;
    volatile uint32_t    pm_snapshot_age_ms;
    volatile uint32_t    humidity_snapshot_age_ms;
    volatile uint32_t    sensor_snapshot_age_ms;
    // Control cadence telemetry
    volatile uint32_t    last_fast_control_ms;
    volatile uint32_t    last_slow_estimator_ms;
    AutoV3Telemetry      auto_v3;
    const char*          mdns_host;
    const char*          local_ip;

    struct CommandAuditEntry {
        uint32_t ts;
        uint32_t command_id;
        uint32_t expires_at_ms;
        uint8_t requested_speed;
        uint8_t previous_speed;
        int16_t requested_value;
        uint8_t source;
        uint8_t owner;
        uint8_t command_type;
        uint8_t state;
        bool mode_before_manual;
        bool mode_after_manual;
        bool sent;
        char reason[20];
        char failure_reason[24];
        char cleared_by[20];
        int16_t effective_value;
    };


    // History ring buffer (pointer set by main.cpp before run())
    struct HistEntry { uint32_t ts; float voc; float pm25; uint8_t speed; };
    HistEntry*  history_buf;   // points to main.cpp's g_history[]
    uint8_t*    history_head;  // points to main.cpp's g_history_head
    uint8_t*    history_count; // points to main.cpp's g_history_count

    void recordCommandAudit(uint8_t requested_speed, uint8_t previous_speed,
                            uint8_t source, uint8_t owner,
                            bool mode_before_manual, bool mode_after_manual,
                            bool sent, const char* reason);
    void recordCommandAudit(const CommandSupervisor::CommandRecord& cmd,
                            uint8_t previous_speed,
                            bool mode_before_manual, bool mode_after_manual,
                            bool sent, const char* reason);

    // Canonical public status view — also consumed by MQTT for parity.
    void setRpmMonitor(RpmMonitor* monitor) { _rpm_monitor = monitor; }
    PublicStatusView buildPublicStatusView();
    void publishCommandState(const CommandSupervisor::StatusSnapshot& state, uint8_t physical_speed);
private:
    friend struct HttpServerTestAccess;
    TCPServer   _server;
    AirController* _ctrl;
    EventLog*      _log;
    Config*        _config;
    DaughterBoard* _fan;
    CommandInbox* _inbox;
    MqttClient*    _mqtt;
    RpmMonitor* _rpm_monitor = nullptr;
    HostnameChangeCallback _hostname_change_callback;
    StaticIpChangeCallback _static_ip_change_callback;

    // SSE clients (keep-alive connections)
    TCPClient   _sse_clients[MAX_CLIENTS];
    bool        _sse_active[MAX_CLIENTS];
    uint32_t    _sse_last_heartbeat_ms[MAX_CLIENTS];
    uint32_t    _sse_accepted;
    uint32_t    _sse_denied;
    uint32_t    _sse_reaped;
    uint32_t    _sse_heartbeats;
    uint32_t    _sse_broadcasts;
    uint32_t    _last_sse_heartbeat_ms;
    uint32_t    _last_sse_broadcast_ms;
    CommandAuditEntry _cmd_audit[CMD_AUDIT_CAPACITY];
    uint8_t _cmd_audit_head;
    uint8_t _cmd_audit_count;
    struct CommandView {
        CommandSupervisor::StatusSnapshot state;
        uint8_t physical_speed;
        uint8_t manual_percent;
        uint8_t startup_state;
        bool startup_active;
        bool schedule_active;
        uint8_t schedule_percent;
    };
    struct CommandLock {
        RecursiveMutex& mutex;
        explicit CommandLock(RecursiveMutex& m) : mutex(m) { mutex.lock(); }
        ~CommandLock() { mutex.unlock(); }
    };
    RecursiveMutex _command_mutex;
    CommandView _command_view{};
    bool _command_dirty = false;
    // HTTP-thread scratch avoids placing eight audit records on its 4 KiB stack.
    CommandAuditEntry _audit_response[8];
    bool takeCommandUpdate();
    bool awaitCommand(TCPClient& client, const CommandInbox::Request& request,
                      CommandInbox::Receipt* receipt);

    void handleClient(TCPClient& client);
    void sendStatus(TCPClient& client);
    void sendConfig(TCPClient& client);
    void updateConfig(TCPClient& client, const char* body, uint16_t body_len);
    void sendHistory(TCPClient& client, uint8_t n);
    void sendMqttTest(TCPClient& client, const char* body, uint16_t body_len);
    void sendTuningRegistry(TCPClient& client);
    void sendCommandAudit(TCPClient& client);
    void sendEvents(TCPClient& client);
    void sendActuation(TCPClient& client);
    void sendHttpDebug(TCPClient& client);
    void sendBootDebug(TCPClient& client);
    void sendDashboard(TCPClient& client);

    // Stream-based canonical views
    PublicConfigView buildPublicConfigView();
    void renderPublicStatusStream(TCPClient& client, const PublicStatusView& view);
    void renderPublicConfigStream(TCPClient& client, const PublicConfigView& view);

    void startSse(TCPClient& client, uint8_t slot);
    void serviceSse();
    uint8_t reapSse();
    uint8_t activeSseCount() const;
    void broadcastSse();
    static const char* sourceName(uint8_t source);
    static const char* ownerName(uint8_t owner);
    static const char* commandTypeName(uint8_t type);
    static const char* commandStateName(uint8_t state);
    static const char* startupStateName(uint8_t state);
    static const char* startupResultName(uint8_t result);

    // Minimal JSON helpers (no heap allocation)
    static int parseJsonInt(const char* body, const char* key, int def);
    static bool parseJsonBool(const char* body, const char* key, bool def);
    static void jsonHeader(TCPClient& c, uint16_t code, const char* ctype, int content_len = -1);
    static void ok200(TCPClient& c, const char* json_body);
    static void err400(TCPClient& c);
    static void requestError(TCPClient& c, uint16_t code);
    static void err503(TCPClient& c, const char* reason);
};
