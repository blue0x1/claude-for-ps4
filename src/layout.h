// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>. All rights reserved.
#pragma once
#include "font.h"

// Word-wraps text to width pixels. Fills newly allocated arrays of line
// starts and lengths (byte offsets). Returns the line count (at least 1).
int layout_wrap(Font *f, const char *text, int len, int width, int **starts, int **lens);
