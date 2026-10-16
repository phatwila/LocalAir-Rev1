#include "mdns_responder.h"
#include "mdns_probe_logic.h"
#include <string.h>

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint16_t MDNS_PORT      = 5353;
static const     IPAddress MDNS_GROUP(224, 0, 0, 251);

// RFC 6762 §11.3 / RFC 6763:
//   A/SRV records: 120s — short enough for DHCP IP-change recovery
//   PTR/TXT records: 4500s (75 min) — stable service directory entries
// Clients refresh at 80% of TTL; we re-announce at 80% of A TTL = 96s,
// so REANNOUNCE_MS = 90 000 ms keeps us comfortably ahead.
static constexpr uint32_t TTL_A         = 120;     // A + SRV records
static constexpr uint32_t TTL_PTR       = 4500;    // PTR + TXT records
static constexpr uint32_t TTL_LEGACY    = 10;      // unicast legacy (§6.7 max)

// Re-announce at ~75% of TTL_A (120 * 0.75 = 90s)
static constexpr uint32_t REANNOUNCE_MS = 90000UL;

// Periodic IGMP re-join: some APs expire silent multicast members after
// their group timeout; a fresh join report keeps 224.0.0.251 forwarded.
static constexpr uint32_t REJOIN_MS = 300000UL;

// Max incoming packets handled per poll() so query bursts do not queue
// behind slow main-loop passes.
static constexpr uint8_t POLL_MAX_PACKETS = 4;

// Startup announce burst: RFC 6762 §8.3 — first gap 1s, then 2s, then 4s.
// We do 3 announces; after the 3rd we switch to the periodic timer.
static const uint32_t ANNOUNCE_GAPS_MS[3] = {1000, 2000, 4000};

static constexpr uint16_t DNS_TYPE_A     = 1;
static constexpr uint16_t DNS_TYPE_PTR   = 12;
static constexpr uint16_t DNS_TYPE_TXT   = 16;
static constexpr uint16_t DNS_TYPE_AAAA  = 28;
static constexpr uint16_t DNS_TYPE_SRV   = 33;
static constexpr uint16_t DNS_TYPE_NSEC  = 47;
static constexpr uint16_t DNS_TYPE_ANY   = 255;
static constexpr uint16_t DNS_CLASS_IN   = 1;
static constexpr uint16_t DNS_CF         = 0x8000; // cache-flush bit

// HTTP service type label: "_http._tcp"
static const char SVC_HTTP[]  = "_http";
static const char SVC_TCP[]   = "_tcp";
static constexpr uint16_t HTTP_PORT = 80;
static constexpr uint16_t PROBE_SPACING_MS = 250;
static constexpr uint16_t PROBE_START_JITTER_MS = 250;

// ── Construction ──────────────────────────────────────────────────────────────

MdnsResponder::MdnsResponder()
    : _label_len(0), _running(false),
      _next_announce_ms(0), _next_rejoin_ms(0), _announce_count(0), _suffix(0)
{
    memset(_base_name, 0, sizeof(_base_name));
    memset(_hostname, 0, sizeof(_hostname));
}

// ── begin() ───────────────────────────────────────────────────────────────────

bool MdnsResponder::begin(const char* base_name) {
    if (!base_name || !base_name[0] || !WiFi.ready()) return false;

    _udp.stop();
    _udp.begin(MDNS_PORT);
    _udp.joinMulticast(MDNS_GROUP);
    strncpy(_base_name, base_name, sizeof(_base_name) - 1);
    _base_name[sizeof(_base_name) - 1] = '\0';
    if (!_claim(_base_name, 0)) return false;

    _next_rejoin_ms = millis() + REJOIN_MS;
    _running = true;
    return true;
}

