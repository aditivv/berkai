#!/usr/bin/env python3
"""
imx708_stream.py — model-ready frame extraction from /dev/video0 (QNX).

Reads the IMX708 camera driver's device node in a loop, unpacks MIPI RAW10,
debayers to 8-bit RGB (or grayscale), and emits frames at a configurable
cadence. The default mode saves frame_NNNN.jpg every N seconds into a folder
a downstream crack-detection model can poll.

This module does NOT depend on OpenCV. Debayer and JPEG encoding fall back to
pure numpy + (ffmpeg or PIL) so it runs on a QNX image where cv2 may be absent.
If cv2 IS importable it is used for higher-quality full-resolution debayering.

Frame format (from the driver, see HANDOFF_video_stream.md §3):
    Resolution   2304 x 1296   (IMX708 2x2-binned mode)
    Pixel format RAW10 Bayer, MIPI-packed
    Line stride  2880 bytes    (= 2304 * 10 / 8)
    Frame size   3,732,480 bytes (= 2880 * 1296)  <- one read() per frame

CLI examples
------------
    # One JPEG every 3 seconds, default RGGB Bayer, contrast-stretched 8-bit:
    python3 imx708_stream.py --interval 3 --out ./frames

    # As fast as the read loop allows, grayscale, raw >>2 tone mapping:
    python3 imx708_stream.py --interval 0 --color gray --tonemap shift

    # Try a different Bayer order and downsize for a YOLO model:
    python3 imx708_stream.py --bayer grbg --resize 640x640 --count 5

Programmatic use (for the next session's model wiring)
------------------------------------------------------
    from imx708_stream import Imx708Stream, StreamConfig
    cfg = StreamConfig(color="rgb", tonemap="stretch")
    with Imx708Stream(cfg) as cam:
        for frame in cam.frames():     # yields HxWx3 (or HxW) uint8 numpy arrays
            run_model(frame)
"""

from __future__ import annotations

import argparse
import logging
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Iterator, Optional

import numpy as np

# ── Constants (the driver's fixed 2x2-binned RAW10 mode) ────────────────────────
DEFAULT_WIDTH = 2304
DEFAULT_HEIGHT = 1296
BITS = 10
DEFAULT_DEVICE = "/dev/video0"

log = logging.getLogger("imx708_stream")

# Optional OpenCV — used only if present.
try:
    import cv2  # type: ignore

    _HAVE_CV2 = True
except Exception:  # pragma: no cover - depends on the host
    cv2 = None  # type: ignore
    _HAVE_CV2 = False

# Optional Pillow — second-choice JPEG encoder if cv2 is missing.
try:
    from PIL import Image  # type: ignore

    _HAVE_PIL = True
except Exception:  # pragma: no cover - depends on the host
    Image = None  # type: ignore
    _HAVE_PIL = False


# OpenCV Bayer conversion codes. OpenCV's enum names are offset by one site from
# the sensor's physical pattern: to demosaic a *physically* RGGB image you must
# pass COLOR_BayerBG2*, not COLOR_BayerRG2*. The table below maps our physical
# pattern label -> the OpenCV code that actually produces it, so that a given
# --bayer flag means the SAME physical pattern in both the cv2 and numpy paths
# (verified: both yield R>B for an RGGB test frame).
def _cv2_codes(bayer: str) -> tuple[int, int]:
    """Return (code_to_BGR, code_to_GRAY) for the given physical Bayer order."""
    if not _HAVE_CV2:
        raise RuntimeError("cv2 not available")
    table = {
        "rggb": (cv2.COLOR_BayerBG2BGR, cv2.COLOR_BayerBG2GRAY),
        "grbg": (cv2.COLOR_BayerGB2BGR, cv2.COLOR_BayerGB2GRAY),
        "gbrg": (cv2.COLOR_BayerGR2BGR, cv2.COLOR_BayerGR2GRAY),
        "bggr": (cv2.COLOR_BayerRG2BGR, cv2.COLOR_BayerRG2GRAY),
    }
    return table[bayer]


# Position of each colour within the 2x2 Bayer cell, for the numpy fallback.
# (row, col) offsets into the cell for R, the two G, and B.
_BAYER_CELL = {
    #         R          G1         G2         B
    "rggb": ((0, 0), (0, 1), (1, 0), (1, 1)),
    "grbg": ((0, 1), (0, 0), (1, 1), (1, 0)),
    "bggr": ((1, 1), (0, 1), (1, 0), (0, 0)),
    "gbrg": ((1, 0), (0, 0), (1, 1), (0, 1)),
}


