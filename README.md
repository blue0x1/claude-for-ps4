<!-- Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved. -->
# Claude for PS4

<p align="center">
  <img src="sce_sys/icon0.png" width="120" alt="Claude for PS4 icon"/>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-01.26-orange?style=flat-square" alt="Version"/>
  <img src="https://img.shields.io/badge/platform-PS4%20%28GoldHEN%29-blue?style=flat-square" alt="Platform"/>
  <img src="https://img.shields.io/badge/firmware-5.05%2B-lightgrey?style=flat-square" alt="Firmware"/>
  <img src="https://img.shields.io/badge/license-All%20Rights%20Reserved-red?style=flat-square" alt="License"/>
</p>

> Unofficial Claude app for the PS4. Not affiliated with Anthropic or Sony.  
> Made by **Chokri Hammedi** ([blue0x1](https://github.com/blue0x1))

---

## What it does

Chat with Claude on your PS4. Claude can also control the console — manage files, install games from your PC, back up saves, control the fan, read GoldHEN plugins and cheats, and more. Everything that changes something asks for confirmation first.

You can also chat from your phone — the PS4 hosts a web page on your home network.

---

## Install

1. Install `IV0000-CLDE00001_00-CLAUDEFORPS40000.pkg` via GoldHEN Package Installer.
2. Launch **Claude for PS4** — it shows a QR code and PIN on first start.
3. Scan with your phone, sign in with the PIN, and pick your sign-in method.

---

## Sign in

**API key** — get a key from [console.anthropic.com](https://console.anthropic.com). Pay per use, no PC needed.

**PC bridge** — uses your Claude account. Requires a PC on the same network running:
```sh
claude          # sign in once with /login
python3 tools/claude-bridge.py
```
The bridge prints its address. Open `http://127.0.0.1:8787` on the PC to pair, or enter the address and PIN in the app.

---

## What Claude can do

| Feature | How to ask |
|---|---|
| Manage files | "What's using my storage?" / "delete the leftover files" |
| Save backups | "Back up my saves" / "restore from yesterday" |
| Install from PC | Put .pkg files in `~/PS4/pkg`, say "install the new games" |
| GoldHEN | "Turn on the 60 FPS plugin for Bloodborne" |
| Fan & health | "Is my PS4 running hot?" / "use quiet fan mode" |
| Memory | "Remember that USB 0 is for backups" |
| Update check | "Is there an app update?" (bridge only) |

---

## Controls

| Button | Action |
|---|---|
| Cross | Type a message |
| Circle | Stop reply |
| Square | New chat |
| Triangle | Reload settings |
| Options | Phone page |
| R2 | Pick model and effort |
| L2 | About |
| D-pad Left | Saved chats |

---

## Build

Needs clang, ld.lld and the [OpenOrbis toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) v0.5.2.

```sh
make OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
```

---

## Licenses

DejaVu Sans font: `assets/fonts/DejaVu-LICENSE.txt` · stb_truetype: public domain / MIT · QR codes: Project Nayuki (MIT) · GoldHEN jailbreak call: GoldHEN Plugins SDK (MIT)
