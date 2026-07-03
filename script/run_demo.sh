#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

require_file() {
  if [ ! -f "$1" ]; then
    printf '[demo] missing required file: %s\n' "$1" >&2
    exit 1
  fi
}

require_value() {
  if [ -z "$2" ]; then
    printf '[demo] missing required argument: %s\n' "$1" >&2
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
Usage: ./script/run_demo.sh [--endpoint <url>] --device-id <id> --device-secret-key <key>
USAGE
}

platform=$(detect_platform || true)
endpoint=''
device_id=''
device_secret_key=''

while [ "$#" -gt 0 ]; do
  case "$1" in
    --endpoint)
      [ "$#" -ge 2 ] || { printf '[demo] --endpoint requires a value\n' >&2; exit 1; }
      endpoint=$2
      shift 2
      ;;
    --device-id)
      [ "$#" -ge 2 ] || { printf '[demo] --device-id requires a value\n' >&2; exit 1; }
      device_id=$2
      shift 2
      ;;
    --device-secret-key)
      [ "$#" -ge 2 ] || { printf '[demo] --device-secret-key requires a value\n' >&2; exit 1; }
      device_secret_key=$2
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

if [ -z "$platform" ]; then
  printf '[demo] unsupported host: %s %s\n' "$(uname -s)" "$(uname -m)" >&2
  printf '%s\n' '[demo] supported native hosts: macOS arm64, Linux x86_64' >&2
  exit 1
fi

binary="$repo_root/build/$platform/device_uplink_demo"

require_value device_id "$device_id"
require_value device_secret_key "$device_secret_key"
require_file "$repo_root/assets/audio.g711a"
require_file "$repo_root/assets/video.h264"

if [ ! -f "$binary" ]; then
  printf '[demo] missing demo binary: %s\n' "$binary" >&2
  printf '%s\n' '[demo] run ./script/build.sh first' >&2
  exit 1
fi
if [ "$platform" = "macos-arm64" ]; then
  require_file "$repo_root/build/$platform/libTiRTC.dylib"
  require_file "$repo_root/build/$platform/libtgrtc.dylib"
fi

cd "$repo_root"
if [ -n "$endpoint" ]; then
  exec "$binary" \
    --endpoint "$endpoint" \
    --device-id "$device_id" \
    --device-secret-key "$device_secret_key"
fi

exec "$binary" \
  --device-id "$device_id" \
  --device-secret-key "$device_secret_key"
