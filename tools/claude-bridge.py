#!/usr/bin/env python3
# Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
"""Claude for PS4 PC bridge.

Lets the PS4 app use your Claude account instead of an API key. It runs the
official Claude Code CLI (`claude`) on this PC, signed in with your account
(`claude` then /login), and streams the replies to the PS4 over your home
network.

Console tools: when the PS4 sends its tool list, the bridge gives Claude Code
those tools through a small MCP server (this same script, run with
--mcp-child). When Claude calls one, the bridge hands the call to the PS4 as a
normal tool_use reply and keeps Claude Code waiting. The PS4 runs the tool
(asking you first for anything risky) and sends the result back, and Claude
Code carries on. Claude Code's own tools stay off, so nothing runs on this PC.

Usage:
    python3 claude-bridge.py [--port 8787] [--model opus]

It prints the address and a pairing code to enter in the PS4 phone setup.
"""

import argparse
import collections
import ipaddress
import json
import os
import queue
import re
import secrets
import shutil
import socket
import socketserver
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote

STATE_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "claude-ps4-bridge"
WORK_DIR = STATE_DIR / "work"
TOKEN_FILE = STATE_DIR / "token"
SESSIONS_FILE = STATE_DIR / "sessions.json"
MAX_BODY = 16 * 1024 * 1024
EFFORTS = {"low", "medium", "high", "xhigh", "max"}
MODEL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._\[\]-]{0,62}$")
TOOL_NAME_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
MCP_SERVER = "ps4"
MCP_PREFIX = f"mcp__{MCP_SERVER}__"
PARK_TIMEOUT = 20 * 60        # a run waiting for a PS4 tool result is killed after this
TOOL_BATCH_WINDOW = 0.4       # wait this long for parallel tool calls to arrive together

BRIDGE_SYSTEM = (
    "You are running inside the 'Claude for PS4' app through a bridge. The tools named "
    f"{MCP_PREFIX}* run on the user's PlayStation 4 console (not on a PC): system info and health, "
    "files (deletes go to a trash that can be restored), storage cleanup, save data backups, "
    "installing packages from USB or straight from this PC, apps, home screen folders, GoldHEN "
    "cheats/patches/plugins/settings, fan profiles, screenshots, payloads, remembered preferences "
    "and checking for app updates. Use them whenever the user asks about or wants something done on the "
    "console, instead of only explaining how. Risky actions show a confirm box on the TV or "
    "phone; if the user denies one, accept that. If a tool says a permission mode blocked it, "
    "tell the user how to change it rather than trying another way."
)

sessions_lock = threading.Lock()


def load_sessions():
    try:
        return json.loads(SESSIONS_FILE.read_text())
    except (OSError, ValueError):
        return {}


def save_session(conv, session_id):
    if not (conv and session_id):
        return
    with sessions_lock:
        known = load_sessions()
        known.pop(conv, None)
        known[conv] = session_id
        if len(known) > 200:  # keep the file small
            known = dict(list(known.items())[-200:])
        tmp = SESSIONS_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(known))
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


def block_text(block):
    """Readable text of one content block (text, tool_use or tool_result)."""
    if not isinstance(block, dict):
        return ""
    t = block.get("type")
    if t == "text":
        return block.get("text", "")
    if t == "tool_use":
        return f"[called PS4 tool {block.get('name')} with {json.dumps(block.get('input', {}))}]"
    if t == "tool_result":
        inner = block.get("content")
        text = inner if isinstance(inner, str) else "\n".join(
            b.get("text", "") for b in inner or [] if isinstance(b, dict) and b.get("type") == "text")
        return f"[PS4 tool result{' (error)' if block.get('is_error') else ''}: {text}]"
    return ""


def content_text(content):
    """Text of a Messages API content field (string or block list)."""
    if isinstance(content, str):
        return content
    return "\n".join(filter(None, (block_text(b) for b in content or [])))


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


def tool_results(message):
    """{tool_use_id: block} if message is a user message made of tool results."""
    if not isinstance(message, dict) or message.get("role") != "user":
        return {}
    content = message.get("content")
    if not isinstance(content, list):
        return {}
    out = {}
    for b in content:
        if isinstance(b, dict) and b.get("type") == "tool_result" and isinstance(b.get("tool_use_id"), str):
            out[b["tool_use_id"]] = b
    return out


def to_mcp_result(block):
    """PS4 tool_result block -> MCP tools/call result."""
    content = []
    inner = block.get("content")
    if isinstance(inner, str):
        inner = [{"type": "text", "text": inner}]
    for b in inner or []:
        if not isinstance(b, dict):
            continue
        if b.get("type") == "text":
            content.append({"type": "text", "text": b.get("text", "")})
        elif b.get("type") == "image":
            src = b.get("source") or {}
            if src.get("type") == "base64" and src.get("data"):
                content.append({"type": "image", "data": src["data"],
                                "mimeType": src.get("media_type", "image/jpeg")})
    if not content:
        content.append({"type": "text", "text": "(no output)"})
    return {"content": content, "isError": bool(block.get("is_error"))}


def mcp_error(text):
    return {"content": [{"type": "text", "text": text}], "isError": True}


def sse(event):
    return f"event: {event['type']}\ndata: {json.dumps(event)}\n\n".encode()


# ---- Runs: one Claude Code process per PS4 turn ---------------------------

class ToolCall:
    def __init__(self, name, arguments):
        self.id = "toolu_bridge_" + secrets.token_hex(10)
        self.name = name
        self.arguments = arguments if isinstance(arguments, dict) else {}
        self.result = None
        self.done = threading.Event()

    def finish(self, result):
        if not self.done.is_set():
            self.result = result
            self.done.set()


