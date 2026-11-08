#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode a capture from /sys/kernel/debug/tdm-pcm/scope.

Each sample is one hex digit: bit 0 FSC, bit 1 DCL, bit 2 DI, bit 3 DO.
Prints the DCL rate, the bit clocks between FSC pulses and the FSC pulse
width, and decodes DI and DO bit by bit (sampled on the falling DCL edge)
starting at the first bit after each FSC pulse ends, offset by --skip
bit clocks, as bytes per frame.

Usage: scope-decode.py FILE [--skip N] [--rising] [--frames N]
"""
import argparse
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--skip", type=int, default=0,
                    help="bit clocks between the end of FSC and bit 0")
    ap.add_argument("--rising", action="store_true",
                    help="sample data on the rising DCL edge")
    ap.add_argument("--frames", type=int, default=4)
    a = ap.parse_args()

    with open(a.file) as f:
        head = f.readline().split()
        data = f.readline().strip()
    n, ns = int(head[1]), int(head[3])
    smp = [int(c, 16) for c in data]
    dt = ns / n
    print(f"{n} samples in {ns} ns: {dt:.1f} ns per sample")

    dcl = [(v >> 1) & 1 for v in smp]
    fsc = [v & 1 for v in smp]
    rise = [i for i in range(1, n) if dcl[i] and not dcl[i - 1]]
    fall = [i for i in range(1, n) if not dcl[i] and dcl[i - 1]]
    if len(rise) > 2:
        per = (rise[-1] - rise[0]) / (len(rise) - 1) * dt
        print(f"DCL: {len(rise)} rising edges, period {per:.1f} ns, "
              f"{1e3 / per:.4f} MHz, high {100 * sum(dcl) / n:.1f} %")
    frise = [i for i in range(1, n) if fsc[i] and not fsc[i - 1]]
    ffall = [i for i in range(1, n) if not fsc[i] and fsc[i - 1]]
    print(f"FSC: {len(frise)} pulses")
    for k in range(len(frise) - 1):
        clocks = sum(1 for r in rise if frise[k] <= r < frise[k + 1])
        print(f"  pulse {k}: {(frise[k + 1] - frise[k]) * dt / 1000:.2f} us"
              f" to the next, {clocks} DCL rising edges")
    for k, r in enumerate(frise):
        e = next((x for x in ffall if x > r), None)
        if e is None:
            continue
        rr = [x for x in rise if r - 20 <= x <= e + 20]
        ff = [x for x in fall if r - 20 <= x <= e + 20]
        print(f"  pulse {k} width {(e - r) * dt:.0f} ns; DCL rises at "
              f"{[x - r for x in rr]}, falls at {[x - r for x in ff]} "
              f"samples from its start")

    edges = rise if a.rising else fall
    for k in range(min(len(frise) - 1, a.frames)):
        e = next((x for x in ffall if x > frise[k]), None)
        if e is None or e > frise[k + 1]:
            continue
        # the bits of this frame only: from the end of FSC to the next FSC
        span = [x for x in edges if e < x <= frise[k + 1]][a.skip:]
        for name, bit in (("DI", 2), ("DO", 3)):
            bits = [(smp[x] >> bit) & 1 for x in span]
            out = []
            for i in range(0, len(bits) - 7, 8):
                b = 0
                for j in range(8):
                    b = (b << 1) | bits[i + j]
                out.append(b)
            txt = " ".join(f"{out[i]:02x}{out[i + 1]:02x}"
                           for i in range(0, len(out) - 1, 2))
            print(f"frame {k} {name} ({len(bits)} bits): {txt}")

if __name__ == "__main__":
    sys.exit(main())
