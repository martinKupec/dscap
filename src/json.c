/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz> */
/* json.c — JSON string escaping + error/summary output for stdout. */
#include <stdio.h>
#include "json.h"

/* escape a string for embedding inside a JSON "..." literal, so stdout stays a
 * single valid object even when the value carries a quote/backslash/control char
 * (user-supplied mode strings, Windows backslash paths, channel names). */
void json_escape(GString *out, const char *s)
{
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
		switch (*p) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\b': g_string_append(out, "\\b"); break;
		case '\f': g_string_append(out, "\\f"); break;
		case '\n': g_string_append(out, "\\n"); break;
		case '\r': g_string_append(out, "\\r"); break;
		case '\t': g_string_append(out, "\\t"); break;
		default:
			if (*p < 0x20) g_string_append_printf(out, "\\u%04x", *p);
			else g_string_append_c(out, *p);
		}
	}
}

char *json_esc(const char *s)
{
	GString *o = g_string_new(NULL);
	json_escape(o, s);
	return g_string_free(o, FALSE);
}

void emit_err(const char *msg)
{
	char *e = json_esc(msg);
	printf("{\"ok\":false,\"error\":\"%s\"}\n", e);
	g_free(e);
}
