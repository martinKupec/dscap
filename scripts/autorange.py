#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Martin Kupec <martin.kupec@kupson.cz>
"""autorange.py — auto-range a DSCope DSO capture, then take the real capture.

Wraps the `dscap` binary: it runs short "range-find" captures, reads back each
channel's code/voltage stats, picks a per-channel vdiv (fill ~80% of range, never
clip) and an offset (centre the trace), then fires one long capture at the tuned
settings. Writes the long capture's JSON summary to stdout.

WHY range-find must be long enough: for a slow-AM / swept signal the carrier
amplitude varies *across the sweep*. A short range-find can land on a
low-amplitude part, pick too sensitive a vdiv, and then CLIP at the envelope
peak in the long capture. So --rangefind-s must span at least one full
envelope/sweep period (default 13 ms). Tune it for your signal; for a steady CW
carrier a few ms is plenty.

Usage (run from the dscap repo root, so ./dscap is found):
  scripts/autorange.py --ch0 10 --ch1 10                # 2 ch, both 10:1, AC
  scripts/autorange.py --ch0 1 --ch1 off                # CH0 only, 1:1
  scripts/autorange.py --ch0 10 --ch1 off --coupling DC --capture-s 0.05

Per-channel arg value = probe factor (1, 10, 100) or "off" to disable that channel.
Exit 0 on success; non-zero if dscap fails or a channel rails at max vdiv.
"""
from __future__ import annotations
import argparse, json, os, subprocess, sys, tempfile

VDIV_LADDER_MV = [10, 20, 50, 100, 200, 500, 1000]   # DSCope DSO; 10 mV floor (5 mV = garbage)
CLAMP_LO, CLAMP_HI = 10, 245                          # offset clamp (dscap)
FILL = 0.80                                           # target fraction of half-scale
CENTRE_TOL = 8                                        # code units before re-centring

HERE = os.path.dirname(os.path.abspath(__file__))
DSCAP = os.path.join(os.path.dirname(HERE), "dscap")  # repo-root/dscap


def run_capture(device, sr, dur, chans, out):
    """chans: list of dicts {idx,probe,coupling,vdiv,offset} or None (disabled)."""
    lines = [f"[capture]", f"device = {device}", "mode = single",
             f"samplerate = {sr}", f"duration_s = {dur}", f"output = {out}"]
    for i, c in enumerate(chans):
        lines.append(f"[channel{i}]")
        if c is None:
            lines.append("enabled=false"); continue
        lines += ["enabled=true", f"vdiv={c['vdiv']}mV",
                  f"probe_factor={c['probe']}", f"coupling={c['coupling']}",
                  f"offset={c['offset']}"]
    ini = "\n".join(lines) + "\n"
    p = os.path.join(tempfile.gettempdir(), "autorange.ini")
    open(p, "w").write(ini)
    r = subprocess.run([DSCAP, p], cwd=os.path.dirname(HERE),
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"dscap failed: {r.stdout.strip()} {r.stderr.strip()[:300]}\n")
        return None
    try:
        j = json.loads(r.stdout)
    except json.JSONDecodeError:
        sys.stderr.write(f"dscap: non-JSON stdout: {r.stdout[:300]}\n")
        return None
    if not j.get("ok", True) or "channels" not in j:
        sys.stderr.write(f"dscap error: {j.get('error', j)}\n")
        return None
    return j


def pick_vdiv(half_node_mv, probe):
    """Smallest vdiv whose half-scale (node V) holds the signal half-amplitude * 1/FILL."""
    for vd in VDIV_LADDER_MV:
        if half_node_mv <= FILL * 5 * vd * probe:
            return vd
    return VDIV_LADDER_MV[-1]


def main():
    ap = argparse.ArgumentParser(description="auto-range a DSCope capture then capture")
    ap.add_argument("--device", default="DSCope U3P100")
    ap.add_argument("--ch0", default="10", help="probe factor (1/10/100) or 'off'")
    ap.add_argument("--ch1", default="off", help="probe factor (1/10/100) or 'off'")
    ap.add_argument("--coupling", default="AC", choices=["AC", "DC"])
    ap.add_argument("--sr", default="500M")
    ap.add_argument("--rangefind-s", type=float, default=0.013,
                    help="range-find duration; MUST span >=1 envelope/sweep period")
    ap.add_argument("--capture-s", type=float, default=0.030, help="final capture duration")
    ap.add_argument("--iters", type=int, default=4)
    ap.add_argument("--out", default="caps/autorange-{ts}.dsl")
    a = ap.parse_args()

    def mkchan(i, spec):
        if spec.lower() == "off":
            return None
        return {"idx": i, "probe": int(spec), "coupling": a.coupling,
                "vdiv": 1000, "offset": 128}
    chans = [mkchan(0, a.ch0), mkchan(1, a.ch1)]
    if all(c is None for c in chans):
        sys.exit("both channels off — nothing to do")

    railed = False
    for it in range(a.iters):
        j = run_capture(a.device, a.sr, a.rangefind_s, chans, "caps/_rf-{ts}.dsl")
        if j is None:
            sys.exit(1)
        done = True
        # map returned channels (only enabled ones are present) back by name index
        ret = {c["name"]: c for c in j["channels"]}
        msg = []
        for i, c in enumerate(chans):
            if c is None:
                continue
            r = ret.get(str(i))
            if r is None:
                continue
            cmin, cmax, cmean = r["code_min"], r["code_max"], r["code_mean"]
            clipped = r["clipped"]; ppmv = r["vpp_mv"]
            msg.append(f"ch{i}:{c['vdiv']}mV/off{c['offset']} codes {cmin}-{cmax} "
                       f"clip={clipped} vpp={ppmv/1000:.2f}V")
            # re-centre
            err = int(round(128 - cmean))
            if abs(err) > CENTRE_TOL:
                c["offset"] = max(CLAMP_LO, min(CLAMP_HI, c["offset"] + err)); done = False
            # vdiv
            want = pick_vdiv(ppmv / 2, c["probe"])
            if clipped:
                ni = VDIV_LADDER_MV.index(c["vdiv"]) + 1
                if ni >= len(VDIV_LADDER_MV):
                    sys.stderr.write(f"  *** ch{i} CLIPPED at max {VDIV_LADDER_MV[-1]}mV/div "
                                     f"-> signal exceeds +/-{5*VDIV_LADDER_MV[-1]*c['probe']/1000:.0f}V "
                                     f"node; use higher attenuation\n")
                    railed = True
                else:
                    c["vdiv"] = VDIV_LADDER_MV[ni]; done = False
            elif want != c["vdiv"]:
                c["vdiv"] = want; done = False
        sys.stderr.write(f"iter{it}: " + " | ".join(msg) + "\n")
        if done:
            sys.stderr.write("converged.\n"); break

    settings = ", ".join(f"ch{i}={c['vdiv']}mV/off{c['offset']}/x{c['probe']}"
                         for i, c in enumerate(chans) if c)
    sys.stderr.write(f"FINAL: {settings}  coupling={a.coupling}\n")
    j = run_capture(a.device, a.sr, a.capture_s, chans, a.out)
    if j is None:
        sys.exit(1)
    print(json.dumps(j, indent=1))
    sys.exit(2 if railed else 0)


if __name__ == "__main__":
    main()
