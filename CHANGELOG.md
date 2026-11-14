# Changelog

## 01.21 (2026-11-14)

- Fix: 01.10 and 01.20 could stop on the start screen. The app linked system libraries (BGFT, AppInstUtil, Random) that apps do not get at start; they are now loaded only when a tool needs them.
- Startup steps are shown on screen and written to /data/claude/boot.log, so a stuck start can be traced.

## 01.20 (2026-11-14)

- New look based on the Claude desktop app: sidebar, centered chat, message box at the bottom, serif replies, drawn controller buttons.
- Markdown in replies on the TV and phone: headings, bold, inline code, code blocks, lists, quotes, rules.
- Model and effort picker: R2 on the TV, menus under the message box on the phone, and in phone Settings. Works in API key mode and PC bridge mode (claude-bridge.py now passes --model and --effort to Claude Code).
- Phone page restyled to match, with light and dark themes.
- New bridge_model setting in claude.ini.

## 01.10 (2026-11-14)

- Phone remote: chat from your phone, answer confirm boxes there, stop a reply, start a new chat. The page stays up while the app runs; sign in with the PIN from Options.
- New tools, each with a confirm box: install_pkg, uninstall_app, set_fan_threshold, send_payload.
- Screenshot questions: list_screenshots and view_image send a screenshot or other image to Claude.
- Requests that fail before any reply (no connection, 429, 5xx, 529) are retried twice with a short wait.
- Warning at start if the PS4 clock is wrong.
- Version shown in the header and sent in the User-Agent. New background image (pic1).

## 01.00 (2026-11-14)

- First release: streamed chat with Claude, console tools with on-screen confirm, phone setup with QR code and PIN, and the PC bridge for signing in with a Claude account. Confirmed working on a real PS4 via the PC bridge.
