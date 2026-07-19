#!/bin/sh
set -eu

image=${PROBE_IMAGE:-docker-hub.tange365.com/runtime/tirtc-accel-probe-runner:test}
tirtc_sdk_variant=${TIRTC_SDK_VARIANT:-standard}
network_name=${NETEM_NETWORK:-tirtc-netem-probe}
subnet=${NETEM_SUBNET:-172.31.0.0/24}
uplink_network_name=${NETEM_UPLINK_NETWORK:-tirtc-netem-uplink}
uplink_subnet=${NETEM_UPLINK_SUBNET:-172.32.0.0/24}
gateway_ip=${NETEM_GATEWAY_IP:-172.31.0.2}
probe_ip=${NETEM_PROBE_IP:-172.31.0.3}
uplink_prefix=$(printf '%s\n' "$uplink_subnet" | awk -F'[./]' '{print $1"."$2"."$3}')
gateway_uplink_ip=${NETEM_GATEWAY_UPLINK_IP:-$uplink_prefix.2}
uplink_gateway_ip=${NETEM_UPLINK_GATEWAY_IP:-$uplink_prefix.1}
gateway_name=${NETEM_GATEWAY_NAME:-tirtc-netem-gateway}
probe_name=${NETEM_PROBE_NAME:-tirtc-netem-probe}

ping_host=${PING_HOST:-wxvoip-test.tange365.com}
ping_count=${PING_COUNT:-4}
ping_interval_sec=${PING_INTERVAL_SEC:-1}
cpu_limit=${CPU_LIMIT:-0.5}
loss=${LOSS:-5}
uplink_loss=${UPLINK_LOSS:-}
downlink_loss=${DOWNLINK_LOSS:-}
delay_ms=${DELAY_MS:-50}
netem_direction=${NETEM_DIRECTION:-uplink}
command=${COMMAND:-audio}
connect_iterations=${CONNECT_ITERATIONS:-1}
connect_timeout_ms=${CONNECT_TIMEOUT_MS:-20000}
timesync_repeat=${TIMESYNC_REPEAT:-20}
timesync_interval_ms=${TIMESYNC_INTERVAL_MS:-100}
timesync_timeout_ms=${TIMESYNC_TIMEOUT_MS:-1000}
probe_start_retries=${PROBE_START_RETRIES:-3}
probe_retry_delay_sec=${PROBE_RETRY_DELAY_SEC:-2}
audio_iterations=${AUDIO_ITERATIONS:-20}
duration_ms=${DURATION_MS:-90000}
frame_ms=${FRAME_MS:-40}
audio_sample_log=${AUDIO_SAMPLE_LOG:-}
audio_input=${AUDIO_INPUT:-}
audio_echo_output=${AUDIO_ECHO_OUTPUT:-}
audio_timesync_repeat=${AUDIO_TIMESYNC_REPEAT:-$timesync_repeat}
tcpdump=${TCPDUMP:-0}
tcpdump_filter=${TCPDUMP_FILTER:-udp or tcp}
tcpdump_snaplen=${TCPDUMP_SNAPLEN:-160}

endpoint=${ENDPOINT:-}
device_id=${DEVICE_ID:-}
device_secret_key=${DEVICE_SECRET_KEY:-}
peer_id=${PEER_ID:-}
token=${TOKEN:-}

