#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf '[demo] missing required command: %s\n' "$1" >&2
    exit 1
  fi
}

require_file() {
  if [ ! -f "$1" ]; then
    printf '[demo] missing required file: %s\n' "$1" >&2
    exit 1
  fi
}

detect_platform() {
  os=$(uname -s)
  arch=$(uname -m)

  case "$os:$arch" in
    Darwin:arm64)
      printf '%s\n' macos-arm64
      ;;
    Linux:x86_64|Linux:amd64)
      printf '%s\n' linux-x86_64
      ;;
    *)
      return 1
      ;;
  esac
}

print_usage() {
  cat <<'USAGE'
Usage: ./script/build.sh [--platform macos-arm64|linux-x86_64]

Builds the demo for the current native host platform.
Cross compilation is not supported by this script.
USAGE
}

validate_sdk() {
  platform=$1
  sdk_dir="$repo_root/3rd/$platform"

  require_file "$sdk_dir/include/tirtc/tiRTC.h"
  require_file "$sdk_dir/include/tirtc/basedef.h"
  require_file "$sdk_dir/lib/libTiRTC.a"

  case "$platform" in
    macos-arm64)
      require_file "$sdk_dir/lib/libTiRTC.dylib"
      require_file "$sdk_dir/lib/libtgrtc.dylib"
      ;;
    linux-x86_64) ;;
    *)
      printf '[demo] unsupported platform: %s\n' "$platform" >&2
      exit 1
      ;;
  esac
}

sdk_complete() {
  platform=$1
  sdk_dir="$repo_root/3rd/$platform"

  [ -f "$sdk_dir/include/tirtc/tiRTC.h" ] || return 1
  [ -f "$sdk_dir/include/tirtc/basedef.h" ] || return 1
  [ -f "$sdk_dir/lib/libTiRTC.a" ] || return 1

  case "$platform" in
    macos-arm64)
      [ -f "$sdk_dir/lib/libTiRTC.dylib" ] || return 1
      [ -f "$sdk_dir/lib/libtgrtc.dylib" ] || return 1
      ;;
    linux-x86_64) ;;
    *)
      return 1
      ;;
  esac
}

host_platform=$(detect_platform || true)
platform=$host_platform

while [ "$#" -gt 0 ]; do
  case "$1" in
    --platform)
      [ "$#" -ge 2 ] || { printf '[demo] --platform requires a value\n' >&2; exit 1; }
      platform=$2
      shift 2
      ;;
    --help)
      print_usage
      exit 0
      ;;
    *)
      printf '[demo] unknown argument: %s\n' "$1" >&2
      exit 1
      ;;
  esac
done

if [ -z "$host_platform" ]; then
  printf '[demo] unsupported host: %s %s\n' "$(uname -s)" "$(uname -m)" >&2
  printf '%s\n' '[demo] supported native hosts: macOS arm64, Linux x86_64' >&2
  exit 1
fi

case "$platform" in
  macos-arm64|linux-x86_64) ;;
  *)
    printf '[demo] unsupported platform: %s\n' "$platform" >&2
    exit 1
    ;;
esac

if [ "$platform" != "$host_platform" ]; then
  printf '[demo] requested platform %s does not match native host %s\n' "$platform" "$host_platform" >&2
  printf '%s\n' '[demo] use a matching host or run the Linux command in README from your own container.' >&2
  exit 1
fi

require_command make
require_file "$repo_root/Makefile"

if ! sdk_complete "$platform"; then
  printf '[demo] SDK for %s is incomplete.\n' "$platform" >&2
  printf '%s\n' '[demo] restore 3rd/<platform> from the vendored package or download a replacement SDK as described in README.md.' >&2
  validate_sdk "$platform"
fi

printf '[demo] building device_uplink_demo for %s\n' "$platform"
cd "$repo_root"
make PLATFORM="$platform" clean-platform
make PLATFORM="$platform"
printf '[demo] build output: %s\n' "$repo_root/build/$platform/device_uplink_demo"
