#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runner="$script_dir/run_tirn_probe_netem_report.sh"

image=${PROBE_IMAGE:-docker-hub.tange365.com/runtime/tirn-probe-device-runner:test}
losses=${LOSSES:-0,10,30,50}
delays_ms=${DELAYS_MS:-0,25,50,100,150}
audio_check_stable_sec=${AUDIO_CHECK_STABLE_SEC:-3}
pull_image=${PULL_IMAGE:-1}

usage() {
  cat <<'USAGE'
Usage:
  ./script/run_tirn_probe_connect_matrix.sh

The script reads test credentials from script/.env through
run_tirn_probe_netem_report.sh. Environment variables accepted by that script
remain available, including:

  PROBE_IMAGE=docker-hub.tange365.com/runtime/tirn-probe-device-runner:test
  PULL_IMAGE=1
  LOSSES=0,10,30,50
  DELAYS_MS=0,25,50,100,150
  CONNECT_ITERATIONS=20
  LOSS_PROFILE=legacy|uplink|downlink
  NETEM_DIRECTION=both|uplink|downlink
  REPORT_DIR=...
  RESUME=0|1

Safety option:
  AUDIO_CHECK_STABLE_SEC=3

Before starting, this script checks host processes and running Docker
containers twice. It exits without starting connect tests if an audio probe
test is detected.
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  '') ;;
  *) usage >&2; exit 2 ;;
esac

case "$audio_check_stable_sec" in
  ''|*[!0-9]*)
    printf '[connect-matrix] AUDIO_CHECK_STABLE_SEC must be a non-negative integer\n' >&2
    exit 2
    ;;
esac
case "$pull_image" in
  0|1) ;;
  *)
    printf '[connect-matrix] PULL_IMAGE must be 0 or 1\n' >&2
    exit 2
    ;;
esac

command -v docker >/dev/null 2>&1 || {
  printf '[connect-matrix] docker is required\n' >&2
  exit 1
}
[ -r "$runner" ] || {
  printf '[connect-matrix] missing readable script: %s\n' "$runner" >&2
  exit 1
}

audio_processes() {
  {
    pgrep -af '[m]onitor_probe_image_audio_test\.sh' 2>/dev/null || true
    pgrep -af '[t]irtc_tirn_probe_device[^[:space:]]*[[:space:]]+audio([[:space:]]|$)' 2>/dev/null || true
    pgrep -af '[r]un_tirn_probe_.*audio.*\.sh' 2>/dev/null || true
  } | awk '!seen[$0]++'
}

audio_containers() {
  for container_id in $(docker ps -q 2>/dev/null); do
    details=$(docker inspect --format \
      '{{.Name}}|{{.Config.Image}}|{{json .Config.Entrypoint}}|{{json .Config.Cmd}}|{{range .Config.Env}}{{println .}}{{end}}' \
      "$container_id" 2>/dev/null || true)
    if printf '%s\n' "$details" | grep -Eq \
      '(^|[[:space:]|])COMMAND=audio([[:space:]|]|$)|tirn-probe-device[^[:space:]"|]*[[:space:]"|]+audio([[:space:]"|]|$)|monitor_probe_image_audio_test'; then
      printf '%s %s\n' "$container_id" "$details" | tr '\n' ' '
      printf '\n'
    fi
  done
}

assert_no_audio_test() {
  process_matches=$(audio_processes)
  container_matches=$(audio_containers)
  if [ -n "$process_matches" ] || [ -n "$container_matches" ]; then
    printf '[connect-matrix] REFUSED: an audio test is currently running.\n' >&2
    if [ -n "$process_matches" ]; then
      printf '[connect-matrix] matching host processes:\n%s\n' "$process_matches" >&2
    fi
    if [ -n "$container_matches" ]; then
      printf '[connect-matrix] matching Docker containers:\n%s\n' "$container_matches" >&2
    fi
    printf '[connect-matrix] no connect test was started.\n' >&2
    exit 1
  fi
}

csv_sorted_words() {
  printf '%s\n' "$1" | tr ',' '\n' | sed '/^[[:space:]]*$/d' | sort -n -u
}

case_filter=
for loss in $(csv_sorted_words "$losses"); do
  for delay_ms in $(csv_sorted_words "$delays_ms"); do
    item="connect:$delay_ms:$loss"
    if [ -n "$case_filter" ]; then
      case_filter="$case_filter,$item"
    else
      case_filter=$item
    fi
  done
done

[ -n "$case_filter" ] || {
  printf '[connect-matrix] no connect cases were selected\n' >&2
  exit 2
}

printf '[connect-matrix] checking for running audio tests\n'
assert_no_audio_test
if [ "$pull_image" = 1 ]; then
  printf '[connect-matrix] pulling probe image: %s\n' "$image"
  docker pull "$image"
fi
if [ "$audio_check_stable_sec" -gt 0 ]; then
  printf '[connect-matrix] no audio test detected; checking again after %ss\n' \
    "$audio_check_stable_sec"
  sleep "$audio_check_stable_sec"
  assert_no_audio_test
fi

printf '[connect-matrix] audio test check passed\n'
printf '[connect-matrix] starting serial connect matrix: losses=%s delays_ms=%s\n' \
  "$losses" "$delays_ms"

CASE_FILTER="$case_filter" \
PROBE_IMAGE="$image" \
LOSSES="$losses" \
DELAYS_MS="$delays_ms" \
  exec sh "$runner"
