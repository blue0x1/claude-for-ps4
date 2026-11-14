#!/usr/bin/env python3
# Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
"""On-console tool check for Claude for PS4.

Stands in for claude-bridge.py (same port and pairing code) and, instead of
asking Claude, answers any message from the PS4 with a fixed list of tool
calls, one per round. Every result the PS4 sends back is printed and written
to tool_check.log. File tests stay inside /data/claude/tooltest.

Stop the real bridge first, then:
    python3 tests/tool_check_bridge.py
and send any message in the app. Press Allow on each confirm box.
"""
import json
import re
import secrets
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os

TOKEN = (Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "claude-ps4-bridge" / "token").read_text().strip()
D = "/data/claude/tooltest"

# (tool, input, what a pass looks like: substring of the result, or None = not an error)
STEPS = [
    ("get_system_info", {}, "Full file access (GoldHEN jailbreak): yes"),
    ("list_directory", {"path": "/data/claude"}, "boot.log"),
    ("make_directory", {"path": D + "/sub"}, "Created"),
    ("write_text_file", {"path": D + "/a.txt", "content": "hello from the tool check\n"}, "Wrote"),
    ("write_text_file", {"path": D + "/a.txt", "content": "second line\n", "append": True}, "Wrote"),
    ("read_text_file", {"path": D + "/a.txt"}, "second line"),
    ("copy_path", {"source": D + "/a.txt", "destination": D + "/sub/b.txt"}, "Copied"),
    ("move_path", {"source": D + "/sub/b.txt", "destination": D + "/c.txt"}, "Moved"),
    ("list_directory", {"path": D}, "c.txt"),
    # Trash and history
    ("delete_path", {"path": D + "/c.txt"}, "to the trash as"),
    ("list_trash", {}, D + "/c.txt"),
    ("restore_from_trash", {"id": "{trash_id}"}, None),
    ("read_text_file", {"path": D + "/c.txt"}, "second line"),
    ("delete_path", {"path": D + "/sub"}, "Deleted"),
    ("delete_path", {"path": D, "recursive": True, "permanent": True}, "Deleted"),
    ("list_directory", {"path": D}, "No such file"),
    ("show_history", {"count": 8}, "delete_path"),
    # Packages and apps
    ("find_pkg_files", {}, ".pkg"),
    ("list_installed_apps", {}, "CLDE00001"),
    ("list_screenshots", {"count": 3}, None),
    ("view_image", {"path": "latest"}, None),
    ("show_notification", {"text": "Claude for PS4 tool check"}, None),
    # Storage manager
    ("storage_report", {}, "Internal storage"),
    ("delete_paths", {"paths": ["/user/app/CUSA99999"]}, "protected"),  # made-up title: must be refused
    # Save data
    ("list_game_saves", {}, "signed in now"),
    ("backup_saves", {"title_id": "CUSA00265", "note": "tool check"}, "backed up"),
    ("list_save_backups", {"title_id": "CUSA00265"}, "tool check"),
    ("restore_saves", {"backup": "/etc"}, "not a save backup"),
    # Install from PC and app update (this checker stands in for the bridge)
    ("list_pc_packages", {}, "Packages on the PC"),
    ("install_from_pc", {"names": ["nope.pkg"]}, "No package named"),
    ("install_status", {}, None),
    ("check_app_update", {}, "Up to date"),
    # GoldHEN
    ("list_cheats", {"title_id": "CUSA00002"}, "Godmode"),
    ("list_game_patches", {"title_id": "CUSA00003"}, "60 FPS"),
    ("list_plugins", {}, "afr.prx"),
    ("set_plugin", {"plugin": "no-such-plugin.prx", "enabled": True}, "There is no plugin"),
    ("goldhen_settings", {}, "[Cheat]"),
    ("set_goldhen_setting", {"section": "Cheat", "key": "NoSuchKey", "value": "1"}, "No such setting"),
    # Health
    ("health_report", {}, "Temperature now: CPU"),
    ("temperature_history", {"minutes": 30}, "CPU temperature"),
    ("set_fan_profile", {"profile": "bad"}, "profile must be"),
    # Memory and permissions
    ("remember", {"text": "The tool check ran here"}, "Remembered"),
    ("get_preferences", {}, "The tool check ran here"),
    ("forget", {"text": "The tool check ran"}, "Forgot"),
    ("set_permission_mode", {"mode": "trusted"}, "cannot loosen"),
    # Home screen folders (read-only here)
    ("list_home_folders", {}, None),
    # Risky tools: only their checks run, nothing is installed or changed.
    ("install_pkg", {"path": "/data/claude/boot.log"}, "not a PS4 package"),
    ("uninstall_app", {"title_id": "bad"}, "title_id must look like"),
    ("launch_app", {"title_id": "bad"}, "title_id must look like"),
    ("set_fan_threshold", {"celsius": 99}, "between 45 and 80"),
    ("send_payload", {"path": "/data/claude/no-such-payload.bin"}, "No such file"),
]

LOG = Path(__file__).with_name("tool_check.log")
seen = {}   # values picked up from earlier results, e.g. {trash_id}
progress = {"base": 0, "next": 0}   # the app stops after 25 rounds; the next message continues