@dataclass(frozen=True)
class StreamConfig:
    """Everything that controls how frames are produced."""

    device: str = DEFAULT_DEVICE
    width: int = DEFAULT_WIDTH
    height: int = DEFAULT_HEIGHT
    bayer: str = "rggb"          # rggb | grbg | bggr | gbrg
    color: str = "rgb"           # rgb | gray
    tonemap: str = "stretch"     # stretch (percentile+gamma) | shift (>>2)
    gamma: float = 0.5           # only used by the "stretch" tonemap
    debayer: str = "auto"        # auto | cv2 | bin
    resize: Optional[tuple[int, int]] = None  # (w, h) or None
    jpeg_quality: int = 90

    @property
    def stride(self) -> int:
        return self.width * BITS // 8

    @property
    def frame_size(self) -> int:
        return self.stride * self.height


# ── RAW10 unpack ────────────────────────────────────────────────────────────────
def unpack_raw10(buf: np.ndarray, width: int, height: int) -> np.ndarray:
    """MIPI-packed RAW10 bytes -> (height, width) uint16 Bayer array (0..1023).

    Mirrors raw10_unpack.py: every 5 bytes encode 4 pixels — 4 high-byte bytes
    plus one byte holding the 2 LSBs of each pixel.
    """
    stride = width * BITS // 8
    need = height * stride
    if buf.size < need:
        raise ValueError(f"frame too small: have {buf.size} bytes, need {need}")
    raw = buf[:need].reshape(height, stride)
    g = raw.reshape(height, width // 4, 5).astype(np.uint16)
    b0, b1, b2, b3, b4 = g[..., 0], g[..., 1], g[..., 2], g[..., 3], g[..., 4]
    px = np.empty((height, width), np.uint16)
    px[:, 0::4] = (b0 << 2) | ((b4 >> 0) & 0x3)
    px[:, 1::4] = (b1 << 2) | ((b4 >> 2) & 0x3)
    px[:, 2::4] = (b2 << 2) | ((b4 >> 4) & 0x3)
    px[:, 3::4] = (b3 << 2) | ((b4 >> 6) & 0x3)
    return px


# ── 10-bit -> 8-bit tone mapping ────────────────────────────────────────────────
def to_8bit(px16: np.ndarray, tonemap: str, gamma: float) -> np.ndarray:
    """Map a 10-bit (0..1023) array to 8-bit (0..255)."""
    if tonemap == "shift":
        return (px16 >> 2).astype(np.uint8)
    if tonemap == "stretch":
        # Percentile stretch + gamma lift — lifts dim indoor scenes into view.
        lo = float(np.percentile(px16, 1.0))
        hi = float(np.percentile(px16, 99.5))
        if hi <= lo:
            hi = lo + 1.0
        s = np.clip((px16.astype(np.float32) - lo) / (hi - lo), 0.0, 1.0)
        return ((s ** gamma) * 255.0).astype(np.uint8)
    raise ValueError(f"unknown tonemap: {tonemap}")


# ── Debayer ─────────────────────────────────────────────────────────────────────
def debayer_numpy(px16: np.ndarray, bayer: str, color: str) -> np.ndarray:
    """Pure-numpy 2x2-bin demosaic. Halves resolution, no color fringing.

    Each 2x2 Bayer cell becomes one output pixel: R and B taken directly, G
    averaged from the two green sites. Fast and artifact-free — ideal when cv2
    is unavailable. Returns a 10-bit (uint16) array still; tone-map afterwards.
    """
    (rr, rc), (g1r, g1c), (g2r, g2c), (br, bc) = _BAYER_CELL[bayer]
    r = px16[rr::2, rc::2]
    g = (px16[g1r::2, g1c::2].astype(np.uint16) + px16[g2r::2, g2c::2]) // 2
    b = px16[br::2, bc::2]
    h = min(r.shape[0], g.shape[0], b.shape[0])
    w = min(r.shape[1], g.shape[1], b.shape[1])
    r, g, b = r[:h, :w], g[:h, :w], b[:h, :w]
    if color == "gray":
        # Rec.601 luma in 10-bit space.
        return (0.299 * r + 0.587 * g + 0.114 * b).astype(np.uint16)
    return np.dstack((r, g, b)).astype(np.uint16)  # RGB order


def debayer(px16: np.ndarray, cfg: StreamConfig) -> np.ndarray:
    """Bayer (uint16) -> 8-bit image (HxWx3 RGB or HxW gray) per config."""
    use_cv2 = cfg.debayer == "cv2" or (cfg.debayer == "auto" and _HAVE_CV2)
    if use_cv2:
        if not _HAVE_CV2:
            raise RuntimeError("debayer=cv2 requested but cv2 is not importable")
        code_bgr, code_gray = _cv2_codes(cfg.bayer)
        bayer8 = to_8bit(px16, cfg.tonemap, cfg.gamma)
        if cfg.color == "gray":
            return cv2.cvtColor(bayer8, code_gray)
        bgr = cv2.cvtColor(bayer8, code_bgr)
        return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)  # normalise to RGB
    # numpy fallback: demosaic in 10-bit, then tone-map.
    img16 = debayer_numpy(px16, cfg.bayer, cfg.color)
    return to_8bit(img16, cfg.tonemap, cfg.gamma)