usage() {
  cat <<'USAGE'
Usage:
  ENDPOINT=... DEVICE_ID=... DEVICE_SECRET_KEY=... PEER_ID=... TOKEN=... \
    LOSS=5 DELAY_MS=50 CPU_LIMIT=0.5 \
    ./script/run_accel_probe_with_netem_gateway.sh

Optional environment variables:
  PROBE_IMAGE=docker-hub.tange365.com/runtime/tirtc-accel-probe-runner:test
  TIRTC_SDK_VARIANT=standard|desktop
  NETEM_NETWORK=tirtc-netem-probe
  NETEM_SUBNET=172.31.0.0/24
  NETEM_UPLINK_NETWORK=tirtc-netem-uplink
  NETEM_UPLINK_SUBNET=172.32.0.0/24
  NETEM_GATEWAY_IP=172.31.0.2
  NETEM_PROBE_IP=172.31.0.3
  NETEM_GATEWAY_UPLINK_IP=172.32.0.2
  NETEM_UPLINK_GATEWAY_IP=172.32.0.1
  PING_HOST=wxvoip-test.tange365.com
  PING_COUNT=4
  PING_INTERVAL_SEC=1
  LOSS=5
  UPLINK_LOSS=5              # optional; overrides LOSS for device -> server traffic
  DOWNLINK_LOSS=0            # optional; overrides LOSS for server -> device traffic
  DELAY_MS=50
  NETEM_DIRECTION=uplink|downlink|both
  CPU_LIMIT=0.5
  COMMAND=audio|connect|timesync|ping|shell
  CONNECT_ITERATIONS=1
  CONNECT_TIMEOUT_MS=20000
  TIMESYNC_REPEAT=20
  TIMESYNC_INTERVAL_MS=100
  TIMESYNC_TIMEOUT_MS=1000
  AUDIO_ITERATIONS=20
  DURATION_MS=90000
  FRAME_MS=40
  AUDIO_SAMPLE_LOG=/path/to/audio-samples.csv  # optional; audio only
  AUDIO_INPUT=/path/to/send_audio.opus          # optional; audio only
  AUDIO_ECHO_OUTPUT=/path/to/received.opus      # requires AUDIO_INPUT
  AUDIO_TIMESYNC_REPEAT=20
  TCPDUMP=0|1
  TCPDUMP_FILTER="udp or tcp"
  TCPDUMP_SNAPLEN=160
USAGE
}

require_value() {
  if [ -z "$2" ]; then
    printf '[netem-gateway] missing required environment variable: %s\n' "$1" >&2
    usage >&2
    exit 1
  fi
}

cleanup() {
  docker rm -f "$gateway_name" >/dev/null 2>&1 || true
}

require_value ENDPOINT "$endpoint"
require_value DEVICE_ID "$device_id"
require_value DEVICE_SECRET_KEY "$device_secret_key"
require_value PEER_ID "$peer_id"
require_value TOKEN "$token"

if [ -n "$audio_echo_output" ] && [ -z "$audio_input" ]; then
  printf '%s\n' '[netem-gateway] AUDIO_ECHO_OUTPUT requires AUDIO_INPUT' >&2
  exit 1
fi

case "$netem_direction" in
  uplink|downlink|both) ;;
  *)
    printf '[netem-gateway] invalid NETEM_DIRECTION: %s\n' "$netem_direction" >&2
    exit 1
    ;;
esac

# Preserve the historical LOSS + NETEM_DIRECTION behavior unless a per-direction
# loss is explicitly supplied. This also permits bidirectional delay with loss on
# only one direction, for example NETEM_DIRECTION=both UPLINK_LOSS=10 DOWNLINK_LOSS=0.
case "$netem_direction" in
  uplink)
    default_uplink_loss=$loss
    default_downlink_loss=0
    uplink_delay_ms=$delay_ms
    downlink_delay_ms=0
    ;;
  downlink)
    default_uplink_loss=0
    default_downlink_loss=$loss
    uplink_delay_ms=0
    downlink_delay_ms=$delay_ms
    ;;
  both)
    default_uplink_loss=$loss
    default_downlink_loss=$loss
    uplink_delay_ms=$delay_ms
    downlink_delay_ms=$delay_ms
    ;;
esac
uplink_loss=${uplink_loss:-$default_uplink_loss}
downlink_loss=${downlink_loss:-$default_downlink_loss}

if ! docker network inspect "$network_name" >/dev/null 2>&1; then
  docker network create --subnet "$subnet" "$network_name" >/dev/null
fi
if ! docker network inspect "$uplink_network_name" >/dev/null 2>&1; then
  docker network create --subnet "$uplink_subnet" "$uplink_network_name" >/dev/null
fi

cleanup
trap cleanup EXIT INT TERM

set --
container_audio_sample_log=
if [ -n "$audio_sample_log" ]; then
  audio_sample_dir=$(dirname "$audio_sample_log")
  audio_sample_name=$(basename "$audio_sample_log")
  mkdir -p "$audio_sample_dir"
  audio_sample_dir=$(CDPATH= cd -- "$audio_sample_dir" && pwd)
  container_audio_sample_log="/audio-samples/$audio_sample_name"
  set -- "$@" -v "$audio_sample_dir:/audio-samples"
fi

