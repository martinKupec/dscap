# `dscap` — headless DSLogic / DSCope capture (agent guide)

A small CLI that drives a DreamSourceLab **DSLogic** (logic analyzer) or
**DSCope** (oscilloscope) without the DSView GUI, using the same
`libsigrok4DSL` library the GUI runs. Output is designed for agents:
**a JSON summary on stdout**, plus a **native `.dsl` capture file** with that
same JSON embedded inside it.

You do not need to understand libsigrok to use this. Read stdout as JSON; if you
need the raw waveform, read the `.dsl`. That's it.

---

## Invoke

```
./dscap enumerate         # identify the attached device (JSON: device/driver/mode)
./dscap <config.ini>      # run the measurement described by the INI, write a .dsl
```

- **stdout** = exactly one JSON object. Parse this. Nothing else goes to stdout.
- **stderr** = human/progress logs. Ignore when parsing.
- **exit code**: `0` success, non-zero on failure (see Errors).

### Config file (INI)

A measurement is described by an INI file (see `capture.ini.example`). Settings are
**validated against the live device and rejected if out of range** (not clamped).

```ini
[capture]
device     = DSCope U3P100   ; optional substring; first match if omitted
mode       = single          ; single (default) | roll | logic | logic-stream | stream
samplerate = 500M            ; k/M/G suffixes; rejected if not in device's valid list
; depth    = 1000000         ; single: per-channel sample count (capped at device max)
; duration_s = 0.002         ; single: alt to depth; roll: wall-clock window (default 1 s)
output     = caps/run-{ts}.dsl   ; {ts} -> YYMMDD-HHMMSS
;res_dir   = /opt/dsview/share/DSView/res  ; DSView firmware/bitstream dir (see below)

[channel0]                   ; one [channelN] section per scope channel
enabled      = true
vdiv         = 10mV          ; mV/V suffixes
probe_factor = 10
coupling     = DC            ; DC | AC
offset       = 245           ; vertical position 0-255 (clamped 10..245), where 0 V sits;
                             ; omit -> mid-scale 128 (can rail a DC-offset input)
```

The `offset` (vertical position / vpos) materially changes the raw sample codes: it
sets where 0 V lands in the 8-bit ADC range. To reproduce a GUI capture, match its
per-channel `vOffset` value. Calibrated voltages account for it, but raw codes won't
match unless the offset does.

Boundaries (samplerate/vdiv ranges, single-shot depth, the per-probe samplerate split
— 1 GHz at 1 probe → 500 MHz at 2 probes) are **device-specific** and queried at runtime,
so the same tool adapts to any DSLogic/DSCope model.

### Capture modes

| `mode` | what it does | stop condition | gapless? |
|--------|--------------|----------------|----------|
| `single` (default) | DSO **INSTANT** deep one-shot — fills the on-board buffer (up to ~128 Mi/ch) and drains it | buffer full (`depth`/`duration_s` bound it; else the device max) | **yes**, within the buffer |
| `roll` | DSO **continuous**, best-effort — concatenates rolling chunks for `duration_s` | wall-clock `duration_s` (default 1 s) | **no** — best-effort; above sustainable USB bandwidth there are gaps |
| `logic` | DSLogic **LOGIC Buffer** — deep one-shot of digital channels | buffer full (`depth`/`duration_s`) | **yes**, within the buffer |
| `logic-stream` | DSLogic **LOGIC Stream** — continuous digital capture | wall-clock `duration_s` (default 1 s) | **no** — emits `overflow` if the FPGA FIFO overruns |
| `stream` | DSCope **ANALOG** ≤10 MHz continuous DAQ (the robust continuous path) | `duration_s` / `depth` (device self-stops), else 1 s | robust (decoupled deep transfers) — **bench-unverified** |

- `single` is the common DSO case. Both DSO modes de-interleave and **concatenate** every
  chunk into one per-channel stream; only the stop condition differs.
- A `single` capture is OOM-guarded at 512 MiB/channel; if it hits that, the JSON sets
  `"capped": true`. Use `depth`/`duration_s` to take a smaller, faster window.
- `roll` is honest about drops: it always reports `"gapless": false` plus
  `expected_samples` and `capture_ratio` (= captured/expected; 1.0 = no gaps), and prints
  a loud stderr warning when the requested rate × channels exceeds ~300 MB/s.

#### LOGIC modes (DSLogic)

- **Threshold** (the key knob, device-global): `[capture] vth` sets the fine input
  threshold *voltage*; `[capture] threshold` picks the coarse level family (a numeric
  value ≥ 4 V → 5 V family, otherwise the 1.8/2.5/3.3 V family — changing it reloads the
  FPGA bitstream and is slow).
- **Channels**: enable a subset via `[channelN]` (keyed by channel index) — fewer enabled
  channels allow a higher max samplerate and more depth/channel. RLE is forced **off** for
  predictable 1-bit/sample raw data.
