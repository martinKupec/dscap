/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz> */
/*
 * dscap.c — headless capture for DreamSourceLab DSCope / DSLogic via the
 * libsigrok4DSL high-level ds_* API (the same code DSView's GUI runs).
 *
 *   ./dscap enumerate        identify the attached device       (JSON to stdout)
 *   ./dscap <config.ini>     run the measurement; print JSON; write a native .dsl
 *
 * stdout = machine-readable JSON (one object). Human/log noise -> stderr.
 * See AGENTS.md for the output contract.
 *
 * This file owns the device session: the datafeed callback + g_cap state, the
 * DSO/ANALOG and LOGIC capture paths, and main. The .dsl container format lives
 * in dsl.c, JSON output in json.c, INI/unit parsing in cfg.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dscap.h"
#include "json.h"
#include "cfg.h"
#include "dsl.h"

/* Default DSView resource dir (firmware + FPGA bitstream). libsigrok4DSL has NO
 * built-in default and errors if it is unset, so we always set one. Override at
 * runtime with [capture] res_dir, else $DSVIEW_RES_DIR, else this compiled default. */
#if defined(_WIN32)
static const char *FW_DIR = "C:\\Program Files\\DSView\\res";   /* next to DSView.exe */
#elif defined(__APPLE__)
static const char *FW_DIR = "/Applications/DSView.app/Contents/Resources/res"; /* unverified */
#else
static const char *FW_DIR = "/opt/dsview/share/DSView/res";
#endif

#define INSTANT_CAP_BYTES_PER_CH (512ULL*1024*1024)   /* OOM guard, per channel */
#define LOGIC_CAP_TOTAL_BYTES (1024ULL*1024*1024)      /* OOM guard, all channels */

/* LOGIC capture: raw LA_CROSS_DATA wire bytes (channel-major 8-byte atoms),
 * de-interleaved per channel after the capture stops. */
#define LOGIC_ATOMIC_SAMPLES 64   /* DSLOGIC_ATOMIC_SAMPLES (1<<6) */
#define LOGIC_ATOMIC_SIZE     8   /* DSLOGIC_ATOMIC_SIZE bytes (one 64-sample word/ch) */

capture_ctx g_cap;

static const char *modename(int m)
{
	switch (m) { case LOGIC: return "LOGIC"; case DSO: return "DSO";
		     case ANALOG: return "ANALOG"; default: return "?"; }
}

static void feed_cb(const struct sr_dev_inst *sdi,
		    const struct sr_datafeed_packet *pkt)
{
	(void)sdi;
	g_cap.packets++;
	switch (pkt->type) {
	case SR_DF_DSO: {
		const struct sr_datafeed_dso *d = pkt->payload;
		g_cap.dso_samples += d->num_samples;
		/* DSView semantics (dsosnapshot.cpp::append_data): d->num_samples is the
		 * PER-CHANNEL count; the wire data is interleaved across ENABLED channels with
		 * stride = enabled count. d->probes lists ALL mode channels (always 2 for a
		 * DSCope, regardless of enable), so derive the enabled count by filtering. */
		unsigned en = 0;
		for (GSList *it = d->probes; it; it = it->next) {
			struct sr_channel *c = it->data; if (c->enabled) en++;
		}
		if (en == 0 || en > MAXCH || !d->data) break;
		unsigned spc = d->num_samples;       /* per channel, NOT divided by probe count */
		const unsigned char *b = d->data;
		if (g_cap.np == 0) {  /* first chunk: record enabled-channel count, alloc accumulators */
			g_cap.np = en;
			for (unsigned p = 0; p < en; p++) g_cap.acc[p] = g_byte_array_new();
		}
		/* single (INSTANT) and roll both de-interleave each chunk and CONCATENATE it
		 * onto the per-channel stream. Bulk-append per channel — matters for deep buffers. */
		if (en == g_cap.np && spc && !g_cap.acc_capped) {
			/* INSTANT: clamp the per-channel total to the requested depth (DSView caps at
			 * _total_sample_count). The device re-emits a full-size tail packet of zeros
			 * after the real one; without this clamp it doubles the stream half-zeroed. */
			if (g_cap.cap_spc) {
				unsigned have = g_cap.acc[0]->len;
				if (have >= g_cap.cap_spc) spc = 0;
				else if (have + spc > g_cap.cap_spc) spc = g_cap.cap_spc - have;
			}
			if (spc) {
				unsigned char *tmp = malloc(spc);
				if (tmp) {
					for (unsigned p = 0; p < en; p++) {
						for (unsigned j = 0; j < spc; j++) tmp[j] = b[j * en + p];
						g_byte_array_append(g_cap.acc[p], tmp, spc);
					}
					free(tmp);
				}
			}
			if (g_cap.acc[0]->len >= INSTANT_CAP_BYTES_PER_CH) { g_cap.acc_capped = 1; g_cap.end = 1; }
			if (g_cap.cap_spc && g_cap.acc[0]->len >= g_cap.cap_spc) g_cap.end = 1;
		}
		g_cap.dso_frames++;
	} break;
	case SR_DF_ANALOG: {
		/* ANALOG continuous: like DSO accumulation, but num_samples is already
		 * PER-CHANNEL and probes is the full sdi->channels list. 8-bit codes only (v1). */
		const struct sr_datafeed_analog *a = pkt->payload;
		unsigned np = g_slist_length(a->probes);
		if (np == 0 || np > MAXCH || !a->data) break;
		if (((a->unit_bits + 7) / 8) != 1) break;   /* v1 handles 1 byte/sample only */
		unsigned spc = a->num_samples;
		const unsigned char *b = a->data;
		if (g_cap.np == 0) {
			g_cap.np = np;
			for (unsigned p = 0; p < np; p++) g_cap.acc[p] = g_byte_array_new();
		}
		if (np == g_cap.np && spc && !g_cap.acc_capped) {
			unsigned char *tmp = malloc(spc);
			if (tmp) {
				for (unsigned p = 0; p < np; p++) {
					for (unsigned j = 0; j < spc; j++) tmp[j] = b[j * np + p];
					g_byte_array_append(g_cap.acc[p], tmp, spc);
				}
				free(tmp);
			}
			if (g_cap.acc[0]->len >= INSTANT_CAP_BYTES_PER_CH) { g_cap.acc_capped = 1; g_cap.end = 1; }
		}
		g_cap.dso_frames++;
	} break;
	case SR_DF_LOGIC: {
		const struct sr_datafeed_logic *l = pkt->payload;
		g_cap.logic_bytes += l->length;
		if (l->format == LA_CROSS_DATA && l->data && l->length) {
			if (!g_cap.logic_acc) g_cap.logic_acc = g_byte_array_new();
			if (g_cap.logic_acc->len < LOGIC_CAP_TOTAL_BYTES)
				g_byte_array_append(g_cap.logic_acc, l->data, l->length);
			else { g_cap.acc_capped = 1; g_cap.end = 1; }
		}
	} break;
	case SR_DF_OVERFLOW: g_cap.overflow = 1; break;
	case SR_DF_END: g_cap.end = 1; break;
	default: break;
	}
}

