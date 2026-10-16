# Automatic control loop

LocalAir computes a fan target from sensor signals and room settings:

```text
Sensors -> trust checks -> Auto demand -> rate limit
        -> bounded fan step -> PWM on P1S0
```

Fan step 1 is fastest. Step 16 is slowest. The percentages shown by the
dashboard describe control demand. They are not airflow measurements.

## Control source priority

The active source has this priority:

```text
Manual > Startup > Schedule > Auto > Fallback
```

Manual control persists as the selected intent until the user returns to Auto.
Schedule control uses its configured percentage while active. It does not
overwrite saved Manual intent. Returning to Auto clears Manual intent.

## Auto inputs

Auto uses the particle sensor, VOC, eCO2, temperature, relative humidity, room
profile, optional room volume, and configured minimum and maximum fan bounds.
The VOC lane subtracts an adaptive clean-air baseline before scoring. Each
signal has its own trust state: Valid, Degraded, Stale, or Fault.

Only Valid and Degraded pollutant readings can seed or update the smoothed
history. Invalid, stale, and faulty readings leave retained history unchanged.
The affected lane loses authority independently. SGP30 warmup keeps the gas
lane unavailable for its warmup period.

eCO2 is an estimated ventilation proxy. It is not measured room CO2, and its
value does not prove occupancy, ventilation performance, or gas removal. The
15 second SGP30 interval is initialization and warmup handling. It does not
claim full sensor or room stabilization. Gas airflow remains a policy proxy.
Airflow and CADR also remain model values. RPM measures fan rotation only.

## Timing and trust

The nominal Auto scheduler runs at 1 Hz. Slow sensor estimation runs every
20 seconds. Sensor freshness and engine trust use separate windows:

| Signal | Valid through | Degraded through | Fault after |
| --- | ---: | ---: | ---: |
| Particle matter | 3 seconds | 10 seconds | 20 seconds |
| VOC and eCO2 | 2 seconds | 5 seconds | 15 seconds |
| Temperature and humidity | 3 seconds | 10 seconds | 20 seconds |

The particle sensor source also uses a 15 second freshness window. The SGP30
source uses a 5 second freshness window. A value must be finite and
nonnegative for PM, VOC, and eCO2. Temperature must be finite. Humidity must
be finite and between 0 and 100 percent.

Close changes in the dominant pollution lane require 10 elapsed seconds. A
clearly stronger lane can switch at once. The loop carries distinct PM and
SGP30 sample generations and updates each estimator once per new usable
generation. Elapsed smoothing uses
`alpha_eff = 1 - (1 - alpha_1s)^(dt_ms/1000)`. Rise scoring is normalized by
elapsed sample time. After a fault-length gap, the first recovery update uses
one nominal second.

Airflow slew uses 10 percentage points per second upward, 20 during purge, 5
downward, and 3 during recovery. Each control call contributes at most one
second. A three-second post-rise hold and existing step dwell periods remain.

## Bounds, fallback, and quantization

Auto first computes a room-aware airflow demand. It then applies the configured
Auto floor and cap. A FaultSafe fallback starts at 40 percent, clamps to those
same Auto bounds, bypasses rate limiting, and goes through fan-step
quantization. The final step is the command sent to the fan backend.

The HTTP configuration validates a floor range of 40 to 100 percent and a cap
range of 1 to 100 percent, then requires the floor to be below the cap.
With the default floor of 40, the cap must exceed 40. Manual and Schedule
requests do not use the Auto floor and cap.

Quantization follows demand selection and applicable rate limiting. The engine
selects the nearest fan step whose mapped percentage fits both configured
bounds, breaking ties toward the slower step. Changed bounds apply before the
next slew allowance. The configuration endpoint returns HTTP 400 when no
mapped step fits. Invalid saved bounds are repaired to a deterministic
representable pair and the repair is logged.
The status view separates the
target demand, applied demand, selected step, and RPM feedback. A selected
step records the command; it does not prove that the fan produced the expected
airflow.

The Auto tick runs every second even when a sensor read fails. Valid PM data
can control Auto while gas remains unseen or faulted. If no lane has authority,
Auto uses FaultSafe at its 40% raw target. Failed SGP30 initialization retries
every 30 seconds. A sustained measurement outage also schedules
reinitialization. A successful retry starts the 15 second initialization and
warmup interval. Sensor ages use monotonic, wrap-safe tracking and remain
faulted until a fresh valid sample arrives.

## Schedule timing

Schedules use minutes after midnight with an inclusive start and exclusive end.
Overnight windows are supported. The schedule remains inactive until
`Time.isValid()` is true, which requires the device clock to become valid.

The saved `timezone_offset_hours` converts the UTC device clock to the
schedule's local time. It is a numeric UTC offset, not a named timezone. The
dashboard syncs the offset when the Schedule group saves. Save that group
again after a daylight-saving or timezone change.

## Readiness and RPM

The firmware starts with `module_present_assumed` set to true. Readiness uses a
15 second boot-settle timer before entering the boot-ready state. This is an
operating assumption and timer gate. It does not physically prove that a
module is installed, that the fan is moving, or that the room receives the
expected airflow.

`actuation_ready` can become true after the assumed module reaches boot-ready.
It drops when the assumed module is marked missing or lost. Direct PWM starts
during startup, before `actuation_ready`; startup PWM therefore does not prove
fan actuation.

RPM samples are taken every five seconds without issuing fan commands. A
completed zero-pulse sample is valid zero feedback, but it does not by itself
prove a mechanical stall. Unavailable or overlong samples are invalid. Public
status reports samples older than 10 seconds as stale and exposes zero RPM with
`rpm_fresh: false`. A fresh, valid, positive sample can update the learned RPM
factor once.

Use `/api/status` for the public contract. The optional
`/api/debug/actuation` route exposes readiness, RPM, and trust diagnostics.
These local routes have no application authentication or TLS. Keep dashboard,
HTTP, SSE, and MQTT access on a trusted network.
