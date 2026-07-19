#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
monitor_pid=$$
monitor_started_at=$(date -Iseconds)
monitor_run_id="$(date '+%Y%m%d_%H%M%S')_p${monitor_pid}"

if [ -f "$script_dir/.env" ]; then
  set -a
  . "$script_dir/.env"
  set +a
fi

image=${PROBE_IMAGE:-docker-hub.tange365.com/runtime/tirtc-accel-probe-runner:test}
state_dir=${STATE_DIR:-$repo_root/reports/.probe-image-audio-monitor}
last_image_file=$state_dir/last-tested-connect-audio-image-id
lock_dir=$state_dir/lock
lock_owner_file=$lock_dir/owner
reports_root=${REPORTS_ROOT:-$repo_root/reports}
rsync_destination=47.119.170.85::temp/

# Run the same Connect + Audio netem matrix as the report runner by default.
# Override these values to narrow the matrix for smoke testing.
delays_ms=${DELAYS_MS:-0,25,50,100,150}
losses=${LOSSES:-0,10,30,50}
test_commands=${TEST_COMMANDS:-connect,audio}
connect_iterations=${CONNECT_ITERATIONS:-20}
audio_iterations=${AUDIO_ITERATIONS:-10}
duration_ms=${DURATION_MS:-90000}
audio_input=${AUDIO_INPUT:-$repo_root/docker/probe-runner/audio/send_audio_16k.opus}
loss_profile=${LOSS_PROFILE:-uplink}
netem_direction=${NETEM_DIRECTION:-both}
parallel_jobs=${PARALLEL_JOBS:-4}
force_test=${FORCE_TEST:-0}
skip_pull=${SKIP_PULL:-0}
skip_upload=${SKIP_UPLOAD:-0}
feishu_webhook_url=${FEISHU_WEBHOOK_URL:-}
feishu_notify_interval_sec=${FEISHU_NOTIFY_INTERVAL_SEC:-60}
progress_pid=
extract_container=
support_script_dir=

log() {
  printf '[probe-image-monitor pid=%s run=%s] %s\n' \
    "$monitor_pid" "$monitor_run_id" "$*"
}

fail() {
  printf '[probe-image-monitor pid=%s run=%s] ERROR: %s\n' \
    "$monitor_pid" "$monitor_run_id" "$*" >&2
  exit 1
}

image_id() {
  docker image inspect --format '{{.Id}}' "$image" 2>/dev/null || true
}

csv_words() {
  printf '%s\n' "$1" | tr ',' ' '
}

validate_flag() {
  case "$2" in
    0|1) ;;
    *) fail "$1 must be 0 or 1" ;;
  esac
}

feishu_notify() {
  message=$1
  [ -n "$feishu_webhook_url" ] || return 0
  payload=$(FEISHU_TEXT="$message" python3 -c \
    'import json, os; print(json.dumps({"msg_type": "text", "content": {"text": os.environ["FEISHU_TEXT"]}}, ensure_ascii=False))')
  if ! curl -fsS --max-time 10 \
      -H 'Content-Type: application/json' \
      --data-binary "$payload" \
      "$feishu_webhook_url" >/dev/null; then
    log 'warning: failed to send Feishu notification'
  fi
}

completed_test_cases() {
  find "$report_dir" -type f \( \
      -path '*/logs/connect_*.log.exit_code' -o \
      -path '*/logs/audio_*.log.exit_code' \
    \) -print 2>/dev/null |
    wc -l | tr -d ' '
}

current_test_case() {
  find "$report_dir" -type f \( \
      -path '*/logs/connect_*.log' -o \
      -path '*/logs/audio_*.log' \
    \) -print 2>/dev/null |
  while IFS= read -r current; do
    [ -f "$current.exit_code" ] && continue
    basename "$current" .log
  done | paste -sd, -
}

progress_loop() {
  while :; do
    sleep "$feishu_notify_interval_sec"
    elapsed_sec=$(($(date '+%s') - test_started_epoch))
    completed=$(completed_test_cases)
    current=$(current_test_case)
    feishu_notify "TiRTC Probe Connect + Audio 测试进度
镜像: $image
镜像ID: $short_id
进度: $completed/$total_cases 个用例完成
当前用例: ${current:-preparing}
已运行: ${elapsed_sec}s
报告目录: $report_name"
  done
}

stop_progress_loop() {
  if [ -n "$progress_pid" ]; then
    kill "$progress_pid" 2>/dev/null || true
    wait "$progress_pid" 2>/dev/null || true
    progress_pid=
  fi
}

