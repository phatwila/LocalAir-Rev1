# Contributing

LocalAir targets F00053AA Aura Air Rev 1.0 hardware. Keep changes within
that hardware scope. Do not imply support for another Aura Air model without
separate hardware evidence and my approval.

## Setup

Use a Linux or WSL shell from the package root. Stage the pinned Particle
Device OS checkout before building:

```bash
bash ./bin/setup-particle-device-os.sh
./bin/build-aura-os.sh
```

The build uses Particle Device OS `v3.3.1` and should use ARM GCC 10.2.1.
It uses `arm-none-eabi-gcc` already on `PATH`. Pass
`--device-os-dir` or `--toolchain-dir "./toolchains/.../bin"` when needed.
Relative paths resolve from the project root inferred by the helper, regardless
of caller directory. Explicit absolute paths, clone names, nested checkouts,
and spaces work automatically. Some Device OS paths require a temporary copy,
adding staging time and disk usage. The helper removes that copy afterward
and repacks the dashboard source automatically.

Dashboard-only changes can repack the embedded page with:

```bash
python3 ./src/web/pack-dashboard.py
```

Review the generated dashboard header and the final diff before opening a PR.
Do not include generated release binaries unless I request them.

## Code and documentation

Keep firmware changes small and explicit. Reuse the existing modules under
`src/` and preserve the public HTTP, SSE, MQTT, and dashboard contracts.
Document user-visible changes in `README.md` or the relevant document.

Treat these facts as part of the public contract:

- module readiness is assumed and timer-gated
- RPM reports rotational feedback, not measured airflow
- eCO2 is an estimated ventilation signal, not measured room CO2
- Auto fallback is clamped to configured bounds before quantization
- schedules require valid device time and use the saved UTC offset
- local HTTP has no authentication or TLS; MQTT uses plain TCP with optional broker credentials

Do not include real device IDs, private IP addresses, Wi-Fi credentials, room
names, workstation paths, private logs, or unreleased evidence in committed
examples, screenshots, or issue reports. Runtime status intentionally reports
the user's own `particle_id` and `local_ip` for identification and diagnosis.

## Pull requests

Describe the user-visible behavior and the hardware affected. Include the
commands you ran and any limitations that remain. Keep commits focused, use a
clear prefix such as `firmware:`, `docs:`, or `tools:`, and explain changes to
public fields, routes, settings, or helper arguments.

Do not add copied code, generated assets, or libraries without checking their
license. Particle Device OS is an external build dependency and remains under
its own license.

Use GitHub issues for reproducible bugs and documentation gaps. Follow
[SECURITY.md](SECURITY.md) for vulnerabilities and
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for project conduct.

LocalAir is source-available under the PolyForm Noncommercial License
1.0.0. By submitting a contribution, you confirm that you have the right to
submit it, and you grant Ray Detwiler a perpetual, worldwide, irrevocable,
royalty-free license to use, modify, sublicense, and relicense it, including
under commercial terms. You keep the copyright in your contribution.

## Project name

Use **LocalAir** as the project and interface name.
Use **Custom local firmware for the Aura Air Rev 1.0.** as the tagline.
Keep hardware model and revision details in compatibility documentation.
Keep actual repository URLs, artifact filenames, protocol identifiers, and recorded firmware versions accurate.
Preserve the required copyright notice in `LICENSE`.
