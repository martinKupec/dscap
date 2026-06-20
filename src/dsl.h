/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz> */
/* dsl.h — native .dsl (zip) exporter: ini header + per-channel members + summary. */
#pragma once
#include <glib.h>
#include "dscap.h"

/* build the native .dsl "header" ini for a DSO/ANALOG capture */
void gen_header(GString *h, const capture_ctx *cap, struct sr_dev_inst *sdi,
		const char *driver, uint64_t samplerate);
/* build the native .dsl "header" ini for a LOGIC capture */
void gen_logic_header(GString *h, const char *driver, uint64_t samplerate,
		      uint64_t total_samples, const int *idxs, const char **names,
		      unsigned nch, uint64_t total_blocks);
/* write header + per-channel O-<idx>/0 + summary.json into a .dsl zip */
int write_dsl(const char *path, GString *header, GString *summary,
	      const capture_ctx *cap, struct sr_dev_inst *sdi);
/* write header + per-channel L-<idx>/<block> (bit-packed, 2 MiB blocks) + summary.json */
int write_logic_dsl(const char *path, GString *header, GString *summary,
		    const int *idxs, GByteArray **chans, unsigned nch);
