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

// ---------------------------------------------------------------------------
// ReliableTcpPrint — wraps TCPClient to ensure `print()` calls block and
// loop correctly if the underlying TX buffer is full, preventing silent chunk
// dropping for payloads > TX buffer size (e.g. 512B).
// ---------------------------------------------------------------------------
class ReliableTcpPrint : public Print {
    TCPClient& _tcp;
public:
    ReliableTcpPrint(TCPClient& tcp) : _tcp(tcp) {}
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* buffer, size_t size) override {
        size_t sent = 0;
        uint32_t deadline = millis() + 5000;
        while (sent < size && _tcp.connected()) {
            int wrote = _tcp.write(buffer + sent, size - sent);
            if (wrote > 0) {
                sent += wrote;
            } else {
                if ((int32_t)(millis() - deadline) > 0) break;
                delay(1);
            }
        }
        return sent;
    }
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
    uint16_t topic_len = (uint16_t)((body[pos] << 8) | body[pos + 1]);
    pos += 2;
    if (pos + topic_len > body_len) return false;
    size_t copy_topic = topic_len < (topic_out_len - 1) ? topic_len : (topic_out_len - 1);
    memcpy(topic_out, body + pos, copy_topic);
    topic_out[copy_topic] = '\0';
    pos += topic_len;

    uint8_t qos = (header >> 1) & 0x03;
    if (qos > 0) {
        if (pos + 2 > body_len) return false;
        pos += 2;
    }

    size_t payload_len = body_len - pos;
    size_t copy_payload = payload_len < (payload_out_len - 1) ? payload_len : (payload_out_len - 1);
    if (copy_payload > 0) memcpy(payload_out, body + pos, copy_payload);
    payload_out[copy_payload] = '\0';
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
    snprintf(client_id, sizeof(client_id), "localair-%s", identity[0] ? identity : "probe");
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

    uint8_t hdr = 0x10; 
    probe.write(&hdr, 1);
    uint8_t lenbuf[4];
    size_t lenlen = encodeRemainingLength(lenbuf, body_len);
    probe.write(lenbuf, lenlen);

    auto writeMqtt = [&](const char* s) {
        size_t len = strlen(s);
        uint8_t lhdr[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
        probe.write(lhdr, 2);
        probe.write(reinterpret_cast<const uint8_t*>(s), len);
    };

    writeMqtt("MQTT");
    uint8_t proto_lvl = 0x04; probe.write(&proto_lvl, 1);
    uint8_t flags = 0x02; // clean session
    if (user) flags |= 0x80;
    if (pass) flags |= 0x40;
    probe.write(&flags, 1);
    uint8_t ka[2] = { 0x00, 0x1E }; // 30s
    probe.write(ka, 2);

    writeMqtt(client_id);
    if (user) writeMqtt(user);
    if (pass) writeMqtt(pass);

    uint8_t header = 0;
    int ch = -1;
    uint32_t deadline = millis() + 3000;
    while ((int32_t)(millis() - deadline) < 0) {
        ch = probe.read();
        if (ch >= 0) break;
        delay(1);
    }
    if (ch < 0) {
        probe.stop();
        safeCopy(result, result_len, "MQTT CONNACK timeout");
        return false;
    }
    header = (uint8_t)ch;
    if ((header >> 4) != 2) {
        probe.stop();
        safeCopy(result, result_len, "MQTT CONNACK missing");
        return false;
    }

    size_t multiplier = 1;
    size_t remaining_len = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        uint32_t byte_deadline = millis() + 1000;
        int rl = -1;
        while ((int32_t)(millis() - byte_deadline) < 0) {
            rl = probe.read();
            if (rl >= 0) break;
            delay(1);
        }
        if (rl < 0) {
            probe.stop();
            safeCopy(result, result_len, "MQTT CONNACK timeout");
            return false;
        }
        remaining_len += (size_t)(rl & 127) * multiplier;
        if ((rl & 128) == 0) break;
        multiplier *= 128;
    }

    uint8_t resp[4] = {0};
    size_t resp_len = 0;
    while (resp_len < remaining_len && resp_len < sizeof(resp)) {
        int byte = probe.read();
        if (byte < 0) {
            probe.stop();
            safeCopy(result, result_len, "MQTT CONNACK read failed");
            return false;
        }
        resp[resp_len++] = (uint8_t)byte;
    }
    if (remaining_len < 2 || resp[1] != 0x00) {
        probe.stop();
        safeCopy(result, result_len, "MQTT broker rejected CONNECT");
        return false;
    }

    uint8_t disc[2] = {0xE0, 0x00};
    probe.write(disc, sizeof(disc));
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
    size_t sent = 0;
    while (sent < len) {
        int wrote = _tcp.write(data + sent, len - sent);
        if (wrote <= 0) return false;
        sent += (size_t)wrote;
    }
    _last_tx_ms = millis();
    return true;
}

