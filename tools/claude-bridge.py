#!/usr/bin/env python3
# Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
"""Claude for PS4 PC bridge.

Lets the PS4 app chat through your Claude account instead of an API key.
It runs the official Claude Code CLI (`claude`) on this PC, signed in with
your account (`claude` then /login), and streams the replies to the PS4 over
your home network.

Usage:
    python3 claude-bridge.py [--port 8787] [--model opus]

It prints the address and a pairing code to enter in the PS4 phone setup.
Chat only: Claude Code's own tools are disabled, and the PS4 console tools
need API key mode.
"""

import argparse
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

STATE_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "claude-ps4-bridge"
WORK_DIR = STATE_DIR / "work"
TOKEN_FILE = STATE_DIR / "token"
SESSIONS_FILE = STATE_DIR / "sessions.json"
MAX_BODY = 4 * 1024 * 1024
EFFORTS = {"low", "medium", "high", "xhigh", "max"}
MODEL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._\[\]-]{0,62}$")

sessions_lock = threading.Lock()


def load_sessions():
    try:
        return json.loads(SESSIONS_FILE.read_text())
    except (OSError, ValueError):
        return {}


def save_sessions(data):
    tmp = SESSIONS_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(data))
    tmp.replace(SESSIONS_FILE)


def get_token(regenerate):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    if TOKEN_FILE.exists() and not regenerate:
        return TOKEN_FILE.read_text().strip()
    token = secrets.token_hex(4)  # 8 hex chars, easy to type on a phone
    TOKEN_FILE.write_text(token)
    TOKEN_FILE.chmod(0o600)
    return token


_account = None
_account_lock = threading.Lock()


def _find(obj, keys):
    """First non-empty string value for any of keys, searching nested dicts."""
    if isinstance(obj, dict):
        for k in keys:
            v = obj.get(k)
            if isinstance(v, str) and v.strip():
                return v.strip()
        for v in obj.values():
            found = _find(v, keys)
            if found:
                return found
    return ""


def account_info(claude_bin):
    """Name and email of the Claude account the CLI is signed in with.

    Uses `claude auth status`, then the CLI's own config file. Cached."""
    global _account
    with _account_lock:
        if _account is not None:
            return _account
        info = {}
        try:
            out = subprocess.run([claude_bin, "auth", "status", "--json"], capture_output=True, text=True,
                                 timeout=20).stdout
            info = json.loads(out or "{}")
        except (OSError, ValueError, subprocess.SubprocessError):
            info = {}
        name = _find(info, ("displayName", "name", "userName"))
        email = _find(info, ("email", "emailAddress", "userEmail"))
        if not (name or email):
            try:
                cfg = json.loads((Path.home() / ".claude.json").read_text())
                acct = cfg.get("oauthAccount") or {}
                name = _find(acct, ("displayName", "name"))
                email = _find(acct, ("emailAddress", "email"))
            except (OSError, ValueError):
                pass
        _account = {"name": name[:80], "email": email[:80]}
        return _account


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.1.1", 9))  # no packet is sent
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def content_text(content):
    """Text of a Messages API content field (string or block list)."""
    if isinstance(content, str):
        return content
    parts = []
    for block in content or []:
        if isinstance(block, dict) and block.get("type") == "text":
            parts.append(block.get("text", ""))
    return "\n".join(parts)


def build_prompt(messages, resumed):
    """The new user message, or the whole transcript if the CLI session is gone."""
    if not messages:
        return ""
    last = content_text(messages[-1].get("content"))
    if resumed or len(messages) == 1:
        return last
    lines = ["(Earlier conversation from the PS4, for context.)"]
    for m in messages[:-1]:
        who = "User" if m.get("role") == "user" else "Assistant"
        lines.append(f"{who}: {content_text(m.get('content'))}")
    lines.append("")
    lines.append(last)
    return "\n".join(lines)


def sse(event):
    return f"event: {event['type']}\ndata: {json.dumps(event)}\n\n".encode()


