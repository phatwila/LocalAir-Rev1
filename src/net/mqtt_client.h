#pragma once

#include "application.h"
#include <stdint.h>

#include "../app/auto_engine_v3.h"
#include "../app/air_controller.h"
#include "../app/command_inbox.h"
#include "../sys/config_store.h"
#include "../sys/public_views.h"

// Local MQTT client for the purifier. This is a self-contained MQTT 3.1.1
// implementation so the firmware does not depend on a vendored MQTT.h library.
class MqttClient {
public:
    static constexpr uint32_t KEEPALIVE_MS = 30000;
    static constexpr uint32_t RECONNECT_MS = 15000;
    static constexpr uint8_t  SOURCE_CODE_MQTT = 4;

    MqttClient(Config* config, CommandInbox* inbox);

    void begin(const char* mdns_host);
    void loop();
    void publishState(const PublicStatusView& view);
    bool testConnection(const Config& config, char* result, size_t result_len);

    bool connected();

private:
    static constexpr uint8_t OWNER_MANUAL = 1;
    static constexpr size_t TOPIC_LEN = 96;
    static constexpr size_t CLIENT_ID_LEN = 40;
    static constexpr size_t RX_LEN = 256;

    Config* _config;
    const char* _mdns_host;
    char _identity[24];
    char _state_topic[TOPIC_LEN];
    char _cmd_topic[TOPIC_LEN];
    char _pct_cmd_topic[TOPIC_LEN];
    char _avail_topic[TOPIC_LEN];
    char _client_id[CLIENT_ID_LEN];
    uint32_t _last_connect_attempt;
    uint32_t _last_tx_ms;
    uint32_t _last_ping_ms;
    bool _ha_discovery_sent;
    bool _connected;
    bool _waiting_pingresp;
    uint16_t _next_packet_id;
    uint16_t _broker_port;
    IPAddress _broker_ip;
    TCPClient _tcp;
    CommandInbox* _inbox;

    void buildTopics(const char* mdns_host);
    void reconnect();
    void disconnectBroker(bool graceful);
    bool connectBroker();
    bool sendConnectPacket();
    bool publishPacket(const char* topic, const char* payload, bool retain);
    bool subscribeTopic(const char* topic);
    bool sendPing();
    bool sendPacket(const uint8_t* data, size_t len);
    bool readPacket(uint8_t* type, uint8_t* payload, size_t payload_cap, size_t* payload_len);
    bool readRemainingLength(size_t* remaining);
    bool readExact(uint8_t* out, size_t len, uint32_t timeout_ms);
    int readByte(uint32_t timeout_ms);
    uint16_t nextPacketId();
    void serviceIncoming();
    void sendHaDiscovery();
    void handleMessage(const char* topic, const char* payload);
};
