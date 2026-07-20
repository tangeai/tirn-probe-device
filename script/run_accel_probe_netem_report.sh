#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

# Keep test credentials and endpoint configuration in script/.env when present.
if [ -f "$script_dir/.env" ]; then
  . "$script_dir/.env"
fi

image=${PROBE_IMAGE:-tirtc-accel-probe-runner:audio-40ms}
losses=${LOSSES:-0,10,30,50}
delays_ms=${DELAYS_MS:-0,25,50,100,150}
connect_iterations=${CONNECT_ITERATIONS:-20}
audio_iterations=${AUDIO_ITERATIONS:-10}
duration_ms=${DURATION_MS:-90000}
frame_ms=${FRAME_MS:-40}
audio_input=${AUDIO_INPUT:-}
ping_count=${PING_COUNT:-100}
ping_interval_sec=${PING_INTERVAL_SEC:-0.1}
resume=${RESUME:-0}
connect_timeout_ms=${CONNECT_TIMEOUT_MS:-60000}
timesync_timeout_ms=${TIMESYNC_TIMEOUT_MS:-3000}
cpu_limit=${CPU_LIMIT:-0}
direction=${NETEM_DIRECTION:-both}
loss_profile=${LOSS_PROFILE:-legacy}
case_timeout_grace_sec=${CASE_TIMEOUT_GRACE_SEC:-300}
audio_runtime_sec=$(((audio_iterations * duration_ms + 999) / 1000))
case_timeout_sec=${CASE_TIMEOUT_SEC:-$((audio_runtime_sec + case_timeout_grace_sec))}
case_filter=${CASE_FILTER:-}
generate_only=${GENERATE_ONLY:-0}
timestamp=$(date '+%Y%m%d_%H%M%S')
report_dir=${REPORT_DIR:-$repo_root/reports/netem_bidirectional_${timestamp}}

endpoint=${ENDPOINT:-}
device_id=${DEVICE_ID:-}
device_secret_key=${DEVICE_SECRET_KEY:-}
peer_id=${PEER_ID:-}
token=${TOKEN:-}

run_id="$(date '+%s')-$$"
network_name="tirtc-matrix-probe-$run_id"
uplink_network_name="tirtc-matrix-uplink-$run_id"
gateway_name="tirtc-matrix-gateway-$run_id"
probe_name="tirtc-matrix-probe-run-$run_id"
subnet_octet=${NETEM_SUBNET_OCTET:-$((($(date '+%s') % 180) + 20))}
uplink_octet=$((subnet_octet + 1))
subnet=${NETEM_SUBNET:-10.$subnet_octet.0.0/24}
uplink_subnet=${NETEM_UPLINK_SUBNET:-10.$uplink_octet.0.0/24}
gateway_ip=${NETEM_GATEWAY_IP:-10.$subnet_octet.0.2}
probe_ip=${NETEM_PROBE_IP:-10.$subnet_octet.0.3}

usage() {
  cat <<'USAGE'
Usage:
  ENDPOINT=... DEVICE_ID=... DEVICE_SECRET_KEY=... PEER_ID=... TOKEN=... \
    ./script/run_accel_probe_netem_report.sh

Defaults:
  DELAYS_MS=0,25,50,100,150
  LOSSES=0,10,30,50
  CONNECT_ITERATIONS=20
  AUDIO_ITERATIONS=10
  DURATION_MS=90000
  FRAME_MS=40
  CPU_LIMIT=0
  NETEM_DIRECTION=both
  LOSS_PROFILE=legacy|uplink|downlink
  PING_COUNT=100
  PING_INTERVAL_SEC=0.1
  RESUME=0|1
  GENERATE_ONLY=0|1
  CASE_TIMEOUT_SEC=<seconds> (default: audio runtime + CASE_TIMEOUT_GRACE_SEC)
  CASE_TIMEOUT_GRACE_SEC=300
  REPORT_DIR=reports/netem_bidirectional_<timestamp>
  AUDIO_INPUT=/path/to/send_audio.opus
USAGE
}

require_value() {
  if [ -z "$2" ]; then
    printf '[netem-report] missing required environment variable: %s\n' "$1" >&2
    usage >&2
    exit 1
  fi
}

csv_to_words() {
  printf '%s\n' "$1" | tr ',' ' '
}

csv_to_sorted_words() {
  printf '%s\n' "$1" | tr ',' '\n' | sort -n -u
}

