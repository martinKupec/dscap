/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz> */
/* cfg.h — INI value + unit-string parsing (no device dependency). */
#pragma once
#include <stdint.h>
#include <glib.h>

uint64_t parse_freq(const char *s);   /* "500M" / "500MHz" / "1G" -> Hz */
uint64_t parse_vdiv(const char *s);   /* "10mV" -> 10, "1V" -> 1000, bare -> mV */
/* GKeyFile string with inline ';'/'#' comments stripped + whitespace trimmed */
char *cfg_str(GKeyFile *kf, const char *grp, const char *key);
/* substitute {ts} in an output path with a YYMMDD-HHMMSS timestamp (caller g_free()s) */
char *expand_out(const char *tmpl);
