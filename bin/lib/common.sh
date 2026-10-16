#!/usr/bin/env bash
set -euo pipefail

COMMON_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$(cd "$COMMON_LIB_DIR/.." && pwd)"
REPO_DIR="$(cd -P "$BIN_DIR/.." && pwd)"
TARGET_BIN_DEFAULT="$REPO_DIR/target/localair-rev1.bin"
DEVICE_OS_REF_DEFAULT="v3.3.1"

log_info() {
  printf '[info] %s\n' "$*"
}

log_warn() {
  printf '[warn] %s\n' "$*" >&2
}

die() {
  printf '[error] %s\n' "$*" >&2
  exit 1
}

require_command() {
  local cmd="$1"
  local hint="${2:-}"
  if command -v "$cmd" >/dev/null 2>&1; then
    return 0
  fi
  if [[ -n "$hint" ]]; then
    die "$cmd is required. $hint"
  fi
  die "$cmd is required."
}

require_option_value() {
  local option="$1"
  local value="${2:-}"
  if [[ -z "$value" || "$value" == --* ]]; then
    die "$option requires a value. Use --help for usage."
  fi
}

resolve_project_path() {
  local value="$1"
  case "$value" in
    *$'\n'*|*$'\r'*) die "Paths must not contain line breaks." ;;
  esac
  if [[ "$value" != /* ]]; then
    value="$REPO_DIR/$value"
  fi
  printf '%s\n' "$value"
}

project_relative_path() {
  local value="$1" base="$REPO_DIR" prefix=""
  while [[ "$base" != / && "$value" != "$base" && "$value" != "$base/"* ]]; do
    base="${base%/*}"
    [[ -n "$base" ]] || base=/
    prefix="../$prefix"
  done
  if [[ "$value" == "$base" ]]; then
    printf '%s\n' "${prefix:-.}"
  elif [[ "$base" == / ]]; then
    printf '%s%s\n' "$prefix" "${value#/}"
  else
    printf '%s%s\n' "$prefix" "${value#"$base/"}"
  fi
}

resolve_particle_cli() {
  local candidate
  if [[ -n "${PARTICLE_CLI:-}" ]]; then
    candidate="$(resolve_project_path "$PARTICLE_CLI")" || return 1
    [[ -f "$candidate" && -x "$candidate" ]] || return 1
    printf '%s\n' "$candidate"
    return 0
  fi

  if command -v particle >/dev/null 2>&1; then
    command -v particle
    return 0
  fi

  for candidate in "$HOME/bin/particle" "$HOME/.particle/bin/particle"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

require_particle_cli() {
  local particle_cli
  particle_cli="$(resolve_particle_cli)" || die "particle CLI is required. Install it or set PARTICLE_CLI."
  printf '%s\n' "$particle_cli"
}

resolve_device_os_dir() {
  resolve_project_path "${1:-${DEVICE_OS_DIR:-device-os}}"
}

resolve_binary_path() {
  resolve_project_path "${1:-target/localair-rev1.bin}"
}

assert_file_exists() {
  local path="$1"
  [[ -f "$path" ]] || die "File not found: $path"
}

binary_size_bytes() {
  local path="$1"
  wc -c <"$path" | tr -d '[:space:]'
}

resolve_expected_device_os_ref() {
  local value="${1:-${EXPECTED_DEVICE_OS_REF:-${LOCALAIR_EXPECTED_DEVICE_OS_REF:-}}}"
  if [[ -z "$value" ]]; then
    value="$DEVICE_OS_REF_DEFAULT"
  fi
  printf '%s\n' "$value"
}

resolve_device_os_tag_ref() {
  local value="$1"
  if [[ "$value" == refs/tags/* ]]; then
    printf '%s\n' "$value"
    return 0
  fi
  printf 'refs/tags/%s\n' "$value"
}

device_os_ref_mismatch_override_enabled() {
  [[ "${LOCALAIR_ALLOW_DEVICE_OS_REF_MISMATCH:-0}" == "1" ]]
}

require_device_os_ref_match() {
  local repo_dir="$1"
  local expected_ref="$2"
  local expected_tag_ref expected_commit actual_commit

  [[ -d "$repo_dir/.git" ]] || die "Device OS checkout not found at $repo_dir"

  expected_tag_ref="$(resolve_device_os_tag_ref "$expected_ref")"
  expected_commit="$(git -C "$repo_dir" rev-parse --verify --quiet "${expected_tag_ref}^{commit}" || true)"
  if [[ -z "$expected_commit" ]]; then
    if device_os_ref_mismatch_override_enabled; then
      log_warn "Device OS ref mismatch override enabled."
      log_warn "Expected Device OS ref not found: ${expected_tag_ref}; continuing anyway."
      return 0
    fi
    die "Expected Device OS ref not found: ${expected_tag_ref}. Fetch tags or run ./bin/setup-particle-device-os.sh."
  fi

  actual_commit="$(git -C "$repo_dir" rev-parse --verify --quiet HEAD || true)"
  [[ -n "$actual_commit" ]] || die "Failed to resolve Device OS HEAD commit at $repo_dir"

  if [[ "$actual_commit" == "$expected_commit" ]]; then
    log_info "Device OS ref guard passed ($expected_tag_ref @ $expected_commit)"
    return 0
  fi

  if device_os_ref_mismatch_override_enabled; then
    log_warn "Device OS ref mismatch override enabled."
    log_warn "Expected $expected_tag_ref @ $expected_commit but found HEAD @ $actual_commit"
    return 0
  fi

  die "Device OS ref mismatch: expected ${expected_tag_ref} @ ${expected_commit}, got HEAD @ ${actual_commit}. Set LOCALAIR_ALLOW_DEVICE_OS_REF_MISMATCH=1 or pass --allow-device-os-ref-mismatch to continue."
}
