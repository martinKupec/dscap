#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz>
"""dsl2npy — convert a .dsl capture to a NumPy .npz archive.

  ./dsl2npy.py capture.dsl [-o out.npz] [--compress] [--raw]

The .npz holds, per channel present in the file:
  analog:  ch<idx>_codes  (uint8 ADC codes)  and  ch<idx>_mv (float32, unless --raw)
  logic:   ch<idx>_bits   (uint8, 0/1 per sample)
plus metadata arrays: samplerate_hz, kind, total_samples, driver,
channel_index, channel_name, channel_enabled.

Load back with:
    d = np.load("out.npz")
    sr = float(d["samplerate_hz"]); t = np.arange(d["ch0_codes"].size) / sr
There is no stored time vector (it is implicit in samplerate_hz).
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dslread


def main():
    ap = argparse.ArgumentParser(description="convert a .dsl capture to .npz")
    ap.add_argument("dsl")
    ap.add_argument("-o", "--out", help="output .npz (default: <input>.npz)")
    ap.add_argument("--compress", action="store_true",
                    help="zip-compress the archive (smaller, slower; good for logic)")
    ap.add_argument("--raw", action="store_true",
                    help="analog: store ADC codes only, skip the calibrated mV array")
    args = ap.parse_args()

    cap = dslread.read(args.dsl)
    out = args.out or os.path.splitext(args.dsl)[0] + ".npz"

    data = {
        "samplerate_hz": np.float64(cap.samplerate),
        "kind": np.array(cap.kind),
        "total_samples": np.int64(cap.total_samples),
        "driver": np.array(cap.driver),
        "channel_index": np.array([c.index for c in cap.channels]),
        "channel_name": np.array([c.name for c in cap.channels]),
        "channel_enabled": np.array([c.enabled for c in cap.channels]),
    }
    for ch in cap.channels:
        if ch.kind == "analog":
            data[f"ch{ch.index}_codes"] = ch.codes
            if not args.raw:
                data[f"ch{ch.index}_mv"] = ch.mv().astype(np.float32)
        else:
            data[f"ch{ch.index}_bits"] = ch.bits

    (np.savez_compressed if args.compress else np.savez)(out, **data)
    arrs = sum(1 for k in data if k.startswith("ch"))
    print(f"{out}: {cap.kind}, {len(cap.channels)} channels, "
          f"{cap.total_samples} samples/ch, {arrs} data arrays, "
          f"{os.path.getsize(out)/1e6:.1f} MB")


if __name__ == "__main__":
    main()