class Run:
    """A Claude Code process. It may outlive one HTTP request: when Claude
    calls a PS4 tool the request ends with tool_use and the run is parked
    until the PS4 posts the result."""

    def __init__(self, conv, tools):
        self.id = secrets.token_hex(8)
        self.conv = conv
        self.tools = tools              # Messages API tool definitions from the PS4
        self.events = queue.Queue()     # ("line", dict) | ("tool", ToolCall) | ("eof", None)
        self.pushback = collections.deque()
        self.pending = {}               # tool_use_id -> ToolCall handed to the PS4
        self.proc = None
        self.session_id = None
        self.parked_at = None
        self.lock = threading.Lock()    # one HTTP request drives the run at a time
        self.dead = False

    def next_event(self, timeout=None):
        if self.pushback:
            return self.pushback.popleft()
        return self.events.get(timeout=timeout) if timeout is not None else self.events.get()

    def kill(self):
        self.dead = True
        for call in list(self.pending.values()):
            call.finish(mcp_error("The PS4 turn was stopped."))
        self.pending.clear()
        if self.proc and self.proc.poll() is None:
            self.proc.kill()


runs_lock = threading.Lock()
runs_by_id = {}
runs_by_conv = {}


def drop_run(run):
    run.kill()
    with runs_lock:
        runs_by_id.pop(run.id, None)
        if runs_by_conv.get(run.conv) is run:
            runs_by_conv.pop(run.conv, None)


def reaper():
    while True:
        time.sleep(30)
        with runs_lock:
            stale = [r for r in runs_by_id.values()
                     if r.parked_at and time.time() - r.parked_at > PARK_TIMEOUT]
        for r in stale:
            sys.stderr.write(f"[bridge] no tool result from the PS4 for {PARK_TIMEOUT // 60} min, ending run\n")
            drop_run(r)


# ---- Local IPC for the MCP child ------------------------------------------

class IPCHandler(socketserver.StreamRequestHandler):
    """One JSON line in, one JSON line out. Only for the MCP child, on 127.0.0.1."""

    def handle(self):
        try:
            req = json.loads(self.rfile.readline())
        except ValueError:
            return
        if not secrets.compare_digest(str(req.get("secret", "")), self.server.secret):
            return
        with runs_lock:
            run = runs_by_id.get(str(req.get("run", "")))
        if req.get("op") == "list":
            reply = {"tools": run.tools if run else []}
        elif req.get("op") == "call":
            if not run or run.dead:
                reply = mcp_error("The PS4 is not connected to this turn any more.")
            else:
                call = ToolCall(str(req.get("name", "")), req.get("arguments"))
                run.events.put(("tool", call))
                # Confirm boxes wait for the user, so allow a long time.
                if not call.done.wait(PARK_TIMEOUT + 60):
                    call.finish(mcp_error("No answer from the PS4."))
                reply = call.result
        else:
            reply = {}
        try:
            self.wfile.write((json.dumps(reply) + "\n").encode())
        except OSError:
            pass


class IPCServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


# ---- Wrong pairing codes ---------------------------------------------------

auth_lock = threading.Lock()
auth_fails = {}  # ip -> [wrong codes in a row, rounds, blocked until]


def is_local(ip):
    """Home network addresses only (and this PC), never the internet."""
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return False
    if a.version == 6 and a.ipv4_mapped:
        a = a.ipv4_mapped
    return a.is_private or a.is_loopback or a.is_link_local


def auth_wait(ip):
    """Seconds this address must still wait, 0 if it may try a code."""
    with auth_lock:
        e = auth_fails.get(ip)
        return max(0, int(e[2] - time.time()) + 1) if e and e[2] > time.time() else 0


def auth_result(ip, ok):
    """5 wrong codes in a row block the address: 30 s, doubling to 15 min."""
    with auth_lock:
        if ok:
            auth_fails.pop(ip, None)
            return
        e = auth_fails.setdefault(ip, [0, 0, 0.0])
        e[0] += 1
        if e[0] >= 5:
            e[2] = time.time() + min(900, 30 << min(e[1], 5))
            e[0] = 0
            e[1] += 1
            sys.stderr.write(f"[bridge] {ip}: 5 wrong pairing codes, blocked for {int(e[2] - time.time())} s\n")
        if len(auth_fails) > 1000:  # don't grow without bound
            auth_fails.clear()


# ---- packages on this PC (install from PC) ---------------------------------

PKG_TYPES = {0x1A: "PS4GD", 0x1B: "PS4AC", 0x1C: "PS4AL"}
pkg_cache = {}          # path -> (mtime, size, info)
pkg_sent = {}           # relative name -> [bytes sent, size, last time]
pkg_lock = threading.Lock()


def sfo_strings(data):
    """Key -> value from a param.sfo blob."""
    out = {}
    if len(data) < 20 or data[:4] != b"\0PSF":
        return out
    key_off, data_off, count = (int.from_bytes(data[i:i + 4], "little") for i in (8, 12, 16))
    for i in range(min(count, 64)):
        e = 20 + i * 16
        if e + 16 > len(data):
            break
        k = int.from_bytes(data[e:e + 2], "little")
        fmt = int.from_bytes(data[e + 2:e + 4], "little")
        ln = int.from_bytes(data[e + 4:e + 8], "little")
        off = int.from_bytes(data[e + 12:e + 16], "little")
        key = data[key_off + k:data.index(b"\0", key_off + k)].decode("ascii", "replace")
        raw = data[data_off + off:data_off + off + ln]
        out[key] = raw.rstrip(b"\0").decode("utf-8", "replace") if fmt in (0x0204, 0x0004) else int.from_bytes(raw[:4], "little")
    return out


