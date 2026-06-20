# Reference captures (real DSView-saved .dsl)

Ground-truth `.dsl` files saved by **DSView v1.3.2** from a **DSCope U3P100**, used
to validate our writer's output format. The `.dsl` binaries are **gitignored**
(local only, large); this README is the committed record. They contain no useful
signal — pure format examination vectors.

## Files

| file | mode | total samples/ch | members | total blocks | samplerate |
|---|---|---|---|---|---|
| `dso-deep-instant_128Misamp_500MHz_2ch.dsl` | DSO INSTANT ("Single"), 2ch | 134217728 (128 Mi) | `O-0/0`,`O-1/0` @ **134 MB** each | 128 | 500 MHz |
| `dso-shallow_1Msamp_20MHz_2ch.dsl` | DSO non-instant ("Start"), 2ch | 1000000 | `O-0/0`,`O-1/0` @ 1 MB each | 1 | 20 MHz |

(originals: `DSCope U3P100-osc-260618-173721.dsl` and `…-173735.dsl`)

## Format facts learned from these (see also storesession.cpp / dsosnapshot.cpp)
- **One zip member per channel** for DSO: `O-<ch>/0` holds ALL samples concatenated
  (8-bit, 1 byte/sample). NOT split into multiple members. The deep file proves a
  single 134 MB member is valid (zip64).
- **`total blocks` is LOGICAL, not a member count**: `ceil(total_samples * unit_bytes
  * num_ch / 2^21)` (DSO LeafBlockPower=21 → 2 MiB unit). Deep: 128Mi×1×2/2Mi = 128;
  shallow: 1M×1×2/2Mi → 1. Our writer hardcodes 1 → must compute this (fix for deep).
- **Channel count splits the hw buffer (depth)**: INSTANT limit = hw_depth/8/en_ch.
  2ch → 128 Mi/ch (two 134 MB members); 1ch → 256 Mi/ch (one ~268 MB `O-0/0`).
  `total blocks` stays 128 (total bytes ≈ fixed 256 MB buffer).
- header `version = 3`, `device mode = 1` (DSO), `capturefile = data`, plus per-probe
  measurement fields (period/max/min/rms/mean/rlen/flen/…) and `hDiv max`/`hDiv min`
  that our writer omits — OPTIONAL (our subset loads fine; these are computed stats).
- extra members `session` (JSON: device/trigger/per-channel view state) and `decoders`
  (here just "[]") — DSView writes them; our loader-tested subset omits them OK.