cleanup() {
  docker rm -f "$probe_name" "$gateway_name" >/dev/null 2>&1 || true
  docker network rm "$network_name" "$uplink_network_name" >/dev/null 2>&1 || true
}

run_case() {
  command=$1
  delay_ms=$2
  loss=$3
  log_file=$4
  audio_sample_log=
  audio_echo_output=
  if [ "$command" = audio ]; then
    audio_sample_log="$report_dir/audio-samples/$(basename "${log_file%.log}").csv"
    if [ -n "$audio_input" ]; then
      audio_echo_output="$report_dir/audio-echo/$(basename "${log_file%.log}").opus"
    fi
  fi

  case "$loss_profile" in
    uplink) uplink_loss=$loss; downlink_loss=0 ;;
    downlink) uplink_loss=0; downlink_loss=$loss ;;
    legacy) uplink_loss=; downlink_loss= ;;
  esac

  printf '[netem-report] command=%s delay=%sms loss=%s%% watchdog=%ss log=%s\n' \
    "$command" "$delay_ms" "$loss" "$case_timeout_sec" "$log_file"
  set +e
  PROBE_IMAGE="$image" \
  NETEM_NETWORK="$network_name" \
  NETEM_UPLINK_NETWORK="$uplink_network_name" \
  NETEM_SUBNET="$subnet" \
  NETEM_UPLINK_SUBNET="$uplink_subnet" \
  NETEM_GATEWAY_IP="$gateway_ip" \
  NETEM_PROBE_IP="$probe_ip" \
  NETEM_GATEWAY_NAME="$gateway_name" \
  NETEM_PROBE_NAME="$probe_name" \
  NETEM_DIRECTION="$direction" \
  UPLINK_LOSS="$uplink_loss" \
  DOWNLINK_LOSS="$downlink_loss" \
  COMMAND="$command" \
  ENDPOINT="$endpoint" \
  DEVICE_ID="$device_id" \
  DEVICE_SECRET_KEY="$device_secret_key" \
  PEER_ID="$peer_id" \
  TOKEN="$token" \
  LOSS="$loss" \
  DELAY_MS="$delay_ms" \
  CPU_LIMIT="$cpu_limit" \
  PING_COUNT="$ping_count" \
  PING_INTERVAL_SEC="$ping_interval_sec" \
  CONNECT_ITERATIONS="$connect_iterations" \
  CONNECT_TIMEOUT_MS="$connect_timeout_ms" \
  AUDIO_ITERATIONS="$audio_iterations" \
  DURATION_MS="$duration_ms" \
  FRAME_MS="$frame_ms" \
  AUDIO_SAMPLE_LOG="$audio_sample_log" \
  AUDIO_INPUT="$audio_input" \
  AUDIO_ECHO_OUTPUT="$audio_echo_output" \
  TIMESYNC_TIMEOUT_MS="$timesync_timeout_ms" \
    python3 "$script_dir/run_accel_probe_case.py" --timeout-sec "$case_timeout_sec" --log "$log_file" -- \
      sh "$script_dir/run_accel_probe_with_netem_gateway.sh"
  rc=$?
  set -e
  printf '%s\n' "$rc" >"$log_file.exit_code"
}

case_selected() {
  [ -z "$case_filter" ] && return 0
  wanted="$1:$2:$3"
  old_ifs=$IFS; IFS=,
  for item in $case_filter; do [ "$item" = "$wanted" ] && { IFS=$old_ifs; return 0; }; done
  IFS=$old_ifs; return 1
}

completed_case() {
  log_file=$1
  [ -f "$log_file" ] && [ -f "$log_file.exit_code" ] &&
    grep -q '\[netem-gateway\] starting gateway:' "$log_file" &&
    grep -q 'ping statistics' "$log_file"
}

case "$resume" in
  0|1) ;;
  *) printf '[netem-report] invalid RESUME: %s\n' "$resume" >&2; exit 1 ;;
esac
case "$generate_only" in
  0|1) ;;
  *) printf '[netem-report] invalid GENERATE_ONLY: %s\n' "$generate_only" >&2; exit 1 ;;
esac

require_value ENDPOINT "$endpoint"
require_value DEVICE_ID "$device_id"
require_value DEVICE_SECRET_KEY "$device_secret_key"
require_value PEER_ID "$peer_id"
require_value TOKEN "$token"