bool MdnsResponder::restart(const char* base_name) {
    if (!base_name || !base_name[0] || !WiFi.ready()) return false;

    char old_base[sizeof(_base_name)];
    char old_hostname[sizeof(_hostname)];
    const uint8_t old_label_len = _label_len;
    const bool old_running = _running;
    const uint16_t old_suffix = _suffix;
    strncpy(old_base, _base_name, sizeof(old_base) - 1);
    old_base[sizeof(old_base) - 1] = '\0';
    strncpy(old_hostname, _hostname, sizeof(old_hostname) - 1);
    old_hostname[sizeof(old_hostname) - 1] = '\0';

    if (_running && _hostname[0]) {
        _sendGoodbye();
    }

    _running = false;
    _suffix = 0;
    _announce_count = 0;
    _next_announce_ms = 0;
    memset(_base_name, 0, sizeof(_base_name));
    memset(_hostname, 0, sizeof(_hostname));
    _label_len = 0;

    _udp.stop();
    _udp.begin(MDNS_PORT);
    _udp.joinMulticast(MDNS_GROUP);
    strncpy(_base_name, base_name, sizeof(_base_name) - 1);
    _base_name[sizeof(_base_name) - 1] = '\0';
    if (!_claim(_base_name, 0)) {
        strncpy(_base_name, old_base, sizeof(_base_name) - 1);
        _base_name[sizeof(_base_name) - 1] = '\0';
        strncpy(_hostname, old_hostname, sizeof(_hostname) - 1);
        _hostname[sizeof(_hostname) - 1] = '\0';
        _label_len = old_label_len;
        _running = old_running;
        _suffix = old_suffix;
        return false;
    }

    _next_rejoin_ms = millis() + REJOIN_MS;
    _running = true;
    return true;
}

void MdnsResponder::stop() {
    if (_running && _hostname[0] && WiFi.ready()) {
        _sendGoodbye();
    }
    _udp.stop();
    _running = false;
    _suffix = 0;
    _announce_count = 0;
    _next_announce_ms = 0;
    _next_rejoin_ms = 0;
    memset(_base_name, 0, sizeof(_base_name));
    memset(_hostname, 0, sizeof(_hostname));
    _label_len = 0;
}

bool MdnsResponder::_claim(const char* base_name, uint16_t start_suffix) {
    char candidate[sizeof(_hostname)];
    for (uint16_t suffix = start_suffix;; ++suffix) {
        if (!_formatCandidate(candidate, sizeof(candidate), base_name, suffix)) {
            return false;
        }
        const uint8_t len = (uint8_t)strlen(candidate);
        if (len == 0) continue;
        if (_probe(candidate, len)) continue;

        strncpy(_hostname, candidate, sizeof(_hostname) - 1);
        _hostname[sizeof(_hostname) - 1] = '\0';
        _label_len = len;
        _suffix = suffix;
        _announce_count = 0;
        _announce();
        _announce_count = 1;
        _next_announce_ms = millis() + ANNOUNCE_GAPS_MS[0];
        return true;
    }
}

bool MdnsResponder::_formatCandidate(char* out, size_t out_len,
                                     const char* base_name, uint16_t suffix) const {
    return mdnsFormatCandidateName(out, out_len, base_name, suffix);
}

// ── poll() ────────────────────────────────────────────────────────────────────

void MdnsResponder::poll() {
    if (!_running) return;

    // Startup burst (up to 3 total announces)
    uint32_t now = millis();
    if (_announce_count < 3 && now >= _next_announce_ms) {
        _announce();
        _announce_count++;
        if (_announce_count < 3) {
            _next_announce_ms = now + ANNOUNCE_GAPS_MS[_announce_count - 1];
        } else {
            _next_announce_ms = now + REANNOUNCE_MS;
        }
    } else if (_announce_count >= 3 && now >= _next_announce_ms) {
        _announce();
        _next_announce_ms = now + REANNOUNCE_MS;
    }

    // Periodic IGMP re-join keeps AP multicast forwarding alive
    if (now >= _next_rejoin_ms) {
        _udp.joinMulticast(MDNS_GROUP);
        _next_rejoin_ms = now + REJOIN_MS;
    }

    // Handle incoming queries (bounded drain per poll)
    for (uint8_t i = 0; i < POLL_MAX_PACKETS; ++i) {
        int sz = _udp.parsePacket();
        if (sz < 12) break;

        uint8_t buf[300];
        uint16_t n = (uint16_t)_udp.read(buf, sizeof(buf));
        if (n < 12) continue;

        IPAddress remote  = _udp.remoteIP();
        uint16_t  rport   = _udp.remotePort();

        // Drop our own reflections
        if (remote == WiFi.localIP()) continue;

        if (buf[2] & 0x80) {
            if (_detectRuntimeConflict(buf, n) && _base_name[0]) {
                // _claim re-probes and consumes packets itself; stop draining
                _claim(_base_name, (uint16_t)(_suffix + 1));
                break;
            }
            continue;
        }

        _handleQuery(buf, n, remote, rport);
    }
}

