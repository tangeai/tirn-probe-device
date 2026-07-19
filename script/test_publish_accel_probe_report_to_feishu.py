import importlib.util
import re
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("publish_accel_probe_report_to_feishu.py")
SPEC = importlib.util.spec_from_file_location("publish_accel_probe_report_to_feishu", SCRIPT)
PUBLISH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PUBLISH)


class PublishReportTest(unittest.TestCase):
    def test_multiline_distribution_verification_pattern(self):
        fetched = "平均 14.59ms\nP50 14.63ms\nP90 16.80ms\nP95 17.06ms\nP99 17.34ms"
        self.assertRegex(
            fetched,
            re.compile(
                r"平均\s+[^\n]+\n\s*P50\s+[^\n]+\n\s*P90\s+[^\n]+\n\s*P95\s+[^\n]+\n\s*P99\s+[^\n]+"
            ),
        )

    def test_created_document_tokens(self):
        document, node = PUBLISH.created_document_tokens({
            "ok": True,
            "data": {
                "document": {"document_id": "docx123"},
                "wiki": {"node_token": "wik123"},
            },
        })
        self.assertEqual(document, "docx123")
        self.assertEqual(node, "wik123")

    def test_resolve_wiki_document_token(self):
        original_run = PUBLISH.run
        try:
            PUBLISH.run = lambda command, cwd=None: '{"code":0,"data":{"node":{"obj_type":"docx","obj_token":"docx123"}}}'
            self.assertEqual(PUBLISH.resolve_wiki_document_token("wiki123"), "docx123")
        finally:
            PUBLISH.run = original_run

    def test_insert_audio_after_heading(self):
        original_run = PUBLISH.run
        commands = []
        try:
            def fake_run(command, cwd=None):
                commands.append(command)
                return '{"code":0,"data":{"file_token":"file123"}}'
            PUBLISH.run = fake_run
            token = PUBLISH.insert_audio_after_heading(
                "docx123", "A01", Path("audio.wav")
            )
        finally:
            PUBLISH.run = original_run
        self.assertEqual(token, "file123")
        self.assertIn("preview", commands[0])
        self.assertIn("回声音频 A01", commands[0])

    def test_block_maps(self):
        payload = {"data": {"items": [
            {
                "block_id": "heading-1",
                "block_type": 5,
                "heading3": {"elements": [{"text_run": {"content": "回声音频 A01"}}]},
            },
            {
                "block_id": "file-1",
                "block_type": 23,
                "parent_id": "view-1",
                "file": {"token": "file-token"},
            },
        ]}}
        headings, files = PUBLISH.block_maps(payload)
        self.assertEqual(headings["回声音频 A01"], "heading-1")
        self.assertEqual(files["file-token"], "view-1")


if __name__ == "__main__":
    unittest.main()
