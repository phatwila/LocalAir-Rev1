# First-Time Setup

This guide takes an F00053AA Aura Air Rev 1.0 purifier from its stock firmware
to LocalAir, flashed over Wi-Fi from your own Particle account. Every step uses
the purifier's buttons, a web browser, or the Particle CLI. You do not need to
clone this repository or install a compiler.

Plan for 30–45 minutes. The firmware update itself takes 5–10 minutes.

## What you need

- An F00053AA Aura Air Rev 1.0. Later Aura hardware is not supported.
- A 2.4 GHz Wi-Fi network. The purifier's Particle P1 module cannot join
  5 GHz-only networks.
- A computer running Windows, macOS, or Linux.
- A micro-USB **data** cable, for the USB steps and for recovery. Many
  charge-only cables look identical and do not work.
- A small screwdriver to remove the back panel, if you use USB.

## Buttons and status LED

The purifier has two buttons:

- **RESET** is a small momentary switch behind the removable back panel,
  next to the micro-USB port.
- **SETUP** is on the bottom of the unit, near the external status LED.

The status LED follows Particle's standard patterns:

| LED | Meaning |
| --- | --- |
| Breathing cyan | Connected to Wi-Fi and the Particle cloud |
| Blinking green | Joining Wi-Fi |
| Blinking cyan | Connecting to the Particle cloud |
| Blinking dark blue | Listening mode: ready for Wi-Fi setup or identification |
| Breathing magenta | Safe mode: connected to the cloud, application not running |
| Blinking magenta | Firmware update in progress, or entering safe mode |
| Blinking yellow | DFU mode: ready for a USB firmware flash |
| Red SOS blinks | Firmware crash; the device restarts afterwards |

See Particle's [status LED reference](https://docs.particle.io/troubleshooting/led/)
for the full list.

## Protect the micro-USB port first

> **Note:** the micro-USB port is extremely fragile. I
> sheared the port off one of my test units by knocking the USB cable. I was
> able to solder it back on, but I *highly* recommend carefully reinforcing the
> port before you use it. Use a small amount of a quick-hardening glue such as
> UV-activated resin or quick-drying CA (super glue) gel, and keep it off the
> pins. This is an important insurance policy: if you rip the copper traces off
> the board along with the port, you lose the only way to fully recover from an
> accidental firmware brick.

To reinforce the port:

1. Remove the back panel and locate the micro-USB port next to RESET.
2. Plug a spare cable in first so glue cannot reach the contacts.
3. Apply a thin bead where the metal shell of the port meets the circuit board.
   Do not let glue run into the opening.
4. Cure it fully before unplugging the spare cable. UV resin needs a UV light;
   CA gel needs several minutes.
5. When you use USB later, leave slack in the cable and support it so it
   cannot lever the port.

## Create a Particle account and install the CLI

