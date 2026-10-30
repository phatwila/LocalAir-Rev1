#pragma once
#include "application.h"
#include <stdint.h>

// Minimal mDNS responder for Particle P1 (Gen 2, STM32F2).
//
// Compliant surface covered:
//   RFC 6762 §6   — multicast queries answered on 224.0.0.251:5353
//   RFC 6762 §6.7 — legacy unicast: queries from source port ≠ 5353 get
//                   a unicast reply with TTL ≤ 10s, no cache-flush bit
//   RFC 6762 §8   — conflict detection probe before claiming name
//   RFC 6762 §8.3 — three unsolicited announcements after claim (1s gaps)
//   RFC 6762 §11  — periodic re-announcement every ~25 min (before 75% TTL)
//   RFC 6762 §6.1  — NSEC record on AAAA queries (no IPv6) eliminates
//                   iOS 2-5s AAAA timeout that makes page loads feel broken
//   RFC 6763       — _http._tcp.local PTR + SRV + TXT records so DNS-SD
//                   discovers us even when raw .local resolution fails
//                   (improves Android Chrome + iOS Safari reliability)
//
// Hostname collision: tries localair.local, localair-1.local, localair-2.local, ...

class MdnsResponder {
public:
    MdnsResponder();

    // Probe for a unique name, run announce sequence, start listening.
    // base_name = "localair" tries localair.local, localair-1.local, ...
    bool begin(const char* base_name = "localair");

    // Send goodbye records, re-probe, and announce a new base hostname.
    bool restart(const char* base_name);

    // Close the UDP socket and clear claim state. Safe to call when WiFi is
    // down; goodbye is only attempted while the network is still up. After
    // stop(), begin() rebuilds the socket, multicast join, probe, and announce.
    void stop();

    // Call every main-loop iteration. Handles queries and re-announces.
    void poll();

    bool running() const { return _running; }

    // Resolved hostname without .local suffix, e.g. "localair" or "localair-2".
    const char* hostname() const { return _hostname; }

private:
    friend struct MdnsResponderTestAccess;
    UDP      _udp;
    char     _base_name[24];
    char     _hostname[24];
    uint8_t  _label_len;
    bool     _running;
    uint32_t _next_announce_ms;   // periodic re-announce timer
    uint32_t _next_rejoin_ms;     // periodic IGMP re-join timer
    uint8_t  _announce_count;     // tracks the 3-burst startup sequence
    uint16_t _suffix;
    uint8_t  _io_failures;        // consecutive send/join failures while running

    // Build/send helpers
    bool _claim(const char* base_name, uint16_t start_suffix);
    bool _formatCandidate(char* out, size_t out_len,
                          const char* base_name, uint16_t suffix) const;
    int _probe(const char* name, uint8_t len);
    bool _announce();
    void _sendGoodbye();
    bool _noteIo(bool ok);
    bool _sendPacket(IPAddress destination, uint16_t port, const uint8_t* data, uint16_t len);
    void _handleQuery(const uint8_t* pkt, uint16_t pkt_len,
                      IPAddress remote, uint16_t rport);
    bool _detectRuntimeConflict(const uint8_t* pkt, uint16_t pkt_len) const;

    // Encode hostname.local as wire-format DNS name into buf at off, return new off
    uint16_t _writeName(uint8_t* buf, uint16_t off) const;
    uint16_t _writeNameForLabel(uint8_t* buf, uint16_t off,
                                const char* name, uint8_t len) const;

    // Write a complete A record into buf at off, return new off
    uint16_t _writeARecord(uint8_t* buf, uint16_t off,
                           uint32_t ttl, bool cache_flush) const;
    uint16_t _writeARecordForLabel(uint8_t* buf, uint16_t off,
                                   const char* name, uint8_t len,
                                   uint32_t ttl, bool cache_flush) const;

    // Write NSEC record asserting only A exists (suppresses iOS AAAA timeout)
    uint16_t _writeNsecRecord(uint8_t* buf, uint16_t off,
                              uint32_t ttl, bool cache_flush) const;

    // Write _http._tcp.local PTR + SRV + TXT into buf at off, return new off
    uint16_t _writeServiceRecords(uint8_t* buf, uint16_t off,
                                  uint32_t ttl_ptr, uint32_t ttl_srv,
                                  bool cache_flush = true) const;

    // Case-insensitive name match at packet offset
    bool _matchName(const uint8_t* pkt, uint16_t pkt_len, uint16_t offset) const;
    bool _matchServiceName(const uint8_t* pkt, uint16_t pkt_len,
                           uint16_t offset) const;
};
