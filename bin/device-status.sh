#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

usage() {
  cat <<'EOF'
Usage: device-status.sh <host-or-ip> [options]

Read LocalAir status from a device. This script never changes device state.

Options:
  --expect-version <version>  Require /api/status.firmware_version to match.
  --expect-id <id>            Require the expected Particle identity.
  --expect-build <number>     Require the expected build number.
  --require-fresh-rpm         Require valid RPM no older than ten seconds.
  --events                   Also fetch and validate /api/events.
  --debug                    Also fetch and validate debug endpoints.
  --watch                    Poll until interrupted.
  --json                     Print raw /api/status JSON only.
  -h, --help                 Show this help.

Examples:
  ./bin/device-status.sh localair.local
  ./bin/device-status.sh localair.local --expect-version localair-rev1+b25 --events
EOF
}

HOST=""
EXPECT_VERSION=""
EXPECT_ID=""
EXPECT_BUILD=""
REQUIRE_FRESH_RPM=0
FETCH_EVENTS=0
FETCH_DEBUG=0
WATCH=0
RAW_JSON=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --expect-version)
      require_option_value "$1" "${2:-}"
      EXPECT_VERSION="$2"
      shift 2
      ;;
    --expect-id)
      require_option_value "$1" "${2:-}"
      EXPECT_ID="$2"
      shift 2 ;;
    --expect-build)
      require_option_value "$1" "${2:-}"
      EXPECT_BUILD="$2"
      shift 2 ;;
    --require-fresh-rpm)
      REQUIRE_FRESH_RPM=1
      shift ;;
    --events)
      FETCH_EVENTS=1
      shift
      ;;
    --debug)
      FETCH_DEBUG=1
      shift
      ;;
    --watch)
      WATCH=1
      shift
      ;;
    --json)
      RAW_JSON=1
      shift
      ;;
    --*)
      die "Unknown option: $1"
      ;;
    *)
      if [[ -n "$HOST" ]]; then
        die "Too many arguments. Use --help."
      fi
      HOST="$1"
      shift
      ;;
  esac
done

[[ -n "$HOST" ]] || die "Host or IP required. Use --help."
require_command curl "Install curl first."
require_command python3 "Install python3 first."

