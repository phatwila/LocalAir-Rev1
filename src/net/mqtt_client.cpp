#include "mqtt_client.h"
#include "json_utils.h"
#include "mqtt_input.h"
#include "public_status_renderer.h"
#include "../algo/fan_speed.h"
#include "../sys/public_views.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

// ---------------------------------------------------------------------------
// CountingPrint — measures the exact byte length of a render pass without
// allocating any output buffer. Implementation-local, never exposed in the
// header.
// ---------------------------------------------------------------------------
class CountingPrint : public Print {
public:
    size_t count = 0;
    size_t write(uint8_t) override { ++count; return 1; }
    size_t write(const uint8_t*, size_t n) override { count += n; return n; }
};

static bool deadlineReached(uint32_t deadline) {
    return (int32_t)(millis() - deadline) >= 0;
}

static bool remainingMs(uint32_t deadline, uint32_t* out) {
    const uint32_t now = millis();
    if ((int32_t)(now - deadline) >= 0) return false;
    *out = deadline - now;
    return true;
}

// All writes for one packet share an absolute deadline. TCP write timeouts
// bound lower-level waits; retries cannot extend the packet deadline.
static bool writeAll(TCPClient& tcp, const uint8_t* data, size_t len, uint32_t deadline) {
    size_t sent = 0;
    while (sent < len && tcp.connected()) {
        uint32_t timeout_ms = 0;
        if (!remainingMs(deadline, &timeout_ms)) break;
        const int wrote = (int)tcp.write(data + sent, len - sent, timeout_ms);
        if (wrote > 0) sent += (size_t)wrote;
        else if (!deadlineReached(deadline)) delay(1);
    }
    return sent == len;
}

static int readByteUntil(TCPClient& tcp, uint32_t deadline) {
    while (!deadlineReached(deadline)) {
        const int value = tcp.read();
        if (value >= 0) return value;
        delay(1);
    }
    return -1;
}

// ReliableTcpPrint retains the streaming renderer while propagating failures.
class ReliableTcpPrint : public Print {
    TCPClient& _tcp;
    uint32_t _deadline;
    bool _ok = true;
public:
    ReliableTcpPrint(TCPClient& tcp, uint32_t deadline) : _tcp(tcp), _deadline(deadline) {}
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* buffer, size_t size) override {
        if (!_ok || !writeAll(_tcp, buffer, size, _deadline)) {
            _ok = false;
            return 0;
        }
        return size;
    }
    bool ok() const { return _ok; }
};

static void safeCopy(char* out, size_t out_len, const char* value) {
    if (!out || out_len == 0) return;
    if (!value) value = "";
    strncpy(out, value, out_len - 1);
    out[out_len - 1] = '\0';
}

static void copyMqttIdentity(const char* input, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!input || !input[0] || strcmp(input, "pending") == 0) {
        safeCopy(out, out_len, "unknown");
        return;
    }
    safeCopy(out, out_len, input);
}

static size_t encodeRemainingLength(uint8_t* out, size_t value) {
    size_t pos = 0;
    do {
        uint8_t byte = (uint8_t)(value % 128);
        value /= 128;
        if (value > 0) byte |= 0x80;
        out[pos++] = byte;
    } while (value > 0 && pos < 4);
    return pos;
}

static bool readTopicPayload(uint8_t header, const uint8_t* body, size_t body_len,
                             char* topic_out, size_t topic_out_len,
                             char* payload_out, size_t payload_out_len) {
    if (!topic_out || topic_out_len == 0 || !payload_out || payload_out_len == 0) return false;
    if (body_len < 2) return false;
    size_t pos = 0;
    const uint16_t topic_len = (uint16_t)((body[pos] << 8) | body[pos + 1]);
    pos += 2;
    if (topic_len == 0 || topic_len >= topic_out_len || pos + topic_len > body_len ||
        memchr(body + pos, '\0', topic_len)) return false;
    memcpy(topic_out, body + pos, topic_len);
    topic_out[topic_len] = '\0';
    pos += topic_len;

    const uint8_t qos = (header >> 1) & 0x03;
    if (qos > 0) {
        if (pos + 2 > body_len) return false;
        pos += 2;
    }

    const size_t payload_len = body_len - pos;
    if (payload_len >= payload_out_len || memchr(body + pos, '\0', payload_len)) return false;
    if (payload_len > 0) memcpy(payload_out, body + pos, payload_len);
    payload_out[payload_len] = '\0';
    return true;
}

} // namespace