uint64_t cfg_u64(struct sr_channel *ch, int key, uint64_t def)
{
	GVariant *gv = NULL;
	if (ds_get_actived_device_config(ch, NULL, key, &gv) == SR_OK && gv) {
		uint64_t v = g_variant_get_uint64(gv); g_variant_unref(gv); return v;
	}
	return def;
}
int have_u32(int key, uint32_t *out)
{
	GVariant *gv = NULL;
	if (ds_get_actived_device_config(NULL, NULL, key, &gv) == SR_OK && gv) {
		*out = g_variant_get_uint32(gv); g_variant_unref(gv); return 1;
	}
	return 0;
}

/* enabled DSO channel matching probe-position p (order matches the feed interleave) */
static struct sr_channel *nth_dso_channel(struct sr_dev_inst *sdi, unsigned p)
{
	unsigned k = 0;
	for (GSList *it = sdi ? sdi->channels : NULL; it; it = it->next) {
		struct sr_channel *ch = it->data;
		if (ch->type == SR_CHANNEL_DSO && ch->enabled) { if (k == p) return ch; k++; }
	}
	return NULL;
}

/* channel matching feed-position p: DSO de-interleaves by enabled DSO channel; ANALOG's
 * probes list is the full sdi->channels, so position p is the p-th channel in order. */
struct sr_channel *nth_capture_channel(struct sr_dev_inst *sdi, unsigned p, int analog)
{
	if (!analog) return nth_dso_channel(sdi, p);
	unsigned k = 0;
	for (GSList *it = sdi ? sdi->channels : NULL; it; it = it->next, k++)
		if (k == p) return it->data;
	return NULL;
}

/* is samplerate sr in the active device's valid list? (per-probe limits included) */
static int samplerate_ok(uint64_t sr)
{
	GVariant *gv = NULL;
	if (ds_get_actived_device_config_list(NULL, SR_CONF_SAMPLERATE, &gv) != SR_OK || !gv)
		return 1;   /* can't query -> don't block */
	int ok = 0;
	GVariant *arr = g_variant_lookup_value(gv, "samplerates", G_VARIANT_TYPE("at"));
	GVariant *it = arr ? arr : gv;
	gsize n = 0;
	const uint64_t *vals = g_variant_get_fixed_array(it, &n, sizeof(uint64_t));
	for (gsize i = 0; vals && i < n; i++) if (vals[i] == sr) { ok = 1; break; }
	if (arr) g_variant_unref(arr);
	g_variant_unref(gv);
	return ok;
}

/* Transform raw LA_CROSS_DATA wire bytes into a logic .dsl + JSON summary. Pure (no
 * device): the wire is channel-major 8-byte atoms [ch0][ch1]...[ch(N-1)] where channel
 * at feed-position c has its k-th atom at byte (k*en_ch + c)*8 — already bit-packed
 * LSB=earliest, i.e. exactly the .dsl L-<idx> layout. Factored out so the format can be
 * round-trip validated offline (see the selftest-logic subcommand). */