case "$direction" in
  uplink|downlink|both) ;;
  *) printf '[netem-report] invalid NETEM_DIRECTION: %s\n' "$direction" >&2; exit 1 ;;
esac

case "$loss_profile" in
  legacy|uplink|downlink) ;;
  *) printf '[netem-report] invalid LOSS_PROFILE: %s\n' "$loss_profile" >&2; exit 1 ;;
esac

mkdir -p "$report_dir/logs"
mkdir -p "$report_dir/commands"
mkdir -p "$report_dir/audio-samples"
mkdir -p "$report_dir/audio-echo"
trap cleanup EXIT INT TERM

write_case_command() {
  command=$1
  delay_ms=$2
  loss=$3
  command_file=$4
  log_file=$5
  audio_sample_log=
  audio_echo_output=
  if [ "$command" = audio ]; then
    audio_sample_log="$report_dir/audio-samples/$(basename "${log_file%.log}").csv"
    if [ -n "$audio_input" ]; then
      audio_echo_output="$report_dir/audio-echo/$(basename "${log_file%.log}").opus"
    fi
  fi
  case "$loss_profile" in
    uplink) uplink_loss=$loss; downlink_loss=0 ;;
    downlink) uplink_loss=0; downlink_loss=$loss ;;
    legacy) uplink_loss=; downlink_loss= ;;
  esac
  cat >"$command_file" <<EOF
#!/bin/sh
set -eu
script_dir='$script_dir'
if [ -f "\$script_dir/.env" ]; then set -a; . "\$script_dir/.env"; set +a; fi
export PROBE_IMAGE='$image' NETEM_DIRECTION='$direction'
export UPLINK_LOSS='$uplink_loss' DOWNLINK_LOSS='$downlink_loss'
export COMMAND='$command' LOSS='$loss' DELAY_MS='$delay_ms' CPU_LIMIT='$cpu_limit'
export PING_COUNT='$ping_count' CONNECT_ITERATIONS='$connect_iterations'
export PING_INTERVAL_SEC='$ping_interval_sec'
export CONNECT_TIMEOUT_MS='$connect_timeout_ms' AUDIO_ITERATIONS='$audio_iterations'
export DURATION_MS='$duration_ms' FRAME_MS='$frame_ms' TIMESYNC_TIMEOUT_MS='$timesync_timeout_ms'
export AUDIO_SAMPLE_LOG='$audio_sample_log'
export AUDIO_INPUT='$audio_input' AUDIO_ECHO_OUTPUT='$audio_echo_output'
set +e
python3 "\$script_dir/run_accel_probe_case.py" --timeout-sec '$case_timeout_sec' --log '$log_file' -- \\
  sh "\$script_dir/run_accel_probe_with_netem_gateway.sh"
rc=\$?
set -e
printf '%s\n' "\$rc" >'$log_file.exit_code'
exit "\$rc"
EOF
  chmod +x "$command_file"
}

if git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git_commit=$(git -C "$repo_root" rev-parse HEAD)
  git_status="$(git -C "$repo_root" status --porcelain | wc -l | tr -d ' ') changed paths"
else
  git_commit=unknown
  git_status=unknown
fi

{
  printf 'started_at=%s\n' "$(date -Iseconds)"
  printf 'generate_only=%s\n' "$generate_only"
  printf 'git_commit=%s\n' "$git_commit"
  printf 'git_status=%s\n' "$git_status"
  printf 'host=%s\n' "$(uname -a)"
  printf 'docker=%s\n' "$(docker version --format '{{.Server.Os}}/{{.Server.Arch}} {{.Server.Version}}' 2>/dev/null || printf unknown)"
  printf 'image=%s\n' "$image"
  printf 'direction=%s\n' "$direction"
  printf 'loss_profile=%s\n' "$loss_profile"
  printf 'cpu_limit=%s\n' "$cpu_limit"
  printf 'delays_ms=%s\n' "$delays_ms"
  printf 'losses=%s\n' "$losses"
  printf 'connect_iterations=%s\n' "$connect_iterations"
  printf 'audio_iterations=%s\n' "$audio_iterations"
  printf 'duration_ms=%s\n' "$duration_ms"
  printf 'frame_ms=%s\n' "$frame_ms"
  printf 'audio_input=%s\n' "$audio_input"
  printf 'ping_count=%s\n' "$ping_count"
  printf 'ping_interval_sec=%s\n' "$ping_interval_sec"
  printf 'connect_timeout_ms=%s\n' "$connect_timeout_ms"
  printf 'case_timeout_sec=%s\n' "$case_timeout_sec"
  printf 'case_timeout_grace_sec=%s\n' "$case_timeout_grace_sec"
  printf 'case_filter=%s\n' "$case_filter"
} >"$report_dir/environment.txt"