# ── Resize (optional, cv2 -> PIL -> nearest-neighbour numpy) ─────────────────────
def resize_image(img: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    w, h = size
    if img.shape[1] == w and img.shape[0] == h:
        return img
    if _HAVE_CV2:
        return cv2.resize(img, (w, h), interpolation=cv2.INTER_AREA)
    if _HAVE_PIL:
        mode = "RGB" if img.ndim == 3 else "L"
        return np.asarray(Image.fromarray(img, mode).resize((w, h), Image.BILINEAR))
    # nearest-neighbour fallback
    ys = (np.linspace(0, img.shape[0] - 1, h)).astype(np.intp)
    xs = (np.linspace(0, img.shape[1] - 1, w)).astype(np.intp)
    out = img[ys][:, xs]
    return out


# ── JPEG encoding (cv2 -> PIL -> ffmpeg via PNM) ────────────────────────────────
def _write_pnm(path: str, img: np.ndarray) -> None:
    """Write a binary PGM (gray) or PPM (RGB) — a format ffmpeg always reads."""
    if img.ndim == 2:
        header = b"P5\n%d %d\n255\n" % (img.shape[1], img.shape[0])
    else:
        header = b"P6\n%d %d\n255\n" % (img.shape[1], img.shape[0])
    with open(path, "wb") as f:
        f.write(header)
        np.ascontiguousarray(img).tofile(f)


def save_jpeg(path: str, img: np.ndarray, quality: int) -> None:
    """Save an 8-bit RGB (HxWx3) or gray (HxW) array as JPEG, no hard cv2 dep."""
    if _HAVE_CV2:
        if img.ndim == 3:  # our arrays are RGB; cv2 writes BGR
            img = cv2.cvtColor(img, cv2.COLOR_RGB2BGR)
        cv2.imwrite(path, img, [int(cv2.IMWRITE_JPEG_QUALITY), quality])
        return
    if _HAVE_PIL:
        mode = "RGB" if img.ndim == 3 else "L"
        Image.fromarray(img, mode).save(path, "JPEG", quality=quality)
        return
    # ffmpeg fallback: write a temp PNM, transcode to JPEG.
    pnm = path + ".pnm"
    _write_pnm(pnm, img)
    try:
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-i", pnm,
             "-q:v", str(max(2, int(31 - quality * 0.29))), path],
            check=True,
        )
    finally:
        try:
            os.remove(pnm)
        except OSError:
            pass


# ── The stream ──────────────────────────────────────────────────────────────────
class Imx708Stream:
    """Opens /dev/video0 and produces model-ready 8-bit frames."""

    def __init__(self, cfg: StreamConfig) -> None:
        self.cfg = cfg
        self._fd: Optional[int] = None

    def __enter__(self) -> "Imx708Stream":
        self.open()
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def open(self) -> None:
        if self._fd is None:
            self._fd = os.open(self.cfg.device, os.O_RDONLY)
            log.info("opened %s (frame_size=%d bytes)", self.cfg.device, self.cfg.frame_size)

    def close(self) -> None:
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def read_raw_frame(self) -> bytes:
        """Block until one full frame is read. The driver returns the whole
        buffer in a single read(); a short read means a partial/dropped frame,
        which we reject rather than splice across frame boundaries."""
        if self._fd is None:
            raise RuntimeError("stream not open")
        data = os.read(self._fd, self.cfg.frame_size)
        if len(data) != self.cfg.frame_size:
            raise IOError(
                f"short read: got {len(data)} bytes, expected {self.cfg.frame_size}"
            )
        return data

    def capture(self) -> np.ndarray:
        """Read + unpack + debayer + tonemap (+resize) -> one 8-bit frame."""
        raw = self.read_raw_frame()
        buf = np.frombuffer(raw, dtype=np.uint8)
        px16 = unpack_raw10(buf, self.cfg.width, self.cfg.height)
        img = debayer(px16, self.cfg)
        if self.cfg.resize is not None:
            img = resize_image(img, self.cfg.resize)
        return img

    def frames(self, interval: float = 0.0, count: int = 0) -> Iterator[np.ndarray]:
        """Yield 8-bit frames. interval=seconds between frames (0=as fast as
        the read loop allows), count=number to yield (0=unbounded)."""
        n = 0
        while count == 0 or n < count:
            t0 = time.monotonic()
            try:
                yield self.capture()
            except IOError as e:
                log.warning("dropping frame: %s", e)
                continue
            n += 1
            if interval > 0:
                sleep = interval - (time.monotonic() - t0)
                if sleep > 0:
                    time.sleep(sleep)