// ── _probe() ─────────────────────────────────────────────────────────────────
// Send RFC-style mDNS probes and watch for conflicting probes or answers.
// Returns true if the candidate name is already taken or loses tie-break.

bool MdnsResponder::_probe(const char* name, uint8_t len) {
    const uint32_t our_ip = mdnsPackIpv4(WiFi.localIP()[0], WiFi.localIP()[1],
                                         WiFi.localIP()[2], WiFi.localIP()[3]);

    delay((unsigned long)random(PROBE_START_JITTER_MS + 1));

    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        uint8_t pkt[96];
        memset(pkt, 0, sizeof(pkt));

        // Header: ID=0, QR=0, QDCOUNT=1, NSCOUNT=1
        pkt[4] = 0; pkt[5] = 1;
        pkt[8] = 0; pkt[9] = 1;

        uint16_t off = 12;
        off = _writeNameForLabel(pkt, off, name, len);
        pkt[off++] = 0; pkt[off++] = DNS_TYPE_ANY;
        pkt[off++] = 0; pkt[off++] = DNS_CLASS_IN;
        off = _writeARecordForLabel(pkt, off, name, len, TTL_A, false);

        _udp.beginPacket(MDNS_GROUP, MDNS_PORT);
        _udp.write(pkt, off);
        _udp.endPacket();

        const uint32_t deadline = millis() + PROBE_SPACING_MS;
        while (millis() < deadline) {
            int sz = _udp.parsePacket();
            if (sz < 12) {
                delay(10);
                continue;
            }

            uint8_t buf[300];
            const uint16_t n = (uint16_t)_udp.read(buf, sizeof(buf));
            if (_udp.remoteIP() == WiFi.localIP()) continue;
            if (mdnsPacketConflicts(buf, n, name, our_ip, MDNS_CONFLICT_MODE_PROBE)) {
                return true;
            }
        }
    }

    return false;
}

// ── _announce() ───────────────────────────────────────────────────────────────
// Unsolicited multicast response with A record + DNS-SD service records.

void MdnsResponder::_announce() {
    uint8_t pkt[280];
    memset(pkt, 0, sizeof(pkt));

    // Header: QR=1, AA=1; ANCOUNT filled in at end
    pkt[2] = 0x84;

    uint16_t off = 12;
    uint16_t rcount = 0;

    // A record
    off = _writeARecord(pkt, off, TTL_A, true);
    rcount++;

    // PTR + SRV + TXT service records
    off = _writeServiceRecords(pkt, off, TTL_PTR, TTL_A);
    rcount += 3; // PTR + SRV + TXT

    // ANCOUNT
    pkt[6] = (rcount >> 8) & 0xFF;
    pkt[7] = rcount & 0xFF;

    _udp.beginPacket(MDNS_GROUP, MDNS_PORT);
    _udp.write(pkt, off);
    _udp.endPacket();
}

void MdnsResponder::_sendGoodbye() {
    uint8_t pkt[280];
    memset(pkt, 0, sizeof(pkt));

    pkt[2] = 0x84;

    uint16_t off = 12;
    uint16_t rcount = 0;

    off = _writeARecord(pkt, off, 0, true);
    rcount++;

    off = _writeServiceRecords(pkt, off, 0, 0);
    rcount += 3;

    pkt[6] = (rcount >> 8) & 0xFF;
    pkt[7] = rcount & 0xFF;

    _udp.beginPacket(MDNS_GROUP, MDNS_PORT);
    _udp.write(pkt, off);
    _udp.endPacket();
}

