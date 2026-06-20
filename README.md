# dscap

A small, headless command-line tool that drives a DreamSourceLab **DSCope**
oscilloscope or **DSLogic** logic analyzer without the DSView GUI. It links the
vendor's own `libsigrok4DSL` — the same library DSView runs — so it sees the
exact per-device capabilities the GUI does.

It is built for **scripts and agents**: every run prints **one JSON object** to
stdout, and a capture also writes a native **`.dsl`** file (a plain zip in
DreamSourceLab's layout, directly openable in DSView) with that same JSON
embedded inside it.

```sh
./dscap enumerate          # identify the attached device  -> JSON
./dscap capture.ini        # run the measurement in the INI -> JSON + .dsl
```

Helper scripts under `scripts/` round it out: **`autorange.py`** picks each
channel's `vdiv`/`offset` automatically (short range-find captures, then one long
capture — no hand-tuning), and **`dsl2png.py`** / **`dsl2npy.py`** turn a `.dsl`
into a plot or a NumPy array.

See **[AGENTS.md](AGENTS.md)** for the full output contract and config format,
and **[capture.ini.example](capture.ini.example)** for a sample config.

## Status

All capture modes are **bench-verified on hardware** — the DSO modes on a
**DSCope U3P100**, the logic modes on a **DSLogic Plus**:

- `mode=single` — instant one-shot, shallow and deep (≤128 Mi/ch, 512 MiB/ch
  OOM guard)
- `mode=roll` — continuous best-effort capture for `duration_s` (`capture_ratio`
  honesty when the host can't keep up)
- `mode=stream` — DSCope ANALOG ≤10 MHz continuous DAQ
- `mode=logic` / `logic-stream` — DSLogic digital capture (Buffer and Stream)

Calibration (codes → mV), the JSON summary and its embedded copy, the native
`.dsl` writer, runtime config validation (reject, not clamp), and per-channel
clipping flags (`REF_MIN`/`REF_MAX`) all match DSView and open in it.

Still on the bench list: LOGIC real-signal fidelity (verified channel↔index
mapping against a known wired signal), `SR_DF_OVERFLOW` provocation, and
cold-device config reset. Only the **DSCope U3P100** and **DSLogic Plus** have
been tested — other models *should* work (all boundaries are queried at runtime,
not hardcoded) but are **untested**.

## When to use this — and when not to

This tool is deliberately narrow. There is real prior art, and for some jobs you
should use it instead:

- **[mainline sigrok / `sigrok-cli`](https://sigrok.org/wiki/DreamSourceLab_DSLogic)**
  — the upstream `dreamsourcelab-dslogic` driver. Mature and packaged, but it is
  **logic-analyzer only** (no DSCope analog / DSO), the **newer models**
  (U3Pro16/32, …) are **unsupported**, and it writes sigrok's `.sr` session
  format, not a DSView-openable `.dsl`. *Use it if you have a supported DSLogic
  and want the standard sigrok toolchain.*

- **[scopehal-sigrok-bridge](https://github.com/ngscopeclient/scopehal-sigrok-bridge)**
  — like this tool, it links `libsigrok4DSL` headlessly, so it *does* reach
  DSCope analog and the newer hardware. But it is a **SCPI + binary socket
  bridge that feeds the ngscopeclient GUI** for live viewing; it is not a
  file-writing CLI (no `.dsl`, no JSON, no one-shot-to-disk). *Use it if you want
  to watch a DSCope live in ngscopeclient.*

**Use `dscap`** when you want a **single binary** that takes **one headless,
config-driven DSO capture** on **current DSCope hardware** and leaves you a
**JSON summary plus a GUI-openable `.dsl` file** — i.e. for automation, agents,
and reproducible bench captures, not interactive scoping.

## Build

`libsigrok4DSL` is built from the pinned **[DSView](https://github.com/DreamSourceLab/DSView)**
submodule, so fetch it first:

```sh
git submodule update --init        # third_party/DSView at the pinned commit
```

Then build with either Make or CMake:

```sh
make                               # -> ./dscap
# or
cmake -S . -B build && cmake --build build -j   # -> ./build/dscap
```

Requires the `glib-2.0`, `libusb-1.0`, and `zlib` development packages (plus a C
compiler and CMake/Make). Nothing prebuilt is relied on — the libsigrok4DSL
subset is compiled from the submodule sources.

At runtime the device needs its FX2/FPGA firmware, which DSView ships under its
resource directory. `dscap` locates it via, in order: the `[capture] res_dir`
config key, the `DSVIEW_RES_DIR` environment variable, then a per-OS compiled
default (`/opt/dsview/share/DSView/res` on Linux, `C:\Program Files\DSView\res`
on Windows — next to `DSView.exe`). The path used is logged to stderr at startup.

Close the DSView GUI first — only one process may own the device at a time. After
a run the device stays warm; power-cycle / re-plug between captures (the tool
detects the warm fallback and exits with a clear error).

## License

`dscap` is © 2026 Martin Kupec, licensed under the **GPLv3 or later** (it links
the GPLv3+ `libsigrok4DSL`). It does **not** include or redistribute any
DreamSourceLab firmware or FPGA bitstream — those ship with DSView and remain
under DreamSourceLab's own terms.

DSView and `libsigrok4DSL` are © DreamSourceLab; sigrok and `sigrok-cli` are © the
sigrok project; scopehal-sigrok-bridge is © its authors. This is an independent
tool, not affiliated with or endorsed by any of them.