class Handler(BaseHTTPRequestHandler):
    server_version = "ClaudePS4Bridge/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[bridge] " + (fmt % args) + "\n")

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def error(self, code, kind, message):
        self.send_json(code, {"type": "error", "error": {"type": kind, "message": message}})

    def do_GET(self):
        if self.path == "/":
            self.send_json(200, {"ok": True, "service": "claude-ps4-bridge"})
        elif self.path.rstrip("/") == "/v1/whoami":
            # The profile name shown on the PS4 (needs the pairing code).
            if not secrets.compare_digest(self.headers.get("x-bridge-token", ""), self.server.cfg.token):
                return self.error(401, "authentication_error", "wrong pairing code")
            self.send_json(200, account_info(self.server.cfg.claude))
        else:
            self.error(404, "not_found_error", "not found")

    def chunk(self, data):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        self.wfile.flush()

    def do_POST(self):
        cfg = self.server.cfg
        if self.path.rstrip("/") != "/v1/messages":
            return self.error(404, "not_found_error", "not found")
        if not secrets.compare_digest(self.headers.get("x-bridge-token", ""), cfg.token):
            return self.error(401, "authentication_error", "Wrong pairing code. Check bridge_token on the PS4.")
        length = int(self.headers.get("content-length") or 0)
        if length <= 0 or length > MAX_BODY:
            return self.error(400, "invalid_request_error", "missing or oversized body")
        try:
            body = json.loads(self.rfile.read(length))
            messages = body["messages"]
        except (ValueError, KeyError, TypeError):
            return self.error(400, "invalid_request_error", "body is not a Messages API request")

        conv = self.headers.get("x-conversation-id", "")[:80]
        with sessions_lock:
            known = load_sessions()
            session_id = known.get(conv) if conv else None
        prompt = build_prompt(messages, resumed=bool(session_id))
        if not prompt.strip():
            return self.error(400, "invalid_request_error", "empty message")

        cmd = [cfg.claude, "-p", "--output-format", "stream-json", "--verbose",
               "--include-partial-messages", "--tools", ""]
        # Model and effort picked on the PS4 win; "default" keeps --model
        # from this script, or Claude Code's own choice.
        model = body.get("model")
        if not (isinstance(model, str) and model != "default" and MODEL_RE.match(model)):
            model = cfg.model
        if model:
            cmd += ["--model", model]
        effort = (body.get("output_config") or {}).get("effort") if isinstance(body.get("output_config"), dict) else None
        if effort in EFFORTS and cfg.effort_flag:
            cmd += ["--effort", effort]
        if session_id:
            cmd += ["--resume", session_id]
        system = body.get("system")
        if isinstance(system, str) and system.strip():
            cmd += ["--append-system-prompt", system]

        try:
            proc = subprocess.Popen(cmd, cwd=WORK_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, bufsize=1)
        except OSError as e:
            return self.error(500, "api_error", f"Could not start Claude Code: {e}")
        proc.stdin.write(prompt)
        proc.stdin.close()

        started = False
        new_session = None
        text_index_open = False
        last_msg_id = None
        stop_reason = "end_turn"
        try:
            for line in proc.stdout:
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue
                if ev.get("session_id"):
                    new_session = ev["session_id"]
                if ev.get("type") == "result":
                    if ev.get("is_error") and not started:
                        msg = ev.get("result") or "Claude Code returned an error."
                        return self.error(502, "api_error", str(msg)[:500])
                    stop_reason = ev.get("stop_reason") or stop_reason
                    continue
                if ev.get("type") != "stream_event" or ev.get("parent_tool_use_id"):
                    continue
                inner = ev.get("event", {})
                if inner.get("type") != "content_block_delta":
                    continue
                delta = inner.get("delta", {})
                if delta.get("type") != "text_delta":
                    continue
                if not started:
                    started = True
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Cache-Control", "no-cache")
                    self.send_header("Transfer-Encoding", "chunked")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.chunk(sse({"type": "message_start", "message": {
                        "id": "msg_bridge", "type": "message", "role": "assistant", "content": [],
                        "model": "claude-code-bridge"}}))
                    self.chunk(sse({"type": "content_block_start", "index": 0,
                                    "content_block": {"type": "text", "text": ""}}))
                    text_index_open = True
                text = delta.get("text", "")
                # Separate the text of consecutive assistant messages.
                msg_id = ev.get("api_message_id")
                if last_msg_id and msg_id and msg_id != last_msg_id:
                    text = "\n\n" + text
                last_msg_id = msg_id or last_msg_id
                self.chunk(sse({"type": "content_block_delta", "index": 0,
                                "delta": {"type": "text_delta", "text": text}}))
            proc.wait(timeout=30)
            if not started:
                err = proc.stderr.read().strip()
                hint = " Run `claude` once on this PC and sign in with /login." if "login" in err.lower() or "auth" in err.lower() else ""
                return self.error(502, "api_error", (err[-400:] or "Claude Code produced no reply.") + hint)
            if text_index_open:
                self.chunk(sse({"type": "content_block_stop", "index": 0}))
            self.chunk(sse({"type": "message_delta", "delta": {"stop_reason": stop_reason}}))
            self.chunk(sse({"type": "message_stop"}))
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            self.log_message("PS4 disconnected, stopping Claude Code")
        finally:
            if proc.poll() is None:
                proc.kill()
            if new_session and conv:
                with sessions_lock:
                    known = load_sessions()
                    known[conv] = new_session
                    # Keep the file small.
                    if len(known) > 200:
                        known = dict(list(known.items())[-200:])
                    save_sessions(known)


def main():
    ap = argparse.ArgumentParser(description="Claude for PS4 PC bridge (uses your Claude Code login)")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default all)")
    ap.add_argument("--model", default="", help="model used when the PS4 picks 'default' (alias or id, e.g. opus)")
    ap.add_argument("--claude", default="claude", help="path to the claude CLI")
    ap.add_argument("--new-code", action="store_true", help="make a new pairing code")
    cfg = ap.parse_args()

    found = shutil.which(cfg.claude)
    if not found:
        sys.exit("Claude Code CLI not found. Install it (npm install -g @anthropic-ai/claude-code), "
                 "run `claude`, and sign in with /login.")
    cfg.claude = found
    try:
        help_text = subprocess.run([found, "--help"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        help_text = ""
    cfg.effort_flag = "--effort" in help_text
    if not cfg.effort_flag:
        print("Note: this Claude Code version has no --effort option; effort picked on the PS4 is ignored. "
              "Update Claude Code to use it.")
    cfg.token = get_token(cfg.new_code)
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    server = ThreadingHTTPServer((cfg.bind, cfg.port), Handler)
    server.cfg = cfg
    ip = lan_ip()
    print("Claude for PS4 bridge is running.")
    print(f"  Bridge address: http://{ip}:{cfg.port}")
    print(f"  Pairing code:   {cfg.token}")
    print("On the PS4 press Options, open the page on your phone, pick PC bridge and enter these.")
    print("Stop with Ctrl+C.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
