#pragma once
#include <stdint.h>

// Production WS180313138 frame decoder, independent of UART transport.
class PmFrameParser {
public:
    enum Result { Pending, BadChecksum, Complete };
    void expire(uint32_t now) {
        if (_used && now - _last_byte > 100) _used = 0;
    }
    Result feed(uint8_t byte, uint32_t now, float* value) {
        expire(now);
        _last_byte = now;
        if (!_used && byte != 0xff) return Pending;
        if (_used == 1 && byte != 0x18) { _used = 0; return Pending; }
        _bytes[_used++] = byte;
        if (_used < sizeof(_bytes)) return Pending;
        _used = 0;
        uint8_t sum = 0;
        for (unsigned i = 1; i <= 7; ++i) sum += _bytes[i];
        if ((uint8_t)(0u - sum) != _bytes[8]) return BadChecksum;
        if (value) *value = _bytes[4];
        return Complete;
    }
private:
    uint8_t _bytes[9]{};
    uint8_t _used = 0;
    uint32_t _last_byte = 0;
};