def pkg_info(path):
    """content_id, type and title of a PS4 .pkg, or None."""
    try:
        st = path.stat()
        hit = pkg_cache.get(str(path))
        if hit and hit[0] == st.st_mtime and hit[1] == st.st_size:
            return hit[2]
        with open(path, "rb") as f:
            h = f.read(0x100)
            if len(h) < 0x80 or h[:4] != b"\x7fCNT":
                return None
            cid = h[0x40:0x64].decode("ascii")
            t = int.from_bytes(h[0x74:0x78], "big")
            flags = int.from_bytes(h[0x78:0x7C], "big")
            kind = "PS4PATCH" if flags & (0x100000 | 0x400000 | 0x800000) else PKG_TYPES.get(t, "PS4GD")
            count = int.from_bytes(h[0x10:0x14], "big")
            table = int.from_bytes(h[0x18:0x1C], "big")
            info = {"content_id": cid, "type": kind, "title": "", "version": ""}
            f.seek(table)
            ents = f.read(32 * min(count, 4096))
            for i in range(0, len(ents), 32):
                if int.from_bytes(ents[i:i + 4], "big") == 0x1000:  # param.sfo, not encrypted
                    off = int.from_bytes(ents[i + 16:i + 20], "big")
                    size = int.from_bytes(ents[i + 20:i + 24], "big")
                    if size < 1 << 20:
                        f.seek(off)
                        sfo = sfo_strings(f.read(size))
                        info["title"] = str(sfo.get("TITLE", ""))
                        info["version"] = str(sfo.get("APP_VER", "") or sfo.get("VERSION", ""))
                    break
        pkg_cache[str(path)] = (st.st_mtime, st.st_size, info)
        return info
    except (OSError, ValueError, UnicodeDecodeError):
        return None


APP_CONTENT_ID = "IV0000-CLDE00001_00-CLAUDEFORPS40000"


def find_app_pkg(cfg):
    """Newest Claude for PS4 package on this PC: --app-pkg, next to this
    script, or in the package folder."""
    cands = []
    if cfg.app_pkg:
        cands.append(Path(cfg.app_pkg).expanduser())
    here = Path(__file__).resolve().parent
    for d in (here, here.parent, cfg.pkg_dir):
        try:
            cands += sorted(d.glob("*.pkg"))
        except OSError:
            pass
    best = None
    for p in cands:
        info = pkg_info(p) if p.is_file() else None
        if info and info["content_id"] == APP_CONTENT_ID and info["version"]:
            if not best or info["version"] > best[1]["version"]:
                best = (p, info)
    return best


def list_packages(root):
    out = []
    if not root.is_dir():
        return out
    for path in sorted(root.rglob("*")):
        if len(path.relative_to(root).parts) > 3 or path.suffix.lower() != ".pkg" or not path.is_file():
            continue
        info = pkg_info(path)
        if not info:
            continue
        rel = path.relative_to(root).as_posix()
        size = path.stat().st_size
        with pkg_lock:
            sent = pkg_sent.get(rel, [0, size, 0])[0]
        out.append({"name": rel, "size": size, "sent": min(sent, size), **info})
        if len(out) >= 500:
            break
    return out


# ---- PC dashboard (this PC only) and one-step pairing ------------------------

STARTED = time.time()
ps4_seen = {"ip": "", "time": 0.0, "requests": 0}

