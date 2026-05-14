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

case "${1:-}" in
  --help)
    printf 'Usage: %s\n' "$0"
    exit 0
    ;;
  '')
    ;;
  *)
    printf '[demo] unknown argument: %s\n' "$1" >&2
    exit 1
    ;;
esac

if [ "$(uname -s)" != "Linux" ]; then
  printf '[demo] build.sh only supports Linux hosts.\n' >&2
  exit 1
fi

case "$(uname -m)" in
  x86_64|amd64) ;;
  *)
    printf '[demo] build.sh requires Linux x86_64.\n' >&2
    exit 1
    ;;
esac

require_file "$repo_root/Makefile"
require_file "$repo_root/3rd/include/tiRTC.h"
require_file "$repo_root/3rd/include/basedef.h"
require_file "$repo_root/3rd/lib/libtirtc.a"

printf '[demo] building linux_device_uplink_demo\n'
cd "$repo_root"
make clean
make
printf '[demo] build output: %s\n' "$repo_root/build/linux_device_uplink_demo"