// ── _handleQuery() ────────────────────────────────────────────────────────────

void MdnsResponder::_handleQuery(const uint8_t* pkt, uint16_t pkt_len,
                                  IPAddress remote, uint16_t rport) {
    uint16_t qdcount = ((uint16_t)pkt[4] << 8) | pkt[5];
    uint16_t tx_id   = ((uint16_t)pkt[0] << 8) | pkt[1];

    // §6.7: queries from port ≠ 5353 are "legacy unicast" — respond
    // unicast to sender, lower TTL, don't set cache-flush.
    bool legacy  = (rport != MDNS_PORT);
    // Legacy DNS has one question. Matching names below are uncompressed,
    // so that question can be copied without dangling compression pointers.
    if (legacy && qdcount != 1) return;
    // QU bit (RFC 6762 §5.4): high bit of QCLASS requests unicast reply.
    // Accumulate across all questions; if any question sets QU, reply unicast.
    bool qu_unicast = false;

    bool want_a    = false;
    bool want_aaaa = false;  // AAAA query → reply with NSEC (we have no IPv6)
    bool want_svc  = false;

    uint16_t off = 12;
    for (uint16_t q = 0; q < qdcount && off < pkt_len; q++) {
        bool match_a   = _matchName(pkt, pkt_len, off);
        bool match_svc = !match_a && _matchServiceName(pkt, pkt_len, off);

        // Skip QNAME
        while (off < pkt_len) {
            if (pkt[off] == 0)              { off++; break; }
            if ((pkt[off] & 0xC0) == 0xC0) { off += 2; break; }
            off += 1 + pkt[off];
        }
        if (off + 4 > pkt_len) return;
        uint16_t qtype  = ((uint16_t)pkt[off]     << 8) | pkt[off + 1];
        uint16_t qclass = ((uint16_t)pkt[off + 2] << 8) | pkt[off + 3];
        off += 4;

        if (qclass & 0x8000) qu_unicast = true; // QU bit set

        if (match_a && (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY))
            want_a = true;
        if (match_a && qtype == DNS_TYPE_AAAA)
            want_aaaa = true;
        if (match_svc && (qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY))
            want_svc = true;
    }

    if (!want_a && !want_aaaa && !want_svc) return;

    // A + NSEC + service records can exceed 280 bytes at the longest name.
    uint8_t resp[512];
    memset(resp, 0, sizeof(resp));

    // Header: QR=1, AA=1
    resp[0] = legacy ? (tx_id >> 8) & 0xFF : 0;
    resp[1] = legacy ? tx_id & 0xFF : 0;
    resp[2] = 0x84;

    uint16_t roff   = 12;
    if (legacy) {
        // RFC 6762 section 6.7 requires the original question as well as ID.
        if (off < 12 || off > pkt_len || off > 64) return;
        memcpy(resp + 12, pkt + 12, off - 12);
        resp[5] = 1;
        roff = off;
    }
    uint16_t rcount = 0;
    bool     unicast = legacy || qu_unicast;
    uint32_t ttl_a  = legacy ? TTL_LEGACY : TTL_A;

    if (want_a) {
        roff = _writeARecord(resp, roff, ttl_a, !legacy);
        rcount++;
    }
    if (want_aaaa) {
        // No AAAA record — reply with NSEC so iOS skips its 2-5s timeout
        roff = _writeNsecRecord(resp, roff, ttl_a, !legacy);
        rcount++;
    }
    if (want_svc) {
        uint32_t svc_ttl = legacy ? TTL_LEGACY : TTL_PTR;
        roff = _writeServiceRecords(resp, roff, svc_ttl,
                                    legacy ? TTL_LEGACY : TTL_A, !legacy);
        rcount += 3; // PTR + SRV + TXT
    }

    resp[6] = (rcount >> 8) & 0xFF;
    resp[7] = rcount & 0xFF;

    // Legacy or QU: reply unicast to sender; standard: multicast
    if (unicast) {
        _udp.beginPacket(remote, rport);
    } else {
        _udp.beginPacket(MDNS_GROUP, MDNS_PORT);
    }
    _udp.write(resp, roff);
    _udp.endPacket();
}

