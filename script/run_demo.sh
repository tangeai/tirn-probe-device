#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
binary="$repo_root/build/linux_device_uplink_demo"

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
      printf 'Usage: %s [--endpoint <url>] --device-id <id> --device-secret-key <key>\n' "$0"
      exit 0
      ;;
    *)
      printf '[demo] unknown argument: %s\n' "$1" >&2
      exit 1
      ;;
  esac
done

require_value device_id "$device_id"
require_value device_secret_key "$device_secret_key"
require_file "$repo_root/assets/audio.g711a"
require_file "$repo_root/assets/video.h264"
require_file "$repo_root/3rd/lib/libtirtc.a"
require_file "$binary"

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