container_audio_input=
if [ -n "$audio_input" ]; then
  if [ ! -f "$audio_input" ]; then
    printf '[netem-gateway] audio input does not exist: %s\n' "$audio_input" >&2
    exit 1
  fi
  audio_input_dir=$(CDPATH= cd -- "$(dirname "$audio_input")" && pwd)
  audio_input_name=$(basename "$audio_input")
  container_audio_input="/audio-input/$audio_input_name"
  set -- "$@" -v "$audio_input_dir:/audio-input:ro"
fi

container_audio_echo_output=
if [ -n "$audio_echo_output" ]; then
  audio_echo_dir=$(dirname "$audio_echo_output")
  audio_echo_name=$(basename "$audio_echo_output")
  mkdir -p "$audio_echo_dir"
  audio_echo_dir=$(CDPATH= cd -- "$audio_echo_dir" && pwd)
  container_audio_echo_output="/audio-echo-output/$audio_echo_name"
  set -- "$@" -v "$audio_echo_dir:/audio-echo-output"
fi

printf '[netem-gateway] starting gateway: loss=%s%% uplink_loss=%s%% downlink_loss=%s%% delay=%sms direction=%s gateway=%s probe=%s cpu=%s probe_net=%s uplink_net=%s\n' \
  "$loss" "$uplink_loss" "$downlink_loss" "$delay_ms" "$netem_direction" "$gateway_ip" "$probe_ip" "$cpu_limit" "$network_name" "$uplink_network_name"

docker run -d \
  --name "$gateway_name" \
  --network "$network_name" \
  --ip "$gateway_ip" \
  --cap-add NET_ADMIN \
  --sysctl net.ipv4.ip_forward=1 \
  -e PROBE_SUBNET="$subnet" \
  -e PROBE_IP="$probe_ip" \
  -e GATEWAY_IP="$gateway_ip" \
  -e GATEWAY_UPLINK_IP="$gateway_uplink_ip" \
  -e UPLINK_GATEWAY_IP="$uplink_gateway_ip" \
  -e LOSS="$loss" \
  -e UPLINK_LOSS="$uplink_loss" \
  -e DOWNLINK_LOSS="$downlink_loss" \
  -e DELAY_MS="$delay_ms" \
  -e UPLINK_DELAY_MS="$uplink_delay_ms" \
  -e DOWNLINK_DELAY_MS="$downlink_delay_ms" \
  -e NETEM_DIRECTION="$netem_direction" \
  "$image" \
  sh -eu -c '
    uplink_dev=
    probe_dev=
    while [ -z "$uplink_dev" ]; do
      sleep 0.1
      uplink_dev=$(ip -o -4 addr show | awk -v uplink_ip="$GATEWAY_UPLINK_IP" "{split(\$4, a, \"/\"); if (a[1] == uplink_ip) {print \$2; exit}}")
    done
    probe_dev=$(ip -o -4 addr show | awk -v probe_ip="$GATEWAY_IP" "{split(\$4, a, \"/\"); if (a[1] == probe_ip) {print \$2; exit}}")
    if [ -z "$probe_dev" ]; then
      printf "[netem-gateway] failed to find probe-facing interface for %s\n" "$GATEWAY_IP" >&2
      exit 1
    fi
    ip route replace default via "$UPLINK_GATEWAY_IP" dev "$uplink_dev"
    iptables -t nat -A POSTROUTING -s "$PROBE_SUBNET" -o "$uplink_dev" -j MASQUERADE
    if [ "$UPLINK_LOSS" != "0" ] || [ "$UPLINK_DELAY_MS" != "0" ]; then
      tc qdisc replace dev "$uplink_dev" root netem loss "$UPLINK_LOSS"% delay "$UPLINK_DELAY_MS"ms
    fi
    if [ "$DOWNLINK_LOSS" != "0" ] || [ "$DOWNLINK_DELAY_MS" != "0" ]; then
      tc qdisc replace dev "$probe_dev" root netem loss "$DOWNLINK_LOSS"% delay "$DOWNLINK_DELAY_MS"ms
    fi
    printf "[netem-gateway] gateway route table:\n"
    ip route
    printf "[netem-gateway] netem direction: %s uplink_dev=%s uplink_loss=%s%% uplink_delay=%sms downlink_dev=%s downlink_loss=%s%% downlink_delay=%sms\n" \
      "$NETEM_DIRECTION" "$uplink_dev" "$UPLINK_LOSS" "$UPLINK_DELAY_MS" "$probe_dev" "$DOWNLINK_LOSS" "$DOWNLINK_DELAY_MS"
    tc qdisc show dev "$uplink_dev"
    tc qdisc show dev "$probe_dev"
    tail -f /dev/null
  ' >/dev/null

