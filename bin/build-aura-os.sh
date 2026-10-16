#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

DEVICE_OS_DIR_OVERRIDE=""
TOOLCHAIN_DIR_OVERRIDE=""
EXTRA_CFLAGS_VALUE="${EXTRA_CFLAGS:-}"
WARNINGS_AS_ERRORS_VALUE="${WARNINGS_AS_ERRORS:-}"
EXPECTED_DEVICE_OS_REF="$(resolve_expected_device_os_ref)"

usage() {
  cat <<'EOF'
Usage: build-aura-os.sh [options]

Build LocalAir into the current build artifact target/localair-rev1.bin.

Options:
  --device-os-dir PATH      Particle Device OS checkout path
  --toolchain-dir PATH      Toolchain bin directory containing arm-none-eabi-gcc
  --expected-ref REF        Expected Device OS baseline ref for guard (default v3.3.1)
  --extra-cflags VALUE      Extra CFLAGS to pass to the build
  --warnings-as-errors VAL  Override WARNINGS_AS_ERRORS
  --allow-device-os-ref-mismatch
                          Skip Device OS ref guard (sets LOCALAIR_ALLOW_DEVICE_OS_REF_MISMATCH=1)
  -h, --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device-os-dir)
      require_option_value "$1" "${2:-}"
      DEVICE_OS_DIR_OVERRIDE="${2:-}"
      shift 2
      ;;
    --toolchain-dir)
      require_option_value "$1" "${2:-}"
      TOOLCHAIN_DIR_OVERRIDE="${2:-}"
      shift 2
      ;;
    --expected-ref)
      require_option_value "$1" "${2:-}"
      EXPECTED_DEVICE_OS_REF="$(resolve_expected_device_os_ref "${2:-}")"
      shift 2
      ;;
    --extra-cflags)
      require_option_value "$1" "${2:-}"
      EXTRA_CFLAGS_VALUE="${2:-}"
      shift 2
      ;;
    --warnings-as-errors)
      require_option_value "$1" "${2:-}"
      WARNINGS_AS_ERRORS_VALUE="${2:-}"
      shift 2
      ;;
    --allow-device-os-ref-mismatch)
      LOCALAIR_ALLOW_DEVICE_OS_REF_MISMATCH=1
      export LOCALAIR_ALLOW_DEVICE_OS_REF_MISMATCH
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown argument: $1"
      ;;
  esac
done

require_command git "Install git first."
require_command make "Install make first."
require_command python3 "Install python3 first."
require_command which "Install which for the pinned Device OS makefiles."
require_command crc32 "Install crc32 from Perl Archive::Zip for Device OS binary checksums."
require_command xxd "Install xxd for Device OS binary checksums."

DEVICE_OS_DIR="$(resolve_device_os_dir "$DEVICE_OS_DIR_OVERRIDE")"
TOOLCHAIN_DIR="${TOOLCHAIN_DIR_OVERRIDE:-${TOOLCHAIN_DIR:-}}"

[[ -d "$DEVICE_OS_DIR/main" ]] || die "Particle Device OS checkout not found at $DEVICE_OS_DIR. Run ./bin/setup-particle-device-os.sh first."
require_device_os_ref_match "$DEVICE_OS_DIR" "$EXPECTED_DEVICE_OS_REF"
DEVICE_OS_DIR="$(cd -P "$DEVICE_OS_DIR" && pwd)"

if [[ -n "$TOOLCHAIN_DIR" ]]; then
  TOOLCHAIN_DIR="$(resolve_project_path "$TOOLCHAIN_DIR")"
  [[ -d "$TOOLCHAIN_DIR" ]] || die "Toolchain directory not found: $TOOLCHAIN_DIR"
  TOOLCHAIN_DIR="$(cd -P "$TOOLCHAIN_DIR" && pwd)"
  [[ -x "$TOOLCHAIN_DIR/arm-none-eabi-gcc" ]] || die "Selected toolchain has no arm-none-eabi-gcc."
  export PATH="$TOOLCHAIN_DIR:$PATH"