static int logic_write_capture(const char *outfile, const char *device, const char *driver,
			       int stream, uint64_t samplerate, const uint8_t *wire,
			       uint64_t wire_len, unsigned en_ch, const int *idxs,
			       const char **names)
{
	uint64_t atoms = wire_len / ((uint64_t)en_ch * LOGIC_ATOMIC_SIZE);   /* whole atoms only */
	uint64_t bytes_per_ch = atoms * LOGIC_ATOMIC_SIZE;
	uint64_t samples_per_ch = atoms * LOGIC_ATOMIC_SAMPLES;

	GByteArray *chans[MAXCH];
	for (unsigned c = 0; c < en_ch; c++) {
		GByteArray *b = g_byte_array_sized_new(bytes_per_ch ? bytes_per_ch : 1);
		for (uint64_t k = 0; k < atoms; k++)
			g_byte_array_append(b, wire + (k * en_ch + c) * LOGIC_ATOMIC_SIZE, LOGIC_ATOMIC_SIZE);
		chans[c] = b;
	}
	uint64_t blocks = (bytes_per_ch + LOGIC_BLOCK_BYTES - 1) / LOGIC_BLOCK_BYTES;
	if (blocks == 0) blocks = 1;

	char *jdev = json_esc(device), *jdrv = json_esc(driver), *jout = json_esc(outfile);
	GString *js = g_string_new(NULL);
	g_string_append_printf(js, "{\n  \"ok\": true,\n");
	g_string_append_printf(js, "  \"device\": \"%s\",\n  \"driver\": \"%s\",\n  \"mode\": \"LOGIC\",\n",
			       jdev, jdrv);
	g_string_append_printf(js, "  \"capture_mode\": \"%s\",\n", stream ? "logic-stream" : "logic");
	g_string_append_printf(js, "  \"samplerate_hz\": %" G_GUINT64_FORMAT ",\n", samplerate);
	g_string_append_printf(js, "  \"channels_enabled\": %u,\n", en_ch);
	g_string_append_printf(js, "  \"samples_per_channel\": %" G_GUINT64_FORMAT ",\n", samples_per_ch);
	g_string_append_printf(js, "  \"logic_bytes\": %llu,\n", g_cap.logic_bytes);
	if (g_cap.acc_capped) g_string_append_printf(js, "  \"capped\": true,\n");
	if (g_cap.overflow) g_string_append_printf(js,
		"  \"overflow\": true,\n  \"warning\": \"SR_DF_OVERFLOW: FPGA FIFO overran (USB too slow); samples dropped\",\n");
	g_string_append_printf(js, "  \"outfile\": \"%s\",\n", jout);
	g_string_append_printf(js, "  \"probes\": [");
	for (unsigned c = 0; c < en_ch; c++) {
		char *jn = json_esc(names[c] ? names[c] : "?");
		g_string_append_printf(js, "%s{\"index\":%d,\"name\":\"%s\"}", c ? "," : "", idxs[c], jn);
		g_free(jn);
	}
	g_string_append_printf(js, "]\n}\n");
	g_free(jdev); g_free(jdrv); g_free(jout);

	GString *hdr = g_string_new(NULL);
	gen_logic_header(hdr, driver, samplerate, samples_per_ch, idxs, names, en_ch, blocks);
	int wrc = write_logic_dsl(outfile, hdr, js, idxs, chans, en_ch);

	fputs(js->str, stdout);
	if (wrc != 0) fprintf(stderr, "WARNING: failed to write %s\n", outfile);
	else fprintf(stderr, "wrote %s (%" G_GUINT64_FORMAT " samples/ch x %u ch, %"
		     G_GUINT64_FORMAT " block(s)/ch)\n", outfile, samples_per_ch, en_ch, blocks);

	g_string_free(js, TRUE); g_string_free(hdr, TRUE);
	for (unsigned c = 0; c < en_ch; c++) g_byte_array_free(chans[c], TRUE);
	return wrc == 0 ? 0 : 1;
}

/* LOGIC (DSLogic) capture path — forked from the DSO path: Buffer (deep one-shot) or
 * Stream (continuous, may overflow). RLE forced off; raw 1-bit/sample CROSS data is
 * de-interleaved into per-channel bit-packed streams matching the .dsl L-<idx> layout. */
