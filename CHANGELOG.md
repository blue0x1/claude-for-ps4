<!-- Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved. -->
# Changelog

## 01.26 (2026-11-14)

- App updates are check-only: check_app_update compares this version with the Claude for PS4 package next to the PC bridge and says how to update (copy it to /data/pkg, uninstall, install; chats and settings stay). Installing over the running app is not possible through the PS4 installer, and replacing the installed package by hand made the PS4 report the app as corrupted, so that was dropped.

## 01.25 (2026-11-14)

- Trash: delete_path now moves things to a trash (on USB drives, a trash on that drive). list_trash, restore_from_trash and empty_trash undo or finish deletes; `permanent: true` skips the trash.
- History: every change Claude makes is logged in /data/claude/history.log with the time, the result and who approved it (TV, phone, or automatic in trusted mode). show_history reads it.
- Storage manager: storage_report shows free space, every game with its update and DLC sizes, updates and DLC left behind by removed games, .pkg files whose content is already installed, captures and other big items. delete_paths removes several items behind one confirm and refuses installed game folders.
- Save data: list_game_saves (per user, with the user name), backup_saves (one game or all, internal or USB, newest 20 versions per game, including save names and icons), list_save_backups and restore_saves, which backs up the current saves before it replaces them.
- Install from PC: put .pkg files in ~/PS4/pkg on the PC (or --pkg-dir). list_pc_packages shows them with title, version and whether they are installed; install_from_pc queues them in the PS4 installer straight over the network; install_status shows percent, speed and time left, and a notification pops up when each one finishes.
- GoldHEN: list_cheats and list_game_patches per game (marking the ones for the installed version), list_plugins with where each is on, set_plugin to turn a plugin on or off per game or for all games, goldhen_settings and set_goldhen_setting for config.ini. The ini files are backed up before each change.
- Health: health_report (temperatures now and recently, fan profile, time on, storage), temperature_history as a small chart, and set_fan_profile (quiet, balanced, cool, max, auto). The profile is applied again when the app starts after a reboot; auto turns the fan up while the console is hot. A pop-up warns at 85 C.
- Permission modes: full (asks before every change), trusted (safe, undoable changes run without asking), read-only and kid mode. Pick it in the phone menu; Claude can only make it stricter.
- Memory: remember, forget and get_preferences. Remembered items are given to Claude in every chat.
- Trusted mode also deletes to the trash without asking (they can be restored); permanent deletes still ask.
- App update check: compares with the Claude for PS4 pkg on the PC bridge.
- Install from PC uses the installer's network registration (the first try was refused with 0x8099006A).
- Phone page: microphone button, with a help card for the keyboard microphone where the browser blocks it, "Help with my screen" for the newest screenshot, and quick actions on the start screen.
- claude-bridge.py: dashboard at http://127.0.0.1:8787 on the PC with the address, pairing code, connected PS4, packages with transfer progress and the newest app version; one-step pairing (enter the PIN from the TV, or let it find the PS4); --https for encrypted PS4 traffic with a self-signed certificate (packages then come from the next port over plain HTTP, since the PS4 installer cannot use it).

## 01.24 (2026-11-14)

- Console tools now work in PC bridge mode (Claude account), not only with an API key. The PS4 sends its tool list to the bridge, and the bridge gives the tools to Claude Code through a local MCP server. Each call runs on the PS4 with the usual confirm boxes. Update both the app and claude-bridge.py.
- The message box shows "Tools on" in both modes.
- Fix: full file access. GoldHEN answers the SDK calls with the carry flag set, so the app took it for "no GoldHEN" and stayed sandboxed (list_installed_apps failed on /user/app). It now makes the SDK's indirect call, checks the result by whether /user/app is visible, retries before tools, and writes the reason to boot.log and get_system_info.
- Phone page: answers only home-network addresses (10.x, 172.16-31.x, 192.168.x, link-local). After 5 wrong PINs a new PIN appears and logins are refused for 30 s, doubling each round up to 15 min.
- Home screen folders: new tools list_home_folders, create_home_folder, move_to_home_folder, rename_home_folder, delete_home_folder and restore_home_backup. Claude makes folders and puts the games you name in them, written the way the PS4 writes its own folders. Each change asks first, backs up the home screen database to /data/claude/backups (newest 10 kept) and checks it after; a failed check puts the backup back. Restart the PS4 to see changes.
- Fix: delete_path and move_path failed with "Function not implemented": the toolchain's lstat is a stub on the PS4. Error messages now use the PS4's error numbers.
- claude-bridge.py: answers only home-network addresses; 5 wrong pairing codes from one address block it for 30 s, doubling up to 15 min.

## 01.23 (2026-11-14)

- Recents in the sidebar: press D-pad Left, pick a chat with Up/Down and press Cross to open it and carry on. In bridge mode it continues the same Claude Code session.
- Chats are now also saved as .json next to the .txt, so they can be reopened. Older .txt chats open too.
- Profile at the bottom left: Claude account name (bridge mode) or PS4 user name (API key mode).
- Log out (asks first): removes the API key and bridge pairing from claude.ini, signs out phones, and opens the phone setup.
- Phone page: menu with the chat list, profile and Log out.
- claude-bridge.py: new /v1/whoami for the profile name. Update the bridge on your PC to see the account name.
- Fix: Reload settings and phone setup no longer overwrite the previous chat file.

## 01.22 (2026-11-14)

- Fix: the PS4 start screen stayed on top of the app. The app now hides it once it starts.

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
