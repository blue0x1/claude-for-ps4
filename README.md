<!-- Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved. -->
<h1 align="center">Claude for PS4</h1>

<p align="center">
  <img src="sce_sys/icon0.png" width="180" alt="Claude for PS4 icon"/>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-01.26-orange?style=flat-square" alt="Version"/>
  <img src="https://img.shields.io/badge/PlayStation%204-GoldHEN-003087?style=flat-square&logo=playstation&logoColor=white" alt="Platform"/>
  <img src="https://img.shields.io/badge/firmware-5.05%2B-lightgrey?style=flat-square" alt="Firmware"/>
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square" alt="License"/>
</p>

<p align="center">
  Unofficial Claude app for the PS4. Not affiliated with Anthropic or Sony.<br/>
  Made by <strong>Chokri Hammedi</strong> (<a href="https://github.com/blue0x1">blue0x1</a>)
</p>


## What it does

Chat with Claude on your PS4. Claude can also control the console: manage files, install games from your PC, back up saves, control the fan, read GoldHEN plugins and cheats, and more. Everything that changes something asks for confirmation first.

You can also chat from your phone. The PS4 hosts a web page on your home network.


## Install

1. Install `IV0000-CLDE00001_00-CLAUDEFORPS40000.pkg` via GoldHEN Package Installer.
2. Launch **Claude for PS4**. It shows a QR code and PIN on first start.
3. Scan with your phone, sign in with the PIN, and pick your sign-in method.


## Sign in

**API key**: get a key from [console.anthropic.com](https://console.anthropic.com). Pay per use, no PC needed.

**PC bridge**: uses your Claude account. Requires a PC on the same network running:
```sh
claude          # sign in once with /login
python3 tools/claude-bridge.py
```
The bridge prints its address. Open `http://127.0.0.1:8787` on the PC to pair, or enter the address and PIN in the app.


## What Claude can do

| Feature | How to ask |
|---|---|
| Chat | Ask anything, get answers on the TV or your phone |
| Manage files | "What's using my storage?" / "delete the leftover files" |
| Save backups | "Back up my saves" / "restore from yesterday" |
| Install from PC | Put .pkg files in `~/PS4/pkg`, say "install the new games" |
| Install .pkg | "Install the pkg on my USB" |
| Uninstall apps | "Remove this game" (save data stays) |
| GoldHEN | "Turn on the 60 FPS plugin for Bloodborne" / "list cheats for this game" |
| Fan & health | "Is my PS4 running hot?" / "use quiet fan mode" |
| Screenshots | "What's on my screen?" (take a screenshot with SHARE first) |
| Home folders | "Make a Racing folder with GTA and NFS" |
| Memory | "Remember that USB 0 is for backups" |
| Permission modes | Full, Trusted, Read-only, Kid (set from the phone menu) |
| Update check | "Is there an app update?" (bridge only) |


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


## Build

Needs clang, ld.lld and the [OpenOrbis toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) v0.5.2.

```sh
make OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
```


## License

MIT. See [LICENSE](LICENSE)

Third-party: DejaVu Sans font (`assets/fonts/DejaVu-LICENSE.txt`), stb_truetype (public domain / MIT), QR codes by Project Nayuki (MIT), GoldHEN jailbreak call based on GoldHEN Plugins SDK (MIT).
