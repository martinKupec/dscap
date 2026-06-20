/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz> */
/* cfg.c — INI value + unit-string parsing helpers. */
#include "cfg.h"

/* parse "500M" / "500MHz" / "1G" -> Hz; "10mV" / "1V" -> mV. Returns 0 on empty. */
static uint64_t parse_unit(const char *s, uint64_t kilo)
{
	if (!s || !*s) return 0;
	char *end = NULL;
	double v = g_ascii_strtod(s, &end);
	while (*end == ' ') end++;
	double mul = 1;
	switch (g_ascii_toupper(*end)) {
	case 'K': mul = kilo; break;
	case 'M': mul = kilo * kilo; break;
	case 'G': mul = kilo * kilo * kilo; break;
	case 'V': mul = (kilo == 1000) ? 1000 : 1; break;   /* "1V" -> 1000 mV */
	default: break;                                      /* bare / mV / Hz */
	}
	return (uint64_t)(v * mul + 0.5);
}

uint64_t parse_freq(const char *s)   /* Hz: k/M/G = kilo/mega/giga */
{
	return parse_unit(s, 1000);
}

uint64_t parse_vdiv(const char *s)
{
	if (!s || !*s) return 0;
	char *end = NULL;
	double v = g_ascii_strtod(s, &end);
	while (*end == ' ') end++;
	if (*end == 'm' || *end == 'M') return (uint64_t)(v + 0.5);        /* mV */
	if (*end == 'v' || *end == 'V') return (uint64_t)(v * 1000 + 0.5); /* V  */
	return (uint64_t)(v + 0.5);                                        /* bare = mV */
}

/* GKeyFile only honors comments at line start, so we clean up after it. */
char *cfg_str(GKeyFile *kf, const char *grp, const char *key)
{
	char *v = g_key_file_get_string(kf, grp, key, NULL);
	if (!v) return NULL;
	for (char *p = v; *p; p++) if (*p == ';' || *p == '#') { *p = 0; break; }
	g_strstrip(v);
	if (!*v) { g_free(v); return NULL; }
	return v;
}

char *expand_out(const char *tmpl)
{
	GDateTime *now = g_date_time_new_now_local();
	char *ts = g_date_time_format(now, "%y%m%d-%H%M%S");
	GString *o = g_string_new(tmpl ? tmpl : "capture-{ts}.dsl");
	g_string_replace(o, "{ts}", ts, 0);
	g_free(ts); g_date_time_unref(now);
	return g_string_free(o, FALSE);
}
