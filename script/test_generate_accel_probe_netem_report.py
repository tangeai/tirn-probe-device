import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("generate_accel_probe_netem_report.py")
SPEC = importlib.util.spec_from_file_location("generate_accel_probe_netem_report", SCRIPT_PATH)
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class ParseLogTest(unittest.TestCase):
    def test_audio_metrics_are_recalculated_from_csv(self):
        csv_text = """\
iteration,send_index,frame_ts_ms,send_ret,send_late_us,observed,echoed,client_send_unix_ns,server_recv_unix_ns,client_echo_recv_unix_ns,uplink_us,downlink_us,echo_us,echo_arrival_gap_us,stutter,stutter_time_us
1,1,1000,10,0,1,1,1000000000,1010000000,1030000000,10000,20000,30000,0,0,0
1,2,1040,-1,0,0,0,1040000000,0,0,0,0,0,0,0,0
1,3,1080,10,0,1,1,1080000000,1090000000,1430000000,10000,340000,350000,400000,1,400000
2,1,2000,10,0,1,1,2000000000,2015000000,2035000000,15000,20000,35000,0,0,0
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            csv_path = Path(temp_dir) / "audio_delay_000_loss_00.csv"
            csv_path.write_text(csv_text)
            result = REPORT.read_audio_metrics(csv_path, 2, skip_frames=0)

        self.assertEqual(result["audio_ok"], "2")
        self.assertEqual(result["audio_total"], "2")
        self.assertEqual(result["sent"], "4")
        self.assertEqual(result["send_failed"], "1")
        self.assertEqual(result["server_received"], "3")
        self.assertEqual(result["echo_received"], "3")
        self.assertEqual(result["server_rate"], "75.00")
        self.assertEqual(result["echo_rate"], "75.00")
        self.assertEqual(result["uplink_latency_avg"], "11.67")
        self.assertEqual(result["downlink_latency_avg"], "126.67")
        self.assertEqual(result["echo_latency_avg"], "32.50")
        self.assertEqual(result["frame_interval_avg"], "400.00")
        self.assertGreater(float(result["stutter_avg"]), 0.0)
        self.assertEqual(REPORT.infer_skip_frames([
            {"iteration": "1", "frame_ts_ms": str(index * 40)} for index in range(251)
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