1. Create a free account at [login.particle.io/signup](https://login.particle.io/signup).
   The free plan covers up to 100 devices.
2. Install the [Particle CLI](https://docs.particle.io/reference/developer-tools/cli/#installing):
   - **macOS or Linux:** run `bash <( curl -sL https://particle.io/install-cli )`,
     then open a new terminal.
   - **Windows:** run the
     [Particle CLI installer](https://binaries.particle.io/particle-cli/installer/win32/ParticleCLISetup.exe),
     then open a new PowerShell window.
   - **Linux only:** run `particle usb configure` once so USB works without root.
3. Sign in and confirm:

   ```bash
   particle login
   particle whoami
   ```

   If your account uses two-step verification, `particle login` asks for the
   code, or you can pass it with `--otp <code>`.

This guide was checked with Particle CLI 3.52.0. Run `particle update-cli` to
update an older install.

## Find the device ID

Each purifier has a 24-character Particle device ID. You need it to claim and
flash the unit. This step usually takes seconds.

**Over USB (recommended):**

1. Remove the back panel and connect the micro-USB cable carefully.
2. Run:

   ```bash
   particle identify
   ```

3. If it reports no device, put the purifier in listening mode: hold **SETUP**
   for about 3 seconds until the LED blinks dark blue. Then run
   `particle serial identify`.

**Without USB:**

1. Hold **SETUP** for about 3 seconds until the LED blinks dark blue.
2. On a phone or computer, join the Wi-Fi network named `Aura-` followed by six
   characters.
3. Open <http://192.168.0.1/device-id> in a browser. The page shows a short
   response such as `{"id":"<device-id>","c":"0"}`. The `id` value is the
   device ID.
4. Rejoin your normal Wi-Fi network.

Write the ID down. In the commands below, replace `<device-id>` with it.

## Connect the purifier to Wi-Fi

A purifier that was set up with the Aura app usually still has its Wi-Fi
credentials. If it reaches breathing cyan on its own, skip this section.

If your network changed, set new credentials over USB:

```bash
particle serial wifi
```

Enter the network name and password when prompted. If the command reports no
device, enter listening mode first (hold **SETUP** about 3 seconds). The LED
blinks green while joining Wi-Fi, then cyan while connecting to the cloud.

As an alternative, Particle's [Web Device Doctor](https://docs.particle.io/tools/doctor/)
can set Wi-Fi over USB from Chrome or Edge.

## Enter safe mode

Safe mode connects the purifier to the Particle cloud without running its
application firmware. It is the most reliable state for claiming and flashing,
because the stock firmware may not stay connected now that Aura Air has
suspended its app.

1. Press and hold **RESET** and **SETUP** together.
2. Release **RESET**, keep holding **SETUP**.
3. When the LED starts blinking magenta, wait about one second and release
   **SETUP**. If you keep holding until it blinks yellow, you have entered DFU
   mode instead; press **RESET** and start again.
4. Wait 30–60 seconds. The LED blinks green and cyan while it connects, then
   **breathes magenta** when safe mode is online.

## Claim the purifier

With the LED breathing magenta, add the purifier to your account:

```bash
particle device add <device-id>
```

In my testing, former Aura units claimed directly into free
personal accounts after Aura Air's bankruptcy.

If the CLI says the device **belongs to someone else**:

- Answer **No** if it offers to request a transfer. The previous owner cannot
  approve it.
- Contact [Particle support](https://support.particle.io/) or post on the
  [Particle community forum](https://community.particle.io/) with the device
  ID and proof that you own the purifier, and ask for the device to be
  released.
- Do not flash the purifier until it is released into your account.

Optionally give it a name you will recognize:

```bash
particle device rename <device-id> localair-1
```

## Download and verify the firmware

From the [v1.1.0 release](https://github.com/phatwila/LocalAir-Rev1/releases/tag/v1.1.0),
download `localair-rev1.bin`, `SHA256SUMS`, and `release-info.json` into one
folder. Open a terminal in that folder and check them:

- **Linux:** `sha256sum -c SHA256SUMS`
- **macOS:** `shasum -a 256 -c SHA256SUMS`
- **Windows PowerShell:** run `Get-FileHash .\localair-rev1.bin` and
  `Get-FileHash .\release-info.json`. Each `Hash` value must match the line for
  that file in `SHA256SUMS`, ignoring letter case.

Do not flash a file that fails this check.

## Flash LocalAir over the air

With the LED breathing magenta, run this from the download folder:

```bash
particle flash <device-id> localair-rev1.bin
```

LocalAir needs a newer Device OS than the stock firmware. The first time, the
Particle cloud upgrades Device OS automatically:

- The LED blinks magenta rapidly and the purifier restarts several times.
- This takes 5–10 minutes. **Do not unplug the purifier.**
- It finishes breathing cyan, running LocalAir. The fan spins up at startup.

## Check that LocalAir is running

1. Confirm the firmware from the cloud:

   ```bash
   particle get <device-id> FW_VER
   particle get <device-id> BUILD_NUM
   particle get <device-id> localIP
   ```

   `FW_VER` should match the release, for example `localair-rev1+b61`.

2. Open the dashboard at `http://localair.local/`. If your computer cannot
   resolve `.local` names, open `http://<device-ip>/` using the `localIP` value.
3. Optional: the repository's `device-status.sh` runs a fuller check. See
   [Verify runtime status](FLASHING.md#verify-runtime-status).

Then choose a hostname and other settings in the dashboard's System tab. See
the [README](../README.md#current-model) for what each setting does.

## Troubleshooting

- **Still breathing magenta after the update:** the application is not
  running yet. Run the `particle flash` command again.
- **No LED activity or no connection after an update for more than 10
  minutes:** unplug the purifier for 10 seconds and plug it back in. One test
  unit needed this after a firmware update.
- **Blinking green for a long time:** it cannot join Wi-Fi. Check that the
  network is 2.4 GHz and the password is right, then repeat
  [Connect the purifier to Wi-Fi](#connect-the-purifier-to-wi-fi).
- **Red SOS blinks:** note the number of blinks after the SOS and recover over
  USB as below.
- **Recover over USB:** connect the cable and run
  `particle flash --local localair-rev1.bin`. The CLI switches the purifier to
  DFU mode and can also install the Device OS it needs. If it cannot, enter DFU
  mode by hand: hold **RESET** and **SETUP**, release **RESET**, and keep
  holding **SETUP** until the LED blinks yellow. Then run the command again.
  See [USB flash](FLASHING.md#usb-flash) for Windows and WSL notes.