- The `.dsl` is written as DSView writes it: per-channel `L-<idx>/<block>` members,
  bit-packed (1 bit/sample, LSB = earliest), 2 MiB (2²⁴-sample) blocks. The container
  layout is round-trip validated offline (`dscap selftest-logic <out.dsl>`,
  re-read with `scripts/dslread.py`); real-signal correctness is still bench-pending.

---

## Output contract

### 1. stdout — JSON summary

Always a single object with an `ok` boolean envelope.

Enumerate (`./dscap enumerate`):
```json
{"ok": true, "device": "DSCope U3P100", "driver": "DSCope", "mode": "DSO"}
```

Capture (`./dscap <config.ini>`), DSO example:
```json
{
  "ok": true,
  "device": "DSCope U3P100",
  "driver": "DSCope",
  "mode": "DSO",
  "samplerate_hz": 500000000,
  "dso_frames": 7,
  "samples_per_channel": 500000,
  "capture_mode": "single",
  "instant": true,
  "outfile": "caps/run-260618-173243.dsl",
  "channels": [
    {"name": "0", "vdiv_mv": 1000, "probe_factor": 1, "hw_offset": 128,
     "code_min": 125, "code_max": 127, "code_mean": 126.0,
     "v_min_mv": 39.22, "v_max_mv": 117.65, "v_mean_mv": 78.43, "vpp_mv": 78.43,
     "clipped": false}
  ]
}
```

#### Field reference

| field | meaning |
|-------|---------|
| `ok` | `true` on success, `false` on a handled error (then `error` is set). |
| `error` | present only when `ok=false`; human string, see Errors. |
| `device` / `driver` | model name and driver (`DSCope`, `DSLogic`, …). |
| `mode` | `DSO` (analog scope), `LOGIC` (logic analyzer), or `ANALOG`. |
| `samplerate_hz` | active sample rate, samples/second. |
| `dso_frames` | DSO only: number of chunks received and concatenated. |
| `samples_per_channel` | DSO only: total samples per channel in the saved (concatenated) capture. |
| `capture_mode` | DSO only: `single` (INSTANT deep one-shot) or `roll` (continuous best-effort). |
| `instant` | DSO only: `true` for `single`, `false` for `roll`. |
| `capped` | present (`true`) only if an OOM guard stopped the capture early (DSO/ANALOG: 512 MiB/channel; LOGIC: 1 GiB total). |
| `gapless`, `expected_samples`, `capture_ratio`, `warning` | `roll` only: `gapless` is always `false`; `capture_ratio` = `samples_per_channel / expected_samples` (1.0 = no gaps, <1.0 = USB underrun dropped samples). |
| `continuous`, `note` | ANALOG `stream` only: `continuous` is `true`; `note` flags it as the robust-but-bench-unverified path. |
| `channels_enabled`, `probes[]` | LOGIC only: number of enabled digital channels, and a `{index,name}` list of them. |
| `logic_bytes` | LOGIC only: total raw wire bytes received from the device. |
| `overflow` | LOGIC `logic-stream` only, present (`true`) if the FPGA FIFO overran (USB too slow) and samples were dropped. |
| `outfile` | path of the written `.dsl` (capture mode only). |
| `channels[]` | DSO only: one entry per enabled scope channel. |

#### Channel fields (DSO)

Raw ADC is **8-bit (codes 0–255)**; `hw_offset` is the mid-scale code (≈128 = 0 V).
Voltages are in **millivolts**. Conversion (same mapping as DSView):

```
v_mv(code) = (hw_offset - code) * vdiv_mv * 10 / 255 * probe_factor
```

