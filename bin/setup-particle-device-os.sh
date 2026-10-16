#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"

DEVICE_OS_DIR="${DEVICE_OS_DIR:-$REPO_DIR/device-os}"
DEVICE_OS_REMOTE="${DEVICE_OS_REMOTE:-https://github.com/particle-iot/device-os.git}"
EXPECTED_DEVICE_OS_REF="$(resolve_expected_device_os_ref)"
DEVICE_OS_REF="${DEVICE_OS_REF:-$EXPECTED_DEVICE_OS_REF}"

usage() {
  cat <<'EOF'
Usage: setup-particle-device-os.sh [options]

Clone or reuse a local Particle Device OS checkout.

Options:
  --device-os-dir PATH   Checkout path
  --remote URL           Git remote URL
  --ref REF              Git ref or tag to check out
  --expected-ref REF     Expected baseline ref for guard (default v3.3.1)
  --allow-device-os-ref-mismatch
                        Skip Device OS ref guard (sets LOCALAIR_ALLOW_DEVICE_OS_REF_MISMATCH=1)
  -h, --help             Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device-os-dir)
      require_option_value "$1" "${2:-}"
      DEVICE_OS_DIR="${2:-}"
      shift 2
      ;;
    --remote)
      require_option_value "$1" "${2:-}"
      DEVICE_OS_REMOTE="${2:-}"
      shift 2
      ;;
    --ref)
      require_option_value "$1" "${2:-}"
      DEVICE_OS_REF="${2:-}"
      shift 2
      ;;
    --expected-ref)
      require_option_value "$1" "${2:-}"
      EXPECTED_DEVICE_OS_REF="$(resolve_expected_device_os_ref "${2:-}")"
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
DEVICE_OS_DIR="$(resolve_device_os_dir "$DEVICE_OS_DIR")"

select_remote_name() {
  local repo_dir="$1"
  local remote_url="$2"
  local remote

  while IFS= read -r remote; do
    [[ -n "$remote" ]] || continue
    if [[ "$(git -C "$repo_dir" remote get-url "$remote" 2>/dev/null || true)" == "$remote_url" ]]; then
      printf '%s\n' "$remote"
      return 0
    fi
  done < <(git -C "$repo_dir" remote)

  if ! git -C "$repo_dir" remote | grep -qx "device-os-upstream"; then
    git -C "$repo_dir" remote add device-os-upstream "$remote_url"
  else
    git -C "$repo_dir" remote set-url device-os-upstream "$remote_url"
  fi
  printf '%s\n' "device-os-upstream"
}

mkdir -p "$(dirname "$DEVICE_OS_DIR")"

if [[ ! -d "$DEVICE_OS_DIR/.git" ]]; then
  log_info "Cloning Particle Device OS into $DEVICE_OS_DIR"
  git clone --recursive "$DEVICE_OS_REMOTE" "$DEVICE_OS_DIR"
fi

if [[ -n "$DEVICE_OS_REF" ]]; then
  if ! git -C "$DEVICE_OS_DIR" diff --quiet --ignore-submodules HEAD --; then
    die "Device OS checkout is dirty; refusing to switch refs automatically."
  fi
  remote_name="$(select_remote_name "$DEVICE_OS_DIR" "$DEVICE_OS_REMOTE")"
  git -C "$DEVICE_OS_DIR" fetch --tags "$remote_name"
  git -C "$DEVICE_OS_DIR" checkout "$DEVICE_OS_REF"
fi

git -C "$DEVICE_OS_DIR" submodule update --init --recursive
require_device_os_ref_match "$DEVICE_OS_DIR" "$EXPECTED_DEVICE_OS_REF"

DEVICE_OS_DIR="$(cd -P "$DEVICE_OS_DIR" && pwd)"
relative_checkout="$(project_relative_path "$DEVICE_OS_DIR")"
printf 'Particle Device OS is staged at project-relative path:\n  %s\n\n' "$relative_checkout"
printf 'Next step, from the project root:\n  ./bin/build-aura-os.sh --device-os-dir %q\n' "$relative_checkout"

cat <<EOF

Default baseline Device OS ref is $DEVICE_OS_REF_DEFAULT.
Guard expected ref: $EXPECTED_DEVICE_OS_REF
Checked out ref: $DEVICE_OS_REF
You can override DEVICE_OS_DIR, DEVICE_OS_REMOTE, or DEVICE_OS_REF if needed.
EOF