def fill(inp):
    return json.loads(re.sub(r"\{(\w+)\}", lambda m: seen.get(m.group(1), m.group(0)), json.dumps(inp)))


def log(line):
    print(line)
    with LOG.open("a") as f:
        f.write(line + "\n")


def result_text(block):
    parts, images = [], 0
    for b in block.get("content") or []:
        if b.get("type") == "text":
            parts.append(b.get("text", ""))
        elif b.get("type") == "image":
            images += 1
    t = "\n".join(parts)
    return t + (f" [+{images} image, {len((block['content'][-1].get('source') or {}).get('data', ''))} b64 chars]" if images else "")


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def sse(self, ev):
        data = f"event: {ev['type']}\ndata: {json.dumps(ev)}\n\n".encode()
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")

    def reply(self, blocks, stop):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        self.sse({"type": "message_start", "message": {"id": "m", "type": "message", "role": "assistant",
                                                       "content": [], "model": "tool-check"}})
        for i, b in enumerate(blocks):
            if b["type"] == "text":
                self.sse({"type": "content_block_start", "index": i, "content_block": {"type": "text", "text": ""}})
                self.sse({"type": "content_block_delta", "index": i, "delta": {"type": "text_delta", "text": b["text"]}})
            else:
                self.sse({"type": "content_block_start", "index": i, "content_block": dict(b, input={})})
                self.sse({"type": "content_block_delta", "index": i,
                          "delta": {"type": "input_json_delta", "partial_json": json.dumps(b["input"])}})
            self.sse({"type": "content_block_stop", "index": i})
        self.sse({"type": "message_delta", "delta": {"stop_reason": stop}})
        self.sse({"type": "message_stop"})
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def do_GET(self):
        if self.path.startswith("/v1/packages"):
            obj = {"dir": "/tool-check", "packages": [{"name": "games/Demo.pkg", "size": 123456789, "sent": 0,
                   "content_id": "UP0000-CUSA09999_00-TOOLCHECKTOOLCHE", "type": "PS4GD", "title": "Tool check demo",
                   "version": "01.00", "url": "/pkg/none/games/Demo.pkg"}]}
        elif self.path.startswith("/v1/app-update"):
            obj = {"version": "01.25", "size": 1, "content_id": "IV0000-CLDE00001_00-CLAUDEFORPS40000",
                   "type": "PS4GD", "file": "x.pkg", "url": "/pkg/none/__app__.pkg"}
        else:
            obj = {"name": "Tool check", "email": ""}
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if not secrets.compare_digest(self.headers.get("x-bridge-token", ""), TOKEN):
            self.send_response(401)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = json.loads(self.rfile.read(int(self.headers["content-length"])))
        msgs = body["messages"]
        # Rounds already answered in this turn = tool_result messages after the last plain user text.
        last_user = max(i for i, m in enumerate(msgs) if m["role"] == "user" and not (
            isinstance(m["content"], list) and m["content"] and m["content"][0].get("type") == "tool_result"))
        done = [m for m in msgs[last_user + 1:] if m["role"] == "user"]
        if not done:
            progress["base"] = progress["next"]   # a new message: carry on from where it stopped
            if progress["base"] == 0:
                log(f"=== tool check started {time.strftime('%H:%M:%S')}, tools offered by the PS4: {len(body.get('tools') or [])}")
            else:
                log(f"=== continuing at step {progress['base'] + 1}")
        else:
            idx = progress["base"] + len(done) - 1
            name, inp, expect = STEPS[idx]
            res = done[-1]["content"][0]
            text = result_text(res)
            err = bool(res.get("is_error"))
            ok = (expect in text) if expect else not err
            m = re.search(r"trash as (\d{8}-\d{6}-\d{2})", text)
            if m:
                seen["trash_id"] = m.group(1)
            log(f"[{'PASS' if ok else 'FAIL'}] {idx + 1:2d} {name} {json.dumps(fill(inp))}\n      {'ERROR: ' if err else ''}{text.strip()[:600]}")
            progress["next"] = idx + 1
        step = progress["base"] + len(done)
        if step < len(STEPS):
            name, inp, _ = STEPS[step]
            self.reply([{"type": "tool_use", "id": f"toolu_check_{step}", "name": name, "input": fill(inp)}], "tool_use")
        else:
            log("=== tool check finished")
            if "--exit" in sys.argv:
                import threading
                threading.Timer(2.0, SRV.shutdown).start()
            self.reply([{"type": "text", "text": "Tool check finished. Results are on the PC in tests/tool_check.log."}], "end_turn")


if __name__ == "__main__":
    if not any(a.startswith("--from=") for a in sys.argv):
        LOG.write_text("")
    for a in sys.argv[1:]:
        if a.startswith("--from="):   # skip steps that already passed
            progress["next"] = int(a.split("=", 1)[1]) - 1
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if args else 8787
    srv = SRV = ThreadingHTTPServer(("0.0.0.0", port), H)
    print(f"Tool check bridge on port {port}. Send any message in Claude for PS4; press Allow on confirm boxes.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
