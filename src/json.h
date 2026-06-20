/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz> */
/* json.h — minimal JSON output helpers (stdout summary + error objects). */
#pragma once
#include <glib.h>

/* escape s for embedding inside a JSON "..." literal */
void json_escape(GString *out, const char *s);
/* newly-allocated JSON-escaped copy of s (caller g_free()s) */
char *json_esc(const char *s);
/* print {"ok":false,"error":"<msg>"} to stdout */
void emit_err(const char *msg);