(`10` = `DS_CONF_DSO_VDIVS`, the number of vertical divisions spanning full scale;
`255` is the full 8-bit code span, matching DSView's own conversion.)

| field | meaning |
|-------|---------|
| `vdiv_mv` | volts/division setting, in mV. |
| `probe_factor` | probe attenuation (1 = ×1, 10 = ×10). |
| `hw_offset` | ADC code corresponding to 0 V. |
| `code_min/max/mean` | raw ADC code stats. |
| `v_min_mv/v_max_mv/v_mean_mv` | calibrated voltages (mV). Note `v_min` ↔ `code_max`. |
| `vpp_mv` | peak-to-peak, `(code_max - code_min) * scale`. |
| `clipped` | `true` if any sample hit the ADC's reported range (`REF_MIN`/`REF_MAX`); the trace is railed and voltages are unreliable. |

### 2. `capture.dsl` — native data file

A `.dsl` is a **plain zip** in DreamSourceLab's native layout:

```
capture.dsl
├── header        # native ini: samplerate, mode, per-channel vdiv/offset
├── O-0/0, O-1/0  # raw 8-bit samples, one member per DSO channel (L-… for logic)
└── summary.json  # the SAME JSON printed to stdout (added by this tool)
```

To get the summary back out of a stored file without re-running anything:
```
unzip -p capture.dsl summary.json
```

The `.dsl` is **directly openable in the DSView GUI** for visual inspection, and
being a standard zip it is trivial to post-process with any tooling. Raw channel
samples live in the `O-<ch>/0` members as one byte per sample (apply the same
calibration formula above to convert to volts).

### Convert / visualize (`scripts/`)

Self-contained Python converters (stdlib + numpy; matplotlib for the plot) read
both analog (`O-<ch>`) and logic (`L-<ch>`) `.dsl` files. They share
`scripts/dslread.py`, which parses the header, loads per-channel samples, and
applies the calibration above (preferring the embedded `summary.json` when
present). Validated bit-exact against the reference decoders.

```
./scripts/dsl2png.py capture.dsl [-o out.png] [--channels 0,1] [--width-px N]
./scripts/dsl2npy.py capture.dsl [-o out.npz] [--compress] [--raw]
```

- **png** — stacked per-channel traces (analog in mV, logic as step traces);
  deep captures are reduced with a per-pixel min/max envelope so spikes survive.
- **npz** — `ch<idx>_codes`/`ch<idx>_mv` (analog) or `ch<idx>_bits` (logic) plus
  `samplerate_hz` and channel metadata. The time vector is implicit:
  `t = np.arange(n) / samplerate_hz`.

### Auto-range (`scripts/autorange.py`)

Drives `dscap` itself: short range-find captures → per-channel `vdiv`/`offset`
selection → one long capture. Avoids hand-tuning `vdiv` and re-running on `clipped`.
```
scripts/autorange.py --ch0 10 --ch1 10                 # 2 ch, both 10:1, AC
scripts/autorange.py --ch0 1  --ch1 off                # CH0 only, 1:1
scripts/autorange.py --ch0 10 --ch1 off --coupling DC --capture-s 0.05 --rangefind-s 0.02
```
Per-channel arg = probe factor (`1`/`10`/`100`) or `off`. Picks the smallest `vdiv`
filling ~80% of range without clipping, re-centres `offset` from `code_mean`. Prints
the long capture's JSON; **exit 2** if a channel rails at max 1 V/div (signal >±50 V
node at 10:1 → higher attenuation needed).
- **`--rangefind-s` must span ≥1 envelope/sweep period** (default 13 ms): a
  swept/AM carrier's amplitude varies across the sweep, so a short range-find
  can under-range and then clip at the envelope peak.
- AC-coupled carriers land one-sided in the ADC at `offset=128` (residual DC);
  re-centring reclaims resolution. A *differential* of two channels is unaffected.

---

## Errors

Handled errors return `{"ok": false, "error": "..."}` on stdout:

| `error` contains | meaning | recovery |
|------------------|---------|----------|
| `no DSLogic/DSCope device found` | nothing attached. | plug the device in. |
| `open fell back to '…' (warm device …)` | the device was left in a runtime state by a previous run or the GUI; the lib silently substituted the Demo device. | **power-cycle / re-plug** the unit, close DSView, retry. |
| `activate failed` / `start_collect` | lower-level libsigrok failure. | check `stderr`, re-plug, check the firmware dir. |

Requirements:
- The DSView firmware/bitstream resource dir must exist; the device needs its
  FX2/FPGA firmware uploaded on activation. Resolution order: `[capture] res_dir`
  → `$DSVIEW_RES_DIR` → per-OS compiled default (Linux
  `/opt/dsview/share/DSView/res`, Windows `C:\Program Files\DSView\res`). The
  chosen path is logged to stderr at startup.
- One process at a time may own the device — **close the DSView GUI first**.
- DSO is free-running with auto-trigger; put a signal on a probe or you'll just
  read the ~0 V baseline (noise).

---

## Build

```
git submodule update --init        # fetch third_party/DSView at the pinned commit
make                               # -> ./dscap
# or: cmake -S . -B build && cmake --build build -j   # -> ./build/dscap
```

Needs `glib-2.0`, `libusb-1.0`, `zlib` dev packages. The libsigrok4DSL subset is
built fresh from the pinned DSView submodule; nothing prebuilt is relied on.

---

_Status: config-driven (INI) DSO capture is implemented for **`mode=single`** (INSTANT
deep one-shot, `depth`/`duration_s` bounded) and **`mode=roll`** (continuous best-effort
for `duration_s`, with `capture_ratio` honesty) — both share one per-channel accumulation
path. Plus device select, per-channel vdiv/factor/coupling/enable, runtime samplerate
validation (reject-if-invalid), `{ts}` output, calibration, JSON summary, and the native
`.dsl` writer (verified against a GUI-saved file). DSLogic **`mode=logic`** and
**`mode=logic-stream`** are implemented (threshold/`vth`, per-channel enables, RLE off,
de-interleave to per-channel `L-<idx>` bit-packed blocks); the `.dsl` container is round-trip
validated offline via `dscap selftest-logic` (re-read with `scripts/dslread.py`). **`mode=stream`**
(DSCope ANALOG ≤10 MHz continuous DAQ) is wired through the DSO accumulation tail (writes
`device mode = ANALOG`, `O-<ch>` members). Real-signal LOGIC channel↔index fidelity is the main
item still pending on-bench verification._