DASHBOARD_HTML = r"""<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Claude for PS4 bridge</title>
<style>
:root{--bg:#faf9f5;--card:#fff;--text:#141413;--dim:#6f6d66;--border:#e3e0d6;--accent:#d97757;--ok:#3f8f5a;--bar:#efece3}
@media (prefers-color-scheme:dark){:root{--bg:#262624;--card:#30302e;--text:#f2f0e8;--dim:#a6a49b;--border:#3e3d39;--bar:#1f1e1d}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}
.wrap{max-width:880px;margin:0 auto;padding:28px 16px 60px}
header{display:flex;align-items:center;gap:12px;margin-bottom:22px}header h1{font:500 24px/1.2 ui-serif,Georgia,serif;margin:0}
.pill{margin-left:auto;font-size:13px;padding:4px 10px;border-radius:999px;background:var(--bar);color:var(--dim)}
.pill.on{color:var(--ok)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:14px}
.card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:16px 18px;margin-bottom:14px}
.card h2{font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.04em;color:var(--dim);margin:0 0 10px}
.big{font-size:19px;font-weight:600;overflow-wrap:anywhere;min-width:0}.card{min-width:0}.dim{color:var(--dim);font-size:13px}
code{font:13px ui-monospace,Menlo,monospace;background:var(--bar);padding:2px 6px;border-radius:6px;word-break:break-all}
table{width:100%;border-collapse:collapse}td{padding:8px 4px;border-top:1px solid var(--border);vertical-align:top}
td.r{text-align:right;white-space:nowrap}.bar{height:6px;background:var(--bar);border-radius:3px;margin-top:6px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--accent);width:0}
input{font:inherit;padding:9px 11px;border:1px solid var(--border);border-radius:10px;background:var(--bg);color:var(--text);width:100%}
.row{display:flex;gap:8px;flex-wrap:wrap}.row>*{flex:1 1 140px}
button{font:inherit;padding:9px 14px;border-radius:10px;border:0;background:var(--accent);color:#fff;cursor:pointer}
button.sec{background:var(--bar);color:var(--text)}#msg{margin-top:10px;font-size:14px}
</style></head><body><div class=wrap>
<header><svg width=34 height=34 viewBox="0 0 512 512"><rect width=512 height=512 rx=128 fill="#e07a56"/><path d="M150 170h212c50 0 76 30 90 90l22 96c8 38-22 64-52 52-20-8-36-34-52-60H142c-16 26-32 52-52 60-30 12-60-14-52-52l22-96c14-60 40-90 90-90z" fill="#1c1917"/><g fill="#f4ede4"><circle cx=196 cy=268 r=24 /><circle cx=256 cy=268 r=24 /><circle cx=316 cy=268 r=24 /></g></svg>
<h1>Claude for PS4 bridge</h1><span class=pill id=ps4pill>PS4 not seen yet</span></header>
<div class=grid>
<div class=card><h2>Bridge address</h2><div class=big id=addr>...</div><div class=dim id=sec></div></div>
<div class=card><h2>Pairing code</h2><div class=big><code id=code>...</code></div><div class=dim>Only shown on this PC.</div></div>
<div class=card><h2>Claude account</h2><div class=big id=acct>...</div><div class=dim id=runs></div></div>
<div class=card><h2>App update for the PS4</h2><div class=big id=appv>none</div><div class=dim id=appf>Build or copy a Claude for PS4 .pkg next to the bridge.</div></div>
</div>
<div class=card><h2>Pair your PS4 in one step</h2>
<p class=dim>Open Claude for PS4 and press Options: the TV shows the PS4's address and a 6-digit PIN. Enter them here and the PS4 is set up to use this bridge.</p>
<div class=row><input id=ip placeholder="PS4 address, e.g. 192.168.1.100"><input id=pin placeholder="PIN from the TV" inputmode=numeric maxlength=8>
<button id=pair>Pair PS4</button><button class=sec id=find>Find my PS4</button></div><div id=msg></div></div>
<div class=card><h2>Packages to install from the PC</h2><div class=dim id=pkdir></div><table id=pk></table></div>
</div><script>
var $=function(i){return document.getElementById(i)};
function h(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')}
function sz(b){var u=['B','KB','MB','GB','TB'],i=0;while(b>=1024&&i<4){b/=1024;i++}return (i?b.toFixed(1):b)+' '+u[i]}
function api(p,b){return fetch(p,{method:b?'POST':'GET',headers:{'X-Dashboard':'1','Content-Type':'application/json'},body:b?JSON.stringify(b):undefined}).then(function(r){return r.json()})}
function load(){api('/dashboard/data').then(function(d){
$('addr').textContent=d.address;$('sec').textContent=d.https?'HTTPS on (packages on '+d.pkg_address+')':'Plain HTTP on your home network';
$('code').textContent=d.token;$('acct').textContent=d.account||'signed in';$('runs').textContent=d.runs+' active chat'+(d.runs==1?'':'s')+', up '+Math.round(d.uptime/60)+' min';
var p=$('ps4pill');if(d.ps4.ip){var ago=Math.round(d.ps4.ago);p.textContent='PS4 '+d.ps4.ip+(ago<90?' connected':' seen '+Math.round(ago/60)+' min ago');p.className=ago<90?'pill on':'pill';if(!$('ip').value)$('ip').value=d.ps4.ip}
if(d.app){$('appv').textContent='Version '+d.app.version;$('appf').textContent=d.app.file+' (ask Claude on the PS4: "is there an app update?")'}
$('pkdir').textContent='Folder: '+d.pkg_dir;var t='';d.packages.forEach(function(x){var pc=x.size?Math.min(100,100*x.sent/x.size):0;
t+='<tr><td><b>'+h(x.title||x.name)+'</b><div class=dim>'+h(x.name)+' &middot; '+h(x.type)+(x.version?' v'+h(x.version):'')+'</div>'+(x.sent?'<div class=bar><i style="width:'+pc.toFixed(1)+'%"></i></div>':'')+'</td><td class=r>'+sz(x.size)+(x.sent?'<div class=dim>'+pc.toFixed(0)+'% sent</div>':'')+'</td></tr>'});
$('pk').innerHTML=t||'<tr><td class=dim>No .pkg files yet. Put them in the folder above; the PS4 can then install them (ask Claude: "install from PC").</td></tr>'}).catch(function(){})}
$('pair').onclick=function(){$('msg').textContent='Pairing...';api('/dashboard/pair',{ip:$('ip').value.trim(),pin:$('pin').value.trim()}).then(function(r){$('msg').textContent=r.message})};
$('find').onclick=function(){$('msg').textContent='Looking on your network...';api('/dashboard/find',{}).then(function(r){$('msg').textContent=r.message;if(r.ip)$('ip').value=r.ip})};
load();setInterval(load,2000);
</script></body></html>"""


def ps4_request(ip, method, path, body=None, cookie=None, timeout=5):
    """Small HTTP client for the PS4's setup server (port 8765)."""
    import http.client
    conn = http.client.HTTPConnection(ip, 8765, timeout=timeout)
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    if cookie:
        headers["Cookie"] = "s=" + cookie
    conn.request(method, path, body=body, headers=headers)
    r = conn.getresponse()
    data = r.read(200000)
    hdrs = dict((k.lower(), v) for k, v in r.getheaders())
    conn.close()
    return r.status, hdrs, data


def pair_ps4(cfg, ip, pin):
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return False, "Enter the PS4's address, like 192.168.1.100."
    if not is_local(ip) or a.is_loopback:
        return False, "That is not an address on your home network."
    if not re.fullmatch(r"\d{4,8}", pin):
        return False, "Enter the PIN shown on the TV."
    from urllib.parse import urlencode
    try:
        st, h, _ = ps4_request(ip, "POST", "/login", urlencode({"pin": pin}))
        m = re.search(r"s=([0-9a-f]{16,64})", h.get("set-cookie", ""))
        if st == 429:
            return False, "Too many wrong PINs on the PS4. Wait a bit and use the new PIN on the TV."
        if st != 303 or not m:
            return False, "The PS4 did not accept that PIN. Use the PIN on the TV right now (it changes after each sign-in)."
        scheme = "https" if cfg.https else "http"
        url = f"{scheme}://{lan_ip()}:{cfg.port}"
        form = urlencode({"mode": "bridge", "bridge_url": url, "bridge_token": cfg.token, "bridge_model": "default",
                          "effort": ""})
        st, _, body = ps4_request(ip, "POST", "/save", form, cookie=m.group(1))
        if st != 200:
            text = re.sub(r"<[^>]+>", " ", body.decode("utf-8", "replace"))
            hint = re.search(r"(The bridge address[^.]*\.|Enter[^.]*\.|Could not[^.]*\.)", text)
            return False, "The PS4 refused the settings: " + (hint.group(1) if hint else f"HTTP {st}")
        return True, f"Paired. The PS4 now uses this bridge ({url})."
    except OSError as e:
        return False, f"Could not reach the PS4 at {ip}:8765 ({e.strerror or e}). Is Claude for PS4 open?"


