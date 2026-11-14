// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Minimal JSON parser, enough for Messages API responses and stream events.
#pragma once

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
    JType         type;
    double        num;
    int           boolean;
    char         *str;     // J_STR: UTF-8, NUL terminated
    struct JVal **items;   // J_ARR / J_OBJ values
    char        **keys;    // J_OBJ keys
    int           count;
} JVal;

// Returns NULL on a parse error.
JVal *json_parse(const char *text);
void json_free(JVal *v);

// Object lookup; NULL if v is not an object or the key is missing.
JVal *json_get(const JVal *v, const char *key);
// String member or NULL.
const char *json_get_str(const JVal *v, const char *key);

// Building and writing values.
JVal *json_new_str(const char *s);
JVal *json_new_obj(void);
JVal *json_new_arr(void);
// Takes ownership of item. For objects, replaces an existing key.
void json_obj_set(JVal *obj, const char *key, JVal *item);
void json_arr_push(JVal *arr, JVal *item);

#include "strbuf.h"
void json_write(const JVal *v, StrBuf *out);