MqttClient::MqttClient(Config* config, CommandInbox* inbox)
    : _config(config),
      _mdns_host(nullptr),
      _last_connect_attempt(0),
      _last_tx_ms(0),
      _last_ping_ms(0),
      _ha_discovery_sent(false),
      _connected(false),
      _waiting_pingresp(false),
      _next_packet_id(1),
      _broker_port(1883),
      _broker_ip(0, 0, 0, 0),
      _inbox(inbox) {
    memset(_identity, 0, sizeof(_identity));
    memset(_state_topic, 0, sizeof(_state_topic));
    memset(_cmd_topic, 0, sizeof(_cmd_topic));
    memset(_pct_cmd_topic, 0, sizeof(_pct_cmd_topic));
    memset(_avail_topic, 0, sizeof(_avail_topic));
    memset(_client_id, 0, sizeof(_client_id));
}

void MqttClient::buildTopics(const char* mdns_host) {
    copyMqttIdentity(mdns_host, _identity, sizeof(_identity));
    snprintf(_state_topic, sizeof(_state_topic), "localair-rev1/%s/state", _identity);
    snprintf(_cmd_topic, sizeof(_cmd_topic), "localair-rev1/%s/cmd", _identity);
    snprintf(_pct_cmd_topic, sizeof(_pct_cmd_topic), "localair-rev1/%s/cmd/percentage", _identity);
    snprintf(_avail_topic, sizeof(_avail_topic), "localair-rev1/%s/available", _identity);
    snprintf(_client_id, sizeof(_client_id), "localair-%s", _identity);
}

void MqttClient::begin(const char* mdns_host) {
    _mdns_host = mdns_host;
    buildTopics(mdns_host);
    _ha_discovery_sent = false;
    _waiting_pingresp = false;
    _next_packet_id = 1;
    _last_tx_ms = millis();
    _last_ping_ms = 0;
    _last_connect_attempt = 0;

    if (!_config || !_config->mqtt_enabled) {
        disconnectBroker(false);
        return;
    }

    _broker_port = _config->mqtt_port ? _config->mqtt_port : 1883;
    _broker_ip = IPAddress((uint8_t)((_config->mqtt_broker_ip >> 24) & 0xFF),
                           (uint8_t)((_config->mqtt_broker_ip >> 16) & 0xFF),
                           (uint8_t)((_config->mqtt_broker_ip >> 8) & 0xFF),
                           (uint8_t)(_config->mqtt_broker_ip & 0xFF));
    reconnect();
}

bool MqttClient::connected() {
    return _connected && _tcp.connected();
}

