#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz>
"""dslread — read a DreamSourceLab .dsl capture into numpy arrays.

A .dsl is a plain zip holding:
  · header        — INI-ish: samplerate, total samples, bits, per-probe cal
  · session       — JSON view config (ignored here)
  · O-<idx>/<blk> — analog/DSO raw samples, 1 byte/sample, ONE member per channel
  · L-<idx>/<blk> — logic samples, bit-packed (bit 0 of each byte = earliest)
  · summary.json  — present only on dscap-written files (authoritative calibration)

This is the shared reader behind dsl2png.py / dsl2npy.py. It is self-contained
(stdlib + numpy) so the published tool does not depend on any external decoder;
its output has been cross-checked against an independent reference decoder.

Analog calibration (same mapping as DSView and dscap):
    v_mv(code) = (hw_offset - code) * vdiv_mv * 10 / 255 * probe_factor
where 10 = DS_CONF_DSO_VDIVS (vertical divisions spanning full scale).
"""
import json
import re
import sys
import zipfile

import numpy as np

DS_CONF_DSO_VDIVS = 10  # vertical divisions spanning full 8-bit code range


def parse_samplerate(s):
    """'1 GHz' / '200 MHz' / '500 kHz' / '1000000' -> samples/second (float)."""
    s = s.strip()
    m = re.match(r"([\d.]+)\s*([GMk]?)Hz", s, re.IGNORECASE)
    if m:
        mult = {"": 1, "k": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]
        return float(m.group(1)) * mult
    return float(s)  # bare number = Hz


def _parse_header(zf):
    """Flat dict of the INI-ish 'header' member (section lines dropped)."""
    txt = zf.read("header").decode("utf-8", "replace")
    hdr = {}
    for line in txt.splitlines():
        line = line.strip()
        if not line or line.startswith("[") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        hdr[k.strip()] = v.strip()
    return hdr


def _probe_index(name):
    """O-3/0 -> 3 ; L-12/1 -> 12."""
    return int(name.split("-", 1)[1].split("/", 1)[0])


def _members_by_channel(zf, prefix):
    """{channel_idx: [member names sorted by block]} for 'O' or 'L' prefix."""
    pat = re.compile(rf"{prefix}-(\d+)/(\d+)$")
    chans = {}
    for n in zf.namelist():
        m = pat.match(n)
        if m:
            chans.setdefault(int(m.group(1)), []).append(n)
    for idx in chans:
        chans[idx].sort(key=lambda n: int(n.rsplit("/", 1)[1]))
    return chans


class Channel:
    __slots__ = ("index", "name", "kind", "enabled", "vdiv_mv", "probe_factor",
                 "hw_offset", "coupling", "codes", "bits")

    def __init__(self, index, name, kind):
        self.index = index
        self.name = name
        self.kind = kind            # 'analog' | 'logic'
        self.enabled = True
        self.vdiv_mv = 0
        self.probe_factor = 1
        self.hw_offset = 128
        self.coupling = 0
        self.codes = None           # analog: uint8 ADC codes
        self.bits = None            # logic: uint8 0/1 per sample

    def mv(self):
        """Analog channel calibrated to millivolts (float array)."""
        if self.kind != "analog":
            raise ValueError(f"channel {self.index} is {self.kind}, not analog")
        scale = self.vdiv_mv * DS_CONF_DSO_VDIVS / 255.0 * self.probe_factor
        return (self.hw_offset - self.codes.astype(np.float64)) * scale


class Capture:
    def __init__(self, path):
        self.path = path
        self.samplerate = 0.0
        self.total_samples = 0
        self.bits = 8
        self.driver = ""
        self.ref_min = 0
        self.ref_max = 255
        self.kind = ""              # 'analog' | 'logic'
        self.channels = []          # list[Channel], in channel-index order

    def enabled_channels(self):
        return [c for c in self.channels if c.enabled]


def read(path):
    """Parse a .dsl into a Capture. Raises ValueError if it has no samples."""
    cap = Capture(path)
    with zipfile.ZipFile(path) as zf:
        names = set(zf.namelist())
        hdr = _parse_header(zf)
        cap.samplerate = parse_samplerate(hdr.get("samplerate", "0"))
        cap.total_samples = int(hdr.get("total samples", 0))
        cap.bits = int(hdr.get("bits", 8))
        cap.driver = hdr.get("driver", "")
        cap.ref_min = int(hdr.get("ref min", 0))
        cap.ref_max = int(hdr.get("ref max", 255))

        # dscap-written files embed the exact calibration dscap itself used.
        summary = None
        if "summary.json" in names:
            try:
                summary = json.loads(zf.read("summary.json"))
            except (ValueError, KeyError):
                summary = None
        scal = {}
        if summary:
            for c in summary.get("channels", []):
                scal[str(c.get("name"))] = c

        analog = _members_by_channel(zf, "O")
        logic = _members_by_channel(zf, "L")
        if analog and not logic:
            cap.kind = "analog"
        elif logic and not analog:
            cap.kind = "logic"
        elif analog and logic:
            raise ValueError("mixed O-/L- members; not supported")
        else:
            raise ValueError("no O-<idx>/<blk> or L-<idx>/<blk> sample members")

        member_chans = analog if cap.kind == "analog" else logic
        for idx in sorted(member_chans):
            name = hdr.get(f"probe{idx}", str(idx))
            ch = Channel(idx, name, cap.kind)
            ch.enabled = hdr.get(f"enable{idx}", "1") not in ("0", "false", "False")
            ch.coupling = int(hdr.get(f"coupling{idx}", 0))
            ch.vdiv_mv = int(hdr.get(f"vDiv{idx}", 0))
            ch.probe_factor = int(hdr.get(f"vFactor{idx}", 1))
            ch.hw_offset = int(hdr.get(f"vOffset{idx}", 128))
            # Prefer dscap's own summary calibration when present.
            s = scal.get(name)
            if s:
                ch.vdiv_mv = int(s.get("vdiv_mv", ch.vdiv_mv))
                ch.probe_factor = int(s.get("probe_factor", ch.probe_factor))
                ch.hw_offset = int(s.get("hw_offset", ch.hw_offset))

            raw = b"".join(zf.read(m) for m in member_chans[idx])
            if cap.kind == "analog":
                ch.codes = np.frombuffer(raw, dtype=np.uint8)
            else:
                b = np.unpackbits(np.frombuffer(raw, dtype=np.uint8),
                                  bitorder="little")
                if cap.total_samples and b.size > cap.total_samples:
                    b = b[:cap.total_samples]
                ch.bits = b
            cap.channels.append(ch)
    return cap


if __name__ == "__main__":
    # Quick inspector: dslread.py <file.dsl>
    if len(sys.argv) != 2:
        sys.exit("usage: dslread.py <capture.dsl>")
    c = read(sys.argv[1])
    print(f"{c.path}: {c.kind}, {c.samplerate/1e6:g} MS/s, "
          f"{c.total_samples} samples, {len(c.channels)} channels")
    for ch in c.channels:
        if ch.kind == "analog":
            mv = ch.mv()
            print(f"  O-{ch.index} '{ch.name}' en={int(ch.enabled)} "
                  f"vdiv={ch.vdiv_mv}mV x{ch.probe_factor} off={ch.hw_offset} "
                  f"codes[{ch.codes.min()}..{ch.codes.max()}] "
                  f"mV[{mv.min():.1f}..{mv.max():.1f}]")
        else:
            print(f"  L-{ch.index} '{ch.name}' en={int(ch.enabled)} "
                  f"samples={ch.bits.size} toggles={int(np.count_nonzero(np.diff(ch.bits)))}")
