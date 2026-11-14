# Claude for PS4

A native PS4 homebrew app (jailbroken console, GoldHEN) for chatting with Claude through the Anthropic API. Claude can also use console tools: it can read system info, browse and manage files, install and uninstall .pkg files, list and launch apps, set the fan threshold, look at your screenshots, and send payloads to GoldHEN BinLoader. Anything that writes, installs, deletes, launches or runs shows a confirm box first (on the TV, or on your phone if you use the phone remote). Nothing happens unless you allow it.

You can also chat from your phone: the PS4 serves a small web page on your home network.

Built with the OpenOrbis PS4 toolchain, SDL2 for the framebuffer, stb_truetype for text, the system HTTPS library (libSceHttp) with streamed replies, and the system on-screen keyboard (which also accepts a USB keyboard).

## Install

1. Install `IV0000-CLDE00001_00-CLAUDEFORPS40000.pkg` with GoldHEN (Debug Settings > Package Installer, or a remote pkg sender).
2. Start "Claude for PS4". The first time, it shows a QR code and a PIN.
3. Scan the code with your phone (same Wi-Fi), type the PIN, and pick how the PS4 reaches Claude (see below). Press Options any time to show the code again.

Settings are saved to `/data/claude/claude.ini`. You can also write that file yourself (see `claude.ini.example`) or put a `claude.ini` in the root of a USB drive. Your key is never built into the app.

## Two ways to sign in

| | API key | PC bridge (your Claude account) |
|---|---|---|
| What you need | An API key from console.anthropic.com | A PC on the same network with Claude Code signed in to your Claude account, running `tools/claude-bridge.py` |
| Billing | Pay per use on your API account | Uses your Claude plan's usage limits |
| PC must be on | No | Yes |
| Console tools (files, apps, pkg install, screenshots) | Yes | No, chat only |
| Model | Any API model (default claude-opus-5-5) | Whatever Claude Code uses, or `--model` on the bridge |

The PS4 cannot sign in to claude.ai directly: Anthropic does not let third-party apps use claude.ai account logins. The bridge stays within the rules by running the official Claude Code CLI on your own PC with your own login, and only passes text between it and the PS4.

### PC bridge setup

On the PC (Python 3 and Claude Code installed):

```sh
claude            # once: sign in with /login, then exit
python3 tools/claude-bridge.py            # add --model opus or sonnet if you like
```

It prints a bridge address (for example `http://192.168.1.50:8787`) and a pairing code. On the PS4 press Options, open the page on your phone, choose "PC bridge" and enter both. Allow port 8787 in the PC firewall if it has one. Claude Code's own tools are turned off in the bridge (`--tools ""`), so the PS4 cannot make it run commands on your PC. Each PS4 chat maps to one Claude Code session, kept in `~/.config/claude-ps4-bridge/`.

## Phone remote

The phone page keeps running while the app is open. Press Options to see the QR code and PIN, open the page and sign in with the PIN. Each PIN signs in one phone, and the phone stays signed in until the app restarts (up to 4 phones). From the phone you can:

- chat (the reply appears on the phone and on the TV)
- Allow or Deny a confirm box
- stop a reply, start a new chat
- change settings

Five wrong PINs make a new one appear on the TV.

Both the phone page and the bridge use plain HTTP on your home network, protected by the PIN and the pairing code. Use them only on a network you trust.

## Controls

| Button | Action |
|---|---|
| Cross | Type a message (system keyboard, a USB keyboard works too) |
| Circle | Stop the reply that is streaming |
| Square | New chat |
| Triangle | Reload claude.ini |
| Options | Phone page (QR code and PIN) |
| Up / Down, right stick | Scroll |
| L1 / R1 | Scroll one page |
| Confirm box | Cross allows, Circle denies (or Allow/Deny on the phone) |

Each chat is saved as text to `/data/claude/chats/`.

## Tools Claude can use

| Tool | Confirm on screen |
|---|---|
| get_system_info: firmware, CPU and SoC temperature, free space, IP | no |
| list_directory, read_text_file (up to 64 KB) | no |
| find_pkg_files on /mnt/usb0, /mnt/usb1, /data | no |
| list_installed_apps (title ID and name) | no |
| show_notification | no |
| list_screenshots, view_image (JPEG/PNG up to 3.5 MB, "latest" for the newest screenshot) | no |
| write_text_file, make_directory, copy_path, move_path, delete_path | yes |
| install_pkg (reads the pkg header, queues the install in Downloads) | yes |
| uninstall_app (save data stays) | yes |
| set_fan_threshold (45 to 80 C, resets on reboot) | yes |
| send_payload (.bin to GoldHEN BinLoader, port 9090) | yes |
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
- **API error 401**: the key in claude.ini is wrong or revoked. In bridge mode it means the pairing code does not match.
- **"Could not reach the PC bridge"**: check that claude-bridge.py is running, the address is right, and the PC firewall allows port 8787.
- **Bridge replies with a login error**: run `claude` on the PC and sign in with `/login`.
- **API error 400 mentioning fallbacks**: set `fallbacks=0` (some models do not accept it).

## Status

Version 01.10. Chat through the PC bridge is confirmed on a real PS4 (CUH-7016B, firmware 14.00, GoldHEN). API key mode, the console tools and the phone remote are tested off-console (host tests, UI and phone page previews) but not yet on hardware. pkg install, uninstall, fan control and payloads use the common homebrew system calls; tell us if one fails on your firmware.

Requests that fail before any reply text (no connection, rate limited, overloaded) are retried twice, after 2 and 4 seconds. At start the app warns if the clock looks wrong, since HTTPS needs the right date.

Planned:
- Saved chats you can reopen, Markdown rendering
- Crash log reader shortcuts
- Voice input

See CHANGELOG.md for what changed.

## Licenses

DejaVu Sans font: see `assets/fonts/DejaVu-LICENSE.txt`. stb_truetype: public domain / MIT. QR codes: Project Nayuki QR Code generator (MIT). GoldHEN jailbreak call based on the GoldHEN Plugins SDK (MIT).