bool MdnsResponder::_detectRuntimeConflict(const uint8_t* pkt, uint16_t pkt_len) const {
    if (!_hostname[0]) return false;
    const uint32_t our_ip = mdnsPackIpv4(WiFi.localIP()[0], WiFi.localIP()[1],
                                         WiFi.localIP()[2], WiFi.localIP()[3]);
    return mdnsPacketConflicts(pkt, pkt_len, _hostname, our_ip, MDNS_CONFLICT_MODE_RUNTIME);
}

// ── Name encoding helpers ────────────────────────────────────────────────────

uint16_t MdnsResponder::_writeName(uint8_t* buf, uint16_t off) const {
    return _writeNameForLabel(buf, off, _hostname, _label_len);
}

uint16_t MdnsResponder::_writeNameForLabel(uint8_t* buf, uint16_t off,
                                           const char* name, uint8_t len) const {
    buf[off++] = len;
    memcpy(buf + off, name, len); off += len;
    buf[off++] = 5;
    memcpy(buf + off, "local", 5); off += 5;
    buf[off++] = 0;
    return off;
}

uint16_t MdnsResponder::_writeARecord(uint8_t* buf, uint16_t off,
                                       uint32_t ttl, bool cache_flush) const {
    return _writeARecordForLabel(buf, off, _hostname, _label_len, ttl, cache_flush);
}

uint16_t MdnsResponder::_writeARecordForLabel(uint8_t* buf, uint16_t off,
                                              const char* name, uint8_t len,
                                              uint32_t ttl, bool cache_flush) const {
    off = _writeNameForLabel(buf, off, name, len);
    // TYPE = A
    buf[off++] = 0; buf[off++] = DNS_TYPE_A;
    // CLASS = IN, optionally with cache-flush
    uint16_t cls = DNS_CLASS_IN | (cache_flush ? DNS_CF : 0);
    buf[off++] = (cls >> 8) & 0xFF; buf[off++] = cls & 0xFF;
    // TTL
    buf[off++] = (ttl >> 24) & 0xFF; buf[off++] = (ttl >> 16) & 0xFF;
    buf[off++] = (ttl >> 8) & 0xFF;  buf[off++] = ttl & 0xFF;
    // RDLENGTH = 4
    buf[off++] = 0; buf[off++] = 4;
    // RDATA = IP
    IPAddress ip = WiFi.localIP();
    buf[off++] = ip[0]; buf[off++] = ip[1];
    buf[off++] = ip[2]; buf[off++] = ip[3];
    return off;
}

// Write NSEC record for hostname.local asserting only A record exists.
// Suppresses iOS AAAA timeout (RFC 6762 §6.1 / RFC 4034 §4).
// Type bitmap window 0: covers types 1-28; byte 3 bit 7 = A(1), byte 4 bit 4 = AAAA(28) absent.
// Bitmap: 5 bytes, bit for A set (type 1 = window 0, bitmap byte 0 bit 6): 0x40 0x00 0x00 0x00 0x00
uint16_t MdnsResponder::_writeNsecRecord(uint8_t* buf, uint16_t off,
                                          uint32_t ttl, bool cache_flush) const {
    off = _writeName(buf, off);  // next domain name = owner name (same)
    buf[off++] = 0; buf[off++] = DNS_TYPE_NSEC;
    uint16_t cls = DNS_CLASS_IN | (cache_flush ? DNS_CF : 0);
    buf[off++] = (cls >> 8) & 0xFF; buf[off++] = cls & 0xFF;
    buf[off++] = (ttl >> 24) & 0xFF; buf[off++] = (ttl >> 16) & 0xFF;
    buf[off++] = (ttl >> 8) & 0xFF;  buf[off++] = ttl & 0xFF;
    // RDLENGTH placeholder
    uint16_t rdlen_off = off; off += 2;
    uint16_t rdata_start = off;
    // Next Domain Name = hostname.local (same as owner)
    off = _writeName(buf, off);
    // RFC 4034 section 4.1.2: omit trailing zero bitmap octets.
    // Window zero, first octet covers types 0..7; type A sets bit 6.
    buf[off++] = 0;    // window block
    buf[off++] = 1;    // bitmap length
    buf[off++] = 0x40; // bit 6 set = type 1 (A)
    uint16_t rdlen = off - rdata_start;
    buf[rdlen_off]     = (rdlen >> 8) & 0xFF;
    buf[rdlen_off + 1] = rdlen & 0xFF;
    return off;
}