docker network connect --ip "$gateway_uplink_ip" "$uplink_network_name" "$gateway_name"
sleep 0.3
docker logs "$gateway_name" || true

docker run --rm \
  "$@" \
  --name "$probe_name" \
  --network "$network_name" \
  --ip "$probe_ip" \
  --cap-add NET_ADMIN \
  --cpus="$cpu_limit" \
  -e GATEWAY_IP="$gateway_ip" \
  -e PING_HOST="$ping_host" \
  -e PING_COUNT="$ping_count" \
  -e PING_INTERVAL_SEC="$ping_interval_sec" \
  -e ENDPOINT="$endpoint" \
  -e DEVICE_ID="$device_id" \
  -e DEVICE_SECRET_KEY="$device_secret_key" \
  -e PEER_ID="$peer_id" \
  -e TOKEN="$token" \
  -e TIRTC_SDK_VARIANT="$tirtc_sdk_variant" \
  -e COMMAND="$command" \
  -e CONNECT_ITERATIONS="$connect_iterations" \
  -e CONNECT_TIMEOUT_MS="$connect_timeout_ms" \
  -e TIMESYNC_REPEAT="$timesync_repeat" \
  -e TIMESYNC_INTERVAL_MS="$timesync_interval_ms" \
  -e TIMESYNC_TIMEOUT_MS="$timesync_timeout_ms" \
  -e PROBE_START_RETRIES="$probe_start_retries" \
  -e PROBE_RETRY_DELAY_SEC="$probe_retry_delay_sec" \
  -e AUDIO_ITERATIONS="$audio_iterations" \
  -e DURATION_MS="$duration_ms" \
  -e FRAME_MS="$frame_ms" \
  -e AUDIO_SAMPLE_LOG="$container_audio_sample_log" \
  -e AUDIO_INPUT="$container_audio_input" \
  -e AUDIO_ECHO_OUTPUT="$container_audio_echo_output" \
  -e AUDIO_TIMESYNC_REPEAT="$audio_timesync_repeat" \
  -e TCPDUMP="$tcpdump" \
  -e TCPDUMP_FILTER="$tcpdump_filter" \
  -e TCPDUMP_SNAPLEN="$tcpdump_snaplen" \
  "$image" \
  sh -eu -c '
    tcpdump_pid=
    cleanup_probe() {
      if [ -n "$tcpdump_pid" ]; then
        kill "$tcpdump_pid" >/dev/null 2>&1 || true
        wait "$tcpdump_pid" >/dev/null 2>&1 || true
      fi
    }
    trap cleanup_probe EXIT INT TERM

    retry_probe() {
      attempt=1
      while :; do
        if "$@"; then
          return 0
        else
          rc=$?
        fi
        printf "[netem-gateway] probe attempt %s/%s failed rc=%s; retrying\n" \
          "$attempt" "$PROBE_START_RETRIES" "$rc" >&2
        [ "$attempt" -ge "$PROBE_START_RETRIES" ] && return "$rc"
        attempt=$((attempt + 1))
        sleep "$PROBE_RETRY_DELAY_SEC"
      done
    }

    ip route replace default via "$GATEWAY_IP"
    printf "[netem-gateway] probe route table:\n"
    ip route
    ping_ip=$(getent hosts "$PING_HOST" | awk "{print \$1; exit}" || true)
    if [ -n "$ping_ip" ]; then
      printf "[netem-gateway] probe route to ping host %s (%s):\n" "$PING_HOST" "$ping_ip"
      ip route get "$ping_ip" || true
    fi
    printf "[netem-gateway] ping before test: host=%s count=%s interval=%ss\n" "$PING_HOST" "$PING_COUNT" "$PING_INTERVAL_SEC"
    ping -i "$PING_INTERVAL_SEC" -c "$PING_COUNT" "$PING_HOST" || true

    if [ "$TCPDUMP" = "1" ]; then
      if ! command -v tcpdump >/dev/null 2>&1; then
        printf "[netem-gateway] tcpdump requested but not found in image\n" >&2
      else
        printf "[netem-gateway] tcpdump start: iface=eth0 snaplen=%s filter=%s\n" "$TCPDUMP_SNAPLEN" "$TCPDUMP_FILTER"
        # shellcheck disable=SC2086
        tcpdump -i eth0 -nn -tttt -s "$TCPDUMP_SNAPLEN" -l $TCPDUMP_FILTER &
        tcpdump_pid=$!
        sleep 0.5
      fi
    fi

    case "$COMMAND" in
      ping)
        exit 0
        ;;
      shell)
        printf "[netem-gateway] probe shell ready; container=%s\n" "$(hostname)"
        tail -f /dev/null
        ;;
      audio)
        set --
        if [ -n "$AUDIO_INPUT" ]; then
          set -- "$@" --audio-input "$AUDIO_INPUT"
        fi
        if [ -n "$AUDIO_ECHO_OUTPUT" ]; then
          set -- "$@" --audio-echo-output "$AUDIO_ECHO_OUTPUT"
        fi
        printf "[netem-gateway] probe audio files: input=%s echo_output=%s sample_log=%s\n" \
          "$AUDIO_INPUT" "$AUDIO_ECHO_OUTPUT" "$AUDIO_SAMPLE_LOG"
        /usr/local/bin/tirtc_accel_device_probe audio \
          --endpoint "$ENDPOINT" \
          --device-id "$DEVICE_ID" \
          --device-secret-key "$DEVICE_SECRET_KEY" \
          --peer-id "$PEER_ID" \
          --token "$TOKEN" \
          --repeat "$AUDIO_TIMESYNC_REPEAT" \
          --interval-ms "$TIMESYNC_INTERVAL_MS" \
          --timeout-ms "$TIMESYNC_TIMEOUT_MS" \
          --audio-iterations "$AUDIO_ITERATIONS" \
          --duration-sec "$(( (DURATION_MS + 999) / 1000 ))" \
          --frame-ms "$FRAME_MS" \
          --audio-sample-log "$AUDIO_SAMPLE_LOG" \
          "$@"
        exit $?
        ;;
      connect)
        printf "[netem-gateway] probe command: /usr/local/bin/tirtc_accel_device_probe connect --endpoint %s --device-id %s --device-secret-key *** --peer-id %s --token *** --iterations %s --connect-timeout-ms %s\n" \
          "$ENDPOINT" "$DEVICE_ID" "$PEER_ID" "$CONNECT_ITERATIONS" "$CONNECT_TIMEOUT_MS"
        # Connect iterations are test samples; an iteration failure must not
        # restart the entire Connect test from iteration 1.
        /usr/local/bin/tirtc_accel_device_probe connect \
          --endpoint "$ENDPOINT" \
          --device-id "$DEVICE_ID" \
          --device-secret-key "$DEVICE_SECRET_KEY" \
          --peer-id "$PEER_ID" \
          --token "$TOKEN" \
          --iterations "$CONNECT_ITERATIONS" \
          --connect-timeout-ms "$CONNECT_TIMEOUT_MS"
        exit $?
        ;;
      timesync)
        printf "[netem-gateway] probe command: /usr/local/bin/tirtc_accel_device_probe timesync --endpoint %s --device-id %s --device-secret-key *** --peer-id %s --token *** --repeat %s --interval-ms %s --timeout-ms %s\n" \
          "$ENDPOINT" "$DEVICE_ID" "$PEER_ID" "$TIMESYNC_REPEAT" "$TIMESYNC_INTERVAL_MS" "$TIMESYNC_TIMEOUT_MS"
        retry_probe /usr/local/bin/tirtc_accel_device_probe timesync \
          --endpoint "$ENDPOINT" \
          --device-id "$DEVICE_ID" \
          --device-secret-key "$DEVICE_SECRET_KEY" \
          --peer-id "$PEER_ID" \
          --token "$TOKEN" \
          --repeat "$TIMESYNC_REPEAT" \
          --interval-ms "$TIMESYNC_INTERVAL_MS" \
          --timeout-ms "$TIMESYNC_TIMEOUT_MS"
        exit $?
        ;;
      *)
        printf "unsupported COMMAND: %s\n" "$COMMAND" >&2
        exit 1
        ;;
    esac
  '
