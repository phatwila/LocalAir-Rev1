#pragma once
#include "application.h"
#include <stdint.h>

// CRC-checked record in emulated EEPROM.
// Layout: [magic u32][version u8][seq u32][len u16][payload len][crc32 u32]
// The CRC covers the header and payload. Device OS commits one byte per
// emulation record, so a reset mid-write leaves a torn record that fails
// the CRC instead of loading partial data.

struct EepromRecordMeta {
    uint8_t  version;
    uint32_t seq;
    uint16_t len;
};

namespace EepromRecord {

static constexpr uint16_t HEADER_LEN = 11;
static constexpr uint16_t OVERHEAD   = HEADER_LEN + 4;

uint32_t crc32Update(uint32_t crc, const uint8_t* data, uint16_t len);
uint32_t crc32(const uint8_t* data, uint16_t len);

uint32_t readU32(uint16_t addr);
uint16_t readU16(uint16_t addr);
void     writeU32(uint16_t addr, uint32_t v);
void     writeU16(uint16_t addr, uint16_t v);

// Returns true only for a record with the expected magic, a payload that
// fits in cap, and a matching CRC. out receives meta->len payload bytes.
bool read(uint16_t off, uint32_t magic, uint8_t* out, uint16_t cap, EepromRecordMeta* meta);
void write(uint16_t off, uint32_t magic, uint8_t version, uint32_t seq,
           const uint8_t* payload, uint16_t len);

}  // namespace EepromRecord
