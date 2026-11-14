# Changelog

## 01.10 (2026-11-14)

- Phone remote: chat from your phone, answer confirm boxes there, stop a reply, start a new chat. The page stays up while the app runs; sign in with the PIN from Options.
- New tools, each with a confirm box: install_pkg, uninstall_app, set_fan_threshold, send_payload.
- Screenshot questions: list_screenshots and view_image send a screenshot or other image to Claude.
- Requests that fail before any reply (no connection, 429, 5xx, 529) are retried twice with a short wait.
- Warning at start if the PS4 clock is wrong.
- Version shown in the header and sent in the User-Agent. New background image (pic1).

## 01.00 (2026-11-14)

- First release: streamed chat with Claude, console tools with on-screen confirm, phone setup with QR code and PIN, and the PC bridge for signing in with a Claude account. Confirmed working on a real PS4 via the PC bridge.