bool MqttClient::readExact(uint8_t* out, size_t len, uint32_t timeout_ms) {
    if (!out || len == 0) return true;
    uint32_t deadline = millis() + timeout_ms;
    size_t pos = 0;
    while (pos < len) {
        while (_tcp.available() > 0 && pos < len) {
            int v = _tcp.read();
            if (v < 0) break;
            out[pos++] = (uint8_t)v;
        }
        if (pos >= len) break;
        if ((int32_t)(millis() - deadline) >= 0) return false;
        delay(1);
    }
    return true;
}

int MqttClient::readByte(uint32_t timeout_ms) {
    uint8_t b = 0;
    if (!readExact(&b, 1, timeout_ms)) return -1;
    return (int)b;
}

bool MqttClient::readRemainingLength(size_t* remaining) {
    if (!remaining) return false;
    size_t multiplier = 1;
    size_t value = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        int ch = readByte(1000);
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

bool MqttClient::readPacket(uint8_t* type, uint8_t* payload, size_t payload_cap, size_t* payload_len) {
    if (type) *type = 0;
    if (payload_len) *payload_len = 0;
    int header = readByte(1000);
    if (header < 0) return false;

    size_t remaining = 0;
    if (!readRemainingLength(&remaining)) return false;
    if (remaining > payload_cap) {
        // Drain oversized packet.
        uint8_t discard[32];
        size_t left = remaining;
        while (left > 0) {
            size_t step = left < sizeof(discard) ? left : sizeof(discard);
            if (!readExact(discard, step, 1000)) return false;
            left -= step;
        }
        return false;
    }
    if (!readExact(payload, remaining, 1000)) return false;
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

    // Fixed Header
    uint8_t hdr = 0x10;
    _tcp.write(&hdr, 1);

    uint8_t lenbuf[4];
    size_t lenlen = encodeRemainingLength(lenbuf, body_len);
    if (_tcp.write(lenbuf, lenlen) != lenlen) return false;

    auto writeMqttString = [&](const char* s) {
        size_t len = strlen(s);
        uint8_t len_hdr[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
        _tcp.write(len_hdr, 2);
        _tcp.write(reinterpret_cast<const uint8_t*>(s), len);
    };

    writeMqttString("MQTT");
    uint8_t proto_lvl = 0x04;
    _tcp.write(&proto_lvl, 1);
    _tcp.write(&flags, 1);
    uint16_t ka = (uint16_t)KEEPALIVE_MS / 1000;
    uint8_t ka_hdr[2] = { (uint8_t)(ka >> 8), (uint8_t)(ka & 0xFF) };
    _tcp.write(ka_hdr, 2);

    writeMqttString(_client_id);
    if (_avail_topic[0]) {
        writeMqttString(_avail_topic);
        writeMqttString("offline");
    }
    if (user) writeMqttString(user);
    if (pass) writeMqttString(pass);

    _last_tx_ms = millis();
    return true;
}

bool MqttClient::publishPacket(const char* topic, const char* payload, bool retain) {
    if (!topic || !payload || !_tcp.connected()) return false;

    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t remaining_length = 2 + topic_len + payload_len;

    // Stream Fixed Header
    uint8_t hdr = 0x30 | (retain ? 0x01 : 0x00);
    if (_tcp.write(&hdr, 1) != 1) return false;

    uint8_t lenbuf[4];
    size_t lenlen = encodeRemainingLength(lenbuf, remaining_length);
    if (_tcp.write(lenbuf, lenlen) != lenlen) return false;

    // Stream Topic
    uint8_t topic_hdr[2] = {
        (uint8_t)((topic_len >> 8) & 0xFF),
        (uint8_t)(topic_len & 0xFF)
    };
    if (_tcp.write(topic_hdr, 2) != 2) return false;
    if (_tcp.write(reinterpret_cast<const uint8_t*>(topic), topic_len) != topic_len) return false;

    // Stream Payload
    if (_tcp.write(reinterpret_cast<const uint8_t*>(payload), payload_len) != payload_len) return false;

    _last_tx_ms = millis();
    return true;
}

bool MqttClient::subscribeTopic(const char* topic) {
    if (!topic || !_tcp.connected()) return false;

    size_t topic_len = strlen(topic);
    size_t remaining_length = 5 + topic_len; // PID(2) + TopicLen(2) + Topic + QoS(1)

    // Stream Header
    uint8_t hdr = 0x82;
    if (_tcp.write(&hdr, 1) != 1) return false;

    uint8_t lenbuf[4];
    size_t lenlen = encodeRemainingLength(lenbuf, remaining_length);
    if (_tcp.write(lenbuf, lenlen) != lenlen) return false;

    // Stream Body
    uint16_t pid = nextPacketId();
    uint8_t pid_hdr[2] = { (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF) };
    if (_tcp.write(pid_hdr, 2) != 2) return false;

    uint8_t topic_hdr[2] = { (uint8_t)(topic_len >> 8), (uint8_t)(topic_len & 0xFF) };
    if (_tcp.write(topic_hdr, 2) != 2) return false;
    if (_tcp.write(reinterpret_cast<const uint8_t*>(topic), topic_len) != topic_len) return false;

    uint8_t qos = 0x00;
    if (_tcp.write(&qos, 1) != 1) return false;

    _last_tx_ms = millis();
    return true;
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
    if ((type >> 4) != 2 || payload_len < 2 || payload[1] != 0x00) {
        disconnectBroker(false);
        return false;
    }

    _connected = true;
    _waiting_pingresp = false;
    _last_tx_ms = millis();
    _last_ping_ms = millis();

    publishPacket(_avail_topic, "online", true);
    subscribeTopic(_cmd_topic);
    subscribeTopic(_pct_cmd_topic);
    if (!_ha_discovery_sent) {
        sendHaDiscovery();
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
    while (_tcp.available() > 0) {
        uint8_t type = 0;
        uint8_t payload[RX_LEN];
        size_t payload_len = 0;
        if (!readPacket(&type, payload, sizeof(payload), &payload_len)) {
            disconnectBroker(false);
            return;
        }
        switch (type >> 4) {
            case 3: {
                char topic[TOPIC_LEN];
                char msg[RX_LEN];
                if (readTopicPayload(type, payload, payload_len, topic, sizeof(topic), msg, sizeof(msg))) {
                    handleMessage(topic, msg);
                }
                break;
            }
            case 9:
                // SUBACK
                break;
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

    // Pass 2: stream directly to TCP — no large intermediate buffer.
    if (!_tcp.connected()) return;
    if (_tcp.write(hdr, hdr_pos) != hdr_pos) return;
    if (_tcp.write(topic_hdr, 2) != 2) return;
    if (_tcp.write(reinterpret_cast<const uint8_t*>(_state_topic), topic_len) != topic_len) return;
    
    ReliableTcpPrint rprint(_tcp);
    renderPublicStatusJson(rprint, view);
    _last_tx_ms = millis();
}

void MqttClient::handleMessage(const char* topic, const char* payload) {
    if (!topic || !payload) return;
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

void MqttClient::sendHaDiscovery() {
    if (!_mdns_host || !_identity[0] || !_tcp.connected()) return;

    auto publishDiscoveryStream = [&](const char* component, const char* sub_id, auto body_renderer) {
        char topic[TOPIC_LEN];
        snprintf(topic, sizeof(topic), "homeassistant/%s/localair_%s_%s/config", component, _identity, sub_id);

        // Pass 1: Measure payload
        CountingPrint counter;
        body_renderer(counter);
        size_t payload_len = counter.count;
        size_t topic_len = strlen(topic);
        size_t rem_len = 2 + topic_len + payload_len;

        // Pass 2: Stream packet
        uint8_t hdr = 0x30 | 0x01; // Publish + Retain
        if (_tcp.write(&hdr, 1) != 1) return;
        uint8_t lenbuf[4];
        size_t lenlen = encodeRemainingLength(lenbuf, rem_len);
        if (_tcp.write(lenbuf, lenlen) != lenlen) return;

        uint8_t t_hdr[2] = { (uint8_t)(topic_len >> 8), (uint8_t)(topic_len & 0xFF) };
        _tcp.write(t_hdr, 2);
        _tcp.write(reinterpret_cast<const uint8_t*>(topic), topic_len);

        body_renderer(_tcp);
        _last_tx_ms = millis();
    };

    // Component: VOC Sensor
    publishDiscoveryStream("sensor", "voc", [&](Print& p) {
        p.print("{\"name\":\""); p.print(_identity); p.print(" VOC\",\"state_topic\":\""); p.print(_state_topic);
        p.print("\",\"availability_topic\":\""); p.print(_avail_topic);
        p.print("\",\"value_template\":\"{{ value_json.sensors.voc_raw | int }}\",\"unit_of_measurement\":\"ppb\",");
        p.print("\"unique_id\":\"localair_"); p.print(_identity); p.print("_voc\",\"object_id\":\"localair_");
        p.print(_identity); p.print("_voc\"}");
    });

    // Component: PM2.5 Sensor
    publishDiscoveryStream("sensor", "pm25", [&](Print& p) {
        p.print("{\"name\":\""); p.print(_identity); p.print(" PM2.5\",\"state_topic\":\""); p.print(_state_topic);
        p.print("\",\"availability_topic\":\""); p.print(_avail_topic);
        p.print("\",\"value_template\":\"{{ value_json.sensors.pm25 }}\",\"unit_of_measurement\":\"ug/m3\",");
        p.print("\"unique_id\":\"localair_"); p.print(_identity); p.print("_pm25\",\"object_id\":\"localair_");
        p.print(_identity); p.print("_pm25\"}");
    });

    // Component: Fan
    publishDiscoveryStream("fan", "fan", [&](Print& p) {
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
    });
}
