import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("generate_accel_probe_netem_report.py")
SPEC = importlib.util.spec_from_file_location("generate_accel_probe_netem_report", SCRIPT_PATH)
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class ParseLogTest(unittest.TestCase):
    def test_stutter_events_are_nested_ordered_items_without_empty_bullets(self):
        lines = REPORT.stutter_event_lines([(0, 12080000, 368070)])
        self.assertEqual(
            lines,
            ["    1. 完整音频位置 `12.080s`，时长 `368.07ms`"],
        )
        self.assertNotIn("    - 1.", lines[0])

    def test_distribution_cells_use_labeled_line_breaks(self):
        case = {
            "ping_loss": "10",
            "ping_avg": "12.00",
            "ping_p50": "11.00",
            "ping_p90": "13.00",
            "ping_p95": "14.00",
            "ping_p99": "15.00",
        }
        self.assertEqual(
            REPORT.ping_cell(case),
            "Loss 10%<br>平均 12.00ms<br>P50 11.00ms<br>P90 13.00ms<br>"
            "P95 14.00ms<br>P99 15.00ms",
        )

        self.assertEqual(
            REPORT.distribution_cell(case, (
                ("平均", "ping_avg"),
                ("P50", "ping_p50"),
                ("P90", "ping_p90"),
                ("P95", "ping_p95"),
                ("P99", "ping_p99"),
            ), "ms"),
            "平均 12.00ms<br>P50 11.00ms<br>P90 13.00ms<br>"
            "P95 14.00ms<br>P99 15.00ms",
        )

    def test_audio_metrics_are_recalculated_from_csv(self):
        csv_text = """\
iteration,send_index,frame_index,frame_duration_us,send_ret,send_late_us,client_send_unix_us,client_send_monotonic_us,estimated_server_send_unix_us,clock_offset_us,time_sync_rtt_us,outbound_packed_ts,echo_received,echo_packed_ts,server_receive_unix_us,client_echo_recv_unix_us,client_echo_recv_monotonic_us,timestamp_decode_status,duplicate_echo_count
1,1,1,40000,10,0,1000000,1000000,1000000,0,20000,0,1,0,1010000,1030000,1030000,ok,0
1,2,2,40000,-1,0,1040000,1040000,1040000,0,20000,0,0,0,0,0,0,not_received,0
1,3,3,40000,10,0,1080000,1080000,1080000,0,20000,0,1,0,1090000,1430000,1430000,ok,0
2,1,1,40000,10,0,2000000,2000000,2000000,0,20000,0,1,0,2015000,2035000,2035000,ok,0
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            csv_path = Path(temp_dir) / "audio_delay_000_loss_00.csv"
            csv_path.write_text(csv_text)
            result = REPORT.read_audio_metrics(csv_path, 2, skip_frames=0)
            skipped_result = REPORT.read_audio_metrics(csv_path, 2, skip_frames=1)

        self.assertEqual(result["audio_ok"], "2")
        self.assertEqual(result["audio_total"], "2")
        self.assertEqual(result["sent"], "4")
        self.assertEqual(result["send_failed"], "1")
        self.assertEqual(result["echo_received"], "3")
        self.assertEqual(result["echo_rate"], "75.00")
        self.assertEqual(result["one_way_unavailable"], "1")
        self.assertEqual(result["uplink_latency_avg"], "11.67")
        self.assertEqual(result["downlink_latency_avg"], "126.67")
        self.assertEqual(result["echo_latency_avg"], "32.50")
        self.assertEqual(result["frame_interval_avg"], "400.00")
        self.assertEqual(result["stutter_avg"], "16.07")
        self.assertEqual(result["representative_iteration"], "1")
        self.assertEqual(result["representative_first_echo_ms"], "30.00")
        self.assertEqual(result["representative_stutter_count"], "1")
        self.assertEqual(result["representative_stutter_events"], [(40000, 40000, 360000)])
        self.assertEqual(skipped_result["stutter_avg"], "0.00")
        self.assertEqual(skipped_result["representative_stutter_count"], "0")
        self.assertEqual(REPORT.infer_skip_frames([
            {"iteration": "1", "frame_duration_us": "40000"} for _ in range(251)
        ], 10000), 250)

    def test_audio_log_summary_is_not_used_without_csv(self):
        first = self.audio_summary(1, 250, "10.00")
        final = self.audio_summary(10, 2500, "20.00")

        with tempfile.TemporaryDirectory() as temp_dir:
            log_path = Path(temp_dir) / "audio_delay_000_loss_00.log"
            log_path.write_text(first + final)
            Path(str(log_path) + ".exit_code").write_text("0\n")

            result = REPORT.parse_log(log_path)

        self.assertNotIn("audio_ok", result)
        self.assertNotIn("sent", result)
        self.assertNotIn("stutter_avg", result)

    @staticmethod
    def audio_summary(round_number, sent, average):
        return f"""\
音频多轮汇总: 成功轮次={round_number}/10
音频包数多轮汇总: 设备发送={sent} 发送失败=0 服务端收到={sent} 设备收到回声={sent} 服务端收包率=100.00% 回声收包率=100.00%
音频首包回声总延迟(设备发出到收到回声): 样本数={round_number} 平均={average}ms 中位数=1.00ms P90=2.00ms P95=3.00ms P99=4.00ms
音频卡顿占比多轮汇总: 样本数={round_number} 平均=0.00% 中位数=0.00% P90=0.00% P95=0.00% P99=0.00%
音频上行延迟多轮汇总(设备到服务端): 样本数={sent} 平均={average}ms 中位数=1.00ms P90=2.00ms P95=3.00ms P99=4.00ms
音频下行延迟多轮汇总(服务端到设备): 样本数={sent} 平均={average}ms 中位数=1.00ms P90=2.00ms P95=3.00ms P99=4.00ms
"""


if __name__ == "__main__":
    unittest.main()
