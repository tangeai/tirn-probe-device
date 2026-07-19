#!/bin/sh
set -eu

requested=${TIRTC_SDK_VARIANT:-}
if [ "${1:-}" = "--tirtc-sdk" ]; then
  if [ "$#" -lt 2 ]; then
    printf '%s\n' '[probe-runner] --tirtc-sdk requires standard or desktop' >&2
    exit 2
  fi
  requested=$2
  shift 2
fi

command=${1:-}
if [ -z "$requested" ]; then
  if [ "$command" = "idle" ]; then
    requested=desktop
  else
    requested=standard
  fi
fi

case "$requested" in
  standard|tgmp-linux-standard)
    variant=tgmp-linux-standard
    binary=/usr/local/libexec/tirtc_accel_device_probe-standard
    ;;
  desktop|tgmp-linux-desktop-standard)
    variant=tgmp-linux-desktop-standard
    binary=/usr/local/libexec/tirtc_accel_device_probe-desktop
    export LD_LIBRARY_PATH="/opt/tirtc/desktop/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    ;;
  *)
    printf '[probe-runner] unsupported TiRTC SDK variant: %s\n' "$requested" >&2
    printf '%s\n' '[probe-runner] expected standard or desktop' >&2
    exit 2
    ;;
esac

if [ "$command" = "idle" ] && [ "$variant" != "tgmp-linux-desktop-standard" ]; then
  printf '%s\n' '[probe-runner] idle requires tgmp-linux-desktop-standard' >&2
  exit 2
fi

printf '[probe-runner] selected TiRTC SDK variant: %s\n' "$variant" >&2
exec "$binary" "$@"