static int run_logic(struct sr_dev_inst *sdi, struct ds_device_full_info *info,
		     GKeyFile *kf, uint64_t cfg_sr, uint64_t cfg_depth, double cfg_dur,
		     int stream, const char *outfile)
{
	/* operation mode FIRST — it force-enables ALL probes (dsl_adjust_probes), so any
	 * per-channel enable must come after it. */
	ds_set_actived_device_config(NULL, NULL, SR_CONF_OPERATION_MODE,
				     g_variant_new_int16(stream ? LO_OP_STREAM : LO_OP_BUFFER));
	/* v1: force RLE OFF for predictable raw 1-bit/sample data */
	ds_set_actived_device_config(NULL, NULL, SR_CONF_RLE, g_variant_new_boolean(FALSE));

	/* threshold is device-global: coarse family (reloads the FPGA bitstream — slow) +
	 * fine VTH voltage to the DAC (the per-signal knob). */
	if (kf) {
		char *th = cfg_str(kf, "capture", "threshold");
		if (th) {
			/* parse the numeric threshold and split on >=4 V (the only families are
			 * 5 V and the 1.8/2.5/3.3 V one) — a substring '5' would misroute "2.5V". */
			int fam = (g_ascii_strtod(th, NULL) >= 4.0) ? SR_TH_5V0 : SR_TH_3V3;
			ds_set_actived_device_config(NULL, NULL, SR_CONF_THRESHOLD, g_variant_new_int16(fam));
			g_free(th);
		}
		char *vth = cfg_str(kf, "capture", "vth");
		if (vth) {
			ds_set_actived_device_config(NULL, NULL, SR_CONF_VTH,
						     g_variant_new_double(g_ascii_strtod(vth, NULL)));
			g_free(vth);
		}
		/* per-channel enables (AFTER operation mode); [channelN] keyed by channel index */
		for (GSList *it = sdi ? sdi->channels : NULL; it; it = it->next) {
			struct sr_channel *ch = it->data;
			if (ch->type != SR_CHANNEL_LOGIC) continue;
			char grp[16]; snprintf(grp, sizeof grp, "channel%d", ch->index);
			if (g_key_file_has_group(kf, grp) && g_key_file_has_key(kf, grp, "enabled", NULL)) {
				gboolean en = g_key_file_get_boolean(kf, grp, "enabled", NULL);
				ds_set_actived_device_config(ch, NULL, SR_CONF_PROBE_EN, g_variant_new_boolean(en));
			}
		}
	}

	/* samplerate: validate against the runtime list (reflects channel count); reject if
	 * invalid. Re-set AFTER enables since channel-mode changes can clamp it silently. */
	if (cfg_sr) {
		if (!samplerate_ok(cfg_sr)) {
			char m[128]; snprintf(m, sizeof m, "samplerate %" G_GUINT64_FORMAT
				" Hz not valid for this device/channel count", cfg_sr);
			emit_err(m); return 1;
		}
		ds_set_actived_device_config(NULL, NULL, SR_CONF_SAMPLERATE, g_variant_new_uint64(cfg_sr));
	}
	uint64_t samplerate = cfg_u64(NULL, SR_CONF_SAMPLERATE, 0);

	/* depth (per channel): explicit samples, or duration_s * samplerate */
	uint64_t want = cfg_depth;
	if (!want && cfg_dur > 0 && samplerate) want = (uint64_t)(cfg_dur * (double)samplerate + 0.5);
	if (want) ds_set_actived_device_config(NULL, NULL, SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(want));
	uint64_t depth = cfg_u64(NULL, SR_CONF_LIMIT_SAMPLES, want);

	ds_trigger_set_en(0);   /* free-running: no trigger condition */

	unsigned en_ch = 0;
	for (GSList *it = sdi ? sdi->channels : NULL; it; it = it->next) {
		struct sr_channel *ch = it->data;
		if (ch->type == SR_CHANNEL_LOGIC && ch->enabled) en_ch++;
	}
	if (en_ch == 0) { emit_err("no logic channels enabled"); return 1; }

	g_cap.end = 0; g_cap.overflow = 0;
	int r = ds_start_collect();
	if (r != SR_OK) { printf("{\"ok\":false,\"error\":\"start_collect %d\"}\n", r); return 1; }
	if (!stream) {  /* Buffer: wait for SR_DF_END, budget scaled to depth/rate */
		double expect_s = (depth && samplerate) ? (double)depth / (double)samplerate : 0;
		double budget_s = expect_s > 0 ? expect_s * 2 + 5 : 30;
		gint64 t0 = g_get_monotonic_time();
		while (!g_cap.end && (g_get_monotonic_time() - t0) < (gint64)(budget_s * 1e6)) g_usleep(50000);
	} else {  /* Stream: wait for the first data BEFORE timing the window — sr_session_stop()
		   * drops an abort issued before sr_session_run() flips session->running, which a
		   * short duration_s can race against. Then run for duration_s (default 1 s). */
		gint64 ts = g_get_monotonic_time();
		while (!g_cap.logic_bytes && !g_cap.end &&
		       (g_get_monotonic_time() - ts) < (gint64)(5e6))
			g_usleep(2000);
		double run_s = cfg_dur > 0 ? cfg_dur : 1.0;
		gint64 t0 = g_get_monotonic_time();
		while (!g_cap.end && (g_get_monotonic_time() - t0) < (gint64)(run_s * 1e6)) g_usleep(10000);
	}
	ds_stop_collect();

	if (!g_cap.logic_acc || g_cap.logic_acc->len == 0) { emit_err("no logic data captured"); return 1; }

	/* collect the enabled logic channels in feed order (position = wire de-interleave
	 * index, ch->index = the .dsl L-<idx> / probe<idx> number) */
	int idxs[MAXCH]; const char *names[MAXCH]; unsigned nch = 0;
	for (GSList *it = sdi ? sdi->channels : NULL; it && nch < MAXCH; it = it->next) {
		struct sr_channel *ch = it->data;
		if (ch->type != SR_CHANNEL_LOGIC || !ch->enabled) continue;
		idxs[nch] = ch->index; names[nch] = ch->name ? ch->name : "?"; nch++;
	}
	return logic_write_capture(outfile, info->name, info->driver_name, stream, samplerate,
				   g_cap.logic_acc->data, g_cap.logic_acc->len, en_ch, idxs, names);
}

