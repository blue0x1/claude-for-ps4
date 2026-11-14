# Claude for PS4

A native PS4 homebrew app (jailbroken console, GoldHEN) for chatting with Claude through the Anthropic API. Claude can also use console tools: it can read system info, browse and manage files, find .pkg files, list and launch installed apps, and show notifications. Anything that writes, moves, deletes or launches shows a confirm box on screen first. Nothing happens unless you press Cross.

Built with the OpenOrbis PS4 toolchain, SDL2 for the framebuffer, stb_truetype for text, the system HTTPS library (libSceHttp) with streamed replies, and the system on-screen keyboard (which also accepts a USB keyboard).

## Install

1. Install `IV0000-CLDE00001_00-CLAUDEFORPS40000.pkg` with GoldHEN (Debug Settings > Package Installer, or a remote pkg sender).
2. Create `claude.ini` from `claude.ini.example` and set `api_key`. Put it either:
   - in the root folder of a USB drive (`/mnt/usb0/claude.ini`), or
   - at `/data/claude/claude.ini` on the PS4, for example over GoldHEN FTP:
     `curl -T claude.ini ftp://192.168.1.100:2121/data/claude/`
3. Start "Claude for PS4" from the home screen.

The API key is only read from that file. It is never built into the app.

## Controls

| Button | Action |
|---|---|
| Cross | Type a message (system keyboard, a USB keyboard works too) |
| Circle | Stop the reply that is streaming |
| Square | New chat |
| Triangle | Reload claude.ini |
| Up / Down, right stick | Scroll |
| L1 / R1 | Scroll one page |
| Confirm box | Cross allows, Circle denies |

Each chat is saved as text to `/data/claude/chats/`.

## Tools Claude can use

| Tool | Confirm on screen |
|---|---|
| get_system_info: firmware, CPU and SoC temperature, free space, IP | no |
| list_directory, read_text_file (up to 64 KB) | no |
| find_pkg_files on /mnt/usb0, /mnt/usb1, /data | no |
| list_installed_apps (title ID and name) | no |
| show_notification | no |
| write_text_file, make_directory, copy_path, move_path, delete_path | yes |
| launch_app (closes this app) | yes |

`/system`, `/system_ex`, `/preinst`, `/update`, `/dev` and the top-level folders themselves (`/data`, `/user`, `/mnt/usb0`) can never be written or deleted, even with a confirm.

At start the app asks GoldHEN to lift the app sandbox, so the tools can reach `/user`, `/system_data` and USB drives. The header shows "(sandboxed)" if that did not work; chat still works, but file tools see less.

## Build

Needs clang, ld.lld and OpenOrbis toolchain v0.5.2 (the newer release tarballs are named differently). `PkgTool.Core` needs libssl 1.1; on distros that only ship OpenSSL 3, extract `libssl1.1` from the Ubuntu 20.04 package and point `LD_LIBRARY_PATH` at it.

```sh
make OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
# host tests and UI preview PNGs
make -C tests OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
```

## Troubleshooting

- **"Could not reach api.anthropic.com (0x...)"**: check the network and the PS4 date and time (a wrong clock breaks HTTPS). If every request fails with a certificate error, the console's CA store may be missing the root used by api.anthropic.com. As a last resort set `tls_verify=0`, which turns off certificate checks for this app only and lets anyone on your network read the key.
- **API error 401**: the key in claude.ini is wrong or revoked.
- **API error 400 mentioning fallbacks**: set `fallbacks=0` (some models do not accept it).

## Status

Phase 1. Built and tested off-console (JSON, stream parsing, request body, tools, word wrap, UI render), not yet run on hardware.

Planned for Phase 2:
- Install .pkg files from USB (AppInstUtil / BGFT), delete installed apps
- Fan threshold control (GoldHEN / ICC)
- Screenshot Q&A: send a captured screenshot to Claude
- Send and run payloads (for example Linux boot)
- Crash log reader shortcuts
- Phone remote: a web page served by the PS4 for chatting from a phone
- Saved chats you can reopen, Markdown rendering, voice input later

## Licenses

DejaVu Sans font: see `assets/fonts/DejaVu-LICENSE.txt`. stb_truetype: public domain / MIT. GoldHEN jailbreak call based on the GoldHEN Plugins SDK (MIT).
