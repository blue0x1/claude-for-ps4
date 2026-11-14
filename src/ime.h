// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
// System on-screen keyboard (also takes input from a USB keyboard).
#pragma once
#include <stddef.h>

int  ime_init(void);
// Opens the keyboard. Returns 0 on success.
int  ime_open(int user_id, const char *title, const char *initial_utf8);
// -1 still open, 0 finished with text in out, 1 cancelled, 2 error.
int  ime_poll(char *out, size_t outlen);
int  ime_is_open(void);
