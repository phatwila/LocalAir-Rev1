#pragma once
#include <stdint.h>

enum class AuraEventType : uint8_t {
    SPEED_CHANGE   = 0,
    JERK_ALERT     = 1,
    RPM_MISMATCH   = 2,
    SENSE_BURST    = 3,
    CADR_CALIBRATED = 4,
    ROOM_ESTIMATED = 5,
    RESERVED_6     = 6,
    REBOOT         = 7,
    WIFI_CONNECT   = 8,
    MQTT_CONNECT   = 9,
    ASSIST_TIMEOUT = 10,
    COMMAND_TIMEOUT = 11,
    COMMAND_ROLLBACK = 12,
};

struct LogEntry {
    uint32_t  ts;
    AuraEventType type;
    uint8_t   speed;
    uint16_t  rpm;
    float     voc;
    float     pm25;
    char      msg[20];
};

// 64-entry ring buffer of control events. Stored in SPI flash at offset 0x1000.
class EventLog {
public:
    static constexpr uint8_t CAPACITY = 64;

    EventLog();
    void push(AuraEventType type, uint8_t speed, uint16_t rpm,
              float voc, float pm25, const char* msg = nullptr);

    uint8_t         count()   const { return _count; }
    const LogEntry& entry(uint8_t i) const; // 0=newest

private:
    LogEntry _buf[CAPACITY];
    uint8_t  _head;
    uint8_t  _count;
};
