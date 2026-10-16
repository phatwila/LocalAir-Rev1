#include "eeprom_record.h"

namespace EepromRecord {

uint32_t crc32Update(uint32_t crc, const uint8_t* data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
    }
    return crc;
}

uint32_t crc32(const uint8_t* data, uint16_t len) {
    return ~crc32Update(0xFFFFFFFF, data, len);
}

uint32_t readU32(uint16_t addr) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)EEPROM.read(addr + i) << (8 * i);
    return v;
}

uint16_t readU16(uint16_t addr) {
    return (uint16_t)EEPROM.read(addr) | ((uint16_t)EEPROM.read(addr + 1) << 8);
}

void writeU32(uint16_t addr, uint32_t v) {
    for (int i = 0; i < 4; i++) EEPROM.write(addr + i, (v >> (8 * i)) & 0xFF);
}

void writeU16(uint16_t addr, uint16_t v) {
    EEPROM.write(addr, v & 0xFF);
    EEPROM.write(addr + 1, (v >> 8) & 0xFF);
}

bool read(uint16_t off, uint32_t magic, uint8_t* out, uint16_t cap, EepromRecordMeta* meta) {
    if (!out) return false;
    if (readU32(off) != magic) return false;
    const uint16_t len = readU16(off + 9);
    if (len > cap) return false;

    uint8_t header[HEADER_LEN];
    for (uint16_t i = 0; i < HEADER_LEN; i++) header[i] = EEPROM.read(off + i);
    for (uint16_t i = 0; i < len; i++) out[i] = EEPROM.read(off + HEADER_LEN + i);

    uint32_t crc = crc32Update(0xFFFFFFFF, header, HEADER_LEN);
    crc = ~crc32Update(crc, out, len);
    if (crc != readU32(off + HEADER_LEN + len)) return false;

    if (meta) {
        meta->version = header[4];
        meta->seq = readU32(off + 5);
        meta->len = len;
    }
    return true;
}

void write(uint16_t off, uint32_t magic, uint8_t version, uint32_t seq,
           const uint8_t* payload, uint16_t len) {
    uint8_t header[HEADER_LEN];
    for (int i = 0; i < 4; i++) header[i] = (magic >> (8 * i)) & 0xFF;
    header[4] = version;
    for (int i = 0; i < 4; i++) header[5 + i] = (seq >> (8 * i)) & 0xFF;
    header[9] = len & 0xFF;
    header[10] = (len >> 8) & 0xFF;

    for (uint16_t i = 0; i < HEADER_LEN; i++) EEPROM.write(off + i, header[i]);
    for (uint16_t i = 0; i < len; i++) EEPROM.write(off + HEADER_LEN + i, payload[i]);

    uint32_t crc = crc32Update(0xFFFFFFFF, header, HEADER_LEN);
    crc = ~crc32Update(crc, payload, len);
    writeU32(off + HEADER_LEN + len, crc);
}

}  // namespace EepromRecord
