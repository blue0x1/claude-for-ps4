// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
#pragma once
// Escapes the app sandbox through GoldHEN so the tools can reach /user,
// /system_data and USB drives. Returns 1 on success.
int plat_jailbreak(void);
int plat_jailbroken(void);
int plat_user_id(void);
