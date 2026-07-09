#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

image=${PROBE_IMAGE:-tirtc-accel-probe-runner:20.04}
binary=${PROBE_BINARY:-./build/linux-x86_64/tirtc_accel_device_probe}
cpu_limit=${CPU_LIMIT:-0.5}
losses=${LOSSES:-0,5,10,20}
delays_ms=${DELAYS_MS:-0,50,100}
command=${COMMAND:-audio}
audio_iterations=${AUDIO_ITERATIONS:-20}
duration_ms=${DURATION_MS:-10000}
frame_ms=${FRAME_MS:-20}

endpoint=${ENDPOINT:-}
device_id=${DEVICE_ID:-}
device_secret_key=${DEVICE_SECRET_KEY:-}
peer_id=${PEER_ID:-}
token=${TOKEN:-}

usage() {
  cat <<'USAGE'
Usage:
  ENDPOINT=... DEVICE_ID=... DEVICE_SECRET_KEY=... PEER_ID=... TOKEN=... \
    ./script/run_accel_probe_matrix.sh

Optional environment variables:
  PROBE_IMAGE=tirtc-accel-probe-runner:20.04
  CPU_LIMIT=0.5
  LOSSES=0,5,10,20
  DELAYS_MS=0,50,100
  COMMAND=audio|connect|timesync
  AUDIO_ITERATIONS=20
  DURATION_MS=10000
  FRAME_MS=20
USAGE
}

require_value() {
  if [ -z "$2" ]; then
    printf '[probe-matrix] missing required environment variable: %s\n' "$1" >&2
    usage >&2
    exit 1
  fi
}

csv_to_words() {
  printf '%s\n' "$1" | tr ',' ' '
}

require_value ENDPOINT "$endpoint"
require_value DEVICE_ID "$device_id"
require_value DEVICE_SECRET_KEY "$device_secret_key"
require_value PEER_ID "$peer_id"
require_value TOKEN "$token"

cd "$repo_root"
if [ ! -x "$binary" ]; then
  printf '[probe-matrix] missing executable: %s\n' "$binary" >&2
  printf '%s\n' '[probe-matrix] build linux-x86_64 first.' >&2
  exit 1
fi

for loss in $(csv_to_words "$losses"); do
  for delay_ms in $(csv_to_words "$delays_ms"); do
    printf '\n[probe-matrix] loss=%s%% delay=%sms cpu=%s command=%s\n' "$loss" "$delay_ms" "$cpu_limit" "$command"
    docker run --rm \
      --cap-add NET_ADMIN \
      --cpus="$cpu_limit" \
      -v "$repo_root":/work \
      -w /work \
      -e ENDPOINT="$endpoint" \
      -e DEVICE_ID="$device_id" \
      -e DEVICE_SECRET_KEY="$device_secret_key" \
      -e PEER_ID="$peer_id" \
      -e TOKEN="$token" \
      -e LOSS="$loss" \
      -e DELAY_MS="$delay_ms" \
      -e COMMAND="$command" \
      -e AUDIO_ITERATIONS="$audio_iterations" \
      -e DURATION_MS="$duration_ms" \
      -e FRAME_MS="$frame_ms" \
      "$image" \
      sh -eu -c '
        if [ "$LOSS" != "0" ] || [ "$DELAY_MS" != "0" ]; then
          tc qdisc add dev eth0 root netem loss "$LOSS"% delay "$DELAY_MS"ms
        fi

        case "$COMMAND" in
          audio)
            exec ./build/linux-x86_64/tirtc_accel_device_probe audio \
              --endpoint "$ENDPOINT" \
              --device-id "$DEVICE_ID" \
              --device-secret-key "$DEVICE_SECRET_KEY" \
              --peer-id "$PEER_ID" \
              --token "$TOKEN" \
              --audio-iterations "$AUDIO_ITERATIONS" \
              --duration-ms "$DURATION_MS" \
              --frame-ms "$FRAME_MS"
            ;;
          connect)
            exec ./build/linux-x86_64/tirtc_accel_device_probe connect \
              --endpoint "$ENDPOINT" \
              --device-id "$DEVICE_ID" \
              --device-secret-key "$DEVICE_SECRET_KEY" \
              --peer-id "$PEER_ID" \
              --token "$TOKEN"
            ;;
          timesync)
            exec ./build/linux-x86_64/tirtc_accel_device_probe timesync \
              --endpoint "$ENDPOINT" \
              --device-id "$DEVICE_ID" \
              --device-secret-key "$DEVICE_SECRET_KEY" \
              --peer-id "$PEER_ID" \
              --token "$TOKEN"
            ;;
          *)
            printf "unsupported COMMAND: %s\n" "$COMMAND" >&2
            exit 1
            ;;
        esac
      '
  done
done
