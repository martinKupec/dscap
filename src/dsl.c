/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz> */
/*
 * dsl.c — native .dsl exporter. A .dsl is a zip holding an ini "header", one
 * data member per channel (DSO: O-<idx>/0; LOGIC: L-<idx>/<block>), and our
 * summary.json. Mirrors what DSView's storesession writes.
 */
#include <stdio.h>
#include <string.h>
#include <minizip/zip.h>
#include "dsl.h"

static int zip_add(zipFile z, const char *name, const void *buf, size_t len)
{
	zip_fileinfo zi; memset(&zi, 0, sizeof zi);
	if (zipOpenNewFileInZip(z, name, &zi, NULL, 0, NULL, 0, NULL,
				Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK) return -1;
	if (len && zipWriteInFileInZip(z, buf, (unsigned)len) != ZIP_OK) { zipCloseFileInZip(z); return -1; }
	zipCloseFileInZip(z);
	return 0;
}

/*
 * DSO calibration (codes -> volts), matching DSView's mapping:
 *   V_mv = (hw_offset - adc_code) * vdiv_mv * DS_CONF_DSO_VDIVS / 255 * probe_factor
 */
void gen_header(GString *h, const capture_ctx *cap, struct sr_dev_inst *sdi,
		const char *driver, uint64_t samplerate)
{
	char *srs = sr_samplerate_string(samplerate);
	g_string_append_printf(h, "[version]\nversion = %d\n[header]\n", HEADER_FORMAT_VERSION);
	g_string_append_printf(h, "driver = %s\n", driver);
	g_string_append_printf(h, "device mode = %d\n", cap->analog ? ANALOG : DSO);
	g_string_append_printf(h, "capturefile = data\n");
	g_string_append_printf(h, "total samples = %u\n", cap->spc);
	g_string_append_printf(h, "total probes = %u\n", cap->np);
	/* total blocks is LOGICAL (DSView's LeafBlockPower=21 -> 2 MiB units spanning all
	 * channels), not a member count: ceil(total_samples * 1 byte * num_ch / 2^21).
	 * DSO still writes one O-<ch>/0 member, but the header must declare the real block
	 * count or DSView truncates a deep reload to the first block. */
	uint64_t dblocks = (((uint64_t)cap->spc * cap->np) + (1ULL << 21) - 1) >> 21;
	if (!dblocks) dblocks = 1;
	g_string_append_printf(h, "total blocks = %" G_GUINT64_FORMAT "\n", dblocks);
	g_string_append_printf(h, "samplerate = %s\n", srs ? srs : "0");
	g_free(srs);
	uint64_t hdiv = cfg_u64(NULL, SR_CONF_TIMEBASE, 0);
	if (hdiv) g_string_append_printf(h, "hDiv = %" G_GUINT64_FORMAT "\n", hdiv);
	g_string_append_printf(h, "bits = 8\n");
	uint32_t rmin, rmax;
	if (have_u32(SR_CONF_REF_MIN, &rmin)) g_string_append_printf(h, "ref min = %u\n", rmin);
	if (have_u32(SR_CONF_REF_MAX, &rmax)) g_string_append_printf(h, "ref max = %u\n", rmax);
	g_string_append_printf(h, "trigger pos = 0\n");
	for (unsigned p = 0; p < cap->np; p++) {
		struct sr_channel *ch = nth_capture_channel(sdi, p, cap->analog);
		if (!ch) continue;
		g_string_append_printf(h, "probe%u = %s\n", p, ch->name ? ch->name : "");
		g_string_append_printf(h, " enable%u = 1\n", p);
		g_string_append_printf(h, " coupling%u = %d\n", p, ch->coupling);
		g_string_append_printf(h, " vDiv%u = %" G_GUINT64_FORMAT "\n", p, ch->vdiv);
		g_string_append_printf(h, " vFactor%u = %" G_GUINT64_FORMAT "\n", p, ch->vfactor);
		g_string_append_printf(h, " vOffset%u = %d\n", p, ch->hw_offset);
		g_string_append_printf(h, " vTrig%u = %d\n", p, ch->trig_value);
	}
}

int write_dsl(const char *path, GString *header, GString *summary,
	      const capture_ctx *cap, struct sr_dev_inst *sdi)
{
	char *dir = g_path_get_dirname(path);
	if (dir && strcmp(dir, ".")) g_mkdir_with_parents(dir, 0755);
	g_free(dir);
	zipFile z = zipOpen64(path, 0);
	if (!z) return -1;
	int rc = 0;
	rc |= zip_add(z, "header", header->str, header->len);
	for (unsigned p = 0; p < cap->np && rc == 0; p++) {
		struct sr_channel *ch = nth_capture_channel(sdi, p, cap->analog);
		int idx = ch ? ch->index : (int)p;
		char nm[24]; snprintf(nm, sizeof nm, "O-%d/0", idx);
		rc |= zip_add(z, nm, cap->buf[p], cap->spc);
	}
	rc |= zip_add(z, "summary.json", summary->str, summary->len);
	zipClose(z, NULL);
	return rc;
}

/* mirrors DSView's storesession: device mode = LOGIC, per-channel probe<idx>,
 * samplerate, total samples/probes/blocks */
void gen_logic_header(GString *h, const char *driver, uint64_t samplerate,
		      uint64_t total_samples, const int *idxs, const char **names,
		      unsigned nch, uint64_t total_blocks)
{
	char *srs = sr_samplerate_string(samplerate);
	g_string_append_printf(h, "[version]\nversion = %d\n[header]\n", HEADER_FORMAT_VERSION);
	g_string_append_printf(h, "driver = %s\n", driver);
	g_string_append_printf(h, "device mode = %d\n", LOGIC);
	g_string_append_printf(h, "capturefile = data\n");
	g_string_append_printf(h, "total samples = %" G_GUINT64_FORMAT "\n", total_samples);
	g_string_append_printf(h, "total probes = %u\n", nch);
	g_string_append_printf(h, "total blocks = %" G_GUINT64_FORMAT "\n", total_blocks);
	g_string_append_printf(h, "samplerate = %s\n", srs ? srs : "0");
	g_free(srs);
	g_string_append_printf(h, "trigger pos = 0\n");
	for (unsigned c = 0; c < nch; c++) {
		/* probe<idx> ties to the L-<idx> data member (DSView keying) */
		g_string_append_printf(h, "probe%d = %s\n", idxs[c], names[c] ? names[c] : "");
		g_string_append_printf(h, " enable%d = 1\n", idxs[c]);
	}
}

int write_logic_dsl(const char *path, GString *header, GString *summary,
		    const int *idxs, GByteArray **chans, unsigned nch)
{
	char *dir = g_path_get_dirname(path);
	if (dir && strcmp(dir, ".")) g_mkdir_with_parents(dir, 0755);
	g_free(dir);
	zipFile z = zipOpen64(path, 0);
	if (!z) return -1;
	int rc = zip_add(z, "header", header->str, header->len);
	for (unsigned c = 0; c < nch && rc == 0; c++) {
		GByteArray *b = chans[c];
		uint64_t off = 0; unsigned blk = 0;
		do {   /* always emit at least L-<idx>/0, even if empty */
			size_t n = b->len - off;
			if (n > LOGIC_BLOCK_BYTES) n = LOGIC_BLOCK_BYTES;
			char nm[32]; snprintf(nm, sizeof nm, "L-%d/%u", idxs[c], blk);
			rc |= zip_add(z, nm, b->data + off, n);
			off += n; blk++;
		} while (off < b->len && rc == 0);
	}
	rc |= zip_add(z, "summary.json", summary->str, summary->len);
	zipClose(z, NULL);
	return rc;
}
