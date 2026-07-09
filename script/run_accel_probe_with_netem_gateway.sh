#!/bin/sh
set -eu

image=${PROBE_IMAGE:-docker-hub.tange365.com/runtime/tirtc-accel-probe-runner:test}
network_name=${NETEM_NETWORK:-tirtc-netem}
subnet=${NETEM_SUBNET:-172.31.0.0/24}
gateway_ip=${NETEM_GATEWAY_IP:-172.31.0.2}
probe_ip=${NETEM_PROBE_IP:-172.31.0.3}
gateway_name=${NETEM_GATEWAY_NAME:-tirtc-netem-gateway}
probe_name=${NETEM_PROBE_NAME:-tirtc-netem-probe}

ping_host=${PING_HOST:-wxvoip-test.tange365.com}
ping_count=${PING_COUNT:-4}
cpu_limit=${CPU_LIMIT:-0.5}
loss=${LOSS:-5}
delay_ms=${DELAY_MS:-50}
command=${COMMAND:-audio}
connect_iterations=${CONNECT_ITERATIONS:-1}
connect_timeout_ms=${CONNECT_TIMEOUT_MS:-20000}
timesync_repeat=${TIMESYNC_REPEAT:-20}
timesync_interval_ms=${TIMESYNC_INTERVAL_MS:-100}
timesync_timeout_ms=${TIMESYNC_TIMEOUT_MS:-1000}
audio_iterations=${AUDIO_ITERATIONS:-20}
duration_ms=${DURATION_MS:-10000}
frame_ms=${FRAME_MS:-20}
audio_timesync_repeat=${AUDIO_TIMESYNC_REPEAT:-$timesync_repeat}

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
  NETEM_NETWORK=tirtc-netem
  NETEM_SUBNET=172.31.0.0/24
  NETEM_GATEWAY_IP=172.31.0.2
  NETEM_PROBE_IP=172.31.0.3
  PING_HOST=wxvoip-test.tange365.com
  PING_COUNT=4
  LOSS=5
  DELAY_MS=50
  CPU_LIMIT=0.5
  COMMAND=audio|connect|timesync
  CONNECT_ITERATIONS=1
  CONNECT_TIMEOUT_MS=20000
  TIMESYNC_REPEAT=20
  TIMESYNC_INTERVAL_MS=100
  TIMESYNC_TIMEOUT_MS=1000
  AUDIO_ITERATIONS=20
  DURATION_MS=10000
  FRAME_MS=20
  AUDIO_TIMESYNC_REPEAT=20
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

if ! docker network inspect "$network_name" >/dev/null 2>&1; then
  docker network create --subnet "$subnet" "$network_name" >/dev/null
fi

cleanup
trap cleanup EXIT INT TERM

printf '[netem-gateway] starting gateway: loss=%s%% delay=%sms gateway=%s probe=%s cpu=%s\n' \
  "$loss" "$delay_ms" "$gateway_ip" "$probe_ip" "$cpu_limit"

docker run -d --rm \
  --name "$gateway_name" \
  --network "$network_name" \
  --ip "$gateway_ip" \
  --cap-add NET_ADMIN \
  --sysctl net.ipv4.ip_forward=1 \
  -e PROBE_IP="$probe_ip" \
  -e LOSS="$loss" \
  -e DELAY_MS="$delay_ms" \
  "$image" \
  sh -eu -c '
    iptables -t nat -A POSTROUTING -s "$PROBE_IP"/32 -j MASQUERADE
    if [ "$LOSS" != "0" ] || [ "$DELAY_MS" != "0" ]; then
      tc qdisc add dev eth0 root netem loss "$LOSS"% delay "$DELAY_MS"ms
    fi
    tail -f /dev/null
  ' >/dev/null

docker run --rm \
  --name "$probe_name" \
  --network "$network_name" \
  --ip "$probe_ip" \
  --cap-add NET_ADMIN \
  --cpus="$cpu_limit" \
  -e GATEWAY_IP="$gateway_ip" \
  -e PING_HOST="$ping_host" \
  -e PING_COUNT="$ping_count" \
  -e ENDPOINT="$endpoint" \
  -e DEVICE_ID="$device_id" \
  -e DEVICE_SECRET_KEY="$device_secret_key" \
  -e PEER_ID="$peer_id" \
  -e TOKEN="$token" \
  -e COMMAND="$command" \
  -e CONNECT_ITERATIONS="$connect_iterations" \
  -e CONNECT_TIMEOUT_MS="$connect_timeout_ms" \
  -e TIMESYNC_REPEAT="$timesync_repeat" \
  -e TIMESYNC_INTERVAL_MS="$timesync_interval_ms" \
  -e TIMESYNC_TIMEOUT_MS="$timesync_timeout_ms" \
  -e AUDIO_ITERATIONS="$audio_iterations" \
  -e DURATION_MS="$duration_ms" \
  -e FRAME_MS="$frame_ms" \
  -e AUDIO_TIMESYNC_REPEAT="$audio_timesync_repeat" \
  "$image" \
  sh -eu -c '
    ip route replace default via "$GATEWAY_IP"
    printf "[netem-gateway] ping before test: host=%s count=%s\n" "$PING_HOST" "$PING_COUNT"
    ping -c "$PING_COUNT" "$PING_HOST" || true

    case "$COMMAND" in
      audio)
        exec /usr/local/bin/tirtc_accel_device_probe audio \
          --endpoint "$ENDPOINT" \
          --device-id "$DEVICE_ID" \
          --device-secret-key "$DEVICE_SECRET_KEY" \
          --peer-id "$PEER_ID" \
          --token "$TOKEN" \
          --repeat "$AUDIO_TIMESYNC_REPEAT" \
          --audio-iterations "$AUDIO_ITERATIONS" \
          --duration-ms "$DURATION_MS" \
          --frame-ms "$FRAME_MS"
        ;;
      connect)
        exec /usr/local/bin/tirtc_accel_device_probe connect \
          --endpoint "$ENDPOINT" \
          --device-id "$DEVICE_ID" \
          --device-secret-key "$DEVICE_SECRET_KEY" \
          --peer-id "$PEER_ID" \
          --token "$TOKEN" \
          --iterations "$CONNECT_ITERATIONS" \
          --connect-timeout-ms "$CONNECT_TIMEOUT_MS"
        ;;
      timesync)
        exec /usr/local/bin/tirtc_accel_device_probe timesync \
          --endpoint "$ENDPOINT" \
          --device-id "$DEVICE_ID" \
          --device-secret-key "$DEVICE_SECRET_KEY" \
          --peer-id "$PEER_ID" \
          --token "$TOKEN" \
          --repeat "$TIMESYNC_REPEAT" \
          --interval-ms "$TIMESYNC_INTERVAL_MS" \
          --timeout-ms "$TIMESYNC_TIMEOUT_MS"
        ;;
      *)
        printf "unsupported COMMAND: %s\n" "$COMMAND" >&2
        exit 1
        ;;
    esac
  '
