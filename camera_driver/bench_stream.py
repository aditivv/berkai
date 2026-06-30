#!/usr/bin/env python3
"""
bench_stream.py — Step 1: measure the capture pipeline's per-phase cost.

Additive diagnostic. Imports the building blocks from imx708_stream and times
each phase of one frame separately:

    read()    blocks for a fresh frame + copies 3.7 MB out of the driver
    unpack    MIPI RAW10 -> 16-bit Bayer (numpy)
    debayer   Bayer -> 8-bit RGB/gray (incl. tone mapping)
    encode    8-bit image -> JPEG (cv2 / PIL / ffmpeg)

It sweeps several configs so you can see which knobs matter:
    debayer:  cv2 (full-res)  vs  bin (numpy half-res)
    color:    rgb             vs  gray
    tonemap:  stretch         vs  shift     (the delta = per-frame percentile cost)
    resize:   none            vs  640x640

Run on the Pi with camera_resmgr already streaming:
    python3 bench_stream.py                 # default: 60 frames/config, full sweep
    python3 bench_stream.py --frames 100
    python3 bench_stream.py --only bin       # only the numpy-debayer configs
    python3 bench_stream.py --quick          # one representative config

Nothing is saved: JPEGs are encoded to a throwaway file and discarded.
"""

from __future__ import annotations

import argparse
import statistics
import time
from dataclasses import dataclass, replace
from typing import Optional

import numpy as np

import imx708_stream as S
from imx708_stream import (
    Imx708Stream,
    StreamConfig,
    debayer,
    resize_image,
    save_jpeg,
    unpack_raw10,
    unpack_raw10_fast8,
)


@dataclass
class PhaseTimes:
    read: list[float]
    unpack: list[float]
    debayer: list[float]
    encode: list[float]

    @classmethod
    def empty(cls) -> "PhaseTimes":
        return cls([], [], [], [])

    def total_ms(self) -> list[float]:
        return [r + u + d + e for r, u, d, e in
                zip(self.read, self.unpack, self.debayer, self.encode)]


def _ms(x: float) -> float:
    return x * 1e3


def bench_config(cam: Imx708Stream, cfg: StreamConfig, frames: int,
                 warmup: int) -> PhaseTimes:
    """Time each phase over `frames` iterations (after `warmup` discarded)."""
    pt = PhaseTimes.empty()
    tmp = "/tmp/bench_discard.jpg"
    for i in range(frames + warmup):
        t0 = time.perf_counter()
        raw = cam.read_raw_frame()
        t1 = time.perf_counter()
        buf = np.frombuffer(raw, dtype=np.uint8)
        if cfg.unpack == "fast8":
            bayer = unpack_raw10_fast8(buf, cfg.width, cfg.height)
        else:
            bayer = unpack_raw10(buf, cfg.width, cfg.height)
        t2 = time.perf_counter()
        img = debayer(bayer, cfg)
        if cfg.resize is not None:
            img = resize_image(img, cfg.resize)
        t3 = time.perf_counter()
        save_jpeg(tmp, img, cfg.jpeg_quality)
        t4 = time.perf_counter()
        if i >= warmup:
            pt.read.append(_ms(t1 - t0))
            pt.unpack.append(_ms(t2 - t1))
            pt.debayer.append(_ms(t3 - t2))
            pt.encode.append(_ms(t4 - t3))
    return pt


def label(cfg: StreamConfig) -> str:
    r = "x".join(map(str, cfg.resize)) if cfg.resize else "full"
    # fast8 ignores tonemap (data already 8-bit), so show "-" there.
    tone = "-" if cfg.unpack == "fast8" else cfg.tonemap
    return f"{cfg.unpack:5} {cfg.debayer:3} {cfg.color:4} {tone:7} {r}"


def build_sweep(base: StreamConfig, only: Optional[str]) -> list[StreamConfig]:
    cfgs: list[StreamConfig] = []
    debayers = ["cv2", "bin"] if S._HAVE_CV2 else ["bin"]
    if only in ("cv2", "bin"):
        debayers = [only]
    for unpack in ("full", "fast8"):
        for db in debayers:
            for color in ("rgb", "gray"):
                # fast8 has no tonemap step; full sweeps both.
                tones = ("shift",) if unpack == "fast8" else ("stretch", "shift")
                for tone in tones:
                    for resize in (None, (640, 640)):
                        cfgs.append(replace(base, unpack=unpack, debayer=db,
                                            color=color, tonemap=tone, resize=resize))
    return cfgs


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--device", default=S.DEFAULT_DEVICE)
    p.add_argument("--frames", type=int, default=60, help="timed frames per config")
    p.add_argument("--warmup", type=int, default=5, help="discarded warmup frames")
    p.add_argument("--only", choices=["cv2", "bin"], default=None)
    p.add_argument("--quick", action="store_true",
                   help="one representative config (bin/gray/shift/640) only")
    args = p.parse_args()

    base = StreamConfig(device=args.device)
    if args.quick:
        sweep = [replace(base, unpack="fast8", debayer="bin", color="gray",
                         tonemap="shift", resize=(640, 640))]
    else:
        sweep = build_sweep(base, args.only)

    print(f"cv2={S._HAVE_CV2}  PIL={S._HAVE_PIL}  "
          f"encoder={'cv2' if S._HAVE_CV2 else 'PIL' if S._HAVE_PIL else 'ffmpeg'}")
    print(f"timing {args.frames} frames/config (+{args.warmup} warmup)\n")
    header = f"{'config':30} {'fps':>6}  {'total':>7}  phases (ms, median)"
    print(header)
    print("-" * len(header))

    with Imx708Stream(base) as cam:
        for cfg in sweep:
            pt = bench_config(cam, cfg, args.frames, args.warmup)
            total = statistics.median(pt.total_ms())
            fps = 1000.0 / total if total > 0 else 0.0
            phases = (f"read {statistics.median(pt.read):5.1f} | "
                      f"unpack {statistics.median(pt.unpack):5.1f} | "
                      f"debayer {statistics.median(pt.debayer):5.1f} | "
                      f"encode {statistics.median(pt.encode):5.1f}")
            print(f"{label(cfg):30} {fps:6.1f}  {total:6.1f}   {phases}")

    print("\nread = wait-for-fresh-frame + 3.7MB copy (small when processing-bound,")
    print("       ~frame-interval when capture-bound). tonemap cost = stretch - shift")
    print("       in the debayer column. Lowest total = your current fps ceiling.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