bool MqttClient::testConnection(const Config& config, char* result, size_t result_len) {
    if (result && result_len > 0) result[0] = '\0';

    if (config.mqtt_broker_ip == 0) {
        safeCopy(result, result_len, "MQTT broker address is empty");
        return false;
    }

    IPAddress broker((uint8_t)((config.mqtt_broker_ip >> 24) & 0xFF),
                     (uint8_t)((config.mqtt_broker_ip >> 16) & 0xFF),
                     (uint8_t)((config.mqtt_broker_ip >> 8) & 0xFF),
                     (uint8_t)(config.mqtt_broker_ip & 0xFF));

    char identity[24];
    copyMqttIdentity(_mdns_host, identity, sizeof(identity));
    char client_id[CLIENT_ID_LEN];
    snprintf(client_id, sizeof(client_id), "probe-localair-%s", identity[0] ? identity : "unknown");
    char avail_topic[TOPIC_LEN];
    snprintf(avail_topic, sizeof(avail_topic), "localair-rev1/%s/available", identity[0] ? identity : "probe");

    TCPClient probe;
    uint32_t port = config.mqtt_port ? config.mqtt_port : 1883;
    if (!probe.connect(broker, port)) {
        safeCopy(result, result_len, "MQTT TCP connect failed");
        return false;
    }

    const char* user = (config.mqtt_user[0]) ? config.mqtt_user : nullptr;
    const char* pass = (config.mqtt_pass[0]) ? config.mqtt_pass : nullptr;

    size_t body_len = 0;
    body_len += 2 + 4; // "MQTT"
    body_len += 1;     // proto lvl
    body_len += 1;     // flags
    body_len += 2;     // keepalive
    body_len += (2 + strlen(client_id));
    if (user) body_len += (2 + strlen(user));
    if (pass) body_len += (2 + strlen(pass));

    const uint32_t deadline = millis() + 3000;
    auto writeProbe = [&](const uint8_t* data, size_t len) {
        return writeAll(probe, data, len, deadline);
    };
    auto writeMqtt = [&](const char* s) {
        const size_t len = strlen(s);
        const uint8_t lhdr[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
        return writeProbe(lhdr, 2) && writeProbe(reinterpret_cast<const uint8_t*>(s), len);
    };

    const uint8_t hdr = 0x10;
    uint8_t lenbuf[4];
    const size_t lenlen = encodeRemainingLength(lenbuf, body_len);
    const uint8_t proto_lvl = 0x04;
    uint8_t flags = 0x02; // clean session
    if (user) flags |= 0x80;
    if (pass) flags |= 0x40;
    const uint8_t ka[2] = { 0x00, 0x1E }; // 30s
    const bool sent = writeProbe(&hdr, 1) && writeProbe(lenbuf, lenlen) &&
                      writeMqtt("MQTT") && writeProbe(&proto_lvl, 1) &&
                      writeProbe(&flags, 1) && writeProbe(ka, 2) &&
                      writeMqtt(client_id) && (!user || writeMqtt(user)) &&
                      (!pass || writeMqtt(pass));
    if (!sent) {
        probe.stop();
        safeCopy(result, result_len, "MQTT CONNECT write failed");
        return false;
    }

    const int header = readByteUntil(probe, deadline);
    if (header < 0) {
        probe.stop();
        safeCopy(result, result_len, "MQTT CONNACK timeout");
        return false;
    }
    if ((header >> 4) != 2) {
        probe.stop();
        safeCopy(result, result_len, "MQTT CONNACK missing");
        return false;
    }

    size_t multiplier = 1;
    size_t remaining_len = 0;
    bool valid_remaining = false;
    for (uint8_t i = 0; i < 4; ++i) {
        const int rl = readByteUntil(probe, deadline);
        if (rl < 0) {
            probe.stop();
            safeCopy(result, result_len, "MQTT CONNACK timeout");
            return false;
        }
        remaining_len += (size_t)(rl & 127) * multiplier;
        if ((rl & 128) == 0) {
            valid_remaining = true;
            break;
        }
        multiplier *= 128;
    }

    uint8_t resp[2] = {0};
    for (size_t i = 0; i < remaining_len && i < sizeof(resp); ++i) {
        const int byte = readByteUntil(probe, deadline);
        if (byte < 0) {
            probe.stop();
            safeCopy(result, result_len, "MQTT CONNACK timeout");
            return false;
        }
        resp[i] = (uint8_t)byte;
    }
    if (!valid_remaining || remaining_len != 2 || resp[1] != 0x00) {
        probe.stop();
        safeCopy(result, result_len, "MQTT broker rejected CONNECT");
        return false;
    }

    const uint8_t disc[2] = {0xE0, 0x00};
    writeProbe(disc, sizeof(disc));
    probe.stop();
    safeCopy(result, result_len, "MQTT broker accepted CONNECT");
    return true;
}

uint16_t MqttClient::nextPacketId() {
    if (++_next_packet_id == 0) _next_packet_id = 1;
    return _next_packet_id;
}

bool MqttClient::sendPacket(const uint8_t* data, size_t len) {
    if (!_tcp.connected() || !data || len == 0) return false;
    if (!writeAll(_tcp, data, len, millis() + TX_PACKET_TIMEOUT_MS)) return false;
    _last_tx_ms = millis();
    return true;
}

bool MqttClient::readExact(uint8_t* out, size_t len, uint32_t deadline) {
    if (!out || len == 0) return true;
    size_t pos = 0;
    while (pos < len && !deadlineReached(deadline)) {
        const int value = _tcp.read();
        if (value >= 0) out[pos++] = (uint8_t)value;
        else delay(1);
    }
    return pos == len;
}

int MqttClient::readByte(uint32_t deadline) {
    uint8_t b = 0;
    if (!readExact(&b, 1, deadline)) return -1;
    return (int)b;
}

bool MqttClient::readRemainingLength(size_t* remaining, uint32_t deadline) {
    if (!remaining) return false;
    size_t multiplier = 1;
    size_t value = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        const int ch = readByte(deadline);
        if (ch < 0) return false;
        value += (size_t)(ch & 127) * multiplier;
        if ((ch & 128) == 0) {
            *remaining = value;
            return true;
        }
        multiplier *= 128;
    }
    return false;
}

