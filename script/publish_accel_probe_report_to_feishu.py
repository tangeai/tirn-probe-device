#!/usr/bin/env python3
"""Convert an accel-probe report directory into a child Feishu Wiki document.

Pipeline:
1. Rebuild report.md from logs, CSV samples, and environment.txt.
2. Create a new document under the fixed test-report Wiki node, or overwrite
   an existing document when --doc is supplied.
3. Upload each representative echo WAV as a docx_file material.
4. Move the generated file View block below its matching audio heading.
5. Fetch the document and verify headings, multiline distributions, and files.

An overwrite intentionally replaces the complete document and uploads every
representative audio file again.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path


PARENT_WIKI_NODE = "QtYxws1LtiG2qokimKzcW1DpnKh"
PARENT_WIKI_URL = f"https://tange-ai.feishu.cn/wiki/{PARENT_WIKI_NODE}"
SCRIPT_DIR = Path(__file__).resolve().parent
REPORT_GENERATOR = SCRIPT_DIR / "generate_accel_probe_netem_report.py"
AUDIO_HEADING_RE = re.compile(r"^### 回声音频 (A\d{2})$", re.MULTILINE)
AUDIO_FILE_RE = re.compile(r"^- 文件：`([^`]+)`$", re.MULTILINE)


def run(command, cwd=None):
    command = [str(item) for item in command]
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(command)}\n{detail}")
    return completed.stdout


def load_json(output, operation):
    try:
        payload = json.loads(output)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{operation} returned invalid JSON: {output[:500]}") from error
    if payload.get("ok") is False or payload.get("code", 0) != 0:
        raise RuntimeError(f"{operation} failed: {json.dumps(payload, ensure_ascii=False)}")
    return payload


def recursive_values(value, keys):
    found = []
    if isinstance(value, dict):
        for key, item in value.items():
            if key in keys and isinstance(item, str) and item:
                found.append(item)
            found.extend(recursive_values(item, keys))
    elif isinstance(value, list):
        for item in value:
            found.extend(recursive_values(item, keys))
    return found


def created_document_tokens(payload):
    document_tokens = recursive_values(
        payload, {"document_id", "doc_id", "obj_token", "object_token"}
    )
    node_tokens = recursive_values(payload, {"node_token", "wiki_node_token"})
    urls = recursive_values(payload, {"url"})
    for url in urls:
        match = re.search(r"/docx/([A-Za-z0-9]+)", url)
        if match:
            document_tokens.append(match.group(1))
        match = re.search(r"/wiki/([A-Za-z0-9]+)", url)
        if match:
            node_tokens.append(match.group(1))
    if not document_tokens:
        raise RuntimeError(f"create response does not contain a document token: {payload}")
    document_token = document_tokens[0]
    node_token = node_tokens[0] if node_tokens else None
    return document_token, node_token


def resolve_wiki_document_token(node_token):
    payload = load_json(run([
        "lark-cli", "api", "GET", "/open-apis/wiki/v2/spaces/get_node",
        "--params", json.dumps({"token": node_token}),
    ]), "resolve Wiki document token")
    node = payload.get("data", {}).get("node", {})
    if node.get("obj_type") != "docx" or not node.get("obj_token"):
        raise RuntimeError(f"Wiki node is not a Docx document: {payload}")
    return node["obj_token"]


def report_audio_entries(report_dir, markdown):
    entries = []
    headings = list(AUDIO_HEADING_RE.finditer(markdown))
    for index, heading in enumerate(headings):
        end = headings[index + 1].start() if index + 1 < len(headings) else len(markdown)
        section = markdown[heading.end():end]
        file_match = AUDIO_FILE_RE.search(section)
        if not file_match:
            continue
        name = file_match.group(1)
        candidates = [
            report_dir / "audio-echo-wav" / name,
            report_dir / "audio-echo" / name,
        ]
        path = next((candidate for candidate in candidates if candidate.is_file()), None)
        if path is None:
            raise RuntimeError(f"audio file listed in report was not found: {name}")
        entries.append((heading.group(1), path))
    return entries


def block_text(block):
    for key in ("heading1", "heading2", "heading3", "heading4"):
        elements = block.get(key, {}).get("elements", [])
        if elements:
            return "".join(
                element.get("text_run", {}).get("content", "") for element in elements
            )
    return ""


def block_maps(payload):
    items = payload.get("data", {}).get("items", [])
    headings = {block_text(block): block["block_id"] for block in items if block_text(block)}
    files = {}
    for block in items:
        if block.get("block_type") == 23:
            token = block.get("file", {}).get("token")
            if token:
                files[token] = block.get("parent_id")
    return headings, files


def list_blocks(document_token):
    output = run([
        "lark-cli", "api", "GET",
        f"/open-apis/docx/v1/documents/{document_token}/blocks",
        "--page-all", "--page-size", "500", "--page-limit", "10",
    ])
    return load_json(output, "list document blocks")


def upload_audio(document_token, audio_path):
    size = audio_path.stat().st_size
    if size > 20 * 1024 * 1024:
        raise RuntimeError(
            f"audio exceeds Feishu upload_all 20 MiB limit: {audio_path} ({size} bytes)"
        )
    form = json.dumps({
        "file_name": audio_path.name,
        "parent_type": "docx_file",
        "parent_node": document_token,
        "size": str(size),
    }, ensure_ascii=False)
    output = run([
        "lark-cli", "api", "POST", "/open-apis/drive/v1/medias/upload_all",
        "--data", form, "--file", f"file={audio_path.name}",
    ], cwd=audio_path.parent)
    payload = load_json(output, f"upload {audio_path.name}")
    tokens = recursive_values(payload, {"file_token"})
    if not tokens:
        raise RuntimeError(f"upload response has no file_token: {payload}")
    return tokens[0]


def insert_audio_after_heading(document_token, label, audio_path):
    output = run([
        "lark-cli", "docs", "+media-insert",
        "--doc", document_token,
        "--type", "file",
        "--file", audio_path.name,
        "--file-view", "preview",
        "--selection-with-ellipsis", f"回声音频 {label}",
    ], cwd=audio_path.parent)
    payload = load_json(output, f"insert {audio_path.name}")
    if not payload.get("data", {}).get("file_token"):
        raise RuntimeError(f"insert response has no file_token: {payload}")
    return payload["data"]["file_token"]


def move_audio_after_heading(document_token, view_block_id, heading_block_id):
    payload = load_json(run([
        "lark-cli", "docs", "+update", "--api-version", "v2",
        "--doc", document_token, "--command", "block_move_after",
        "--block-id", heading_block_id, "--src-block-ids", view_block_id,
    ]), "move audio block")
    result = payload.get("data", {}).get("result")
    if result == "failed":
        raise RuntimeError(f"move audio block failed: {payload}")


def wait_for_audio_block(document_token, file_token, attempts=10):
    for attempt in range(attempts):
        payload = list_blocks(document_token)
        headings, files = block_maps(payload)
        if file_token in files:
            return headings, files[file_token]
        if attempt + 1 < attempts:
            time.sleep(1)
    raise RuntimeError(f"uploaded file block did not appear: {file_token}")


def main():
    parser = argparse.ArgumentParser(
        description=f"发布 accel-probe 测试报告到固定父节点 {PARENT_WIKI_URL}"
    )
    parser.add_argument("report_dir", type=Path, help="测试数据目录（含 logs/、audio-samples/）")
    parser.add_argument("--title", help="飞书子文档标题；默认使用测试目录名")
    parser.add_argument(
        "--doc",
        help="全量覆盖现有飞书文档（URL 或 token）；不指定时在固定父节点新建文档",
    )
    parser.add_argument("--output", default="report.md", help="报告文件名，默认 report.md")
    parser.add_argument("--skip-audio", action="store_true", help="只发布文字和表格，不上传音频")
    parser.add_argument("--audio-skip-duration-ms", type=int, default=10000)
    args = parser.parse_args()

    report_dir = args.report_dir.resolve()
    if not report_dir.is_dir():
        parser.error(f"report directory does not exist: {report_dir}")
    if shutil.which("lark-cli") is None:
        parser.error("lark-cli is not installed or not in PATH")

    report_path = report_dir / args.output
    run([
        sys.executable, REPORT_GENERATOR,
        "--report-dir", report_dir,
        "--output", report_path,
        "--audio-skip-duration-ms", str(args.audio_skip_duration_ms),
    ])
    markdown = report_path.read_text(encoding="utf-8")
    audio_entries = [] if args.skip_audio else report_audio_entries(report_dir, markdown)
    title = args.title or report_dir.name

    if args.doc:
        token_match = re.search(r"/(?:wiki|docx)/([A-Za-z0-9]+)", args.doc)
        document_token = token_match.group(1) if token_match else args.doc
        node_token = document_token if "/wiki/" in args.doc else None
        if node_token:
            document_token = resolve_wiki_document_token(node_token)
        update_command = [
            "lark-cli", "docs", "+update", "--doc", args.doc,
            "--mode", "overwrite", "--markdown", f"@./{report_path.name}",
        ]
        if args.title:
            update_command.extend(["--new-title", title])
        load_json(run(update_command, cwd=report_path.parent), "overwrite Feishu document")
        created_url = args.doc if "/" in args.doc else f"https://tange-ai.feishu.cn/docx/{document_token}"
        print(f"overwritten: {created_url}", file=sys.stderr, flush=True)
    else:
        create_output = run([
            "lark-cli", "docs", "+create",
            "--wiki-node", PARENT_WIKI_NODE,
            "--title", title,
            "--markdown", f"@./{report_path.name}",
        ], cwd=report_path.parent)
        create_payload = load_json(create_output, "create Feishu document")
        document_token, node_token = created_document_tokens(create_payload)
        created_url_token = node_token or document_token
        created_url_kind = "wiki" if node_token else "docx"
        created_url = f"https://tange-ai.feishu.cn/{created_url_kind}/{created_url_token}"
        print(f"created: {created_url}", file=sys.stderr, flush=True)

    uploaded = 0
    for label, audio_path in audio_entries:
        insert_audio_after_heading(document_token, label, audio_path)
        uploaded += 1

    fetched = run([
        "lark-cli", "docs", "+fetch", "--doc", document_token, "--format", "pretty",
    ])
    if "stutter_i = gap_i" not in fetched:
        raise RuntimeError("verification failed: stutter formula missing")
    distribution_lines = ["平均", "P50", "P90", "P95", "P99"]
    if not re.search(
        r"平均\s+[^\n]+\n\s*P50\s+[^\n]+\n\s*P90\s+[^\n]+\n\s*P95\s+[^\n]+\n\s*P99\s+[^\n]+",
        fetched,
    ):
        raise RuntimeError(
            "verification failed: no multiline distribution cell containing "
            + "/".join(distribution_lines)
        )
    if re.search(r"(?:Ping|上行延迟|下行延迟|帧间隔分布|卡顿|首包回声)\s+平均/P50", fetched):
        raise RuntimeError("verification failed: distribution suffix remains in a column name")
    if not args.skip_audio and fetched.count("<file token=") != uploaded:
        raise RuntimeError(
            f"verification failed: expected {uploaded} audio blocks, "
            f"found {fetched.count('<file token=')}"
        )

    print(json.dumps({
        "success": True,
        "title": title,
        "document_token": document_token,
        "wiki_node_token": node_token,
        "audio_uploaded": uploaded,
        "url": created_url,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
