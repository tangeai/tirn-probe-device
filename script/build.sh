#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf '[tirn-probe-device] missing required command: %s\n' "$1" >&2
    exit 1
  fi
}

require_file() {
  if [ ! -f "$1" ]; then
    printf '[tirn-probe-device] missing required file: %s\n' "$1" >&2
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
Usage: ./script/build.sh [--platform macos-arm64|linux-x86_64] [--sdk-dir <path>]

Builds tirn-probe-device for the current native host platform.
Cross compilation is not supported by this script.
The SDK defaults to third_party/tirtc/<platform>. It can also be selected with
--sdk-dir or the TIRTC_SDK_DIR environment variable.
USAGE
}

validate_sdk() {
  platform=$1
  sdk_dir=$2

  require_file "$sdk_dir/include/tirtc/tiRTC.h"
  require_file "$sdk_dir/include/tirtc/basedef.h"

  case "$platform" in
    macos-arm64)
      require_file "$sdk_dir/lib/libTiRTC.dylib"
      require_file "$sdk_dir/lib/libtgrtc.dylib"
      ;;
    linux-x86_64)
      require_file "$sdk_dir/lib/libTiRTC.a"
      ;;
    *)
      printf '[tirn-probe-device] unsupported platform: %s\n' "$platform" >&2
      exit 1
      ;;
  esac
}

sdk_complete() {
  platform=$1
  sdk_dir=$2

  [ -f "$sdk_dir/include/tirtc/tiRTC.h" ] || return 1
  [ -f "$sdk_dir/include/tirtc/basedef.h" ] || return 1

  case "$platform" in
    macos-arm64)
      [ -f "$sdk_dir/lib/libTiRTC.dylib" ] || return 1
      [ -f "$sdk_dir/lib/libtgrtc.dylib" ] || return 1
      ;;
    linux-x86_64)
      [ -f "$sdk_dir/lib/libTiRTC.a" ] || return 1
      ;;
    *)
      return 1
      ;;
  esac
}

host_platform=$(detect_platform || true)
platform=$host_platform
sdk_dir=${TIRTC_SDK_DIR:-}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --platform)
      [ "$#" -ge 2 ] || { printf '[tirn-probe-device] --platform requires a value\n' >&2; exit 1; }
      platform=$2
      shift 2
      ;;
    --sdk-dir)
      [ "$#" -ge 2 ] || { printf '[tirn-probe-device] --sdk-dir requires a value\n' >&2; exit 1; }
      sdk_dir=$2
      shift 2
      ;;
    --help)
      print_usage
      exit 0
      ;;
    *)
      printf '[tirn-probe-device] unknown argument: %s\n' "$1" >&2
      exit 1
      ;;
  esac
done

if [ -z "$host_platform" ]; then
  printf '[tirn-probe-device] unsupported host: %s %s\n' "$(uname -s)" "$(uname -m)" >&2
  printf '%s\n' '[tirn-probe-device] supported native hosts: macOS arm64, Linux x86_64' >&2
  exit 1
fi

case "$platform" in
  macos-arm64|linux-x86_64) ;;
  *)
    printf '[tirn-probe-device] unsupported platform: %s\n' "$platform" >&2
    exit 1
    ;;
esac

if [ "$platform" != "$host_platform" ]; then
  printf '[tirn-probe-device] requested platform %s does not match native host %s\n' "$platform" "$host_platform" >&2
  printf '%s\n' '[tirn-probe-device] use a matching native host.' >&2
  exit 1
fi

require_command make
require_file "$repo_root/Makefile"

if [ -z "$sdk_dir" ]; then
  sdk_dir="$repo_root/third_party/tirtc/$platform"
elif [ "${sdk_dir#/}" = "$sdk_dir" ]; then
  sdk_dir="$repo_root/$sdk_dir"
fi

if ! sdk_complete "$platform" "$sdk_dir"; then
  printf '[tirn-probe-device] SDK for %s is incomplete.\n' "$platform" >&2
  printf '[tirn-probe-device] checked SDK directory: %s\n' "$sdk_dir" >&2
  printf '%s\n' '[tirn-probe-device] install a compatible TiRTC SDK or pass --sdk-dir; see README.md.' >&2
  validate_sdk "$platform" "$sdk_dir"
fi

printf '[tirn-probe-device] building for %s\n' "$platform"
cd "$repo_root"
make PLATFORM="$platform" TIRTC_SDK_DIR="$sdk_dir" clean-platform
make PLATFORM="$platform" TIRTC_SDK_DIR="$sdk_dir"
printf '[tirn-probe-device] build output: %s\n' "$repo_root/build/$platform/tirn_probe_device"