uint16_t MdnsResponder::_writeServiceRecords(uint8_t* buf, uint16_t off,
                                              uint32_t ttl_ptr,
                                              uint32_t ttl_srv, bool cache_flush) const {
    // ── PTR: _http._tcp.local → <hostname>._http._tcp.local ──────────────
    // NAME: _http._tcp.local
    buf[off++] = 5; memcpy(buf + off, "_http", 5); off += 5;
    buf[off++] = 4; memcpy(buf + off, "_tcp", 4);  off += 4;
    buf[off++] = 5; memcpy(buf + off, "local", 5); off += 5;
    buf[off++] = 0;
    // TYPE = PTR, CLASS = IN (no cache-flush on PTR per RFC 6762 §11.3)
    buf[off++] = 0; buf[off++] = DNS_TYPE_PTR;
    buf[off++] = 0; buf[off++] = DNS_CLASS_IN;
    // TTL
    buf[off++] = (ttl_ptr >> 24) & 0xFF; buf[off++] = (ttl_ptr >> 16) & 0xFF;
    buf[off++] = (ttl_ptr >> 8) & 0xFF;  buf[off++] = ttl_ptr & 0xFF;
    // RDLENGTH placeholder — fill after writing RDATA
    uint16_t rdlen_off = off; off += 2;
    uint16_t rdata_start = off;
    // RDATA: <hostname>._http._tcp.local
    buf[off++] = _label_len;
    memcpy(buf + off, _hostname, _label_len); off += _label_len;
    buf[off++] = 5; memcpy(buf + off, "_http", 5); off += 5;
    buf[off++] = 4; memcpy(buf + off, "_tcp", 4);  off += 4;
    buf[off++] = 5; memcpy(buf + off, "local", 5); off += 5;
    buf[off++] = 0;
    uint16_t rdlen = off - rdata_start;
    buf[rdlen_off]     = (rdlen >> 8) & 0xFF;
    buf[rdlen_off + 1] = rdlen & 0xFF;

    // ── SRV: <hostname>._http._tcp.local → hostname.local :80 ────────────
    // NAME: <hostname>._http._tcp.local
    buf[off++] = _label_len;
    memcpy(buf + off, _hostname, _label_len); off += _label_len;
    buf[off++] = 5; memcpy(buf + off, "_http", 5); off += 5;
    buf[off++] = 4; memcpy(buf + off, "_tcp", 4);  off += 4;
    buf[off++] = 5; memcpy(buf + off, "local", 5); off += 5;
    buf[off++] = 0;
    // TYPE = SRV, CLASS = IN | cache-flush
    buf[off++] = 0; buf[off++] = DNS_TYPE_SRV;
    uint16_t cls = DNS_CLASS_IN | (cache_flush ? DNS_CF : 0);
    buf[off++] = (cls >> 8) & 0xFF; buf[off++] = cls & 0xFF;
    // TTL
    buf[off++] = (ttl_srv >> 24) & 0xFF; buf[off++] = (ttl_srv >> 16) & 0xFF;
    buf[off++] = (ttl_srv >> 8) & 0xFF;  buf[off++] = ttl_srv & 0xFF;
    // RDLENGTH placeholder
    uint16_t srv_rdlen_off = off; off += 2;
    uint16_t srv_rdata_start = off;
    // SRV RDATA: priority=0, weight=0, port=80, target=hostname.local
    buf[off++] = 0; buf[off++] = 0; // priority
    buf[off++] = 0; buf[off++] = 0; // weight
    buf[off++] = (HTTP_PORT >> 8) & 0xFF; buf[off++] = HTTP_PORT & 0xFF;
    off = _writeName(buf, off); // target
    uint16_t srv_rdlen = off - srv_rdata_start;
    buf[srv_rdlen_off]     = (srv_rdlen >> 8) & 0xFF;
    buf[srv_rdlen_off + 1] = srv_rdlen & 0xFF;

    // ── TXT: <hostname>._http._tcp.local — RFC 6763 §6.1 requires a TXT ──────
    // NAME: <hostname>._http._tcp.local (same as SRV)
    buf[off++] = _label_len;
    memcpy(buf + off, _hostname, _label_len); off += _label_len;
    buf[off++] = 5; memcpy(buf + off, "_http", 5); off += 5;
    buf[off++] = 4; memcpy(buf + off, "_tcp",  4); off += 4;
    buf[off++] = 5; memcpy(buf + off, "local", 5); off += 5;
    buf[off++] = 0;
    // TYPE = TXT, CLASS = IN | cache-flush
    buf[off++] = 0; buf[off++] = DNS_TYPE_TXT;
    uint16_t txt_cls = DNS_CLASS_IN | (cache_flush ? DNS_CF : 0);
    buf[off++] = (txt_cls >> 8) & 0xFF; buf[off++] = txt_cls & 0xFF;
    // TTL (same as PTR)
    buf[off++] = (ttl_ptr >> 24) & 0xFF; buf[off++] = (ttl_ptr >> 16) & 0xFF;
    buf[off++] = (ttl_ptr >> 8) & 0xFF;  buf[off++] = ttl_ptr & 0xFF;
    // RDLENGTH = 1, RDATA = single zero byte (empty TXT per RFC 6763 §6.1)
    buf[off++] = 0; buf[off++] = 1;
    buf[off++] = 0x00;

    return off;
}

