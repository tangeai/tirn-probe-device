#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [[ -f "$script_dir/.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "$script_dir/.env"
  set +a
fi

image=${PROBE_IMAGE:-docker-hub.tange365.com/runtime/tirtc-accel-probe-runner:test}
tirtc_sdk_variant=desktop
connections=${CONNECTIONS:-1000}
connections_per_process=${CONNECTIONS_PER_PROCESS:-300}
duration_sec=${DURATION_SEC:-600}
interval_ms=${INTERVAL_MS:-10}
connect_timeout_ms=${CONNECT_TIMEOUT_MS:-60000}
log_level=${LOG_LEVEL:-3}
log_dir=${LOG_DIR:-$repo_root/reports}
rtc_thread_stat_sample_every=${RTC_THREAD_STAT_SAMPLE_EVERY:-0}

require_value() {
  if [[ -z "${2:-}" ]]; then
    printf '[idle-probe] missing required setting: %s\n' "$1" >&2
    printf '[idle-probe] configure it in script/.env or export it before running.\n' >&2
    exit 1
  fi
}

require_value ENDPOINT "${ENDPOINT:-}"
require_value DEVICE_ID "${DEVICE_ID:-}"
require_value DEVICE_SECRET_KEY "${DEVICE_SECRET_KEY:-}"
require_value PEER_ID "${PEER_ID:-}"
require_value TOKEN "${TOKEN:-}"

if (( connections <= 0 )); then
  printf '[idle-probe] CONNECTIONS must be greater than zero: %s\n' "$connections" >&2
  exit 1
fi
if (( connections_per_process <= 0 )); then
  printf '[idle-probe] CONNECTIONS_PER_PROCESS must be greater than zero: %s\n' \
    "$connections_per_process" >&2
  exit 1
fi
if [[ ! "$rtc_thread_stat_sample_every" =~ ^[0-9]+$ ]]; then
  printf '[idle-probe] RTC_THREAD_STAT_SAMPLE_EVERY must be a non-negative integer: %s\n' \
    "$rtc_thread_stat_sample_every" >&2
  exit 1
fi

mkdir -p "$log_dir"
timestamp=$(date '+%Y%m%d_%H%M%S')
run_id="${timestamp}_$$"
worker_count=$(( (connections + connections_per_process - 1) / connections_per_process ))
worker_pids=()
container_names=()

cleanup() {
  local name
  local pid

  trap - INT TERM EXIT
  for pid in "${worker_pids[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  for name in "${container_names[@]:-}"; do
    docker rm -f "$name" >/dev/null 2>&1 || true
  done
}

trap cleanup INT TERM EXIT

printf '[idle-probe] image=%s total_connections=%s connections_per_process=%s workers=%s ramp_interval_ms=%s hold_duration_sec=%s\n' \
  "$image" "$connections" "$connections_per_process" "$worker_count" "$interval_ms" "$duration_sec"
printf '[idle-probe] run_id=%s log_dir=%s\n' "$run_id" "$log_dir"
printf '[idle-probe] RTC_THREAD_STAT sample_every=%s (0=disabled, 1=all, N=keep one of every N)\n' \
  "$rtc_thread_stat_sample_every"

remaining=$connections
for ((worker = 1; worker <= worker_count; worker++)); do
  worker_connections=$connections_per_process
  if (( remaining < worker_connections )); then
    worker_connections=$remaining
  fi
  remaining=$((remaining - worker_connections))

  container_name="idle-probe-${run_id}-${worker}"
  log_file="$log_dir/idle_${connections}_${run_id}_worker_${worker}_${worker_connections}.log"
  container_names+=("$container_name")

  printf '[idle-probe] worker=%s/%s connections=%s container=%s log=%s\n' \
    "$worker" "$worker_count" "$worker_connections" "$container_name" "$log_file"

  (
    docker run --rm \
      --name "$container_name" \
      --ulimit nofile=65535:65535 \
      -e ENDPOINT \
      -e DEVICE_ID \
      -e DEVICE_SECRET_KEY \
      -e PEER_ID \
      -e TOKEN \
      -e TIRTC_SDK_VARIANT="$tirtc_sdk_variant" \
      "$image" \
      sh -c 'exec /usr/local/bin/tirtc_accel_device_probe idle \
        --endpoint "$ENDPOINT" \
        --device-id "$DEVICE_ID" \
        --device-secret-key "$DEVICE_SECRET_KEY" \
        --peer-id "$PEER_ID" \
        --token "$TOKEN" \
        --connections "$1" \
        --interval-ms "$2" \
        --connect-timeout-ms "$3" \
        --duration-sec "$4" \
        --log-level "$5"' \
      idle-probe "$worker_connections" "$interval_ms" "$connect_timeout_ms" "$duration_sec" "$log_level" \
      2>&1 | awk -v worker="$worker" -v sample_every="$rtc_thread_stat_sample_every" '
        /\[RTC_THREAD_STAT\]/ {
          stat_count++
          if (sample_every == 0 || ((stat_count - 1) % sample_every) != 0) {
            suppressed++
            next
          }
        }
        {
          print "[worker " worker "] " $0
          fflush()
        }
        END {
          if (suppressed > 0) {
            print "[worker " worker "] [idle-probe] suppressed RTC_THREAD_STAT lines=" suppressed
            fflush()
          }
        }
      ' | tee "$log_file"
  ) &
  worker_pids+=("$!")
done

failed_workers=0
for index in "${!worker_pids[@]}"; do
  if wait "${worker_pids[$index]}"; then
    printf '[idle-probe] worker=%s completed\n' "$((index + 1))"
  else
    printf '[idle-probe] worker=%s failed\n' "$((index + 1))" >&2
    failed_workers=$((failed_workers + 1))
  fi
done

trap - INT TERM EXIT
if (( failed_workers > 0 )); then
  printf '[idle-probe] completed with failed_workers=%s/%s\n' "$failed_workers" "$worker_count" >&2
  exit 1
fi

printf '[idle-probe] completed workers=%s total_connections=%s\n' "$worker_count" "$connections"