fi
require_command arm-none-eabi-gcc "Install the ARM GCC toolchain or set TOOLCHAIN_DIR."
compiler_path="$(python3 -c 'import os, shutil; print(os.path.abspath(shutil.which("arm-none-eabi-gcc")))')"
# Relative PATH entries must not change meaning inside make -C.
export PATH="$(dirname "$compiler_path"):$PATH"

path_stage="$(python3 "$SCRIPT_DIR/lib/build_paths.py" "$REPO_DIR" "$DEVICE_OS_DIR")"
trap 'rm -rf -- "$path_stage"' EXIT
build_app="$(cat "$path_stage/app-path")"
build_os="$(cat "$path_stage/device-os-path")"

# Repack the dashboard blob from src/web/dashboard.html before building.
require_command python3 "Install python3 for the dashboard pack step."
log_info "Packing dashboard blob"
(cd "$REPO_DIR" && python3 ./src/web/pack-dashboard.py)

BUILD_NUMBER_FILE="$REPO_DIR/build-number.txt"
BUILD_VERSION_HEADER="$REPO_DIR/target/generated/build_version.h"
build_number="$(tr -d '[:space:]' < "$BUILD_NUMBER_FILE" 2>/dev/null || printf '0')"
if [[ ! "$build_number" =~ ^[0-9]+$ ]]; then
  die "Invalid build number in $BUILD_NUMBER_FILE"
fi
next_build_number=$((build_number + 1))
mkdir -p "$(dirname "$BUILD_VERSION_HEADER")"
cat > "$BUILD_VERSION_HEADER" <<EOF_VERSION
#pragma once

#define LOCALAIR_BUILD_NUMBER $next_build_number
#define LOCALAIR_FIRMWARE_BASE "localair-rev1"
#define LOCALAIR_FIRMWARE_VERSION "localair-rev1+b$next_build_number"
EOF_VERSION

log_info "Building LocalAir"
build_source_commit="$(git -C "$REPO_DIR" rev-parse HEAD)"
build_started_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
log_info "Build version: localair-rev1+b$next_build_number"
log_info "Device OS: $DEVICE_OS_DIR"
log_info "Toolchain: $(arm-none-eabi-gcc --version | head -1)"

mkdir -p "$REPO_DIR/target"
# Device OS derives the output basename from APPDIR, including public checkouts
# named LocalAir-Rev1 and owner-chosen directory names.
app_basename="$(basename "$build_app")"
for ext in bin elf hex lst map; do
  rm -f "$REPO_DIR/target/$app_basename.$ext"
done
rm -f "$REPO_DIR/target/localair-rev1."{bin,elf,hex,lst,map} \
      "$REPO_DIR/target/aura-os-v2."{bin,elf,hex,lst,map} 2>/dev/null || true
rm -rf "$build_os/build/target/user/platform-8/aura-os-v2" \
       "$build_os/build/target/user/platform-8-m/aura-os-v2" \
       "$build_os/build/target/user/platform-8/localair-rev1" \
       "$build_os/build/target/user/platform-8-m/localair-rev1"
rm -rf "$build_os/build/target/user/platform-8/$app_basename" \
       "$build_os/build/target/user/platform-8-m/$app_basename"
find "$build_os/build/target" \
  -not -path '*/aura-os-v2/*' \
  -not -path '*/localair-rev1/*' \
  -name '*.o' -exec touch {} + 2>/dev/null || true
find "$build_os/build/target" \
  -not -path '*/aura-os-v2/*' \
  -not -path '*/localair-rev1/*' \
  -name '*.a' -exec touch {} + 2>/dev/null || true

gcc_major="$(arm-none-eabi-gcc -dumpversion 2>/dev/null | cut -d. -f1)"
declare -a make_extra_args=()
if [[ -n "$WARNINGS_AS_ERRORS_VALUE" ]]; then
  make_extra_args+=("WARNINGS_AS_ERRORS=$WARNINGS_AS_ERRORS_VALUE")