// ── Name match helpers ───────────────────────────────────────────────────────

static bool ciEqual(char a, char b) {
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

// Match <hostname>.local at packet offset
bool MdnsResponder::_matchName(const uint8_t* pkt, uint16_t pkt_len,
                                uint16_t offset) const {
    if (offset + 1 + _label_len + 6 + 1 > pkt_len) return false;
    if (pkt[offset] != _label_len) return false;
    for (uint8_t i = 0; i < _label_len; i++)
        if (!ciEqual((char)pkt[offset + 1 + i], _hostname[i])) return false;
    offset += 1 + _label_len;
    if (pkt[offset] != 5) return false;
    const char local[] = "local";
    for (uint8_t i = 0; i < 5; i++)
        if (!ciEqual((char)pkt[offset + 1 + i], local[i])) return false;
    offset += 6;
    return (offset < pkt_len && pkt[offset] == 0);
}

// Match _http._tcp.local at packet offset (for PTR/SRV queries)
bool MdnsResponder::_matchServiceName(const uint8_t* pkt, uint16_t pkt_len,
                                       uint16_t offset) const {
    if (offset + 16 > pkt_len) return false;
    // _http label
    if (pkt[offset] != 5) return false;
    const char http[] = "_http";
    for (uint8_t i = 0; i < 5; i++)
        if (!ciEqual((char)pkt[offset + 1 + i], http[i])) return false;
    offset += 6;
    // _tcp label
    if (offset + 5 > pkt_len || pkt[offset] != 4) return false;
    const char tcp[] = "_tcp";
    for (uint8_t i = 0; i < 4; i++)
        if (!ciEqual((char)pkt[offset + 1 + i], tcp[i])) return false;
    offset += 5;
    // local label
    if (offset + 6 > pkt_len || pkt[offset] != 5) return false;
    const char local[] = "local";
    for (uint8_t i = 0; i < 5; i++)
        if (!ciEqual((char)pkt[offset + 1 + i], local[i])) return false;
    offset += 6;
    return (offset < pkt_len && pkt[offset] == 0);
}
