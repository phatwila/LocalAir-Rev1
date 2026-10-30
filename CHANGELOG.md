# Changelog

All notable changes to LocalAir are listed here. Each release names its
package version, firmware build, and binary SHA-256.

## v1.1.0 (2026-10-30)

Firmware `localair-rev1+b61`, build 61, 121,244 bytes.
SHA-256 `b0cfaa6f53f033fb56cf3f4ae041c5bce3a5a791396b5bc785fd2a3bc56fa813`.

### Auto fan control

- Successive Auto downshifts are spaced by at least 15 seconds.
- Downshift steps stay inside the configured minimum and maximum fan limits.

### Manual control

- When a fan apply timeout, apply failure, or backend loss ends Manual
  control, the firmware saves Auto. A reboot no longer brings back a cleared
  Manual setting.

### Local discovery (mDNS)

- Retries recovery after Wi-Fi loss, an IP change, or a socket failure.
- A failed responder retries after 5 s and backs off to 60 s.
- Socket setup and every send are checked. The responder stops after 3
  consecutive failures and restarts on the retry schedule.
- Failed announcements and multicast rejoins retry after 1 s.

### MQTT

- Every packet read and write has a deadline, so a stalled broker cannot hang
  the firmware.
- The connection fails if the broker does not acknowledge the subscription.
- Retained command messages are ignored.
- Truncated topics, payloads with NUL bytes, and out-of-range numbers are
  rejected.
- Incoming traffic is limited to 4 packets or 20 ms per loop.
- The broker test uses its own client ID, so it no longer disconnects the live
  session.

## v1.0.0 (2026-10-16)

Firmware `localair-rev1+b56`, build 56, 120,044 bytes.
SHA-256 `8b294e461fbf2071fa3d56893e9bae5d1b433ea8993cba86d2cf8964f61e676f`.

First public release.

- Dashboard served from the purifier at `http://localair.local/`.
- Automatic fan control from the particle and gas/VOC sensors, with room
  profiles, fan limits, and a daily schedule.
- Manual control from the dashboard, REST API, or MQTT.
- Live updates over SSE, and MQTT with Home Assistant discovery.
- mDNS naming, optional static IP, and status LED settings.
- Settings stored in two checksummed copies.
- Reset diagnostics at `/api/debug/boot`.
- UV-C lamp and sterionizer on at boot.