elif [[ "$gcc_major" =~ ^[0-9]+$ ]] && (( gcc_major >= 13 )); then
  make_extra_args+=("WARNINGS_AS_ERRORS=n")
  gcc13_gthread_compat="$build_app/src/compat/gcc13_gthread_cond.h"
  EXTRA_CFLAGS_VALUE="${EXTRA_CFLAGS_VALUE:+$EXTRA_CFLAGS_VALUE }-include $gcc13_gthread_compat"
  log_info "Using WARNINGS_AS_ERRORS=n for Device OS compatibility with GCC $gcc_major"
fi

make -j4 -C "$build_os/user" \
  PLATFORM=P1 \
  APPDIR="$build_app" \
  USER_MAKEFILE=build.mk \
  EXTRA_CFLAGS="$EXTRA_CFLAGS_VALUE" \
  ${make_extra_args[@]+"${make_extra_args[@]}"} \
  all

make -j1 -C "$build_os/main" \
  PLATFORM=P1 \
  APPDIR="$build_app" \
  USER_MAKEFILE=build.mk \
  EXTRA_CFLAGS="$EXTRA_CFLAGS_VALUE" \
  ${make_extra_args[@]+"${make_extra_args[@]}"} \
  all

for ext in bin elf hex lst map; do
  legacy="$REPO_DIR/target/$app_basename.$ext"
  renamed="$REPO_DIR/target/localair-rev1.$ext"
  if [[ -f "$legacy" && ! -f "$renamed" ]]; then
    mv "$legacy" "$renamed"
  fi
done

assert_file_exists "$TARGET_BIN_DEFAULT"
python3 - "$REPO_DIR/target/build-inputs.json" "$build_source_commit" "$build_started_at" "$TARGET_BIN_DEFAULT" "$next_build_number" "$DEVICE_OS_DIR" "$compiler_path" "$EXTRA_CFLAGS_VALUE" "$WARNINGS_AS_ERRORS_VALUE" <<'PY'
import datetime, hashlib, json, pathlib, platform, subprocess, sys, zlib
output, source, started, binary, build, device_os, compiler, flags, warnings = sys.argv[1:]
sha = lambda p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
run = lambda *cmd: subprocess.check_output(cmd, text=True).strip()
device_os_commit = run("git", "-C", device_os, "rev-parse", "HEAD")
# Experiments may use untagged commits. Preserve an exact tag when available;
# otherwise record the full commit, which remains ineligible for release proof.
tag = subprocess.run(["git", "-C", device_os, "describe", "--tags", "--exact-match", "HEAD"],
                     text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
device_os_ref = tag.stdout.strip() if tag.returncode == 0 else device_os_commit
data = dict(schema="localair-build-inputs/v1", source_commit=source,
    started_at=started, completed_at=datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z"),
    binary_sha256=sha(binary), binary_size=pathlib.Path(binary).stat().st_size,
    build_number=int(build), firmware_version="localair-rev1+b"+build,
    device_os_ref=device_os_ref, device_os_commit=device_os_commit,
    compiler=compiler, compiler_sha256=sha(compiler), compiler_version=run(compiler, "-dumpfullversion"),
    extra_cflags=flags, warnings_as_errors=warnings, platform="P1", user_makefile="build.mk")
app = pathlib.Path(binary).resolve().parents[1]
data["dashboard_packing"] = dict(
    python_version=platform.python_version(), zlib_version=zlib.ZLIB_VERSION,
    zlib_runtime_version=zlib.ZLIB_RUNTIME_VERSION, compresslevel=9, mtime=0,
    html_sha256=sha(app / "src/web/dashboard.html"),
    header_sha256=sha(app / "src/web/dashboard.h"),
    packer_sha256=sha(app / "src/web/pack-dashboard.py"))
pathlib.Path(output).write_text(json.dumps(data, indent=2)+"\n")
PY
printf '%s\n' "$next_build_number" > "$BUILD_NUMBER_FILE"
log_info "Build complete: $TARGET_BIN_DEFAULT ($(binary_size_bytes "$TARGET_BIN_DEFAULT") bytes)"
