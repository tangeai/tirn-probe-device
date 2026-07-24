#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

cpus=${CPUS:-0.5,1,2}
audio_iterations=${AUDIO_ITERATIONS:-2}
loss=${LOSS:-0}
delay_ms=${DELAY_MS:-0}
timesync_timeout_ms=${TIMESYNC_TIMEOUT_MS:-3000}
base_octet=${NETEM_BASE_OCTET:-20}
report_dir=${REPORT_DIR:-$repo_root/reports}

endpoint=${ENDPOINT:-}
device_id=${DEVICE_ID:-}
device_secret_key=${DEVICE_SECRET_KEY:-}
peer_id=${PEER_ID:-}
token=${TOKEN:-}

usage() {
  cat <<'USAGE'
Usage:
  ENDPOINT=... DEVICE_ID=... DEVICE_SECRET_KEY=... PEER_ID=... TOKEN=... \
    ./script/run_tirn_probe_cpu_compare.sh

Optional environment variables:
  CPUS=0.5,1,2
  AUDIO_ITERATIONS=2
  LOSS=0
  DELAY_MS=0
  TIMESYNC_TIMEOUT_MS=3000
  NETEM_BASE_OCTET=20
  REPORT_DIR=reports
  PROBE_IMAGE=docker-hub.tange365.com/runtime/tirn-probe-device-runner:test
USAGE
}

require_value() {
  if [ -z "$2" ]; then
    printf '[cpu-compare] missing required environment variable: %s\n' "$1" >&2
    usage >&2
    exit 1
  fi
}

csv_to_words() {
  printf '%s\n' "$1" | tr ',' ' '
}

cpu_tag() {
  printf '%s\n' "$1" | sed 's/[^0-9A-Za-z]/_/g'
}

extract_summary() {
  log_file=$1
  awk '
    /ping statistics/ { print; getline; print; getline; print; next }
    /timesync:/ || /^rtt:/ || /device_to_server_estimated:/ { print; next }
    /音频包统计:/ { print; next }
    /音频多轮汇总:/ { summary = 1 }
    summary { print }
  ' "$log_file"
}

require_value ENDPOINT "$endpoint"
require_value DEVICE_ID "$device_id"
require_value DEVICE_SECRET_KEY "$device_secret_key"
require_value PEER_ID "$peer_id"
require_value TOKEN "$token"

mkdir -p "$report_dir"

printf '[cpu-compare] host: '
uname -a
printf '[cpu-compare] docker: '
docker version --format '{{.Server.Os}}/{{.Server.Arch}} {{.Server.Version}}' 2>/dev/null || docker version

index=0
for cpu in $(csv_to_words "$cpus"); do
  tag=$(cpu_tag "$cpu")
  probe_octet=$((base_octet + index * 2))
  uplink_octet=$((probe_octet + 1))
  log_file="$report_dir/tirn_probe_cpu_${tag}_audio_${audio_iterations}.log"

  printf '\n[cpu-compare] cpu=%s loss=%s delay_ms=%s audio_iterations=%s log=%s\n' \
    "$cpu" "$loss" "$delay_ms" "$audio_iterations" "$log_file"

  NETEM_NETWORK="tirtc-netem-probe-cpu-$tag-$probe_octet" \
  NETEM_UPLINK_NETWORK="tirtc-netem-uplink-cpu-$tag-$uplink_octet" \
  NETEM_SUBNET="172.$probe_octet.0.0/24" \
  NETEM_UPLINK_SUBNET="172.$uplink_octet.0.0/24" \
  NETEM_GATEWAY_IP="172.$probe_octet.0.2" \
  NETEM_PROBE_IP="172.$probe_octet.0.3" \
  COMMAND=audio \
  ENDPOINT="$endpoint" \
  DEVICE_ID="$device_id" \
  DEVICE_SECRET_KEY="$device_secret_key" \
  PEER_ID="$peer_id" \
  TOKEN="$token" \
  LOSS="$loss" \
  DELAY_MS="$delay_ms" \
  CPU_LIMIT="$cpu" \
  AUDIO_ITERATIONS="$audio_iterations" \
  TIMESYNC_TIMEOUT_MS="$timesync_timeout_ms" \
    sh "$script_dir/run_tirn_probe_with_netem_gateway.sh" >"$log_file" 2>&1

  extract_summary "$log_file"
  index=$((index + 1))
done
