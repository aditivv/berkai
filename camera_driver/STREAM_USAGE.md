# imx708_stream.py — run & verify on the Pi

Extracts a stream of **model-ready 8-bit frames** from `/dev/video0` (the
working IMX708 driver). Reads raw RAW10 → unpacks → debayers → 8-bit →
optional resize → saves `frame_NNNN.jpg` every N seconds. No OpenCV required.

## 0. Sync the file to the Pi
Edited on Windows; must land at `~/berkai/camera_driver/imx708_stream.py`.
Confirm it arrived before running:
```bash
grep -n "COLOR_BayerBG2BGR" ~/berkai/camera_driver/imx708_stream.py   # should print line ~91
```

## 1. Make sure the driver is up (as root)
```bash
cd ~/berkai/camera_driver
su                                       # password: root
./camera_resmgr >/tmp/cam.log 2>&1 &     # wait for "=== camera_resmgr ready ==="
exit                                     # capture runs fine as qnxuser (node is 0666)
```

## 2. Decide where debayer runs — check cv2 once
```bash
python3 -c "import cv2, numpy; print('cv2', cv2.__version__)"
```
- **cv2 present** → full-resolution 2304×1296 debayer is used automatically.
- **cv2 missing** → the script falls back to a pure-numpy 2×2-bin demosaic
  (half-res, 1152×648, artifact-free) and encodes JPEGs via PIL or, failing
  that, ffmpeg. Nothing to install. This is the expected QNX path.

## 3. First capture — settle Bayer order + exposure on a LIT scene
Point the camera at something bright and detailed, then:
```bash
cd ~/berkai/camera_driver
python3 imx708_stream.py --interval 2 --count 4 --out ./frames
```
View them from your PC:
```bash
python3 -m http.server 8090 --directory ./frames     # then open http://<pi-ip>:8090/
ifconfig | grep inet                                  # find <pi-ip>
```

**Bayer order is unconfirmed.** If colours look wrong (e.g. red↔blue swapped),
re-run with each option and keep the one that looks right:
```bash
for b in rggb grbg bggr gbrg; do
  python3 imx708_stream.py --bayer $b --count 1 --interval 0 --out ./test_$b
done
```
A given `--bayer` flag means the **same physical pattern** whether cv2 or the
numpy path is used (verified), so the choice carries over.

**Exposure.** If frames are too dark even on a lit scene, the default
`--tonemap stretch` (percentile + gamma 0.5) already lifts dim scenes a lot. If
still too dark, raise sensor exposure in `imx708_regs.h` (`0x0202/0x0203`, and
`0x0340/0x0341` frame length to allow longer integration), then `make` +
restart `camera_resmgr`. Longer exposure trades frame rate for brightness —
fine for "1 frame every few seconds".

## 4. Normal run (the deliverable)
```bash
# One JPEG every 3 s, 5 frames, default RGGB, stretched 8-bit RGB:
python3 imx708_stream.py --interval 3 --count 5 --out ./frames

# Run forever, grayscale, sized for a YOLO model:
python3 imx708_stream.py --interval 0 --count 0 --color gray --resize 640x640 --out ./frames
```
Each saved frame logs its shape and mean brightness, so you can tell live
whether the scene is exposed correctly without opening every file.

## Key options
| flag | default | meaning |
|------|---------|---------|
| `--interval` | `3.0` | seconds between saved frames; `0` = as fast as the loop allows |
| `--count` | `5` | frames to save; `0` = run forever |
| `--bayer` | `rggb` | `rggb` / `grbg` / `bggr` / `gbrg` — pick what looks right |
| `--color` | `rgb` | `rgb` or `gray` |
| `--tonemap` | `stretch` | `stretch` (percentile+gamma, good for dim scenes) or `shift` (raw `>>2`) |
| `--gamma` | `0.5` | shadow lift for `stretch` |
| `--debayer` | `auto` | `auto` (cv2 if present, else `bin`) / `cv2` / `bin` |
| `--resize` | off | e.g. `640x640` for the model |
| `--out` | `./frames` | output folder the model can poll |

## Programmatic use (next session — model wiring)
```python
from imx708_stream import Imx708Stream, StreamConfig
with Imx708Stream(StreamConfig(color="rgb", resize=(640, 640))) as cam:
    for frame in cam.frames():        # HxWx3 uint8 numpy arrays
        detections = detect_defects(frame)   # ai_triage.py, later
```

## Notes / gotchas
- One `read()` = one full frame (3,732,480 bytes). A short read is rejected and
  the frame skipped rather than spliced across boundaries.
- Single DMA buffer + polling: each frame is the latest complete one, no queue.
  Fine for crack detection; not frame-perfect video.
- Throughput is bottlenecked by the polling read + numpy unpack (tens of ms),
  not the sensor. "1 frame every few seconds" is trivial; smooth real-time
  video would need the ISR/double-buffer driver upgrade (out of scope).
- Off-device verification (synthetic RAW10 frame) confirmed: lossless unpack,
  exact stride/frame size, cv2 and numpy debayer agree on colour, and all three
  JPEG encoders produce valid files.
- A stale `__pycache__/*.pyc` in this OneDrive folder can shadow edits when
  testing locally on Windows; it's irrelevant on the Pi (fresh files). `git`
  ignores `__pycache__`.
```
