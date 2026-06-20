#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz>
"""dsl2png — plot a .dsl capture to a PNG.

  ./dsl2png.py capture.dsl [-o out.png] [--channels 0,1] [--width-px N] [--dpi N]

Analog (DSO/ANALOG): one stacked subplot per channel, y in mV, shared time axis.
Logic (DSLogic): stacked digital step traces, one row per channel.

Large captures (a deep `single` is up to ~128 M samples) are reduced with a
per-pixel min/max ENVELOPE so transient spikes survive the downsampling rather
than being decimated away.
"""
import argparse
import os
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dslread


def _time_unit(duration_s):
    if duration_s >= 1.0:
        return 1.0, "s"
    if duration_s >= 1e-3:
        return 1e3, "ms"
    if duration_s >= 1e-6:
        return 1e6, "µs"
    return 1e9, "ns"


def _envelope(y, target):
    """Reduce y to <=target bins, returning (x_idx, ymin, ymax) or None if no
    reduction is needed (then plot y raw)."""
    n = y.size
    if n <= target:
        return None
    step = n // target
    m = (n // step) * step
    yb = y[:m].reshape(-1, step)
    xc = (np.arange(yb.shape[0]) + 0.5) * step
    return xc, yb.min(axis=1), yb.max(axis=1)


def _select(cap, spec):
    chans = [c for c in cap.channels if c.enabled]
    if spec:
        want = {int(x) for x in spec.split(",")}
        chans = [c for c in cap.channels if c.index in want]
    return chans


def main():
    ap = argparse.ArgumentParser(description="plot a .dsl capture to PNG")
    ap.add_argument("dsl")
    ap.add_argument("-o", "--out", help="output .png (default: <input>.png)")
    ap.add_argument("--channels", help="comma-separated channel indices (default: all enabled)")
    ap.add_argument("--width-px", type=int, default=2000,
                    help="envelope target columns (downsample budget; default 2000)")
    ap.add_argument("--dpi", type=int, default=110)
    ap.add_argument("--width-in", type=float, default=12.0)
    ap.add_argument("--row-in", type=float, default=1.6, help="inches per channel row")
    args = ap.parse_args()

    cap = dslread.read(args.dsl)
    chans = _select(cap, args.channels)
    if not chans:
        sys.exit("no channels selected")
    out = args.out or os.path.splitext(args.dsl)[0] + ".png"
    sr = cap.samplerate or 1.0
    n = cap.total_samples or (chans[0].codes.size if cap.kind == "analog" else chans[0].bits.size)
    tmul, tunit = _time_unit(n / sr)

    if cap.kind == "analog":
        fig, axes = plt.subplots(len(chans), 1, sharex=True, squeeze=False,
                                 figsize=(args.width_in, max(2.0, args.row_in * len(chans))))
        for ax, ch in zip(axes[:, 0], chans):
            y = ch.mv()
            env = _envelope(y, args.width_px)
            if env is None:
                t = np.arange(y.size) / sr * tmul
                ax.plot(t, y, lw=0.7)
            else:
                xc, ymin, ymax = env
                t = xc / sr * tmul
                ax.fill_between(t, ymin, ymax, lw=0, alpha=0.85)
            ax.set_ylabel(f"ch{ch.index} '{ch.name}'\nmV", fontsize=8)
            ax.grid(True, alpha=0.3)
            ax.margins(x=0)
        axes[-1, 0].set_xlabel(f"time ({tunit})")
    else:  # logic
        fig, ax = plt.subplots(figsize=(args.width_in, max(2.0, 0.5 * len(chans) + 1)))
        for row, ch in enumerate(chans):
            y = ch.bits.astype(np.float32)
            env = _envelope(y, args.width_px)
            base = row * 1.5
            ax.axhline(base, color="0.8", lw=0.5, zorder=0)  # idle-low baseline
            if env is None:
                t = np.arange(y.size) / sr * tmul
                ax.step(t, base + y, where="post", lw=0.8)
            else:
                xc, ymin, ymax = env
                t = xc / sr * tmul
                ax.fill_between(t, base + ymin, base + ymax, lw=0, step="mid")
            ax.text(-0.01, base + 0.4, f"ch{ch.index} '{ch.name}'", ha="right",
                    va="center", fontsize=8, transform=ax.get_yaxis_transform())
        ax.set_yticks([])
        ax.set_xlabel(f"time ({tunit})")
        ax.margins(x=0)

    fig.suptitle(f"{os.path.basename(args.dsl)}  —  {cap.driver} {cap.kind}, "
                 f"{sr/1e6:g} MS/s, {n} samples/ch", fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(out, dpi=args.dpi)
    print(f"{out}: {cap.kind}, {len(chans)} channels plotted, "
          f"{os.path.getsize(out)/1e3:.0f} KB")


if __name__ == "__main__":
    main()
