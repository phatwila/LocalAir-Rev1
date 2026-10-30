# Flashing LocalAir

LocalAir supports F00053AA Aura Air Rev 1.0 hardware with a Particle P1
module. Confirm the model before flashing. Later Aura Air hardware, enterprise
models, and rereleased devices are outside this package's hardware scope.

Flashing uses the [Particle CLI](https://docs.particle.io/reference/developer-tools/cli/#installing)
directly. The CLI is available for Windows, macOS, and Linux. Tested CLI version:
**3.52.0**. Confirm `particle --version` works before you start.

New to these purifiers or to Particle? Start with
[First-time setup](FIRST_TIME_SETUP.md). It covers the buttons, status LED,
account, device ID, Wi-Fi, safe mode, and claiming.

## Release binary

Release v1.1.0 contains build 61:

```text
version: localair-rev1+b61
file: localair-rev1.bin
size: 121244 bytes
SHA-256: b0cfaa6f53f033fb56cf3f4ae041c5bce3a5a791396b5bc785fd2a3bc56fa813
```

Download `localair-rev1.bin`, `SHA256SUMS`, and `release-info.json` from the
[v1.1.0 release](https://github.com/phatwila/LocalAir-Rev1/releases/tag/v1.1.0)
into one folder, then verify them from that folder:

```bash
sha256sum -c SHA256SUMS           # Linux
shasum -a 256 -c SHA256SUMS       # macOS
```

On Windows PowerShell, run `Get-FileHash .\localair-rev1.bin` and
`Get-FileHash .\release-info.json`. Each hash must match its line in
`SHA256SUMS`, ignoring letter case.

The checksum must match before you flash. The commands below assume you run
them from the download folder.

## Optional source build

The release binary needs no local build. To change the firmware, follow
[Building from Source](../README.md#building-from-source). A successful source
build writes `target/localair-rev1.bin` and advances its build identity.

## USB flash

Connect only the intended purifier with a micro-USB **data** cable. The port
is behind the removable back panel and is fragile; see
[Protect the micro-USB port first](FIRST_TIME_SETUP.md#protect-the-micro-usb-port-first).
Run `particle usb list` to check that the computer sees it. USB flashing needs
`particle login` so the CLI can download Device OS, but the purifier does not
need to be claimed.

```bash
particle flash --local localair-rev1.bin
```

The CLI normally switches the purifier to DFU mode by itself. It also updates
Device OS when the installed version does not satisfy the binary. If it cannot
enter DFU, do it by hand: hold **RESET** and **SETUP**, release **RESET**, and
keep holding **SETUP** until the LED blinks yellow. Then run the command again.

If the purifier has no working Wi-Fi credentials, run `particle serial wifi`
after flashing.

On Linux, run `particle usb configure` once so USB works without root.

**WSL2:** USB devices must be attached to WSL with
[usbipd-win](https://learn.microsoft.com/en-us/windows/wsl/connect-usb).
DFU mode re-enumerates the device, so reattach it after the switch and rerun
the flash command. Installing the Windows Particle CLI and flashing from
PowerShell avoids this step.

## OTA flash

OTA requires a Particle CLI login (`particle login`) and a purifier that is
claimed by your account and online. Safe mode (breathing magenta) is the most
reliable state for a stock unit; see
[Enter safe mode](FIRST_TIME_SETUP.md#enter-safe-mode).

```bash
particle flash <device-id> localair-rev1.bin
```

Replace `<device-id>` with the purifier's 24-character Particle device ID. The
first flash also upgrades Device OS over the air: the LED blinks magenta
rapidly and the purifier restarts several times over 5–10 minutes. Do not
unplug it.

When the LED breathes cyan, check the cloud identity and fresh local address:

```bash
particle get <device-id> FW_VER
particle get <device-id> BUILD_NUM
particle get <device-id> localIP
```

Cloud values should report `localair-rev1+b61` and build `61` for this binary.

## Verify runtime status

Open `http://localair.local/` or `http://<device-ip>/` to see the dashboard.
For a scripted check, clone the repository and run `device-status.sh` from it
(Bash, `curl`, and `python3` required):

```bash
git clone https://github.com/phatwila/LocalAir-Rev1.git
cd LocalAir-Rev1
./bin/device-status.sh <device-ip> --expect-id <device-id> \
  --expect-version localair-rev1+b61 --expect-build 61 \
  --require-fresh-rpm --events
```

It checks public status JSON, readiness, and positive RPM feedback, and with
these options also the release version, build, fresh RPM, and `/api/events`.
Positive RPM establishes rotation, not airflow.

The `--events` and `--debug` options request additional JSON endpoints.
`--json` prints the raw `/api/status` response. `--watch` polls every two
seconds. Use `./bin/device-status.sh --help` for the complete option list.

After the first flash, set your hostname and preferences in the dashboard's
System tab, then confirm them in `/api/config`.

If mDNS does not resolve, use the device IP from your router or, for a claimed
device, `particle get <device-id> localIP`. If `/api/events` flakes while
cloud and status checks pass, record that limitation separately.

You can also read status directly:

```bash
curl http://localair.local/api/status
```

Local HTTP has no application authentication or TLS. MQTT also uses plain TCP.
Use these interfaces only on a trusted network.

## Network changes

The dashboard can save IP addressing settings, including a static IP. Wi-Fi
credentials are set separately with `particle serial wifi`; see [First-time setup](FIRST_TIME_SETUP.md#connect-the-purifier-to-wi-fi). Address
changes apply at the next Wi-Fi reconnect. Saving settings does not force a
reconnect or create a router reservation. If the hostname changes, read the
active `mdns_host` field from `/api/status` and use that name on later checks.

Prefer a router DHCP reservation to preserve the current address. Record your
address and control settings before flashing, then confirm they are preserved.
Client support for `.local` varies; use direct HTTP to the IP when name
resolution fails. Do not change device configuration to repair a client DNS issue.

For a failed or interrupted flash, stop and diagnose connectivity and device
state before another attempt. Recover an unreachable application through USB
DFU with the verified binary. Flashing does not restore account ownership or
Wi-Fi credentials. Confirm the intended device before
recovery and check runtime status again afterward.
