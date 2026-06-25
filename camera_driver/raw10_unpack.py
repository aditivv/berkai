#!/usr/bin/env python3
"""
Unpack a MIPI CSI-2 RAW10 frame (as captured from /dev/video0) into formats
that are actually viewable.

The IMX708 sends RAW10 in MIPI packing: every 5 bytes hold 4 pixels —
4 bytes of high-8-bits, then 1 byte holding the 2 LSBs of each pixel.
ffmpeg cannot read that directly (it has no bayer_*10 format), so we unpack to
16-bit here.

Usage:
    python3 raw10_unpack.py /tmp/frame.raw [width height]
    # defaults: 2304 1296  (the driver's 2x2-binned mode)

Outputs (next to the input file):
    frame.raw.pgm       8-bit grayscale, brightness-normalized — open this to
                        confirm the image instantly (no debayer, no deps beyond
                        numpy). Any image viewer reads PGM.
    frame.raw.bayer16   16-bit little-endian Bayer — feed to ffmpeg for a
                        proper color image:
        ffmpeg -f rawvideo -pixel_format bayer_rggb16le \\
               -video_size 2304x1296 -i frame.raw.bayer16 frame.png
    (if colors look wrong, swap rggb -> grbg / bggr / gbrg)
"""
import sys
import numpy as np

inp = sys.argv[1] if len(sys.argv) > 1 else "/tmp/frame.raw"
W   = int(sys.argv[2]) if len(sys.argv) > 2 else 2304
H   = int(sys.argv[3]) if len(sys.argv) > 3 else 1296

stride = W * 10 // 8                      # bytes per line (2880 for 2304px)
raw = np.fromfile(inp, dtype=np.uint8)
need = H * stride
if raw.size < need:
    sys.exit("file too small: have %d bytes, need %d (W=%d H=%d)"
             % (raw.size, need, W, H))
raw = raw[:need].reshape(H, stride)

# Each line: stride bytes = (W/4) groups of 5 bytes = W pixels.
g = raw.reshape(H, W // 4, 5).astype(np.uint16)
b0, b1, b2, b3, b4 = g[..., 0], g[..., 1], g[..., 2], g[..., 3], g[..., 4]

px = np.empty((H, W), np.uint16)
px[:, 0::4] = (b0 << 2) | ((b4 >> 0) & 0x3)
px[:, 1::4] = (b1 << 2) | ((b4 >> 2) & 0x3)
px[:, 2::4] = (b2 << 2) | ((b4 >> 4) & 0x3)
px[:, 3::4] = (b3 << 2) | ((b4 >> 6) & 0x3)   # values now 0..1023

# 16-bit Bayer for ffmpeg debayering.
px.astype("<u2").tofile(inp + ".bayer16")

# Contrast-stretched 8-bit grayscale PGM for an instant look.
# Stretch the 1st..99.5th percentile to 0..255 (ignores hot pixels / black
# offset), then apply gamma 0.5 to lift the shadows — so even a dark scene is
# clearly visible without re-capturing.
peak = int(px.max())
lo = float(np.percentile(px, 1.0))
hi = float(np.percentile(px, 99.5))
if hi <= lo:
    hi = lo + 1.0
stretched = np.clip((px.astype(np.float32) - lo) / (hi - lo), 0.0, 1.0)
norm = ((stretched ** 0.5) * 255.0).astype(np.uint8)   # gamma 0.5 brighten
with open(inp + ".pgm", "wb") as f:
    f.write(b"P5\n%d %d\n255\n" % (W, H))
    norm.tofile(f)

print("max=%d  median=%d  p99.5=%d  (%.0f%% of full scale at p99.5)"
      % (peak, int(np.median(px)), int(hi), 100.0 * hi / 1023))
print("wrote %s.pgm  (grayscale, normalized — view this first)" % inp)
print("wrote %s.bayer16  (for color via ffmpeg)" % inp)
print("color: ffmpeg -f rawvideo -pixel_format bayer_rggb16le "
      "-video_size %dx%d -i %s.bayer16 %s.png" % (W, H, inp, inp))