def find_ps4():
    """Looks for Claude for PS4's setup page on this PC's /24 network."""
    import concurrent.futures
    me = lan_ip()
    if me.startswith("127."):
        return None
    base = me.rsplit(".", 1)[0]

    def probe(i):
        ip = f"{base}.{i}"
        if ip == me:
            return None
        try:
            with socket.create_connection((ip, 8765), timeout=0.4):
                pass
            st, _, data = ps4_request(ip, "GET", "/", timeout=2)
            return ip if b"Claude" in data else None
        except OSError:
            return None

    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        for ip in ex.map(probe, range(1, 255)):
            if ip:
                return ip
    return None


def make_cert():
    """Self-signed certificate for --https (made once with openssl)."""
    cert, key = STATE_DIR / "bridge-cert.pem", STATE_DIR / "bridge-key.pem"
    if cert.exists() and key.exists():
        return cert, key
    if not shutil.which("openssl"):
        sys.exit("--https needs the openssl command to make a certificate.")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
                    "-subj", "/CN=claude-ps4-bridge", "-keyout", str(key), "-out", str(cert)],
                   check=True, capture_output=True)
    os.chmod(key, 0o600)
    return cert, key


# ---- HTTP side (talks to the PS4) -----------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "ClaudePS4Bridge/1.1"
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

    def allowed(self):
        """Home network check, then the pairing code with a lockout. Sends
        the error itself and returns False when the request must stop."""
        ip = self.client_address[0]
        if not is_local(ip):
            self.error(403, "permission_error", "The bridge only answers on your home network.")
            return False
        wait = auth_wait(ip)
        if wait:
            self.error(429, "rate_limit_error", f"Too many wrong pairing codes. Try again in {wait} s.")
            return False
        ok = secrets.compare_digest(self.headers.get("x-bridge-token", ""), self.server.cfg.token)
        auth_result(ip, ok)
        if ok and not ipaddress.ip_address(ip).is_loopback:
            ps4_seen.update(ip=ip, time=time.time(), requests=ps4_seen["requests"] + 1)
        if not ok:
            self.error(401, "authentication_error", "Wrong pairing code. Check bridge_token on the PS4.")
        return ok

    def do_GET(self):
        if not is_local(self.client_address[0]):
            return self.error(403, "permission_error", "The bridge only answers on your home network.")
        local_pc = ipaddress.ip_address(self.client_address[0]).is_loopback
        if self.path.startswith("/pkg/"):
            return self.send_pkg()
        if self.server.pkg_only:
            return self.error(404, "not_found_error", "not found")
        if self.path in ("/", "/dashboard") and local_pc and "text/html" in self.headers.get("Accept", ""):
            body = DASHBOARD_HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/dashboard/data" and local_pc and self.headers.get("X-Dashboard") == "1":
            cfg = self.server.cfg
            with runs_lock:
                nruns = len(runs_by_id)
            self.send_json(200, {
                "address": f"{'https' if cfg.https else 'http'}://{lan_ip()}:{cfg.port}", "https": cfg.https,
                "pkg_address": f"http://{lan_ip()}:{cfg.port + 1}", "token": cfg.token,
                "account": (lambda a: a.get("name") or a.get("email"))(account_info(cfg.claude)),
                "runs": nruns, "uptime": time.time() - STARTED, "pkg_dir": str(cfg.pkg_dir),
                "packages": list_packages(cfg.pkg_dir),
                "app": (lambda a: {"version": a[1]["version"], "file": str(a[0])} if a else None)(find_app_pkg(cfg)),
                "ps4": {"ip": ps4_seen["ip"], "ago": time.time() - ps4_seen["time"] if ps4_seen["time"] else 0}})
        elif self.path == "/":
            self.send_json(200, {"ok": True, "service": "claude-ps4-bridge", "tools": True, "packages": True})
        elif self.path.rstrip("/") == "/v1/packages":
            if not self.allowed():
                return None
            cfg = self.server.cfg
            pk = list_packages(cfg.pkg_dir)
            # The PS4 installer cannot use the bridge's self-signed HTTPS, so
            # with --https packages come from the plain port next to it.
            base = ""
            if cfg.https:
                host = (self.headers.get("Host") or lan_ip()).rsplit(":", 1)[0]
                base = f"http://{host}:{cfg.port + 1}"
            for p in pk:
                p["url"] = f"{base}/pkg/{cfg.dl_token}/{quote(p['name'])}"
            self.send_json(200, {"dir": str(cfg.pkg_dir), "packages": pk})
        elif self.path.rstrip("/") == "/v1/app-update":
            if not self.allowed():
                return None
            cfg = self.server.cfg
            app = find_app_pkg(cfg)
            if not app:
                return self.send_json(200, {"version": ""})
            base = ""
            if cfg.https:
                host = (self.headers.get("Host") or lan_ip()).rsplit(":", 1)[0]
                base = f"http://{host}:{cfg.port + 1}"
            self.send_json(200, {"version": app[1]["version"], "size": app[0].stat().st_size,
                                 "content_id": app[1]["content_id"], "type": app[1]["type"],
                                 "file": app[0].name, "url": f"{base}/pkg/{cfg.dl_token}/__app__.pkg"})
        elif self.path.rstrip("/") == "/v1/whoami":
            # The profile name shown on the PS4 (needs the pairing code).
            if not self.allowed():
                return None
            self.send_json(200, account_info(self.server.cfg.claude))
        else:
            self.error(404, "not_found_error", "not found")

    def send_pkg(self):
        """Serves a .pkg to the PS4 installer, with Range support. The URL
        carries a per-run download key since the installer sends no headers."""
        cfg = self.server.cfg
        parts = self.path.split("/", 3)
        if len(parts) < 4 or not secrets.compare_digest(parts[2], cfg.dl_token):
            return self.error(404, "not_found_error", "not found")
        rel = unquote(parts[3].split("?", 1)[0])
        if rel == "__app__.pkg":
            app = find_app_pkg(cfg)
            if not app:
                return self.error(404, "not_found_error", "not found")
            path = app[0].resolve()
            root = path.parent
        else:
            root = cfg.pkg_dir.resolve()
            path = (root / rel).resolve()
            if root not in path.parents or path.suffix.lower() != ".pkg" or not path.is_file():
                return self.error(404, "not_found_error", "not found")
        size = path.stat().st_size
        start, end = 0, size - 1
        rng = self.headers.get("Range", "")
        m = re.match(r"bytes=(\d*)-(\d*)$", rng.strip())
        if m and (m.group(1) or m.group(2)):
            if m.group(1):
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
            else:
                start = max(0, size - int(m.group(2)))
            end = min(end, size - 1)
            if start > end:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        if self.command == "HEAD":
            return None
        left = end - start + 1
        name = path.relative_to(root).as_posix()
        with pkg_lock:
            e = pkg_sent.setdefault(name, [0, size, 0])
            if start == 0 and time.time() - e[2] > 600:
                e[0] = 0  # a new install of the same file
        try:
            with open(path, "rb") as f:
                f.seek(start)
                while left > 0:
                    buf = f.read(min(1 << 20, left))
                    if not buf:
                        break
                    self.wfile.write(buf)
                    left -= len(buf)
                    with pkg_lock:
                        e[0] += len(buf)
                        e[2] = time.time()
        except (BrokenPipeError, ConnectionResetError):
            pass
        return None

    def do_HEAD(self):
        if is_local(self.client_address[0]) and self.path.startswith("/pkg/"):
            return self.send_pkg()
        return self.error(404, "not_found_error", "not found")

    def chunk(self, data):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        self.wfile.flush()

    def do_POST(self):
        cfg = self.server.cfg
        if self.server.pkg_only:
            return self.error(404, "not_found_error", "not found")
        if self.path in ("/dashboard/pair", "/dashboard/find"):
            # This PC only, and a custom header other web pages cannot send.
            if not ipaddress.ip_address(self.client_address[0]).is_loopback or self.headers.get("X-Dashboard") != "1":
                return self.error(403, "permission_error", "forbidden")
            length = int(self.headers.get("content-length") or 0)
            try:
                req = json.loads(self.rfile.read(min(length, 4096)) or b"{}")
            except ValueError:
                req = {}
            if self.path == "/dashboard/find":
                ip = find_ps4()
                return self.send_json(200, {"ip": ip or "", "message": f"Found Claude for PS4 at {ip}." if ip else
                                            "Not found. Open Claude for PS4 and press Options, then try again."})
            ok, msg = pair_ps4(cfg, str(req.get("ip", ""))[:64], str(req.get("pin", ""))[:16])
            return self.send_json(200, {"ok": ok, "message": msg})
        if self.path.rstrip("/") != "/v1/messages":
            return self.error(404, "not_found_error", "not found")
        if not self.allowed():
            return None
        length = int(self.headers.get("content-length") or 0)
        if length <= 0 or length > MAX_BODY:
            return self.error(400, "invalid_request_error", "missing or oversized body")
        try:
            body = json.loads(self.rfile.read(length))
            messages = body["messages"]
            assert isinstance(messages, list) and messages
        except (ValueError, KeyError, TypeError, AssertionError):
            return self.error(400, "invalid_request_error", "body is not a Messages API request")

        conv = self.headers.get("x-conversation-id", "")[:80] or "anon-" + secrets.token_hex(6)
        results = tool_results(messages[-1])

        # A PS4 tool result for a parked run: hand it over and keep streaming.
        with runs_lock:
            run = runs_by_conv.get(conv)
        if run and results and any(tid in run.pending for tid in results):
            if not run.lock.acquire(timeout=5):
                return self.error(409, "invalid_request_error", "This chat is already busy.")
            try:
                run.parked_at = None
                for tid, call in list(run.pending.items()):
                    block = results.get(tid)
                    call.finish(to_mcp_result(block) if block else mcp_error("The PS4 sent no result."))
                run.pending.clear()
                return self.stream_run(run)
            finally:
                run.lock.release()

        # Anything else starts a new turn; a run left waiting is given up.
        if run:
            drop_run(run)
        with sessions_lock:
            session_id = load_sessions().get(conv)
        if results:
            prompt = build_prompt(messages, resumed=bool(session_id))
            prompt = "(The bridge restarted while a PS4 tool ran. Its result:)\n" + prompt
        else:
            prompt = build_prompt(messages, resumed=bool(session_id))
        if not prompt.strip():
            return self.error(400, "invalid_request_error", "empty message")

        tools = []
        for t in body.get("tools") or []:
            if isinstance(t, dict) and isinstance(t.get("name"), str) and TOOL_NAME_RE.match(t["name"]):
                tools.append({"name": t["name"], "description": str(t.get("description", ""))[:4000],
                              "input_schema": t.get("input_schema") or {"type": "object"}})

        run = Run(conv, tools)
        run.session_id = session_id
        cmd = [cfg.claude, "-p", "--output-format", "stream-json", "--verbose",
               "--include-partial-messages", "--tools", "", "--strict-mcp-config"]
        env = dict(os.environ)
        if tools:
            mcp = {"mcpServers": {MCP_SERVER: {
                "type": "stdio", "command": sys.executable,
                "args": ["-I", os.path.abspath(__file__), "--mcp-child"],
                "env": {"PS4_BRIDGE_PORT": str(cfg.ipc_port), "PS4_BRIDGE_SECRET": cfg.ipc_secret,
                        "PS4_BRIDGE_RUN": run.id}}}}
            cmd += ["--mcp-config", json.dumps(mcp), "--allowedTools", f"mcp__{MCP_SERVER}"]
            # Tool calls wait for confirm boxes on the PS4.
            env["MCP_TOOL_TIMEOUT"] = str((PARK_TIMEOUT + 60) * 1000)
        # Model and effort picked on the PS4 win; "default" keeps --model
        # from this script, or Claude Code's own choice.
        model = body.get("model")
        if not (isinstance(model, str) and model != "default" and MODEL_RE.match(model)):
            model = cfg.model
        if model:
            cmd += ["--model", model]
        oc = body.get("output_config")
        effort = oc.get("effort") if isinstance(oc, dict) else None
        if effort in EFFORTS and cfg.effort_flag:
            cmd += ["--effort", effort]
        if session_id:
            cmd += ["--resume", session_id]
        system = body.get("system")
        extra = [system.strip()] if isinstance(system, str) and system.strip() else []
        if tools:
            extra.append(BRIDGE_SYSTEM)
        if extra:
            cmd += ["--append-system-prompt", "\n\n".join(extra)]

        try:
            run.proc = subprocess.Popen(cmd, cwd=WORK_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, bufsize=1, env=env)
        except OSError as e:
            return self.error(500, "api_error", f"Could not start Claude Code: {e}")
        with runs_lock:
            runs_by_id[run.id] = run
            runs_by_conv[conv] = run
        run.proc.stdin.write(prompt)
        run.proc.stdin.close()
        threading.Thread(target=self.pump, args=(run,), daemon=True).start()
        with run.lock:
            return self.stream_run(run)

    @staticmethod
    def pump(run):
        for line in run.proc.stdout:
            try:
                run.events.put(("line", json.loads(line)))
            except ValueError:
                continue
        run.events.put(("eof", None))

    def stream_run(self, run):
        """Turns run events into one SSE reply. Ends at a tool call (run is
        parked) or when Claude Code finishes (run is dropped)."""
        st = {"started": False, "index": -1, "text_open": False, "last_msg": None}

        def start():
            if st["started"]:
                return
            st["started"] = True
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            self.chunk(sse({"type": "message_start", "message": {
                "id": "msg_bridge", "type": "message", "role": "assistant", "content": [],
                "model": "claude-code-bridge"}}))

        def text(t, msg_id):
            if not t:
                return
            start()
            if not st["text_open"]:
                st["index"] += 1
                st["text_open"] = True
                self.chunk(sse({"type": "content_block_start", "index": st["index"],
                                "content_block": {"type": "text", "text": ""}}))
            # Separate the text of consecutive assistant messages.
            if st["last_msg"] and msg_id and msg_id != st["last_msg"]:
                t = "\n\n" + t
            st["last_msg"] = msg_id or st["last_msg"]
            self.chunk(sse({"type": "content_block_delta", "index": st["index"],
                            "delta": {"type": "text_delta", "text": t}}))

        def close_text():
            if st["text_open"]:
                self.chunk(sse({"type": "content_block_stop", "index": st["index"]}))
                st["text_open"] = False

        def finish(stop_reason):
            close_text()
            self.chunk(sse({"type": "message_delta", "delta": {"stop_reason": stop_reason}}))
            self.chunk(sse({"type": "message_stop"}))
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()

        def handle_line(ev):
            """Returns ("result", event) for the final result, else None."""
            if ev.get("session_id"):
                run.session_id = ev["session_id"]
            if ev.get("type") == "result":
                return ev
            if ev.get("type") != "stream_event" or ev.get("parent_tool_use_id"):
                return None
            inner = ev.get("event", {})
            if inner.get("type") == "content_block_delta" and inner.get("delta", {}).get("type") == "text_delta":
                text(inner["delta"].get("text", ""), ev.get("api_message_id"))
            return None

        stop_reason = "end_turn"
        try:
            while True:
                kind, val = run.next_event()
                if kind == "line":
                    res = handle_line(val)
                    if res is not None:
                        if res.get("is_error") and not st["started"]:
                            drop_run(run)
                            msg = res.get("result") or "Claude Code returned an error."
                            return self.error(502, "api_error", str(msg)[:500])
                        stop_reason = res.get("stop_reason") or stop_reason
                elif kind == "tool":
                    calls = [val]
                    # Parallel calls and the text before them arrive close together.
                    deadline = time.time() + TOOL_BATCH_WINDOW
                    while True:
                        left = deadline - time.time()
                        if left <= 0:
                            break
                        try:
                            k2, v2 = run.next_event(timeout=left)
                        except queue.Empty:
                            break
                        if k2 == "tool":
                            calls.append(v2)
                        elif k2 == "line":
                            handle_line(v2)
                        else:
                            run.pushback.append((k2, v2))
                            break
                    start()
                    close_text()
                    for call in calls:
                        name = call.name[len(MCP_PREFIX):] if call.name.startswith(MCP_PREFIX) else call.name
                        st["index"] += 1
                        self.chunk(sse({"type": "content_block_start", "index": st["index"],
                                        "content_block": {"type": "tool_use", "id": call.id, "name": name,
                                                          "input": {}}}))
                        self.chunk(sse({"type": "content_block_delta", "index": st["index"],
                                        "delta": {"type": "input_json_delta",
                                                  "partial_json": json.dumps(call.arguments)}}))
                        self.chunk(sse({"type": "content_block_stop", "index": st["index"]}))
                        run.pending[call.id] = call
                    run.parked_at = time.time()
                    save_session(run.conv, run.session_id)
                    finish("tool_use")
                    return None
                else:  # eof
                    try:
                        run.proc.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        pass
                    save_session(run.conv, run.session_id)
                    drop_run(run)
                    if not st["started"]:
                        err = run.proc.stderr.read().strip()
                        hint = (" Run `claude` once on this PC and sign in with /login."
                                if "login" in err.lower() or "auth" in err.lower() else "")
                        return self.error(502, "api_error", (err[-400:] or "Claude Code produced no reply.") + hint)
                    finish(stop_reason)
                    return None
        except (BrokenPipeError, ConnectionResetError):
            self.log_message("PS4 disconnected, stopping Claude Code")
            save_session(run.conv, run.session_id)
            drop_run(run)
        return None