cleanup() {
  stop_progress_loop
  for child_pid in ${batch_pids:-}; do
    kill "$child_pid" 2>/dev/null || true
  done
  if [ -n "$extract_container" ]; then
    docker rm -f "$extract_container" >/dev/null 2>&1 || true
  fi
  rm -f "$lock_owner_file"
  rmdir "$lock_dir" 2>/dev/null || true
}

prepare_image_assets() {
  asset_id=$(printf '%s' "$after_id" | sed 's/^sha256://' | cut -c1-64)
  asset_root="$state_dir/image-assets/$asset_id"
  support_script_dir="$asset_root/script"
  image_audio_dir="$asset_root/audio"
  if [ ! -f "$asset_root/.complete" ]; then
    mkdir -p "$support_script_dir" "$image_audio_dir"
    extract_container=$(docker create --platform linux/amd64 \
      --entrypoint /bin/true "$image")
    docker cp "$extract_container:/opt/tirtc-probe-tests/script/." "$support_script_dir/"
    docker cp "$extract_container:/opt/tirtc-probe-tests/audio/." "$image_audio_dir/"
    docker rm "$extract_container" >/dev/null
    extract_container=
    printf '%s\n' "$after_id" >"$asset_root/.complete"
    log "extracted scripts and audio for image $asset_id to $asset_root"
  else
    log "using cached scripts and audio for image $asset_id from $asset_root"
  fi
  for required_script in run_accel_probe_netem_report.sh \
      run_accel_probe_with_netem_gateway.sh run_accel_probe_case.py \
      generate_accel_probe_netem_report.py; do
    [ -f "$support_script_dir/$required_script" ] ||
      fail "image is missing support script: $required_script"
  done
  chmod +x "$support_script_dir"/*.sh "$support_script_dir"/*.py

  audio_name=$(basename "$audio_input")
  case "$audio_name" in
    send_audio_8k.opus|send_audio_16k.opus)
      audio_input="$image_audio_dir/$audio_name"
      [ -f "$audio_input" ] || fail "image is missing audio input: $audio_name"
      ;;
    *)
      [ -f "$audio_input" ] || fail "missing custom audio input: $audio_input"
      ;;
  esac
  log "selected image-matched support scripts: $support_script_dir"
  log "selected audio input: $audio_input"
}

command -v docker >/dev/null 2>&1 || fail 'docker is required'
command -v python3 >/dev/null 2>&1 || fail 'python3 is required'
command -v rsync >/dev/null 2>&1 || fail 'rsync is required'
command -v tar >/dev/null 2>&1 || fail 'tar is required'
case "$parallel_jobs" in
  ''|*[!0-9]*) fail 'PARALLEL_JOBS must be a positive integer' ;;
  0) fail 'PARALLEL_JOBS must be greater than zero' ;;
esac
validate_flag FORCE_TEST "$force_test"
validate_flag SKIP_PULL "$skip_pull"
validate_flag SKIP_UPLOAD "$skip_upload"
command_count=0
for test_command in $(csv_words "$test_commands"); do
  case "$test_command" in
    connect|audio) command_count=$((command_count + 1)) ;;
    *) fail "TEST_COMMANDS contains unsupported command: $test_command" ;;
  esac
done
[ "$command_count" -gt 0 ] || fail 'TEST_COMMANDS must select connect, audio, or both'
if [ -n "$feishu_webhook_url" ]; then
  command -v curl >/dev/null 2>&1 || fail 'curl is required when FEISHU_WEBHOOK_URL is configured'
  case "$feishu_notify_interval_sec" in
    ''|*[!0-9]*) fail 'FEISHU_NOTIFY_INTERVAL_SEC must be a positive integer' ;;
    0) fail 'FEISHU_NOTIFY_INTERVAL_SEC must be greater than zero' ;;
  esac
fi
[ -f "$script_dir/.env" ] || fail "missing test configuration: $script_dir/.env"

mkdir -p "$state_dir" "$reports_root"
if ! mkdir "$lock_dir" 2>/dev/null; then
  lock_owner=unknown
  if [ -f "$lock_owner_file" ]; then
    lock_owner=$(tr '\n' ' ' <"$lock_owner_file")
  fi
  fail "another monitor run is active: $lock_dir owner=[$lock_owner]"
fi
trap cleanup EXIT INT TERM
{
  printf 'pid=%s\n' "$monitor_pid"
  printf 'run_id=%s\n' "$monitor_run_id"
  printf 'started_at=%s\n' "$monitor_started_at"
  printf 'hostname=%s\n' "$(hostname)"
} >"$lock_owner_file"

before_id=$(image_id)
log "checking image: $image"
if [ "$skip_pull" = 0 ]; then
  docker pull "$image"
else
  log 'SKIP_PULL=1; using the current local image'
fi
after_id=$(image_id)
[ -n "$after_id" ] || fail "cannot inspect image after pull: $image"
prepare_image_assets

last_tested_id=
if [ -f "$last_image_file" ]; then
  last_tested_id=$(sed -n '1p' "$last_image_file")
fi
log "image before pull: ${before_id:-missing}"
log "image after pull:  $after_id"
log "last tested image: ${last_tested_id:-none}"

if [ "$force_test" = 0 ] && [ "$after_id" = "$last_tested_id" ]; then
  log 'no untested image update; nothing to do'
  exit 0
fi

total_scenarios=0
total_cases=0
for loss in $(csv_words "$losses"); do
  for delay_ms in $(csv_words "$delays_ms"); do
    total_scenarios=$((total_scenarios + 1))
    total_cases=$((total_cases + command_count))
  done
done

short_id=$(printf '%s' "$after_id" | sed 's/^sha256://' | cut -c1-12)
report_name="probe_connect_audio_${short_id}_${monitor_run_id}"
report_dir="$reports_root/$report_name"
archive="$reports_root/$report_name.tar.gz"

run_one_case() {
  case_delay=$1
  case_loss=$2
  case_index=$3
  case_report_dir="$report_dir/cases/delay_${case_delay}_loss_${case_loss}"
  subnet_octet=$((20 + case_index * 2))
  case_filter=
  for test_command in $(csv_words "$test_commands"); do
    selected_case="$test_command:$case_delay:$case_loss"
    if [ -n "$case_filter" ]; then
      case_filter="$case_filter,$selected_case"
    else
      case_filter=$selected_case
    fi
  done
  PROBE_IMAGE="$image" \
  DELAYS_MS="$case_delay" \
  LOSSES="$case_loss" \
  LOSS_PROFILE="$loss_profile" \
  NETEM_DIRECTION="$netem_direction" \
  NETEM_SUBNET_OCTET="$subnet_octet" \
  CONNECT_ITERATIONS="$connect_iterations" \
  AUDIO_ITERATIONS="$audio_iterations" \
  DURATION_MS="$duration_ms" \
  AUDIO_INPUT="$audio_input" \
  CASE_FILTER="$case_filter" \
  REPORT_DIR="$case_report_dir" \
    "$support_script_dir/run_accel_probe_netem_report.sh"
}

wait_batch() {
  batch_rc=0
  for child_pid in $batch_pids; do
    wait "$child_pid" || batch_rc=1
  done
  return "$batch_rc"
}

run_parallel_matrix() {
  matrix_rc=0
  batch_pids=
  batch_count=0
  case_index=0
  mkdir -p "$report_dir/cases"
  for case_loss in $(csv_words "$losses"); do
    for case_delay in $(csv_words "$delays_ms"); do
      case_index=$((case_index + 1))
      log "starting parallel scenario $case_index/$total_scenarios: delay=${case_delay}ms uplink_loss=${case_loss}% commands=$test_commands"
      run_one_case "$case_delay" "$case_loss" "$case_index" &
      batch_pids="$batch_pids $!"
      batch_count=$((batch_count + 1))
      if [ "$batch_count" -ge "$parallel_jobs" ]; then
        wait_batch || matrix_rc=1
        batch_pids=
        batch_count=0
      fi
    done
  done
  if [ -n "$batch_pids" ]; then
    wait_batch || matrix_rc=1
  fi
  return "$matrix_rc"
}

merge_case_reports() {
  mkdir -p "$report_dir/logs" "$report_dir/commands" \
    "$report_dir/audio-samples" "$report_dir/audio-echo"
  environment_copied=0
  for case_dir in "$report_dir"/cases/*; do
    [ -d "$case_dir" ] || continue
    if [ "$environment_copied" = 0 ] && [ -f "$case_dir/environment.txt" ]; then
      cp "$case_dir/environment.txt" "$report_dir/environment.txt"
      environment_copied=1
    fi
    for kind in logs commands audio-samples audio-echo; do
      for source_file in "$case_dir/$kind"/*; do
        [ -f "$source_file" ] || continue
        mv "$source_file" "$report_dir/$kind/"
      done
    done
  done
  [ -f "$report_dir/environment.txt" ] || return 1
  {
    printf 'delays_ms=%s\n' "$delays_ms"
    printf 'losses=%s\n' "$losses"
    printf 'test_commands=%s\n' "$test_commands"
    printf 'connect_iterations=%s\n' "$connect_iterations"
    printf 'audio_iterations=%s\n' "$audio_iterations"
    printf 'duration_ms=%s\n' "$duration_ms"
    printf 'direction=%s\n' "$netem_direction"
    printf 'loss_profile=%s\n' "$loss_profile"
    printf 'parallel_jobs=%s\n' "$parallel_jobs"
  } >>"$report_dir/environment.txt"
  python3 "$support_script_dir/generate_accel_probe_netem_report.py" \
    --report-dir "$report_dir" --output "$report_dir/report.md"
}

log "new image detected; running tests: commands=$test_commands delays=$delays_ms losses=$losses loss_profile=$loss_profile direction=$netem_direction connect_iterations=$connect_iterations audio_iterations=$audio_iterations duration_ms=$duration_ms parallel_jobs=$parallel_jobs"
feishu_notify "TiRTC Probe 发现新镜像，开始 Connect + Audio 测试
镜像: $image
镜像ID: $short_id
用例数: $total_cases
Loss方向: $loss_profile
Delay方向: $netem_direction
测试类型: $test_commands
Connect 每用例轮数: $connect_iterations
Audio 每用例轮数: $audio_iterations
Audio 每轮时长: ${duration_ms}ms
并行数: $parallel_jobs
报告目录: $report_name"
test_started_epoch=$(date '+%s')
if [ -n "$feishu_webhook_url" ]; then
  progress_loop &
  progress_pid=$!
fi
set +e
run_parallel_matrix
runner_rc=$?
set -e
stop_progress_loop
merge_case_reports || fail 'failed to merge parallel case reports'

failed_cases=0
failed_connect_cases=0
failed_audio_cases=0
for exit_file in "$report_dir"/logs/connect_*.log.exit_code \
    "$report_dir"/logs/audio_*.log.exit_code; do
  [ -f "$exit_file" ] || continue
  rc=$(sed -n '1p' "$exit_file")
  if [ "$rc" != 0 ]; then
    failed_cases=$((failed_cases + 1))
    case "$(basename "$exit_file")" in
      connect_*) failed_connect_cases=$((failed_connect_cases + 1)) ;;
      audio_*) failed_audio_cases=$((failed_audio_cases + 1)) ;;
    esac
  fi
done

[ -f "$report_dir/report.md" ] || fail "report was not generated: $report_dir/report.md"
{
  printf 'monitor_image_id=%s\n' "$after_id"
  printf 'monitor_runner_exit_code=%s\n' "$runner_rc"
  printf 'monitor_failed_connect_cases=%s\n' "$failed_connect_cases"
  printf 'monitor_failed_audio_cases=%s\n' "$failed_audio_cases"
  printf 'monitor_failed_cases=%s\n' "$failed_cases"
} >>"$report_dir/environment.txt"

tar -czf "$archive" -C "$reports_root" "$report_name"
[ -f "$archive" ] || fail "archive was not generated: $archive"
log "prepared archive: $archive"
feishu_notify "TiRTC Probe Connect + Audio 测试执行结束，正在上传结果
镜像ID: $short_id
测试脚本退出码: $runner_rc
失败用例数: $failed_cases
压缩包: $(basename "$archive")"
if [ "$skip_upload" = 0 ]; then
  log "uploading archive through external endpoint: $archive"
  rsync "$archive" "$rsync_destination"
else
  log 'SKIP_UPLOAD=1; keeping the archive locally'
fi

if [ "$runner_rc" -ne 0 ] || [ "$failed_cases" -ne 0 ]; then
  feishu_notify "TiRTC Probe Connect + Audio 测试失败
镜像ID: $short_id
测试脚本退出码: $runner_rc
失败用例数: $failed_cases
测试数据压缩包: $(basename "$archive")
下次检查将自动重试"
  fail "connect/audio test failed (runner_rc=$runner_rc failed_cases=$failed_cases); image remains pending for retry"
fi

if [ "$skip_upload" = 0 ]; then
  printf '%s\n' "$after_id" >"$last_image_file"
else
  log 'image is not marked tested because result upload was skipped'
fi
log "completed report: $report_dir/report.md"
if [ "$skip_upload" = 0 ]; then
  log "uploaded archive: $rsync_destination$(basename "$archive")"
  log "intranet download: rsync 172.18.144.119::temp/$(basename "$archive") $(basename "$archive")"
fi
feishu_notify "TiRTC Probe Connect + Audio 测试完成
镜像ID: $short_id
用例: $total_cases/$total_cases 成功
报告压缩包: $(basename "$archive")
结果位置: $(if [ "$skip_upload" = 0 ]; then printf 'rsync 47.119.170.85::temp/%s %s' "$(basename "$archive")" "$(basename "$archive")"; else printf '%s' "$archive"; fi)"
