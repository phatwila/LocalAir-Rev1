#pragma once

#include "json_utils.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef bool (*MqttBrokerResolver)(const char* host, uint32_t* out_ip);

struct ParsedMqttBrokerField {
    bool present;
    bool valid;
    bool clear;
    uint32_t ip;
};

enum MqttCommandKind : uint8_t {
    MQTT_COMMAND_NONE = 0,
    MQTT_COMMAND_SPEED = 1,
    MQTT_COMMAND_MODE = 2,
};

struct MqttCommandIntent {
    MqttCommandKind kind;
    int speed;
    char mode[16];
};

inline bool resolveMqttBrokerHostname(const char* host, uint32_t* out_ip) {
    if (!host || !out_ip) return false;
    IPAddress resolved = WiFi.resolve(host);
    if (!resolved) return false;
    *out_ip = ((uint32_t)resolved[0] << 24) | ((uint32_t)resolved[1] << 16) |
              ((uint32_t)resolved[2] << 8) | (uint32_t)resolved[3];
    return true;
}

inline bool parsePlainInt(const char* payload, int* out) {
    if (!payload || !out) return false;
    while (*payload && isspace((unsigned char)*payload)) payload++;
    if (*payload == '\0') return false;

    char* end = nullptr;
    long value = strtol(payload, &end, 10);
    if (end == payload) return false;
    while (end && *end && isspace((unsigned char)*end)) {
        ++end;
    }
    if (end && *end != '\0') return false;
    if (value < INT_MIN || value > INT_MAX) return false;
    *out = (int)value;
    return true;
}

inline bool parseBrokerText(const char* text, uint32_t* out_ip,
                            MqttBrokerResolver resolver = resolveMqttBrokerHostname) {
    if (!text || !out_ip) return false;
    if (parseJsonDottedQuad(text, out_ip)) return true;
    if (!jsonContainsAlpha(text) || !resolver) return false;
    return resolver(text, out_ip);
}

inline ParsedMqttBrokerField parseMqttBrokerField(
    const char* body, const char* key,
    MqttBrokerResolver resolver = resolveMqttBrokerHostname) {
    ParsedMqttBrokerField out = {false, true, false, 0};
    out.present = jsonFieldPresent(body, key);
    if (!out.present) return out;
    if (!jsonStringFieldPresent(body, key)) {
        out.valid = false;
        return out;
    }

    char buf[64];
    if (!jsonGetString(body, key, buf, sizeof(buf))) {
        out.valid = false;
        return out;
    }
    if (buf[0] == '\0') {
        out.clear = true;
        return out;
    }
    if (!parseBrokerText(buf, &out.ip, resolver)) {
        out.valid = false;
    }
    return out;
}

inline bool parseMqttCommandIntent(const char* payload, MqttCommandIntent* out) {
    if (!payload || !out) return false;
    memset(out, 0, sizeof(*out));
    out->kind = MQTT_COMMAND_NONE;

    int speed = -1;
    if (jsonGetInt(payload, "speed", &speed) || parsePlainInt(payload, &speed)) {
        out->kind = MQTT_COMMAND_SPEED;
        out->speed = speed;
        return true;
    }

    if (jsonGetString(payload, "mode", out->mode, sizeof(out->mode))) {
        for (char* p = out->mode; *p; ++p) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        }
        out->kind = MQTT_COMMAND_MODE;
        return true;
    }

    return false;
}
