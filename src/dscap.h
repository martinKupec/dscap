/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz> */
/*
 * dscap.h — shared capture state + device helpers for the dscap tool.
 *
 * DSO/ANALOG capture accumulates into ONE per-channel stream held in a single
 * capture_ctx: mode=single (INSTANT deep one-shot), mode=roll (continuous
 * best-effort) and mode=stream (ANALOG) share this path; only the stop condition
 * differs. buf[] is the converged read view set after the capture stops — it
 * points into acc[]->data, it is not separately allocated.
 *
 * The datafeed callback has a fixed libsigrok signature, so it reaches the one
 * instance through the g_cap global; the .dsl writers take it by const pointer.
 */
#pragma once
#include <stdint.h>
#include <glib.h>
#include <libsigrok.h>
#include <libsigrok-internal.h>   /* sr_dev_inst.channels / sr_channel */

#define MAXCH 32                  /* DSLogic U3Pro32 has 32 logic channels; DSO <=2 */
#define HEADER_FORMAT_VERSION 3
#define LOGIC_BLOCK_BYTES (1u << 21)   /* 2 MiB = 2^24 bit-packed samples per .dsl L-block */

typedef struct {
	volatile int end;            /* stop flag: SR_DF_END, OOM cap, or duration done */
	int instant;                 /* mode=single: SR_CONF_INSTANT deep one-shot */
	int analog;                  /* mode=stream: ANALOG continuous DAQ */
	int acc_capped;              /* accumulation hit the OOM guard */
	int overflow;                /* SR_DF_OVERFLOW seen (stream FIFO underrun) */
	volatile unsigned dso_frames;   /* read by the stop loop to detect start-of-acquisition */
	unsigned long long packets;
	unsigned long long dso_samples;
	volatile unsigned long long logic_bytes;   /* also the stream stop loop's start-of-acquisition signal */
	unsigned np;                 /* number of active probes */
	unsigned spc;                /* samples per channel (min across ch) */
	unsigned cap_spc;            /* DSO instant: per-channel target depth (0 = no cap) */
	GByteArray *acc[MAXCH];      /* per-channel accumulated DSO/ANALOG samples */
	unsigned char *buf[MAXCH];   /* converged view: buf[p] = acc[p]->data */
	GByteArray *logic_acc;       /* accumulated raw logic wire bytes */
} capture_ctx;

extern capture_ctx g_cap;        /* the one instance the datafeed callback feeds */

/* device-config + channel-order helpers (defined in dscap.c, used by the writers) */
uint64_t cfg_u64(struct sr_channel *ch, int key, uint64_t def);
int have_u32(int key, uint32_t *out);
struct sr_channel *nth_capture_channel(struct sr_dev_inst *sdi, unsigned p, int analog);
