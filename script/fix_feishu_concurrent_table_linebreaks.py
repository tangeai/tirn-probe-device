#!/usr/bin/env python3
"""Locally repair line breaks in the test tables of a Feishu Docx.

The script updates existing text blocks in place. It never overwrites Markdown,
recreates tables, or touches file/View blocks.
"""

import argparse
import copy
import datetime as dt
import json
import re
import subprocess
import sys
from pathlib import Path


IDLE_TITLE = "服务端闲时测试"
CONCURRENT_TITLE = "服务端并发测试"
CONNECT_TITLE = "Connect 结果"
AUDIO_TITLE = "Audio 结果"
EXPECTED_IDLE_CONNECT_UPDATES = 20
EXPECTED_IDLE_AUDIO_UPDATES = 100
EXPECTED_CONNECT_UPDATES = 140
EXPECTED_AUDIO_UPDATES = 100
METRIC_BREAK_RE = re.compile(r"(?<!^)(?=(?:平均|P50|P90|P95|P99)\s)")


def run(command):
    completed = subprocess.run(
        [str(item) for item in command], check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
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


def block_text(block):
    for key in ("heading1", "heading2", "heading3", "heading4", "text"):
        elements = block.get(key, {}).get("elements", [])
        if elements:
            return "".join(
                element.get("text_run", {}).get("content", "") for element in elements
            )
    return ""


def resolve_document_token(doc):
    match = re.search(r"/(wiki|docx)/([A-Za-z0-9]+)", doc)
    if not match:
        return doc
    if match.group(1) == "docx":
        return match.group(2)
    payload = load_json(run([
        "lark-cli", "api", "GET", "/open-apis/wiki/v2/spaces/get_node",
        "--params", json.dumps({"token": match.group(2)}),
    ]), "resolve Wiki document")
    node = payload.get("data", {}).get("node", {})
    if node.get("obj_type") != "docx" or not node.get("obj_token"):
        raise RuntimeError("Wiki node is not a Docx document")
    return node["obj_token"]


def fetch_blocks(document_token):
    return load_json(run([
        "lark-cli", "api", "GET",
        f"/open-apis/docx/v1/documents/{document_token}/blocks",
        "--page-all", "--page-size", "500", "--page-limit", "0",
    ]), "list document blocks").get("data", {}).get("items", [])


def fetch_revision(document_token):
    payload = load_json(run([
        "lark-cli", "api", "GET",
        f"/open-apis/docx/v1/documents/{document_token}",
    ]), "get document")
    return payload.get("data", {}).get("document", {}).get("revision_id")


def unique_heading_index(items, title, block_type, start=0, stop=None):
    stop = len(items) if stop is None else stop
    found = [
        index for index, block in enumerate(items[start:stop], start)
        if block.get("block_type") == block_type and block_text(block).strip() == title
    ]
    if len(found) != 1:
        raise RuntimeError(
            f"expected one {title!r} heading in indexes [{start}, {stop}), found {len(found)}"
        )
    return found[0]


def table_after_heading(items, heading_index, stop_index):
    tables = [
        block for block in items[heading_index + 1:stop_index]
        if block.get("block_type") == 31 and block.get("parent_id") == items[heading_index].get("parent_id")
    ]
    if len(tables) != 1:
        raise RuntimeError(
            f"expected one table after {block_text(items[heading_index])!r}, found {len(tables)}"
        )
    return tables[0]["block_id"]


def table_text_blocks(items, table_id):
    by_id = {block["block_id"]: block for block in items}
    result = []
    for block in items:
        if block.get("block_type") != 2:
            continue
        parent = by_id.get(block.get("parent_id"), {})
        if parent.get("block_type") == 32 and parent.get("parent_id") == table_id:
            result.append(block)
    return result


def format_distribution(text):
    if "\n" in text:
        return None
    lines = METRIC_BREAK_RE.sub("\n", text).splitlines()
    expected = ["平均", "P50", "P90", "P95", "P99"]
    offset = 1 if lines and lines[0].startswith(("Loss ", "loss ")) else 0
    if len(lines) != len(expected) + offset:
        return None
    if offset and not lines[0].startswith(("Loss ", "loss ")):
        return None
    if any(not line.startswith(label + " ") for line, label in zip(lines[offset:], expected)):
        return None
    if any("（Δ " not in line or not line.endswith("）") for line in lines):
        return None
    return "\n".join(lines)


def format_plain_distribution(text):
    """Split a non-Delta distribution imported as one concatenated text run."""
    if "\n" in text or "（Δ " in text:
        return None
    lines = METRIC_BREAK_RE.sub("\n", text).splitlines()
    expected = ["平均", "P50", "P90", "P95", "P99"]
    offset = 1 if lines and lines[0].startswith(("Loss ", "loss ")) else 0
    if len(lines) != len(expected) + offset:
        return None
    if offset and not (lines[0].endswith(";") or lines[0].endswith("%")):
        return None
    if any(not line.startswith(label + " ") for line, label in zip(lines[offset:], expected)):
        return None
    return "\n".join(lines)


def format_scalar_delta(text):
    if "\n" in text or text.count("Δ ") != 1:
        return None
    left, delta = text.split("Δ ", 1)
    if not left or not delta:
        return None
    return f"{left}\nΔ {delta}"


def replacement_element(block, content):
    elements = block.get("text", {}).get("elements", [])
    if not elements or any("text_run" not in element for element in elements):
        raise RuntimeError(f"unsupported text element layout in block {block['block_id']}")
    styles = [element["text_run"].get("text_element_style", {}) for element in elements]
    if any(style != styles[0] for style in styles[1:]):
        raise RuntimeError(f"mixed text styles in block {block['block_id']}")
    element = copy.deepcopy(elements[0])
    element["text_run"]["content"] = content
    return element


def build_update(block, content):
    return {
        "block_id": block["block_id"],
        "update_text_elements": {"elements": [replacement_element(block, content)]},
    }


def select_updates(items):
    idle = unique_heading_index(items, IDLE_TITLE, 3)
    concurrent = unique_heading_index(items, CONCURRENT_TITLE, 3, idle + 1)
    idle_connect = unique_heading_index(items, CONNECT_TITLE, 4, idle + 1, concurrent)
    idle_audio = unique_heading_index(items, AUDIO_TITLE, 4, idle_connect + 1, concurrent)
    connect = unique_heading_index(items, CONNECT_TITLE, 4, concurrent + 1)
    audio = unique_heading_index(items, AUDIO_TITLE, 4, connect + 1)
    next_heading = next(
        (index for index, block in enumerate(items[audio + 1:], audio + 1)
         if block.get("block_type") in (3, 4) and block.get("parent_id") == items[audio].get("parent_id")),
        len(items),
    )
    idle_connect_table = table_after_heading(items, idle_connect, idle_audio)
    idle_audio_table = table_after_heading(items, idle_audio, concurrent)
    connect_table = table_after_heading(items, connect, audio)
    audio_table = table_after_heading(items, audio, next_heading)

    idle_connect_updates = []
    for block in table_text_blocks(items, idle_connect_table):
        formatted = format_plain_distribution(block_text(block))
        if formatted is not None:
            idle_connect_updates.append(build_update(block, formatted))

    idle_audio_updates = []
    for block in table_text_blocks(items, idle_audio_table):
        formatted = format_plain_distribution(block_text(block))
        if formatted is not None:
            idle_audio_updates.append(build_update(block, formatted))

    connect_updates = []
    for block in table_text_blocks(items, connect_table):
        text = block_text(block)
        formatted = format_distribution(text) or format_scalar_delta(text)
        if formatted is not None:
            connect_updates.append(build_update(block, formatted))

    audio_updates = []
    for block in table_text_blocks(items, audio_table):
        formatted = format_distribution(block_text(block))
        if formatted is not None:
            audio_updates.append(build_update(block, formatted))

    if len(idle_connect_updates) not in (0, EXPECTED_IDLE_CONNECT_UPDATES):
        raise RuntimeError(
            f"expected 0 or {EXPECTED_IDLE_CONNECT_UPDATES} idle Connect updates, "
            f"found {len(idle_connect_updates)}"
        )
    if len(idle_audio_updates) not in (0, EXPECTED_IDLE_AUDIO_UPDATES):
        raise RuntimeError(
            f"expected 0 or {EXPECTED_IDLE_AUDIO_UPDATES} idle Audio updates, "
            f"found {len(idle_audio_updates)}"
        )
    if len(connect_updates) not in (0, EXPECTED_CONNECT_UPDATES):
        raise RuntimeError(
            f"expected 0 or {EXPECTED_CONNECT_UPDATES} concurrent Connect updates, "
            f"found {len(connect_updates)}"
        )
    if len(audio_updates) not in (0, EXPECTED_AUDIO_UPDATES):
        raise RuntimeError(
            f"expected 0 or {EXPECTED_AUDIO_UPDATES} concurrent Audio updates, "
            f"found {len(audio_updates)}"
        )
    return idle_connect_updates, idle_audio_updates, connect_updates, audio_updates


def file_signature(items):
    return sorted(
        (block["block_id"], block.get("parent_id"), block.get("file", {}).get("token"),
         block.get("file", {}).get("name"))
        for block in items if block.get("block_type") == 23
    )


def originals_for(items, updates):
    by_id = {block["block_id"]: block for block in items}
    return [build_update(by_id[update["block_id"]], block_text(by_id[update["block_id"]]))
            for update in updates]


def batch_update(document_token, requests):
    if len(requests) > 200:
        raise RuntimeError(f"batch contains {len(requests)} requests; maximum is 200")
    load_json(run([
        "lark-cli", "api", "PATCH",
        f"/open-apis/docx/v1/documents/{document_token}/blocks/batch_update",
        "--params", json.dumps({"document_revision_id": -1}),
        "--data", json.dumps({"requests": requests}, ensure_ascii=False),
    ]), "batch update blocks")


def verify_after(before, after, updates, before_files):
    update_ids = {update["block_id"] for update in updates}
    before_by_id = {block["block_id"]: block for block in before}
    after_by_id = {block["block_id"]: block for block in after}
    if set(before_by_id) != set(after_by_id):
        raise RuntimeError("document block IDs changed")
    if file_signature(after) != before_files or len(before_files) != 40:
        raise RuntimeError("audio attachment blocks changed")
    for block_id in update_ids:
        text = block_text(after_by_id[block_id])
        if "\n" not in text:
            raise RuntimeError(f"line break missing after update: {block_id}")
    for block_id in set(before_by_id) - update_ids:
        if before_by_id[block_id] != after_by_id[block_id]:
            raise RuntimeError(f"non-target block changed: {block_id}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--doc", required=True, help="Feishu Wiki/Docx URL or document token")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply", action="store_true")
    parser.add_argument("--snapshot", type=Path, help="pre-update snapshot JSON path")
    args = parser.parse_args()

    document_token = resolve_document_token(args.doc)
    before = fetch_blocks(document_token)
    revision = fetch_revision(document_token)
    idle_connect_updates, idle_audio_updates, connect_updates, audio_updates = select_updates(before)
    groups = [idle_connect_updates, idle_audio_updates, connect_updates, audio_updates]
    updates = [update for group in groups for update in group]
    before_files = file_signature(before)
    if len(before_files) != 40:
        raise RuntimeError(f"expected 40 audio attachments, found {len(before_files)}")

    snapshot = args.snapshot or Path(
        f"feishu-linebreak-snapshot-{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}.json"
    )
    snapshot_payload = {
        "document_token": document_token,
        "revision_id": revision,
        "audio_files": before_files,
        "idle_connect_updates": idle_connect_updates,
        "idle_audio_updates": idle_audio_updates,
        "connect_updates": connect_updates,
        "audio_updates": audio_updates,
        "rollback_requests": originals_for(before, updates),
    }

    examples = [
        {
            "block_id": update["block_id"],
            "before": block_text({"text": {"elements": [originals_for(before, [update])[0]
                ["update_text_elements"]["elements"][0]]}}),
            "after": update["update_text_elements"]["elements"][0]["text_run"]["content"],
        }
        for update in updates[:3]
    ]
    if args.dry_run:
        print(json.dumps({
            "success": True, "mode": "dry-run", "document_token": document_token,
            "revision_id": revision,
            "idle_connect_updates": len(idle_connect_updates),
            "idle_audio_updates": len(idle_audio_updates),
            "concurrent_connect_updates": len(connect_updates),
            "concurrent_audio_updates": len(audio_updates),
            "audio_files": len(before_files),
            "examples": examples,
        }, ensure_ascii=False, indent=2))
        return

    snapshot.write_text(json.dumps(snapshot_payload, ensure_ascii=False, indent=2) + "\n")
    try:
        for group in groups:
            if group:
                batch_update(document_token, group)
        after = fetch_blocks(document_token)
        verify_after(before, after, updates, before_files)
    except Exception:
        batch_update(document_token, snapshot_payload["rollback_requests"][:200])
        batch_update(document_token, snapshot_payload["rollback_requests"][200:])
        raise

    print(json.dumps({
        "success": True, "mode": "apply", "document_token": document_token,
        "revision_id_before": revision, "connect_updated": len(connect_updates),
        "audio_updated": len(audio_updates),
        "idle_connect_updated": len(idle_connect_updates),
        "idle_audio_updated": len(idle_audio_updates),
        "audio_files_unchanged": len(before_files),
        "snapshot": str(snapshot),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