# ---- MCP child: stdio server started by Claude Code ------------------------

def mcp_child():
    port = int(os.environ["PS4_BRIDGE_PORT"])
    secret = os.environ["PS4_BRIDGE_SECRET"]
    run_id = os.environ["PS4_BRIDGE_RUN"]
    out_lock = threading.Lock()

    def ipc(req):
        req.update(secret=secret, run=run_id)
        with socket.create_connection(("127.0.0.1", port)) as s:
            s.sendall((json.dumps(req) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                part = s.recv(65536)
                if not part:
                    break
                data += part
        return json.loads(data or b"{}")

    def send(obj):
        with out_lock:
            sys.stdout.write(json.dumps(obj) + "\n")
            sys.stdout.flush()

    def call_tool(msg_id, params):
        try:
            result = ipc({"op": "call", "name": MCP_PREFIX + str(params.get("name", "")),
                          "arguments": params.get("arguments") or {}})
        except (OSError, ValueError) as e:
            result = mcp_error(f"Bridge not reachable: {e}")
        send({"jsonrpc": "2.0", "id": msg_id, "result": result})

    for line in sys.stdin:
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        method, msg_id = msg.get("method"), msg.get("id")
        if msg_id is None:
            continue  # notifications
        if method == "initialize":
            ver = (msg.get("params") or {}).get("protocolVersion") or "2025-06-18"
            send({"jsonrpc": "2.0", "id": msg_id, "result": {
                "protocolVersion": ver, "capabilities": {"tools": {}},
                "serverInfo": {"name": "claude-ps4", "version": "1.1"}}})
        elif method == "tools/list":
            try:
                tools = ipc({"op": "list"}).get("tools", [])
            except (OSError, ValueError):
                tools = []
            send({"jsonrpc": "2.0", "id": msg_id, "result": {"tools": [
                {"name": t["name"], "description": t.get("description", ""),
                 "inputSchema": t.get("input_schema") or {"type": "object"}} for t in tools]}})
        elif method == "tools/call":
            threading.Thread(target=call_tool, args=(msg_id, msg.get("params") or {}), daemon=True).start()
        elif method == "ping":
            send({"jsonrpc": "2.0", "id": msg_id, "result": {}})
        else:
            send({"jsonrpc": "2.0", "id": msg_id, "error": {"code": -32601, "message": "method not found"}})


def main():
    if "--mcp-child" in sys.argv:
        return mcp_child()
    ap = argparse.ArgumentParser(description="Claude for PS4 PC bridge (uses your Claude Code login)")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default all)")
    ap.add_argument("--model", default="", help="model used when the PS4 picks 'default' (alias or id, e.g. opus)")
    ap.add_argument("--claude", default="claude", help="path to the claude CLI")
    ap.add_argument("--new-code", action="store_true", help="make a new pairing code")
    ap.add_argument("--app-pkg", default="", help="Claude for PS4 .pkg the PS4 compares its version with")
    ap.add_argument("--https", action="store_true",
                    help="encrypt PS4 traffic with a self-signed certificate (packages then use the next port, plain HTTP)")
    ap.add_argument("--pkg-dir", default=str(Path.home() / "PS4" / "pkg"),
                    help="folder with .pkg files the PS4 can install from this PC (default ~/PS4/pkg)")
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
    cfg.pkg_dir = Path(cfg.pkg_dir).expanduser()
    try:
        cfg.pkg_dir.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass
    cfg.dl_token = secrets.token_urlsafe(18)
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    ipc = IPCServer(("127.0.0.1", 0), IPCHandler)
    ipc.secret = secrets.token_hex(16)
    cfg.ipc_port = ipc.server_address[1]
    cfg.ipc_secret = ipc.secret
    threading.Thread(target=ipc.serve_forever, daemon=True).start()
    threading.Thread(target=reaper, daemon=True).start()

    server = ThreadingHTTPServer((cfg.bind, cfg.port), Handler)
    server.daemon_threads = True
    server.cfg = cfg
    server.pkg_only = False
    if cfg.https:
        import ssl
        cert, key = make_cert()
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(str(cert), str(key))
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        plain = ThreadingHTTPServer((cfg.bind, cfg.port + 1), Handler)
        plain.daemon_threads = True
        plain.cfg = cfg
        plain.pkg_only = True
        threading.Thread(target=plain.serve_forever, daemon=True).start()
    ip = lan_ip()
    print("Claude for PS4 bridge is running (console tools on).")
    print(f"  Bridge address: {'https' if cfg.https else 'http'}://{ip}:{cfg.port}")
    print(f"  Dashboard:      {'https' if cfg.https else 'http'}://127.0.0.1:{cfg.port}  (open it in a browser on this PC)")
    print(f"  Pairing code:   {cfg.token}")
    print(f"  Packages:       {cfg.pkg_dir} (put .pkg files here to install them from the PS4)")
    print("Pair in one step: open the dashboard and enter the PIN the PS4 shows after pressing Options.")
    print("Or on the PS4 press Options, open the page on your phone, pick PC bridge and enter these.")
    print("Stop with Ctrl+C.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        with runs_lock:
            left = list(runs_by_id.values())
        for r in left:
            r.kill()


if __name__ == "__main__":
    main()