for loss in $(csv_to_sorted_words "$losses"); do
  loss_tag=$(printf '%02d' "$loss")
  for delay_ms in $(csv_to_sorted_words "$delays_ms"); do
    delay_tag=$(printf '%03d' "$delay_ms")
    case "$loss_profile" in
      uplink) case_stem="delay_${delay_tag}_uplink_loss_${loss_tag}_downlink_loss_00" ;;
      downlink) case_stem="delay_${delay_tag}_uplink_loss_00_downlink_loss_${loss_tag}" ;;
      legacy) case_stem="delay_${delay_tag}_loss_${loss_tag}" ;;
    esac
    connect_log="$report_dir/logs/connect_${case_stem}.log"
    write_case_command connect "$delay_ms" "$loss" \
      "$report_dir/commands/connect_${case_stem}.sh" "$connect_log"
    if [ "$generate_only" = 1 ]; then
      :
    elif ! case_selected connect "$delay_ms" "$loss"; then
      :
    elif [ "$resume" = 1 ] && completed_case "$connect_log"; then
      printf '[netem-report] resume skip command=connect delay=%sms loss=%s%%\n' "$delay_ms" "$loss"
    else
      run_case connect "$delay_ms" "$loss" "$connect_log"
    fi
    audio_log="$report_dir/logs/audio_${case_stem}.log"
    write_case_command audio "$delay_ms" "$loss" \
      "$report_dir/commands/audio_${case_stem}.sh" "$audio_log"
    if [ "$generate_only" = 1 ]; then
      :
    elif ! case_selected audio "$delay_ms" "$loss"; then
      :
    elif [ "$resume" = 1 ] && completed_case "$audio_log"; then
      printf '[netem-report] resume skip command=audio delay=%sms loss=%s%%\n' "$delay_ms" "$loss"
    else
      run_case audio "$delay_ms" "$loss" "$audio_log"
    fi
  done
done

if [ "$generate_only" = 1 ]; then
  matrix_command="$report_dir/commands/run_${loss_profile}_loss_matrix.sh"
  cat >"$matrix_command" <<EOF
#!/bin/sh
set -u
report_dir='$report_dir'
script_dir='$script_dir'
rc=0
for loss in \$(printf '%s\n' '$losses' | tr ',' '\n' | sort -n -u); do
  loss_tag=\$(printf '%02d' "\$loss")
  for delay_ms in \$(printf '%s\n' '$delays_ms' | tr ',' '\n' | sort -n -u); do
    delay_tag=\$(printf '%03d' "\$delay_ms")
    for command in connect audio; do
      case '$loss_profile' in
        uplink) case_stem="delay_\${delay_tag}_uplink_loss_\${loss_tag}_downlink_loss_00" ;;
        downlink) case_stem="delay_\${delay_tag}_uplink_loss_00_downlink_loss_\${loss_tag}" ;;
        legacy) case_stem="delay_\${delay_tag}_loss_\${loss_tag}" ;;
      esac
      command_file="\$report_dir/commands/\${command}_\${case_stem}.sh"
      [ -f "\$command_file" ] || continue
      printf '\n[netem-matrix] running loss=%s%% delay=%sms command=%s\n' \
        "\$loss" "\$delay_ms" "\$command"
      "\$command_file" || rc=1
    done
  done
done
python3 "\$script_dir/generate_accel_probe_netem_report.py" \\
  --report-dir "\$report_dir" --output "\$report_dir/report.md"
exit "\$rc"
EOF
  chmod +x "$matrix_command"
  printf '[netem-report] commands generated: %s\n' "$report_dir/commands"
  printf '[netem-report] one-click matrix: %s\n' "$matrix_command"
  exit 0
fi

printf 'finished_at=%s\n' "$(date -Iseconds)" >>"$report_dir/environment.txt"
python3 "$script_dir/generate_accel_probe_netem_report.py" \
  --report-dir "$report_dir" \
  --output "$report_dir/report.md"
printf '[netem-report] report: %s\n' "$report_dir/report.md"
