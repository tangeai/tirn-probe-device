import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("fix_feishu_concurrent_table_linebreaks.py")
SPEC = importlib.util.spec_from_file_location("fix_feishu_concurrent_table_linebreaks", SCRIPT)
FIX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIX)


class LinebreakFormattingTest(unittest.TestCase):
    def test_distribution_places_each_metric_on_its_own_line(self):
        text = (
            "Loss 0%（Δ +0.00pp）平均 25.40ms（Δ +0.09ms）"
            "P50 25.40ms（Δ +0.10ms）P90 25.40ms（Δ +0.10ms）"
            "P95 25.40ms（Δ +0.00ms）P99 25.40ms（Δ +0.00ms）"
        )
        self.assertEqual(FIX.format_distribution(text).splitlines(), [
            "Loss 0%（Δ +0.00pp）", "平均 25.40ms（Δ +0.09ms）",
            "P50 25.40ms（Δ +0.10ms）", "P90 25.40ms（Δ +0.10ms）",
            "P95 25.40ms（Δ +0.00ms）", "P99 25.40ms（Δ +0.00ms）",
        ])

    def test_distribution_accepts_negative_and_zero_delta(self):
        text = (
            "平均 42.39ms（Δ +0.00ms）P50 38.20ms（Δ -1.61ms）"
            "P90 44.90ms（Δ +3.25ms）P95 59.23ms（Δ +17.11ms）"
            "P99 135.22ms（Δ +92.69ms）"
        )
        self.assertEqual(len(FIX.format_distribution(text).splitlines()), 5)

    def test_plain_connect_distribution_is_split(self):
        text = "loss 0%;平均 25.31msP50 25.30msP90 25.30msP95 25.40msP99 25.40ms"
        self.assertEqual(FIX.format_plain_distribution(text).splitlines(), [
            "loss 0%;", "平均 25.31ms", "P50 25.30ms", "P90 25.30ms",
            "P95 25.40ms", "P99 25.40ms",
        ])

    def test_plain_audio_distribution_accepts_loss_without_semicolon(self):
        text = "Loss 0%平均 25.39msP50 25.40msP90 25.40msP95 25.40msP99 25.40ms"
        self.assertEqual(FIX.format_plain_distribution(text).splitlines(), [
            "Loss 0%", "平均 25.39ms", "P50 25.40ms", "P90 25.40ms",
            "P95 25.40ms", "P99 25.40ms",
        ])

    def test_plain_metric_distribution_without_loss_is_split(self):
        text = "平均 14.59msP50 14.63msP90 16.80msP95 17.06msP99 17.34ms"
        self.assertEqual(len(FIX.format_plain_distribution(text).splitlines()), 5)

    def test_scalar_delta_is_split_into_two_lines(self):
        self.assertEqual(FIX.format_scalar_delta("543.50msΔ +273.79ms"),
                         "543.50ms\nΔ +273.79ms")

    def test_connect_success_delta_is_split_into_two_lines(self):
        self.assertEqual(
            FIX.format_scalar_delta("20/20 (100.00%)Δ +0 成功（+0.00pp）"),
            "20/20 (100.00%)\nΔ +0 成功（+0.00pp）",
        )

    def test_already_formatted_or_unknown_text_is_ignored(self):
        self.assertIsNone(FIX.format_distribution("平均 1ms\nP50 1ms"))
        self.assertIsNone(FIX.format_scalar_delta("plain text"))

    def test_replacement_collapses_equally_styled_runs(self):
        style = {"bold": False}
        block = {
            "block_id": "block-1",
            "text": {"elements": [
                {"text_run": {"content": "平均 1ms", "text_element_style": style}},
                {"text_run": {"content": "P50 1ms", "text_element_style": style}},
            ]},
        }
        element = FIX.replacement_element(block, "平均 1ms\nP50 1ms")
        self.assertEqual(element["text_run"]["content"], "平均 1ms\nP50 1ms")
        self.assertEqual(element["text_run"]["text_element_style"], style)


if __name__ == "__main__":
    unittest.main()