bool MqttClient::readPacket(uint8_t* type, uint8_t* payload, size_t payload_cap, size_t* payload_len,
                            uint32_t timeout_ms) {
    if (type) *type = 0;
    if (payload_len) *payload_len = 0;
    if (!payload && payload_cap > 0) return false;
    const uint32_t deadline = millis() + timeout_ms;
    const int header = readByte(deadline);
    if (header < 0) return false;

    size_t remaining = 0;
    if (!readRemainingLength(&remaining, deadline)) return false;
    // Never drain an oversized frame. Close it before it can monopolize the loop.
    if (remaining > payload_cap) return false;
    if (!readExact(payload, remaining, deadline)) return false;
    if (type) *type = (uint8_t)header;
    if (payload_len) *payload_len = remaining;
    return true;
}

bool MqttClient::sendConnectPacket() {
    if (!_tcp.connected()) return false;

    const char* user = (_config && _config->mqtt_user[0]) ? _config->mqtt_user : nullptr;
    const char* pass = (_config && _config->mqtt_pass[0]) ? _config->mqtt_pass : nullptr;

    uint8_t flags = 0x02; // clean session
    if (_avail_topic[0]) flags |= 0x04 | 0x20; // will + retain
    if (user) flags |= 0x80;
    if (pass) flags |= 0x40;

    size_t body_len = 0;
    body_len += 2 + 4; // "MQTT"
    body_len += 1;     // protocol level
    body_len += 1;     // flags
    body_len += 2;     // keepalive
    body_len += (2 + strlen(_client_id));
    if (_avail_topic[0]) {
        body_len += (2 + strlen(_avail_topic));
        body_len += (2 + 7); // "offline"
    }
    if (user) body_len += (2 + strlen(user));
    if (pass) body_len += (2 + strlen(pass));

    const uint32_t deadline = millis() + TX_PACKET_TIMEOUT_MS;
    auto write = [&](const uint8_t* data, size_t len) {
        return writeAll(_tcp, data, len, deadline);
    };
    auto writeMqttString = [&](const char* s) {
        const size_t len = strlen(s);
        const uint8_t len_hdr[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
        return write(len_hdr, 2) && write(reinterpret_cast<const uint8_t*>(s), len);
    };

    const uint8_t hdr = 0x10;
    uint8_t lenbuf[4];
    const size_t lenlen = encodeRemainingLength(lenbuf, body_len);
    const uint8_t proto_lvl = 0x04;
    const uint16_t ka = (uint16_t)KEEPALIVE_MS / 1000;
    const uint8_t ka_hdr[2] = { (uint8_t)(ka >> 8), (uint8_t)(ka & 0xFF) };
    const bool sent = write(&hdr, 1) && write(lenbuf, lenlen) &&
                      writeMqttString("MQTT") && write(&proto_lvl, 1) &&
                      write(&flags, 1) && write(ka_hdr, 2) &&
                      writeMqttString(_client_id) &&
                      (!_avail_topic[0] || (writeMqttString(_avail_topic) && writeMqttString("offline"))) &&
                      (!user || writeMqttString(user)) && (!pass || writeMqttString(pass));
    if (!sent) return false;

    _last_tx_ms = millis();
    return true;
}

bool MqttClient::publishPacket(const char* topic, const char* payload, bool retain) {
    if (!topic || !payload || !_tcp.connected()) return false;

    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t remaining_length = 2 + topic_len + payload_len;

    const uint32_t deadline = millis() + TX_PACKET_TIMEOUT_MS;
    const uint8_t hdr = 0x30 | (retain ? 0x01 : 0x00);
    uint8_t lenbuf[4];
    const size_t lenlen = encodeRemainingLength(lenbuf, remaining_length);
    const uint8_t topic_hdr[2] = {
        (uint8_t)((topic_len >> 8) & 0xFF),
        (uint8_t)(topic_len & 0xFF)
    };
    const bool sent = writeAll(_tcp, &hdr, 1, deadline) &&
                      writeAll(_tcp, lenbuf, lenlen, deadline) &&
                      writeAll(_tcp, topic_hdr, 2, deadline) &&
                      writeAll(_tcp, reinterpret_cast<const uint8_t*>(topic), topic_len, deadline) &&
                      writeAll(_tcp, reinterpret_cast<const uint8_t*>(payload), payload_len, deadline);
    if (!sent) return false;

    _last_tx_ms = millis();
    return true;
}

bool MqttClient::subscribeTopic(const char* topic, uint16_t* packet_id) {
    if (!topic || !packet_id || !_tcp.connected()) return false;

    const size_t topic_len = strlen(topic);
    const size_t remaining_length = 5 + topic_len; // PID(2) + TopicLen(2) + Topic + QoS(1)
    const uint32_t deadline = millis() + TX_PACKET_TIMEOUT_MS;
    const uint8_t hdr = 0x82;
    uint8_t lenbuf[4];
    const size_t lenlen = encodeRemainingLength(lenbuf, remaining_length);
    const uint16_t pid = nextPacketId();
    const uint8_t pid_hdr[2] = { (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF) };
    const uint8_t topic_hdr[2] = { (uint8_t)(topic_len >> 8), (uint8_t)(topic_len & 0xFF) };
    const uint8_t qos = 0x00;
    const bool sent = writeAll(_tcp, &hdr, 1, deadline) &&
                      writeAll(_tcp, lenbuf, lenlen, deadline) &&
                      writeAll(_tcp, pid_hdr, 2, deadline) &&
                      writeAll(_tcp, topic_hdr, 2, deadline) &&
                      writeAll(_tcp, reinterpret_cast<const uint8_t*>(topic), topic_len, deadline) &&
                      writeAll(_tcp, &qos, 1, deadline);
    if (!sent) return false;

    *packet_id = pid;
    _last_tx_ms = millis();
    return true;
}

bool MqttClient::awaitSubscriptions(uint16_t cmd_packet_id, uint16_t pct_packet_id) {
    bool cmd_accepted = false;
    bool pct_accepted = false;
    const uint32_t deadline = millis() + SUBACK_TIMEOUT_MS;
    while ((!cmd_accepted || !pct_accepted) && !deadlineReached(deadline)) {
        uint8_t type = 0;
        uint8_t payload[RX_LEN];
        size_t payload_len = 0;
        uint32_t left = 0;
        if (!remainingMs(deadline, &left)) return false;
        const uint32_t timeout = left < RX_PACKET_TIMEOUT_MS ? left : RX_PACKET_TIMEOUT_MS;
        if (!readPacket(&type, payload, sizeof(payload), &payload_len, timeout)) return false;
        if (type == 0x90) {
            if (payload_len != 3) return false;
            const uint16_t packet_id = ((uint16_t)payload[0] << 8) | payload[1];
            if (packet_id != cmd_packet_id && packet_id != pct_packet_id) continue;
            if (payload[2] != 0x00) return false;
            if (packet_id == cmd_packet_id) cmd_accepted = true;
            if (packet_id == pct_packet_id) pct_accepted = true;
        } else if ((type >> 4) == 3) {
            char topic[TOPIC_LEN];
            char msg[RX_LEN];
            if (!readTopicPayload(type, payload, payload_len, topic, sizeof(topic), msg, sizeof(msg))) return false;
            handleMessage(topic, msg, (type & 0x01) != 0);
        } else if ((type >> 4) == 13) {
            _waiting_pingresp = false;
        }
    }
    return cmd_accepted && pct_accepted;
}

bool MqttClient::sendPing() {
    uint8_t packet[2] = {0xC0, 0x00};
    _waiting_pingresp = true;
    _last_ping_ms = millis();
    return sendPacket(packet, sizeof(packet));
}

void MqttClient::disconnectBroker(bool graceful) {
    if (graceful && _connected) {
        publishPacket(_avail_topic, "offline", true);
    }
    _connected = false;
    _waiting_pingresp = false;
    if (_tcp.connected()) {
        _tcp.stop();
    }
}

bool MqttClient::connectBroker() {
    if (!_config || !_config->mqtt_enabled) return false;
    if (_broker_port == 0) _broker_port = 1883;
    if (_config->mqtt_broker_ip == 0) return false;

    if (_tcp.connected()) _tcp.stop();
    if (!_tcp.connect(_broker_ip, _broker_port)) {
        return false;
    }

    if (!sendConnectPacket()) {
        disconnectBroker(false);
        return false;
    }

    uint8_t type = 0;
    uint8_t payload[16];
    size_t payload_len = 0;
    if (!readPacket(&type, payload, sizeof(payload), &payload_len)) {
        disconnectBroker(false);
        return false;
    }
    if ((type >> 4) != 2 || payload_len != 2 || payload[1] != 0x00) {
        disconnectBroker(false);
        return false;
    }

    uint16_t cmd_packet_id = 0;
    uint16_t pct_packet_id = 0;
    if (!subscribeTopic(_cmd_topic, &cmd_packet_id) ||
        !subscribeTopic(_pct_cmd_topic, &pct_packet_id) ||
        !awaitSubscriptions(cmd_packet_id, pct_packet_id)) {
        disconnectBroker(false);
        return false;
    }

    _connected = true;
    _waiting_pingresp = false;
    _last_tx_ms = millis();
    _last_ping_ms = millis();

    if (!publishPacket(_avail_topic, "online", true)) {
        disconnectBroker(false);
        return false;
    }
    if (!_ha_discovery_sent) {
        if (!sendHaDiscovery()) {
            disconnectBroker(false);
            return false;
        }
        _ha_discovery_sent = true;
    }
    return true;
}

void MqttClient::reconnect() {
    if (!_config || !_config->mqtt_enabled) return;
    _last_connect_attempt = millis();
    connectBroker();
}

void MqttClient::serviceIncoming() {
    if (!_tcp.connected()) {
        disconnectBroker(false);
        return;
    }
    const uint32_t loop_deadline = millis() + RX_LOOP_BUDGET_MS;
    for (uint8_t count = 0;
         count < MAX_INCOMING_PACKETS_PER_LOOP && _tcp.available() > 0;
         ++count) {
        uint32_t left = 0;
        if (!remainingMs(loop_deadline, &left)) return;
        uint8_t type = 0;
        uint8_t payload[RX_LEN];
        size_t payload_len = 0;
        const uint32_t timeout = left < RX_PACKET_TIMEOUT_MS ? left : RX_PACKET_TIMEOUT_MS;
        if (!readPacket(&type, payload, sizeof(payload), &payload_len, timeout)) {
            disconnectBroker(false);
            return;
        }
        switch (type >> 4) {
            case 3: {
                char topic[TOPIC_LEN];
                char msg[RX_LEN];
                if (!readTopicPayload(type, payload, payload_len, topic, sizeof(topic), msg, sizeof(msg))) {
                    disconnectBroker(false);
                    return;
                }
                handleMessage(topic, msg, (type & 0x01) != 0);
                break;
            }
            case 13:
                _waiting_pingresp = false;
                break;
            default:
                break;
        }
    }
}

void MqttClient::loop() {
    if (!_config || !_config->mqtt_enabled) {
        if (_connected) disconnectBroker(true);
        return;
    }

    if (_tcp.connected() && !_connected) {
        _connected = true;
    }
    if (!_tcp.connected() && _connected) {
        disconnectBroker(false);
    }

    if (_connected) {
        serviceIncoming();
        uint32_t now = millis();
        if (_waiting_pingresp) {
            if ((uint32_t)(now - _last_ping_ms) > KEEPALIVE_MS) {
                disconnectBroker(false);
                return;
            }
        } else if ((uint32_t)(now - _last_tx_ms) >= KEEPALIVE_MS) {
            if (!sendPing()) {
                disconnectBroker(false);
                return;
            }
        }
        return;
    }

    uint32_t now = millis();
    if ((uint32_t)(now - _last_connect_attempt) > RECONNECT_MS) {
        reconnect();
    }
}

void MqttClient::publishState(const PublicStatusView& view) {
    if (!_config || !_config->mqtt_enabled || !_connected) return;

    // Pass 1: measure exact payload length with zero allocation.
    CountingPrint counter;
    renderPublicStatusJson(counter, view);
    const size_t payload_len = counter.count;

    // Build the MQTT PUBLISH packet header directly from known lengths.
    // topic_field = 2-byte length prefix + topic bytes
    // remaining   = topic_field + payload_len
    const size_t topic_len = strlen(_state_topic);
    const size_t remaining = 2 + topic_len + payload_len;

    // Packet header: fixed byte + up to 4 remaining-length bytes
    uint8_t hdr[5];
    size_t hdr_pos = 0;
    hdr[hdr_pos++] = 0x31; // PUBLISH, retain=1, QoS=0
    uint8_t lenbuf[4];
    size_t lenlen = encodeRemainingLength(lenbuf, remaining);
    for (size_t i = 0; i < lenlen; ++i) hdr[hdr_pos++] = lenbuf[i];

    // Topic length prefix (big-endian u16)
    uint8_t topic_hdr[2] = {
        (uint8_t)((topic_len >> 8) & 0xFF),
        (uint8_t)(topic_len & 0xFF)
    };

    // Pass 2: every write shares one deadline; partial frames close the session.
    if (!_tcp.connected()) return;
    const uint32_t deadline = millis() + TX_PACKET_TIMEOUT_MS;
    ReliableTcpPrint rprint(_tcp, deadline);
    const bool sent = writeAll(_tcp, hdr, hdr_pos, deadline) &&
                      writeAll(_tcp, topic_hdr, 2, deadline) &&
                      writeAll(_tcp, reinterpret_cast<const uint8_t*>(_state_topic), topic_len, deadline);
    if (sent) renderPublicStatusJson(rprint, view);
    if (!sent || !rprint.ok()) {
        disconnectBroker(false);
        return;
    }
    _last_tx_ms = millis();
}

void MqttClient::handleMessage(const char* topic, const char* payload, bool retained_message) {
    if (!topic || !payload || retained_message) return;
    if (strcmp(topic, _cmd_topic) == 0) {
        MqttCommandIntent intent{};
        if (parseMqttCommandIntent(payload, &intent) && intent.kind == MQTT_COMMAND_SPEED) {
            int speed = intent.speed;
            if (speed >= 1 && speed <= 16 && _inbox) {
                _inbox->enqueue({CommandInbox::MANUAL, SOURCE_CODE_MQTT, (uint8_t)speed,
                                 fanPercentFromSpeed((uint8_t)speed, 1, 16)}, millis(), false);
            }
            return;
        }

        if (intent.kind == MQTT_COMMAND_MODE) {
            if (strcmp(intent.mode, "auto") == 0 && _inbox) {
                _inbox->enqueue({CommandInbox::AUTO, SOURCE_CODE_MQTT, 0, 0}, millis(), false);
            }
        }
        return;
    }

    if (strcmp(topic, _pct_cmd_topic) == 0) {
        int percent = -1;
        if ((jsonGetInt(payload, "percentage", &percent) || jsonGetInt(payload, "value", &percent) ||
             parsePlainInt(payload, &percent)) && percent >= 0 && percent <= 100 && _inbox) {
            uint8_t speed = fanSpeedFromPercent((uint8_t)percent, 1, 16);
            _inbox->enqueue({CommandInbox::MANUAL, SOURCE_CODE_MQTT, speed, (uint8_t)percent},
                            millis(), false);
        }
    }
}

bool MqttClient::sendHaDiscovery() {
    if (!_mdns_host || !_identity[0] || !_tcp.connected()) return false;

    auto publishDiscoveryStream = [&](const char* component, const char* sub_id, auto body_renderer) {
        char topic[TOPIC_LEN];
        snprintf(topic, sizeof(topic), "homeassistant/%s/localair_%s_%s/config", component, _identity, sub_id);

        // Pass 1: Measure payload
        CountingPrint counter;
        body_renderer(counter);
        size_t payload_len = counter.count;
        size_t topic_len = strlen(topic);
        size_t rem_len = 2 + topic_len + payload_len;

        // Pass 2: all writes share one deadline and propagate partial frames.
        const uint32_t deadline = millis() + TX_PACKET_TIMEOUT_MS;
        const uint8_t hdr = 0x30 | 0x01; // Publish + Retain
        uint8_t lenbuf[4];
        const size_t lenlen = encodeRemainingLength(lenbuf, rem_len);
        const uint8_t t_hdr[2] = { (uint8_t)(topic_len >> 8), (uint8_t)(topic_len & 0xFF) };
        ReliableTcpPrint body(_tcp, deadline);
        const bool sent = writeAll(_tcp, &hdr, 1, deadline) &&
                          writeAll(_tcp, lenbuf, lenlen, deadline) &&
                          writeAll(_tcp, t_hdr, 2, deadline) &&
                          writeAll(_tcp, reinterpret_cast<const uint8_t*>(topic), topic_len, deadline);
        if (sent) body_renderer(body);
        if (!sent || !body.ok()) return false;
        _last_tx_ms = millis();
        return true;
    };

    // Component: VOC Sensor
    if (!publishDiscoveryStream("sensor", "voc", [&](Print& p) {
        p.print("{\"name\":\""); p.print(_identity); p.print(" VOC\",\"state_topic\":\""); p.print(_state_topic);
        p.print("\",\"availability_topic\":\""); p.print(_avail_topic);
        p.print("\",\"value_template\":\"{{ value_json.sensors.voc_raw | int }}\",\"unit_of_measurement\":\"ppb\",");
        p.print("\"unique_id\":\"localair_"); p.print(_identity); p.print("_voc\",\"object_id\":\"localair_");
        p.print(_identity); p.print("_voc\"}");
    })) return false;
 
    // Component: PM2.5 Sensor
    if (!publishDiscoveryStream("sensor", "pm25", [&](Print& p) {
        p.print("{\"name\":\""); p.print(_identity); p.print(" PM2.5\",\"state_topic\":\""); p.print(_state_topic);
        p.print("\",\"availability_topic\":\""); p.print(_avail_topic);
        p.print("\",\"value_template\":\"{{ value_json.sensors.pm25 }}\",\"unit_of_measurement\":\"ug/m3\",");
        p.print("\"unique_id\":\"localair_"); p.print(_identity); p.print("_pm25\",\"object_id\":\"localair_");
        p.print(_identity); p.print("_pm25\"}");
    })) return false;
 
    // Component: Fan
    if (!publishDiscoveryStream("fan", "fan", [&](Print& p) {
        p.print("{\"name\":\""); p.print(_identity); p.print(" Fan\",\"state_topic\":\""); p.print(_state_topic);
        p.print("\",\"state_value_template\":\"{{ 'ON' if value_json.control.speed | int(0) > 0 else 'OFF' }}\",");
        p.print("\"availability_topic\":\""); p.print(_avail_topic);
        p.print("\",\"command_topic\":\""); p.print(_cmd_topic);
        p.print("\",\"percentage_state_topic\":\""); p.print(_state_topic);
        p.print("\",\"percentage_value_template\":\"{{ value_json.control.fan_percent }}\",");
        p.print("\"percentage_command_topic\":\""); p.print(_pct_cmd_topic);
        p.print("\",\"percentage_command_template\":\"{{ value }}\",\"speed_range_min\":1,\"speed_range_max\":16,");
        p.print("\"unique_id\":\"localair_"); p.print(_identity); p.print("_fan\",\"object_id\":\"localair_");
        p.print(_identity); p.print("_fan\",");
        p.print("\"payload_on\":\"{\\\"speed\\\":10}\",\"payload_off\":\"{\\\"mode\\\":\\\"auto\\\"}\"}");
    })) return false;
    return true;
}