int main(int argc, char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		fprintf(stderr, "usage: dscap <config.ini>   |   dscap enumerate\n");
		return 2;
	}
	/* offline self-test: run the real LOGIC de-interleave + .dsl writer on synthetic
	 * wire data, so the container/format can be round-tripped through scripts/dslread.py
	 * with no device attached. Per-channel packed byte b = (c*131 + b*17 + 0x5A)&0xFF. */
	if (!strcmp(argv[1], "selftest-logic")) {
		const char *out = argc > 2 ? argv[2] : "selftest-logic.dsl";
		const unsigned en_ch = 3, atoms = 5;            /* 3 ch x 5 atoms = 320 samples/ch */
		const uint64_t bpc = (uint64_t)atoms * LOGIC_ATOMIC_SIZE;
		uint64_t wire_len = (uint64_t)atoms * en_ch * LOGIC_ATOMIC_SIZE;
		uint8_t *wire = malloc(wire_len);
		for (unsigned k = 0; k < atoms; k++)
			for (unsigned c = 0; c < en_ch; c++)
				for (unsigned j = 0; j < LOGIC_ATOMIC_SIZE; j++) {
					uint64_t b = (uint64_t)k * LOGIC_ATOMIC_SIZE + j;   /* byte index within channel */
					wire[(k * en_ch + c) * LOGIC_ATOMIC_SIZE + j] = (uint8_t)((c * 131 + b * 17 + 0x5A) & 0xFF);
				}
		(void)bpc;
		int idxs[3] = {0, 1, 2};
		const char *names[3] = {"0", "1", "2"};
		g_cap.logic_bytes = wire_len;
		int rc = logic_write_capture(out, "Selftest", "DSLogic", 0, 1000000,
					     wire, wire_len, en_ch, idxs, names);
		free(wire);
		return rc;
	}
	int do_enum = !strcmp(argv[1], "enumerate");
	const char *cfgpath = do_enum ? NULL : argv[1];

	/* ---- load config (INI) ---- */
	GKeyFile *kf = NULL;
	char *cfg_device = NULL, *cfg_mode = NULL, *cfg_output = NULL, *cfg_res = NULL;
	uint64_t cfg_sr = 0, cfg_depth = 0; double cfg_dur = 0;
	if (cfgpath) {
		kf = g_key_file_new();
		GError *e = NULL;
		if (!g_key_file_load_from_file(kf, cfgpath, G_KEY_FILE_NONE, &e)) {
			char m[256]; snprintf(m, sizeof m, "cannot read config '%s': %s", cfgpath, e ? e->message : "?");
			emit_err(m); if (e) g_error_free(e); return 1;
		}
		cfg_device = cfg_str(kf, "capture", "device");
		cfg_mode   = cfg_str(kf, "capture", "mode");
		cfg_output = cfg_str(kf, "capture", "output");
		cfg_res    = cfg_str(kf, "capture", "res_dir");
		char *srs  = cfg_str(kf, "capture", "samplerate");
		cfg_sr = parse_freq(srs); g_free(srs);
		char *durs = cfg_str(kf, "capture", "duration_s");
		cfg_dur = durs ? g_ascii_strtod(durs, NULL) : 0; g_free(durs);
		char *deps = cfg_str(kf, "capture", "depth");
		cfg_depth = deps ? g_ascii_strtoull(deps, NULL, 10) : 0; g_free(deps);
	}

	/* diagnostic: DSCAP_SRLOG=<0..5> routes libsigrok4DSL's own log (driver
	 * transfer/stop tracing) to stderr. Must precede ds_lib_init so the level is
	 * applied when the lib creates its log context. 3=info (bmFORCE_RDY,
	 * finish_acquisition), 5=detail (per-transfer status/bytes). stdout JSON unaffected. */
	const char *srlog = getenv("DSCAP_SRLOG");
	if (srlog && *srlog) ds_log_level(atoi(srlog));

	if (ds_lib_init() != SR_OK) { fprintf(stderr, "ds_lib_init failed\n"); return 1; }
	const char *env_res = getenv("DSVIEW_RES_DIR");
	const char *res_dir = cfg_res ? cfg_res : (env_res && *env_res ? env_res : FW_DIR);
	fprintf(stderr, "firmware resource dir: %s\n", res_dir);
	ds_set_firmware_resource_dir(res_dir);
	ds_set_datafeed_callback(feed_cb);

	struct ds_device_base_info *list = NULL; int n = 0;
	if (ds_get_device_list(&list, &n) != SR_OK) { fprintf(stderr, "list failed\n"); return 1; }
	int pick = -1;
	for (int i = 0; i < n; i++) {
		if (strstr(list[i].name, "Demo")) continue;
		if (cfg_device && !strstr(list[i].name, cfg_device)) continue;  /* name filter */
		pick = i; break;
	}
	if (pick < 0) { emit_err(cfg_device ? "configured device not found" : "no DSLogic/DSCope device found"); ds_lib_exit(); return 0; }

	ds_device_handle want = list[pick].handle;
	fprintf(stderr, "Activating %s ...\n", list[pick].name);
	int r = ds_active_device(want);
	if (r != SR_OK) { emit_err("activate failed"); ds_lib_exit(); return 1; }

	struct ds_device_full_info info; memset(&info, 0, sizeof info);
	ds_get_actived_device_info(&info);
	if (info.handle != want) {
		emit_err("open fell back to Demo (warm device; power-cycle/re-plug)");
		ds_lib_exit(); return 2;
	}

	struct sr_dev_inst *sdi = info.di;

	if (do_enum) {
		printf("{\"ok\":true,\"device\":\"%s\",\"driver\":\"%s\",\"mode\":\"%s\"}\n",
		       info.name, info.driver_name, modename(ds_get_actived_device_mode()));
		ds_lib_exit(); return 0;
	}

	/* ---- select work mode ----
	 * single       = DSO INSTANT deep one-shot (default; gapless within the buffer)
	 * roll         = DSO continuous, best-effort for duration_s (NOT guaranteed gapless)
	 * logic        = DSLogic LOGIC Buffer (deep one-shot)
	 * logic-stream = DSLogic LOGIC Stream (continuous, may overflow)
	 * stream       = ANALOG <=10 MHz continuous DAQ (deferred) */
	const char *mode_name = cfg_mode ? cfg_mode : "single";
	int want_single = !strcmp(mode_name, "single");
	int want_roll   = !strcmp(mode_name, "roll");
	int want_stream = !strcmp(mode_name, "stream");
	int want_logic  = !strcmp(mode_name, "logic");
	int want_logic_stream = !strcmp(mode_name, "logic-stream");
	if (!want_single && !want_roll && !want_stream && !want_logic && !want_logic_stream) {
		char m[112]; snprintf(m, sizeof m,
			"unknown mode '%s' (use single|roll|logic|logic-stream|stream)", mode_name);
		emit_err(m); ds_lib_exit(); return 1;
	}
	int want_mode = (want_logic || want_logic_stream) ? LOGIC : (want_stream ? ANALOG : DSO);
	if (ds_get_actived_device_mode() != want_mode)
		ds_set_actived_device_config(NULL, NULL, SR_CONF_DEVICE_MODE, g_variant_new_int16(want_mode));
	int mode = ds_get_actived_device_mode();

	if (want_logic || want_logic_stream) {
		if (mode != LOGIC) { emit_err("could not enter LOGIC mode"); ds_lib_exit(); return 1; }
		char *lout = expand_out(cfg_output);
		int rc = run_logic(sdi, &info, kf, cfg_sr, cfg_depth, cfg_dur, want_logic_stream, lout);
		g_free(lout); ds_lib_exit(); return rc;
	}
	if (want_stream) {  /* ANALOG continuous DAQ (<=10 MHz); shares the DSO accumulation tail */
		if (mode != ANALOG) { emit_err("could not enter ANALOG mode"); ds_lib_exit(); return 1; }
		g_cap.analog = 1;
	} else {
		if (mode != DSO) { emit_err("could not enter DSO mode"); ds_lib_exit(); return 1; }
		g_cap.instant = want_single;   /* single = INSTANT deep one-shot; roll = continuous */
	}

	if (g_cap.analog)
		ds_trigger_set_en(0);   /* free-running continuous acquisition */
	else
		ds_set_actived_device_config(NULL, NULL, SR_CONF_TRIGGER_SOURCE,
					     g_variant_new_byte(DSO_TRIGGER_AUTO));

	/* The scope channel type is mode-dependent: DSO mode exposes SR_CHANNEL_DSO,
	 * ANALOG mode SR_CHANNEL_ANALOG (channel_modes[] in dsl.h). Filtering on the
	 * wrong type silently drops every per-channel setting in ANALOG mode. */
	int scope_ch_type = g_cap.analog ? SR_CHANNEL_ANALOG : SR_CHANNEL_DSO;

	/* ---- apply per-channel settings from [channelN] groups ---- */
	if (kf) {
		unsigned p = 0;
		for (GSList *it = sdi ? sdi->channels : NULL; it; it = it->next) {
			struct sr_channel *ch = it->data;
			if (ch->type != scope_ch_type) continue;
			char grp[16]; snprintf(grp, sizeof grp, "channel%u", p++);
			if (!g_key_file_has_group(kf, grp)) continue;
			if (g_key_file_has_key(kf, grp, "enabled", NULL)) {
				gboolean en = g_key_file_get_boolean(kf, grp, "enabled", NULL);
				ds_set_actived_device_config(ch, NULL, SR_CONF_PROBE_EN, g_variant_new_boolean(en));
			}
			char *vs = cfg_str(kf, grp, "vdiv");
			if (vs) { ds_set_actived_device_config(ch, NULL, SR_CONF_PROBE_VDIV, g_variant_new_uint64(parse_vdiv(vs))); g_free(vs); }
			char *pf = cfg_str(kf, grp, "probe_factor");
			if (pf) { ds_set_actived_device_config(ch, NULL, SR_CONF_PROBE_FACTOR, g_variant_new_uint64(g_ascii_strtoull(pf, NULL, 10))); g_free(pf); }
			char *cp = cfg_str(kf, grp, "coupling");
			if (cp) { int c = !g_ascii_strcasecmp(cp, "AC") ? SR_AC_COUPLING : SR_DC_COUPLING;
				  ds_set_actived_device_config(ch, NULL, SR_CONF_PROBE_COUPLING, g_variant_new_byte(c)); g_free(cp); }
			char *of = cfg_str(kf, grp, "offset");   /* vertical position code 0-255 (clamped 10..245) */
			if (of) { ds_set_actived_device_config(ch, NULL, SR_CONF_PROBE_OFFSET,
					g_variant_new_uint16((uint16_t)g_ascii_strtoull(of, NULL, 10))); g_free(of); }
		}
	}

	/* ---- samplerate: validate against the device's runtime list, reject if bad ---- */
	if (cfg_sr) {
		if (!samplerate_ok(cfg_sr)) {
			char m[128]; snprintf(m, sizeof m, "samplerate %" G_GUINT64_FORMAT " Hz not valid for this device/channel count", cfg_sr);
			emit_err(m); ds_lib_exit(); return 1;
		}
		ds_set_actived_device_config(NULL, NULL, SR_CONF_SAMPLERATE, g_variant_new_uint64(cfg_sr));
	}
	uint64_t samplerate = cfg_u64(NULL, SR_CONF_SAMPLERATE, 0);
	char *outfile = expand_out(cfg_output);

	unsigned en_ch = 0;
	for (GSList *it = sdi ? sdi->channels : NULL; it; it = it->next) {
		struct sr_channel *ch = it->data;
		if (ch->type == scope_ch_type && ch->enabled) en_ch++;
	}

	/* ---- depth / INSTANT (set AFTER channel enables + samplerate, before start) ----
	 * single: SR_CONF_INSTANT selects the deep one-shot path and defaults
	 * LIMIT_SAMPLES to the per-channel max (hw_depth/8/en_ch). Optionally bound it
	 * via [capture] depth (samples) or duration_s (seconds * samplerate);
	 * reject-not-clamp against the instant max. */
	uint64_t depth = 0;
	if (g_cap.instant) {
		ds_set_actived_device_config(NULL, NULL, SR_CONF_INSTANT, g_variant_new_boolean(TRUE));
		uint64_t inst_max = cfg_u64(NULL, SR_CONF_LIMIT_SAMPLES, 0);
		uint64_t want = cfg_depth;
		if (!want && cfg_dur > 0 && samplerate)
			want = (uint64_t)(cfg_dur * (double)samplerate + 0.5);
		if (want) {
			if (inst_max && want > inst_max) {
				char m[160]; snprintf(m, sizeof m,
					"depth %" G_GUINT64_FORMAT " samples/ch exceeds this device's instant max %"
					G_GUINT64_FORMAT, want, inst_max);
				emit_err(m); ds_lib_exit(); return 1;
			}
			ds_set_actived_device_config(NULL, NULL, SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(want));
		}
		depth = cfg_u64(NULL, SR_CONF_LIMIT_SAMPLES, want ? want : inst_max);
	} else if (g_cap.analog) {  /* ANALOG continuous: bound by depth/duration so it self-stops */
		uint64_t want = cfg_depth;
		if (!want && cfg_dur > 0 && samplerate) want = (uint64_t)(cfg_dur * (double)samplerate + 0.5);
		if (want) ds_set_actived_device_config(NULL, NULL, SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(want));
		depth = cfg_u64(NULL, SR_CONF_LIMIT_SAMPLES, want);
	} else {  /* roll: best-effort continuous — warn loudly when USB can't keep up */
		double bw = (double)samplerate * (double)(en_ch ? en_ch : 1);  /* bytes/s @ 1 B/sample */
		if (bw > 300e6)
			fprintf(stderr, "WARNING: mode=roll at ~%.0f MB/s exceeds sustainable USB bandwidth "
				"(~300 MB/s); the capture will have gaps (not gapless). Use mode=single "
				"for a gapless deep buffer.\n", bw / 1e6);
	}

	g_cap.end = 0;
	/* INSTANT single: cap the per-channel accumulation at the requested depth so the
	 * device's redundant zero tail packet is dropped (matches DSView's total cap). */
	g_cap.cap_spc = g_cap.instant ? (unsigned)depth : 0;
	r = ds_start_collect();
	if (r != SR_OK) { printf("{\"ok\":false,\"error\":\"start_collect %d\"}\n", r); ds_lib_exit(); return 1; }
	if (g_cap.instant) {  /* deep one-shot: wait for SR_DF_END, budget scaled to depth/rate */
		double expect_s = (depth && samplerate) ? (double)depth / (double)samplerate : 0;
		double budget_s = expect_s > 0 ? expect_s * 2 + 5 : 30;
		gint64 t0 = g_get_monotonic_time();
		while (!g_cap.end && (g_get_monotonic_time() - t0) < (gint64)(budget_s * 1e6))
			g_usleep(50000);
	} else {  /* roll / stream: continuous, concatenate chunks for duration_s (default 1 s).
		   * Wait for the first data packet BEFORE timing the window: sr_session_stop()
		   * drops an abort issued before sr_session_run() flips session->running, and the
		   * DSCope FPGA arm can outlast a short duration_s. Data only flows once the
		   * session is running, so gating on it guarantees the later stop is honored. */
		gint64 ts = g_get_monotonic_time();
		while (!g_cap.dso_frames && !g_cap.end &&
		       (g_get_monotonic_time() - ts) < (gint64)(5e6))
			g_usleep(2000);
		double run_s = cfg_dur > 0 ? cfg_dur : 1.0;
		gint64 t0 = g_get_monotonic_time();
		while (!g_cap.end && (g_get_monotonic_time() - t0) < (gint64)(run_s * 1e6))
			g_usleep(10000);
	}
	ds_stop_collect();

	/* converge: expose each channel's accumulated stream; trim to the common length */
	g_cap.spc = 0;
	for (unsigned p = 0; p < g_cap.np; p++) {
		unsigned len = g_cap.acc[p] ? g_cap.acc[p]->len : 0;
		g_cap.buf[p] = g_cap.acc[p] ? g_cap.acc[p]->data : NULL;
		if (p == 0 || len < g_cap.spc) g_cap.spc = len;
	}

	if (g_cap.np == 0 || g_cap.spc == 0) {
		printf("{\"ok\":false,\"error\":\"no %s samples captured\"}\n",
		       g_cap.analog ? "ANALOG" : "DSO"); ds_lib_exit(); return 1;
	}

	/* ---- build the JSON summary (used for stdout AND embedded in the .dsl) ---- */
	const char *modestr = g_cap.analog ? "ANALOG" : "DSO";
	const char *capmode = g_cap.analog ? "stream" : (g_cap.instant ? "single" : "roll");
	char *jdev = json_esc(info.name), *jdrv = json_esc(info.driver_name), *jout = json_esc(outfile);
	GString *js = g_string_new(NULL);
	g_string_append_printf(js, "{\n  \"ok\": true,\n");
	g_string_append_printf(js, "  \"device\": \"%s\",\n  \"driver\": \"%s\",\n  \"mode\": \"%s\",\n",
			       jdev, jdrv, modestr);
	g_string_append_printf(js, "  \"samplerate_hz\": %" G_GUINT64_FORMAT ",\n", samplerate);
	g_string_append_printf(js, "  \"dso_frames\": %u,\n  \"samples_per_channel\": %u,\n",
			       g_cap.dso_frames, g_cap.spc);
	g_string_append_printf(js, "  \"capture_mode\": \"%s\",\n  \"instant\": %s,\n",
			       capmode, g_cap.instant ? "true" : "false");
	if (g_cap.acc_capped)
		g_string_append_printf(js, "  \"capped\": true,\n");
	if (g_cap.analog) {  /* ANALOG: robust continuous (decoupled deep transfers), bench-unverified */
		g_string_append_printf(js,
			"  \"continuous\": true,\n  \"note\": \"ANALOG continuous DAQ (<=10 MHz); robust path, bench-unverified\",\n");
	} else if (!g_cap.instant) {  /* roll: loud not-gapless honesty (expected vs actual) */
		unsigned long long expected =
			(unsigned long long)((cfg_dur > 0 ? cfg_dur : 1.0) * (double)samplerate + 0.5);
		double ratio = expected ? (double)g_cap.spc / (double)expected : 0;
		g_string_append_printf(js,
			"  \"gapless\": false,\n  \"expected_samples\": %llu,\n  \"capture_ratio\": %.4f,\n"
			"  \"warning\": \"continuous DSO roll is best-effort; not gapless above sustainable USB bandwidth\",\n",
			expected, ratio);
	}
	g_string_append_printf(js, "  \"outfile\": \"%s\",\n", jout);
	/* ADC range the device actually reports (DSCope U3P100: 10/245); samples
	 * at/beyond it are clipped. The 0/255 init is only a fallback for a device
	 * that doesn't expose SR_CONF_REF_MIN/MAX. */
	uint32_t ref_min = 0, ref_max = 255;
	have_u32(SR_CONF_REF_MIN, &ref_min);
	have_u32(SR_CONF_REF_MAX, &ref_max);
	g_string_append_printf(js, "  \"channels\": [\n");
	for (unsigned p = 0; p < g_cap.np; p++) {
		struct sr_channel *ch = nth_capture_channel(sdi, p, g_cap.analog);
		uint64_t vdiv = ch ? ch->vdiv : 0;
		int hwoff   = ch ? ch->hw_offset : 128;
		uint64_t fac = ch ? cfg_u64(ch, SR_CONF_PROBE_FACTOR, 1) : 1;
		double k = (double)vdiv * DS_CONF_DSO_VDIVS / 255.0 * (double)fac; /* mV/code (matches DSView) */
		unsigned cmin = 255, cmax = 0; unsigned long long sum = 0;
		for (unsigned i = 0; i < g_cap.spc; i++) {
			unsigned char v = g_cap.buf[p][i];
			if (v < cmin) cmin = v;
			if (v > cmax) cmax = v;
			sum += v;
		}
		double mean = g_cap.spc ? (double)sum / g_cap.spc : 0;
		int clipped = (cmin <= ref_min || cmax >= ref_max);
		char *jname = json_esc(ch && ch->name ? ch->name : "?");
		g_string_append_printf(js,
			"    {\"name\":\"%s\",\"vdiv_mv\":%" G_GUINT64_FORMAT ",\"probe_factor\":%" G_GUINT64_FORMAT
			",\"hw_offset\":%d,\"code_min\":%u,\"code_max\":%u,\"code_mean\":%.1f,"
			"\"v_min_mv\":%.2f,\"v_max_mv\":%.2f,\"v_mean_mv\":%.2f,\"vpp_mv\":%.2f,\"clipped\":%s}%s\n",
			jname, vdiv, fac, hwoff,
			cmin, cmax, mean,
			(hwoff - (double)cmax) * k, (hwoff - (double)cmin) * k,
			(hwoff - mean) * k, ((double)cmax - cmin) * k,
			clipped ? "true" : "false",
			(p + 1 < g_cap.np) ? "," : "");
		g_free(jname);
	}
	g_string_append_printf(js, "  ]\n}\n");
	g_free(jdev); g_free(jdrv); g_free(jout);

	/* ---- write the native .dsl (header + O-<ch>/0 + summary.json) ---- */
	GString *hdr = g_string_new(NULL);
	gen_header(hdr, &g_cap, sdi, info.driver_name, samplerate);
	int wrc = write_dsl(outfile, hdr, js, &g_cap, sdi);

	fputs(js->str, stdout);
	if (wrc != 0) fprintf(stderr, "WARNING: failed to write %s\n", outfile);
	else fprintf(stderr, "wrote %s (%u samples/ch x %u ch + header + summary.json)\n",
		     outfile, g_cap.spc, g_cap.np);

	g_string_free(js, TRUE); g_string_free(hdr, TRUE);
	for (unsigned p = 0; p < g_cap.np; p++) if (g_cap.acc[p]) g_byte_array_free(g_cap.acc[p], TRUE);
	ds_lib_exit();
	return wrc == 0 ? 0 : 1;
}