base_url() {
  local host="$1"
  if [[ "$host" =~ ^https?:// ]]; then
    printf '%s\n' "${host%/}"
  else
    printf 'http://%s\n' "${host%/}"
  fi
}

fetch_json() {
  local url="$1"
  curl -fsS --connect-timeout 3 --max-time 8 "$url"
}

validate_status() {
  local json="$1"
  local expected="$2"
  STATUS_JSON="$json" EXPECT_VERSION_VALUE="$expected" EXPECT_ID_VALUE="$EXPECT_ID" EXPECT_BUILD_VALUE="$EXPECT_BUILD" REQUIRE_FRESH_RPM="$REQUIRE_FRESH_RPM" python3 - <<'PY'
import json
import math
import os
import sys

try:
    status = json.loads(os.environ["STATUS_JSON"])
except Exception as exc:
    print(f"[error] invalid /api/status JSON: {exc}", file=sys.stderr)
    sys.exit(2)

required_top = ["particle_id", "mdns_host", "local_ip", "firmware_version", "build_number", "control"]
missing = [key for key in required_top if key not in status]
if missing:
    print(f"[error] /api/status missing: {', '.join(missing)}", file=sys.stderr)
    sys.exit(2)

control = status.get("control")
if not isinstance(control, dict):
    print("[error] /api/status.control is not an object", file=sys.stderr)
    sys.exit(2)

required_control = ["fan_ready", "actuation_ready", "rpm_actual", "mode"]
missing = [key for key in required_control if key not in control]
if missing:
    print(f"[error] /api/status.control missing: {', '.join(missing)}", file=sys.stderr)
    sys.exit(2)

expected = os.environ.get("EXPECT_VERSION_VALUE", "")
actual = str(status.get("firmware_version", ""))
if expected and actual != expected:
    print(f"[error] firmware_version mismatch: expected {expected}, got {actual}", file=sys.stderr)
    sys.exit(3)

if status.get("mdns_host") in ("", "pending", None):
    print("[error] mdns_host is not ready", file=sys.stderr)
    sys.exit(4)
if status.get("particle_id") in ("", None):
    print("[error] particle_id is not ready", file=sys.stderr)
    sys.exit(4)
if control.get("fan_ready") is not True:
    print("[error] fan_ready is not true", file=sys.stderr)
    sys.exit(4)
if control.get("actuation_ready") is not True:
    print("[error] actuation_ready is not true", file=sys.stderr)
    sys.exit(4)
try:
    rpm_actual = float(control.get("rpm_actual"))
except (TypeError, ValueError):
    print("[error] rpm_actual is not numeric", file=sys.stderr)
    sys.exit(4)
if not math.isfinite(rpm_actual) or rpm_actual <= 0:
    print("[error] rpm_actual is not positive", file=sys.stderr)
    sys.exit(4)

for env, key in (("EXPECT_ID_VALUE", "particle_id"), ("EXPECT_BUILD_VALUE", "build_number")):
    expected_value = os.environ.get(env, "")
    if expected_value and str(status.get(key)) != expected_value:
        sys.exit(f"[error] {key} mismatch")
if os.environ["REQUIRE_FRESH_RPM"] == "1":
    age = control.get("rpm_age_ms")
    if (control.get("rpm_valid") is not True or control.get("rpm_fresh") is not True
            or type(age) is not int or not 0 <= age <= 10000):
        sys.exit("[error] RPM feedback is invalid or stale")

print("mdns_host=" + str(status.get("mdns_host")))
print("particle_id=" + str(status.get("particle_id")))
print("local_ip=" + str(status.get("local_ip")))
print("firmware_version=" + actual)
print("build_number=" + str(status.get("build_number")))
print("mode=" + str(control.get("mode")))
print("fan_ready=" + str(control.get("fan_ready")).lower())
print("actuation_ready=" + str(control.get("actuation_ready")).lower())
print("rpm_actual=" + str(control.get("rpm_actual")))
PY
}

validate_json_endpoint() {
  local name="$1"
  local json="$2"
  ENDPOINT_NAME="$name" ENDPOINT_JSON="$json" python3 - <<'PY'
import json
import os
import sys

name = os.environ["ENDPOINT_NAME"]
try:
    json.loads(os.environ["ENDPOINT_JSON"])
except Exception as exc:
    print(f"[error] invalid {name} JSON: {exc}", file=sys.stderr)
    sys.exit(2)
print(f"{name}=ok")
PY
}

poll_once() {
  local base="$1"
  local status_json
  status_json="$(fetch_json "$base/api/status")"
  if [[ "$RAW_JSON" -eq 1 ]]; then
    validate_status "$status_json" "$EXPECT_VERSION" >&2
    printf '%s\n' "$status_json"
  else
    validate_status "$status_json" "$EXPECT_VERSION"
  fi

  if [[ "$FETCH_EVENTS" -eq 1 ]]; then
    validate_json_endpoint "/api/events" "$(fetch_json "$base/api/events")"
  fi

  if [[ "$FETCH_DEBUG" -eq 1 ]]; then
    validate_json_endpoint "/api/debug/http" "$(fetch_json "$base/api/debug/http")"
    validate_json_endpoint "/api/debug/commands" "$(fetch_json "$base/api/debug/commands")"
    validate_json_endpoint "/api/debug/actuation" "$(fetch_json "$base/api/debug/actuation")"
  fi
}

BASE_URL="$(base_url "$HOST")"

if [[ "$WATCH" -eq 1 ]]; then
  while true; do
    poll_once "$BASE_URL"
    sleep 2
  done
fi

poll_once "$BASE_URL"