# ── Snapshot mode (the deliverable: periodic JPEGs) ─────────────────────────────
def run_snapshots(cfg: StreamConfig, out_dir: str, interval: float, count: int) -> int:
    os.makedirs(out_dir, exist_ok=True)
    saved = 0
    log.info(
        "snapshot mode: every %.1fs -> %s  (cv2=%s pil=%s ffmpeg-fallback=%s)",
        interval, out_dir, _HAVE_CV2, _HAVE_PIL, not (_HAVE_CV2 or _HAVE_PIL),
    )
    with Imx708Stream(cfg) as cam:
        for img in cam.frames(interval=interval, count=count):
            path = os.path.join(out_dir, f"frame_{saved:04d}.jpg")
            t0 = time.monotonic()
            save_jpeg(path, img, cfg.jpeg_quality)
            dt = (time.monotonic() - t0) * 1e3
            log.info(
                "wrote %s  shape=%s  mean=%.1f  (%.0f ms encode)",
                path, "x".join(map(str, img.shape)), float(np.mean(img)), dt,
            )
            saved += 1
    return saved


# ── CLI ─────────────────────────────────────────────────────────────────────────
def _parse_resize(s: Optional[str]) -> Optional[tuple[int, int]]:
    if not s:
        return None
    w, _, h = s.lower().partition("x")
    return (int(w), int(h))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--device", default=DEFAULT_DEVICE, help="camera node (default %(default)s)")
    p.add_argument("--out", default="./frames", help="output dir for JPEGs (default %(default)s)")
    p.add_argument("--interval", type=float, default=3.0,
                   help="seconds between saved frames; 0 = as fast as possible (default %(default)s)")
    p.add_argument("--count", type=int, default=5,
                   help="number of frames to save; 0 = run forever (default %(default)s)")
    p.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    p.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    p.add_argument("--bayer", choices=list(_BAYER_CELL), default="rggb",
                   help="Bayer order — try variants if colours look wrong (default %(default)s)")
    p.add_argument("--color", choices=["rgb", "gray"], default="rgb")
    p.add_argument("--tonemap", choices=["stretch", "shift"], default="stretch",
                   help="10->8 bit: stretch=percentile+gamma (good for dim scenes), shift=>>2")
    p.add_argument("--gamma", type=float, default=0.5, help="gamma for --tonemap stretch")
    p.add_argument("--debayer", choices=["auto", "cv2", "bin"], default="auto",
                   help="auto uses cv2 if present, else numpy 2x2-bin (half-res)")
    p.add_argument("--resize", default=None, help="resize output, e.g. 640x640")
    p.add_argument("--jpeg-quality", type=int, default=90)
    return p


def main(argv: Optional[list[str]] = None) -> int:
    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )
    args = build_parser().parse_args(argv)
    cfg = StreamConfig(
        device=args.device, width=args.width, height=args.height,
        bayer=args.bayer, color=args.color, tonemap=args.tonemap, gamma=args.gamma,
        debayer=args.debayer, resize=_parse_resize(args.resize),
        jpeg_quality=args.jpeg_quality,
    )
    try:
        n = run_snapshots(cfg, args.out, args.interval, args.count)
    except FileNotFoundError:
        log.error("%s not found — is camera_resmgr running (started as root)?", cfg.device)
        return 2
    except PermissionError:
        log.error("permission denied on %s", cfg.device)
        return 2
    log.info("done: saved %d frame(s) to %s", n, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
