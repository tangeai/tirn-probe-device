#!/usr/bin/env python3
import argparse
import csv
import datetime as dt
import math
import re
from pathlib import Path


LEGACY_CASE_RE = re.compile(r"(connect|audio)_delay_(\d+)_loss_(\d+)\.(?:log|csv)$")
DIRECTED_CASE_RE = re.compile(
    r"(connect|audio)_delay_(\d+)_uplink_loss_(\d+)_downlink_loss_(\d+)\.(?:log|csv)$"
)


def match(text, pattern, default="—"):
    found = re.search(pattern, text, re.MULTILINE)
    return found.groups() if found else default


def read_environment(path):
    values = {}
    if not path.exists():
        return values
    for line in path.read_text(errors="replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def percentile(values, ratio):
    if not values:
        return None
    ordered = sorted(values)
    rank = ratio * (len(ordered) - 1)
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return ordered[lower]
    fraction = rank - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def percentile_nearest(values, ratio):
    if not values:
        return None
    ordered = sorted(values)
    # Match C llround() for the non-negative metrics emitted by the probe.
    index = math.floor(ratio * (len(ordered) - 1) + 0.5)
    return ordered[index]


def add_distribution(result, prefix, values, scale=1.0, nearest=False):
    if not values:
        return
    scaled = [value / scale for value in values]
    result[f"{prefix}_count"] = str(len(scaled))
    result[f"{prefix}_avg"] = f"{sum(scaled) / len(scaled):.2f}"
    percentile_fn = percentile_nearest if nearest else percentile
    for label, ratio in (("p50", 0.50), ("p90", 0.90), ("p95", 0.95), ("p99", 0.99)):
        result[f"{prefix}_{label}"] = f"{percentile_fn(scaled, ratio):.2f}"


def integer(row, key, default=0):
    try:
        return int(row.get(key, default))
    except (TypeError, ValueError):
        return default


def infer_skip_frames(rows, skip_duration_ms):
    frame_times = {}
    for row in rows:
        iteration = integer(row, "iteration", -1)
        frame_ts_ms = integer(row, "frame_ts_ms", -1)
        if iteration >= 0 and frame_ts_ms >= 0:
            frame_times.setdefault(iteration, []).append(frame_ts_ms)
    durations = []
    for values in frame_times.values():
        ordered = sorted(set(values))
        durations.extend(right - left for left, right in zip(ordered, ordered[1:])
                         if right > left)
    frame_duration_ms = percentile(durations, 0.50) if durations else None
    if not frame_duration_ms:
        return 0
    return max(0, math.ceil(skip_duration_ms / frame_duration_ms))


def read_audio_metrics(path, target_iterations=None, skip_frames=None, skip_duration_ms=10000):
    """Recalculate audio business metrics from a probe sample CSV.

    Older CSV files do not contain the measured call duration used as the
    stutter-rate denominator. For those files it is reconstructed from the
    send timeline, the inferred final frame duration, and the probe's one
    second echo-drain window.
    """
    if not path.exists():
        return {}
    with path.open(newline="", errors="replace") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        return {}

    if skip_frames is None:
        skip_frames = infer_skip_frames(rows, skip_duration_ms)
    result = {
        "sample_path": path,
        "audio_metrics_source": "csv",
        "audio_skip_frames": str(skip_frames),
    }
    all_iterations = {}
    for row in rows:
        iteration = integer(row, "iteration", -1)
        if iteration >= 0:
            all_iterations.setdefault(iteration, []).append(row)
    iterations = {}
    for iteration, iteration_rows in all_iterations.items():
        ordered = sorted(iteration_rows, key=lambda row: integer(row, "send_index"))
        retained = ordered[skip_frames:]
        if retained:
            iterations[iteration] = retained
    rows = [row for iteration_rows in iterations.values() for row in iteration_rows]

    # Successful rounds describe the test outcome and must not change merely
    # because their measurement window was removed by warm-up filtering.
    result["audio_ok"] = str(len(all_iterations))
    result["audio_total"] = str(target_iterations or len(all_iterations))
    if not rows:
        return result
    sent = len(rows)
    send_failed = sum(integer(row, "send_ret") < 0 for row in rows)
    server_received = sum(integer(row, "observed") != 0 for row in rows)
    echo_received = sum(integer(row, "echoed") != 0 for row in rows)
    result.update({
        "sent": str(sent),
        "send_failed": str(send_failed),
        "server_received": str(server_received),
        "echo_received": str(echo_received),
        "server_rate": f"{server_received * 100.0 / sent:.2f}" if sent else "0.00",
        "echo_rate": f"{echo_received * 100.0 / sent:.2f}" if sent else "0.00",
    })

    uplink = [integer(row, "uplink_us") for row in rows if integer(row, "observed") != 0]
    downlink = [integer(row, "downlink_us") for row in rows
                if integer(row, "observed") != 0 and integer(row, "echoed") != 0]
    echo = [integer(row, "echo_us") for row in rows if integer(row, "echoed") != 0]
    frame_intervals = [integer(row, "echo_arrival_gap_us") for row in rows
                       if integer(row, "echo_arrival_gap_us") > 0]
    first_echo = []
    stutter_rates = []
    for ordered in iterations.values():
        if ordered and integer(ordered[0], "echoed") != 0:
            first_echo.append(integer(ordered[0], "echo_us"))

        stutter_time_us = sum(integer(row, "stutter_time_us") for row in ordered)
        call_duration_us = max((integer(row, "call_duration_us") for row in ordered), default=0)
        if call_duration_us <= 0 and ordered:
            send_times = [integer(row, "client_send_unix_ns") for row in ordered]
            frame_times = sorted({integer(row, "frame_ts_ms") for row in ordered})
            frame_durations = [right - left for left, right in zip(frame_times, frame_times[1:])
                               if right > left]
            frame_duration_us = int(percentile(frame_durations, 0.50) * 1000) \
                if frame_durations else 0
            call_duration_us = max(send_times) // 1000 - min(send_times) // 1000
            call_duration_us += frame_duration_us + 1000000
        stutter_rates.append(stutter_time_us * 100.0 / call_duration_us
                             if call_duration_us > 0 else 0.0)

    add_distribution(result, "uplink_latency", uplink, scale=1000.0, nearest=True)
    add_distribution(result, "downlink_latency", downlink, scale=1000.0, nearest=True)
    add_distribution(result, "echo_latency", first_echo, scale=1000.0, nearest=True)
    add_distribution(result, "frame_interval", frame_intervals, scale=1000.0, nearest=True)
    add_distribution(result, "stutter", stutter_rates, nearest=True)
    return result


def parse_case_name(name, direction):
    found = DIRECTED_CASE_RE.fullmatch(name)
    if found:
        command, delay, uplink_loss, downlink_loss = found.groups()
        return command, int(delay), int(uplink_loss), int(downlink_loss)

    found = LEGACY_CASE_RE.fullmatch(name)
    if not found:
        return None
    command, delay, loss = found.groups()
    loss = int(loss)
    if direction == "uplink":
        return command, int(delay), loss, 0
    if direction == "downlink":
        return command, int(delay), 0, loss
    return command, int(delay), loss, loss


def parse_log(path):
    text = path.read_text(errors="replace")
    result = {"path": path, "text": text}
    packets = match(text, r"(\d+) packets transmitted, (\d+) received, (?:\+\d+ errors, )?([\d.]+)% packet loss")
    if packets != "—":
        result["ping_tx"], result["ping_rx"], result["ping_loss"] = packets
    rtt = match(text, r"rtt min/avg/max/mdev = ([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+) ms")
    if rtt != "—":
        result["ping_min"], result["ping_avg"], result["ping_max"], result["ping_mdev"] = rtt
    ping_samples = [
        float(value)
        for value in re.findall(r"icmp_seq=\d+.*?time[=<]([\d.]+)\s*ms", text)
    ]
    add_distribution(result, "ping", ping_samples)
    exit_path = Path(str(path) + ".exit_code")
    result["exit_code"] = exit_path.read_text().strip() if exit_path.exists() else "—"

    connect = match(text, r"connect_success: (\d+)/(\d+) ([\d.]+)%")
    if connect != "—":
        result["connect_ok"], result["connect_total"], result["connect_rate"] = connect
    cost = match(text, r"connect_cost: count=(\d+) avg=([\d.]+)ms p50=([\d.]+)ms p90=([\d.]+)ms p95=([\d.]+)ms p99=([\d.]+)ms")
    if cost != "—":
        (result["connect_count"], result["connect_avg"], result["connect_p50"],
         result["connect_p90"], result["connect_p95"], result["connect_p99"]) = cost

    return result


def value(case, key, suffix=""):
    raw = case.get(key)
    return f"{raw}{suffix}" if raw is not None else "—"


def ping_cell(case):
    loss = value(case, "ping_loss", "%")
    distribution = "/".join(value(case, key, "ms") for key in (
        "ping_avg", "ping_p50", "ping_p90", "ping_p95", "ping_p99"))
    return f"loss {loss}; {distribution}"


def status_cell(case):
    code = case.get("exit_code")
    text = case.get("text", "")
    connect_match = re.search(r"connect_success: (\d+)/(\d+)", text)
    if connect_match and connect_match.group(1) == connect_match.group(2):
        return "完成（业务成功）"
    if case.get("audio_ok") is not None and case.get("audio_ok") == case.get("audio_total"):
        return "完成（业务成功）"
    if code == "0":
        return "完成"
    if code == "137":
        return "进程被 SIGKILL"
    if code == "125":
        return "Docker 启动失败"
    if "40303(peer id not auth)" in text:
        return "服务端403 peer未授权"
    if "connect_success:" in text:
        return "部分业务失败"
    if "音频多轮汇总:" in text:
        return "部分音频轮次失败"
    if code == "1":
        return "建连/预对时失败"
    return "未完成"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--audio-skip-frames", type=int,
        help="每轮固定跳过的起始帧数；默认根据 CSV 帧间隔换算约 10 秒",
    )
    parser.add_argument(
        "--audio-skip-duration-ms", type=int, default=10000,
        help="未指定 --audio-skip-frames 时用于换算跳帧数的时长（默认 10000ms）",
    )
    args = parser.parse_args()
    if args.audio_skip_frames is not None and args.audio_skip_frames < 0:
        parser.error("--audio-skip-frames must be >= 0")
    if args.audio_skip_duration_ms < 0:
        parser.error("--audio-skip-duration-ms must be >= 0")

    report_dir = Path(args.report_dir)
    env = read_environment(report_dir / "environment.txt")
    try:
        target_audio_iterations = int(env.get("audio_iterations", ""))
    except ValueError:
        target_audio_iterations = None
    cases = {}
    for path in sorted((report_dir / "logs").glob("*.log")):
        identity = parse_case_name(path.name, env.get("direction", "both"))
        if identity:
            command, delay, uplink_loss, downlink_loss = identity
            cases[(delay, uplink_loss, downlink_loss, command)] = parse_log(path)
    # Audio business metrics are generated exclusively from sample CSV files.
    for path in sorted((report_dir / "audio-samples").glob("*.csv")):
        identity = parse_case_name(path.name, env.get("direction", "both"))
        if not identity or identity[0] != "audio":
            continue
        command, delay, uplink_loss, downlink_loss = identity
        key = (delay, uplink_loss, downlink_loss, command)
        case = cases.setdefault(key, {})
        case.update(read_audio_metrics(
            path,
            target_audio_iterations,
            skip_frames=args.audio_skip_frames,
            skip_duration_ms=args.audio_skip_duration_ms,
        ))

    # Present the matrix by increasing directional loss first, then delay.
    combinations = sorted(
        {(delay, uplink_loss, downlink_loss) for delay, uplink_loss, downlink_loss, _ in cases},
        key=lambda item: (item[1], item[2], item[0]),
    )
    connect_combinations = [
        item for item in combinations if (*item, "connect") in cases
    ]
    audio_combinations = [
        item for item in combinations if (*item, "audio") in cases
    ]
    lines = [
        "# TiRTC Accel Probe Netem 矩阵测试报告",
        "",
        f"生成时间：{dt.datetime.now().astimezone().isoformat(timespec='seconds')}",
        "",
        "## 测试配置",
        "",
        f"- Git commit：`{env.get('git_commit', '—')}`",
        f"- 运行环境：`{env.get('host', '—')}`",
        f"- Docker：`{env.get('docker', '—')}`",
        f"- Probe 镜像：`{env.get('image', '—')}`",
        f"- Netem delay 方向：`{env.get('direction', '—')}`",
        f"- CPU 限制：`{env.get('cpu_limit', '—')}`（0 表示无限额）",
        f"- Delay：`{env.get('delays_ms', '—')}ms`；Loss：`{env.get('losses', '—')}%`",
        f"- Connect：每组 `{env.get('connect_iterations', '—')}` 次",
        f"- Audio：每组 `{env.get('audio_iterations', '—')}` 轮，"
        f"每轮 `{env.get('duration_ms', '—')}ms`，帧间隔 `{env.get('frame_ms', '—')}ms`",
        f"- Ping：每次测试 `{env.get('ping_count', '—')}` 包；报告分别保留 connect/audio 前置 ping",
        "",
        "设备上行流量在网关外网接口施加 netem，返回设备的下行流量在网关 probe 接口施加 netem。"
        "新格式日志分别记录上行与下行丢包率；旧格式日志按 Netem 方向解释 LOSS。",
        "音频发送/收包、上下行延迟、首包回声、帧间隔和卡顿指标仅以 "
        "`audio-samples/*.csv` 重新计算，不从日志读取音频汇总。",
        f"每轮计算前固定跳过起始帧：`{args.audio_skip_frames}` 帧。" if args.audio_skip_frames is not None else
        f"每轮计算前根据 CSV 帧间隔换算并跳过约 `{args.audio_skip_duration_ms}ms` 的起始帧。",
        "旧 CSV 未记录实际 call duration，卡顿占比按发送时间轴、末帧时长和 1 秒收包窗口推导；"
        "缺少采样 CSV 时音频指标显示 `—`。",
        "",
        "## Connect 结果",
        "",
        "| Delay | 上行 Loss | 下行 Loss | Ping Loss；RTT 平均/P50/P90/P95/P99 | Connect 成功 | 平均 | P50 | P90 | P95 | P99 | 状态 | 退出码 | 日志 | 测试脚本 |",
        "|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---|---:|---|---|",
    ]
    for delay, uplink_loss, downlink_loss in connect_combinations:
        case = cases.get((delay, uplink_loss, downlink_loss, "connect"), {})
        rel = f"logs/{case['path'].name}" if case else "—"
        script_rel = f"commands/{case['path'].stem}.sh" if case else "—"
        success = f"{value(case, 'connect_ok')}/{value(case, 'connect_total')} ({value(case, 'connect_rate', '%')})"
        lines.append(
            f"| {delay}ms | {uplink_loss}% | {downlink_loss}% | {ping_cell(case)} | {success} | "
            f"{value(case, 'connect_avg', 'ms')} | {value(case, 'connect_p50', 'ms')} | "
            f"{value(case, 'connect_p90', 'ms')} | {value(case, 'connect_p95', 'ms')} | "
            f"{value(case, 'connect_p99', 'ms')} | {status_cell(case)} | "
            f"{value(case, 'exit_code')} | [{Path(rel).name}]({rel}) | [{Path(script_rel).name}]({script_rel}) |"
        )

    lines += [
        "",
        "## Audio 结果",
        "",
        "| Delay | 上行 Loss | 下行 Loss | Ping Loss；RTT 平均/P50/P90/P95/P99 | 成功轮次 | 发送/失败 | 服务端收包率 | 回声率 | 上行延迟 平均/P50/P90/P95/P99 | 下行延迟 平均/P50/P90/P95/P99 | 帧间隔分布 平均/P50/P90/P95/P99 | 卡顿平均/P50/P90/P95/P99 | 首包回声 P50/P90/P95/P99 | 状态 | 退出码 | 样本 CSV | 日志 | 测试脚本 |",
        "|---:|---:|---:|---|---:|---:|---:|---:|---|---|---|---|---|---|---:|---|---|---|",
    ]
    for delay, uplink_loss, downlink_loss in audio_combinations:
        case = cases.get((delay, uplink_loss, downlink_loss, "audio"), {})
        log_path = case.get("path")
        sample_path = case.get("sample_path")
        rel = f"logs/{log_path.name}" if log_path else "—"
        sample_rel = f"audio-samples/{sample_path.name}" if sample_path else "—"
        stem = log_path.stem if log_path else sample_path.stem if sample_path else ""
        script_rel = f"commands/{stem}.sh" if stem else "—"
        rounds = f"{value(case, 'audio_ok')}/{value(case, 'audio_total')}"
        sent = f"{value(case, 'sent')}/{value(case, 'send_failed')}"
        stutter = "/".join(value(case, key, "%") for key in (
            "stutter_avg", "stutter_p50", "stutter_p90", "stutter_p95", "stutter_p99"))
        uplink = "/".join(value(case, key, "ms") for key in (
            "uplink_latency_avg", "uplink_latency_p50", "uplink_latency_p90",
            "uplink_latency_p95", "uplink_latency_p99"))
        downlink = "/".join(value(case, key, "ms") for key in (
            "downlink_latency_avg", "downlink_latency_p50", "downlink_latency_p90",
            "downlink_latency_p95", "downlink_latency_p99"))
        frame_interval = "/".join(value(case, key, "ms") for key in (
            "frame_interval_avg", "frame_interval_p50", "frame_interval_p90",
            "frame_interval_p95", "frame_interval_p99"))
        echo = "/".join(value(case, key, "ms") for key in (
            "echo_latency_p50", "echo_latency_p90", "echo_latency_p95", "echo_latency_p99"))
        lines.append(
            f"| {delay}ms | {uplink_loss}% | {downlink_loss}% | {ping_cell(case)} | {rounds} | {sent} | "
            f"{value(case, 'server_rate', '%')} | {value(case, 'echo_rate', '%')} | {uplink} | {downlink} | "
            f"{frame_interval} | {stutter} | "
            f"{echo} | {status_cell(case)} | {value(case, 'exit_code')} | "
            f"[{Path(sample_rel).name}]({sample_rel}) | [{Path(rel).name}]({rel}) | "
            f"[{Path(script_rel).name}]({script_rel}) |"
        )

    failed = [
        (delay, uplink_loss, downlink_loss, command, case.get("exit_code"))
        for (delay, uplink_loss, downlink_loss, command), case in cases.items()
        if case.get("exit_code") not in (None, "0")
    ]
    lines += ["", "## 结论与限制", ""]
    if failed:
        lines.append(f"- 共 `{len(failed)}` 个命令返回非零；状态列区分业务失败、预对时失败、Docker 启动失败和 SIGKILL。")
    else:
        lines.append("- 所有矩阵命令均返回 0。")
    lines += [
        "- Ping 数据来自 probe 容器，经同一 netem gateway 到测试目标，可用于核对实际 RTT 和丢包。",
        "- macOS arm64 Docker Desktop 运行 linux/amd64 镜像，绝对 CPU/时延可能受虚拟化和指令模拟影响。",
        "- `loss` 在上下行独立随机应用；一次请求-响应事务的成功概率不能直接等同于单向配置值。",
        "- 原始日志和退出码均保存在 `logs/`，报告未包含 token 或 device secret。",
        "- Connect 退出码 1 表示成功次数未达到总尝试次数；Audio 退出码 1 表示成功轮次未达到总轮次。`no samples` 表示没有成功样本，不是解析遗漏。",
        "- 退出码 137 表示进程收到 SIGKILL，属于运行中止，不能解读为网络丢包。",
        "",
    ]
    Path(args.output).write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
