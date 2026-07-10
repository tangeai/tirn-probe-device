#!/bin/sh
set -eu

image=${PROBE_IMAGE:-docker-hub.tange365.com/runtime/tirtc-accel-probe-runner:test}
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
if ! docker network inspect "$uplink_network_name" >/dev/null 2>&1; then
  docker network create --subnet "$uplink_subnet" "$uplink_network_name" >/dev/null
fi

cleanup
trap cleanup EXIT INT TERM

printf '[netem-gateway] starting gateway: loss=%s%% delay=%sms gateway=%s probe=%s cpu=%s probe_net=%s uplink_net=%s\n' \
  "$loss" "$delay_ms" "$gateway_ip" "$probe_ip" "$cpu_limit" "$network_name" "$uplink_network_name"

docker run -d \
  --name "$gateway_name" \
  --network "$network_name" \
  --ip "$gateway_ip" \
  --cap-add NET_ADMIN \
  --sysctl net.ipv4.ip_forward=1 \
  -e PROBE_SUBNET="$subnet" \
  -e PROBE_IP="$probe_ip" \
  -e GATEWAY_UPLINK_IP="$gateway_uplink_ip" \
  -e UPLINK_GATEWAY_IP="$uplink_gateway_ip" \
  -e LOSS="$loss" \
  -e DELAY_MS="$delay_ms" \
  "$image" \
  sh -eu -c '
    uplink_dev=
    while [ -z "$uplink_dev" ]; do
      sleep 0.1
      uplink_dev=$(ip -o -4 addr show | awk -v uplink_ip="$GATEWAY_UPLINK_IP" "{split(\$4, a, \"/\"); if (a[1] == uplink_ip) {print \$2; exit}}")
    done
    ip route replace default via "$UPLINK_GATEWAY_IP" dev "$uplink_dev"
    iptables -t nat -A POSTROUTING -s "$PROBE_SUBNET" -o "$uplink_dev" -j MASQUERADE
    if [ "$LOSS" != "0" ] || [ "$DELAY_MS" != "0" ]; then
      tc qdisc replace dev "$uplink_dev" root netem loss "$LOSS"% delay "$DELAY_MS"ms
    fi
    printf "[netem-gateway] gateway route table:\n"
    ip route
    printf "[netem-gateway] netem device: %s\n" "$uplink_dev"
    tc qdisc show dev "$uplink_dev"
    tail -f /dev/null
  ' >/dev/null

docker network connect --ip "$gateway_uplink_ip" "$uplink_network_name" "$gateway_name"
sleep 0.3
docker logs "$gateway_name" || true

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
    printf "[netem-gateway] probe route table:\n"
    ip route
    ping_ip=$(getent hosts "$PING_HOST" | awk "{print \$1; exit}" || true)
    if [ -n "$ping_ip" ]; then
      printf "[netem-gateway] probe route to ping host %s (%s):\n" "$PING_HOST" "$ping_ip"
      ip route get "$ping_ip" || true
    fi
    printf "[netem-gateway] ping before test: host=%s count=%s\n" "$PING_HOST" "$PING_COUNT"
    ping -c "$PING_COUNT" "$PING_HOST" || true

    case "$COMMAND" in
      audio)
        printf "[netem-gateway] probe command: /usr/local/bin/tirtc_accel_device_probe audio --endpoint %s --device-id %s --device-secret-key *** --peer-id %s --token *** --repeat %s --interval-ms %s --timeout-ms %s --audio-iterations %s --duration-ms %s --frame-ms %s\n" \
          "$ENDPOINT" "$DEVICE_ID" "$PEER_ID" "$AUDIO_TIMESYNC_REPEAT" "$TIMESYNC_INTERVAL_MS" "$TIMESYNC_TIMEOUT_MS" "$AUDIO_ITERATIONS" "$DURATION_MS" "$FRAME_MS"
        exec /usr/local/bin/tirtc_accel_device_probe audio \
          --endpoint "$ENDPOINT" \
          --device-id "$DEVICE_ID" \
          --device-secret-key "$DEVICE_SECRET_KEY" \
          --peer-id "$PEER_ID" \
          --token "$TOKEN" \
          --repeat "$AUDIO_TIMESYNC_REPEAT" \
          --interval-ms "$TIMESYNC_INTERVAL_MS" \
          --timeout-ms "$TIMESYNC_TIMEOUT_MS" \
          --audio-iterations "$AUDIO_ITERATIONS" \
          --duration-ms "$DURATION_MS" \
          --frame-ms "$FRAME_MS"
        ;;
      connect)
        printf "[netem-gateway] probe command: /usr/local/bin/tirtc_accel_device_probe connect --endpoint %s --device-id %s --device-secret-key *** --peer-id %s --token *** --iterations %s --connect-timeout-ms %s\n" \
          "$ENDPOINT" "$DEVICE_ID" "$PEER_ID" "$CONNECT_ITERATIONS" "$CONNECT_TIMEOUT_MS"
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
        printf "[netem-gateway] probe command: /usr/local/bin/tirtc_accel_device_probe timesync --endpoint %s --device-id %s --device-secret-key *** --peer-id %s --token *** --repeat %s --interval-ms %s --timeout-ms %s\n" \
          "$ENDPOINT" "$DEVICE_ID" "$PEER_ID" "$TIMESYNC_REPEAT" "$TIMESYNC_INTERVAL_MS" "$TIMESYNC_TIMEOUT_MS"
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
