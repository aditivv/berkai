# Handoff Prompt — Continuous Video Stream from the QNX IMX708 Camera

> Paste everything below into a fresh session. It is fully self-contained.
> For the deep history of how the camera driver was built and fixed, also read
> `progress/PROGRESS.md` (especially Sections 9, 15, and 16).

---

## 0. MISSION FOR THIS SESSION (scope-limited on purpose)

The custom QNX camera driver is **done and working** — `/dev/video0` already
streams live RAW10 frames from a Raspberry Pi Camera Module 3 (IMX708) on a
Raspberry Pi 5 running QNX SDP 8.0.4.

**This session's only goal:** extract the camera output as a **continuous
stream of model-ready frames** — such that *every frame*, or *one frame every
few seconds*, can be fed into a computer-vision model. The model is for crack
detection in pipes and will be wired up in a *later* session. **Do not build
the model integration now.** Build the extraction + frame-format layer only.

A "model-ready frame" means: a single **debayered, 8-bit image** (RGB or
grayscale), at the cadence we choose (continuous, or 1 every N seconds),
available to hand to the model — as an in-memory array, a saved JPEG/PNG, or an
MJPEG stream. Pick whichever is simplest to verify; favor saving periodic
JPEGs or an MJPEG endpoint.

**Definition of done:** a running process that reads `/dev/video0` in a loop and
produces a stream of correctly-debayered 8-bit frames at a configurable
interval, demonstrated by saving (or streaming) a few consecutive frames that
visibly show the scene.

---

## 1. HARDWARE / OS CONTEXT

- **Board:** Raspberry Pi 5 (BCM2712 SoC + RP1 I/O chip over PCIe).
- **OS:** QNX SDP 8.0.4 (NOT Linux — no V4L2, no libcamera, no `cv2.VideoCapture`
  for the CSI camera).
- **Sensor:** Sony IMX708 (Camera Module 3), on the **CD0** CSI connector,
  control bus `/dev/i2c6`, I2C addr `0x1A`.
- **Pi access:** SSH in as `qnxuser`. Root is `root`/`root` on the stock image
  (`su` then password `root`). The camera driver needs root (physical-memory
  mapping ability `PROCMGR_AID_MEM_PHYS`).
- **On the Pi already:** `python3` (3.14) with **numpy**, and **ffmpeg 8.0**.
  No `bayer_*10` pixel format in ffmpeg (only 8/16-bit) — see §4.
- **Display:** assume none. View images by serving over
  `python3 -m http.server <port> --directory <dir>` and opening
  `http://<pi-ip>:<port>/<file>` in a browser on your PC. (`ifconfig | grep inet`
  for the Pi IP.)

---

## 2. CURRENT WORKING STATE OF THE CAMERA (what the prior session achieved)

The driver `camera_driver/camera_resmgr.c` is a QNX resource manager that
publishes `/dev/video0`. It is confirmed working end-to-end: the sensor streams,
frames are DMA'd into RAM, and reading the device returns real pixel data
(~99.6% non-zero buffer). The full bring-up chain that was fixed: correct CSI
block (CSI0), enabling the MIPI config clock, HSFREQRANGE for 900 Mbps/lane,
a sensor power-on reset, AUTO_ARM continuous capture, a CH_DEBUG-polling read
path, and the PCIe DMA inbound-window address translation. All of that is in
place — **you should not need to touch the C driver** for this session.

Key behavioral facts you depend on:
- **One `read()` of `/dev/video0` returns exactly one full frame** (it blocks
  until a fresh frame lands, polling the hardware frame counter), then you get
  the whole buffer back.
- The device node is `crw-rw-rw-` (0666), so **`qnxuser` can read it** — but the
  **driver process must be running** (started as root) for the node to exist.
- The driver overwrites a **single DMA buffer** each frame and is **polling**
  (no interrupt). Fine for periodic capture; not optimized for high-fps
  low-latency video (that upgrade — ISR + double buffering — is out of scope).

---

## 3. FRAME FORMAT (the most important detail for this session)

`/dev/video0` delivers **MIPI CSI-2 RAW10, packed**:

```
Resolution:   2304 x 1296   (IMX708 2x2-binned mode)
Pixel format: RAW10 Bayer, MIPI-packed
Line stride:  2880 bytes  (= 2304 * 10 / 8)
Frame size:   3,732,480 bytes  (= 2880 * 1296)   <-- read this many bytes per frame
```

**MIPI RAW10 packing:** every **5 bytes encode 4 pixels** — 4 bytes hold the
high 8 bits of 4 pixels, the 5th byte holds the 2 LSBs of each. Unpack to 10-bit
values (0–1023) as:

```
P0 = (b0 << 2) | ( b4        & 0x03)
P1 = (b1 << 2) | ((b4 >> 2)  & 0x03)
P2 = (b2 << 2) | ((b4 >> 4)  & 0x03)
P3 = (b3 << 2) | ((b4 >> 6)  & 0x03)
```

A reference numpy implementation already exists: **`camera_driver/raw10_unpack.py`**
(reads a raw frame, writes a 16-bit Bayer file + a contrast-stretched grayscale
PGM). Reuse its unpack logic.

**Two unconfirmed things to verify early on a well-lit scene:**
1. **Bayer order** — assumed RGGB (`bayer_rggb16le` / `cv2.COLOR_BayerRG2*`),
   but NOT confirmed. Try rggb / grbg / bggr / gbrg and keep whichever looks
   right.
2. **Exposure** — current frames are **underexposed** in normal indoor light.
   Analog gain was raised to 8× (`imx708_regs.h`, reg `0x0204/0x0205 = 0x0380`),
   but images are still dim. For usable crack-detection frames you may need to
   raise **exposure time** (`0x0202/0x0203`) and/or **frame length**
   (`0x0340/0x0341`) in `imx708_regs.h`, or just ensure good lighting. (Longer
   exposure trades frame rate for brightness — acceptable for "1 frame / few
   seconds".)

---

## 4. HOW TO BUILD / RUN / CAPTURE (verified commands)

```bash
# On the Pi, in the repo's camera_driver folder:
cd ~/berkai/camera_driver
make                                  # builds ./camera_resmgr (and ./rp1_clk_dump)

# Start the driver as root and leave it running:
su                                    # password: root
./camera_resmgr >/tmp/cam.log 2>&1 &  # wait for "=== camera_resmgr ready ===" in the log
exit                                  # (capture can be done as qnxuser; node is 0666)

# Capture one raw frame:
dd if=/dev/video0 of=/tmp/frame.raw bs=3732480 count=1

# Unpack + make something viewable:
python3 raw10_unpack.py /tmp/frame.raw
ffmpeg -y -f rawvideo -pixel_format bayer_rggb16le -video_size 2304x1296 \
       -i /tmp/frame.raw.bayer16 -update 1 /tmp/frame.png
# grayscale (auto-brightened, good for dark scenes):
ffmpeg -y -i /tmp/frame.raw.pgm -update 1 /tmp/frame_gray.png

# View from your PC:
python3 -m http.server 8090 --directory /tmp
#   -> browser: http://<pi-ip>:8090/frame.png   (or frame_gray.png)
```

Note: ffmpeg has **no `bayer_rggb10`** — RAW10 must be unpacked to 16-bit first
(that's what `raw10_unpack.py` produces). `cv2` can debayer directly if it's
available (see §6).

---

## 5. SUGGESTED APPROACH FOR THIS SESSION

Write a small Python capture module, e.g. `camera_driver/imx708_stream.py`,
that:

1. Opens `/dev/video0` and, in a loop, reads exactly `3,732,480` bytes per
   iteration (one frame; the read blocks until a fresh frame is ready).
2. Unpacks RAW10 → a `1296 x 2304` 16-bit Bayer array (numpy, vectorized — reuse
   `raw10_unpack.py`). Watch the line stride (2880, no extra padding).
3. Debayers Bayer → RGB (or grayscale). Options: `cv2.cvtColor(..., COLOR_BayerRG2BGR)`
   if cv2 is present; else a simple numpy 2x2 bilinear/nearest debayer; else shell
   out to ffmpeg per frame (slower).
4. Converts 10-bit → 8-bit (right-shift 2, or a contrast stretch like the
   PGM path), and optionally resizes for the model (YOLO commonly 640x640).
5. Emits at a configurable cadence:
   - **Periodic snapshot mode** (recommended first): save `frame_NNNN.jpg`
     every N seconds into a folder the model can poll. Simplest to verify.
   - **MJPEG stream mode:** `app.py` already has a `/video_feed` MJPEG endpoint
     (currently wired to a USB webcam / placeholder) — produce JPEG frames into
     that generator so the dashboard *and* the model can consume one source.
   - **Direct generator:** `yield` numpy frames to be passed into the model
     later.

Then prove it: run the loop, drop a few consecutive JPEGs, and view them to
confirm the stream is live and correctly debayered/exposed.

---

## 6. KNOWN CONSTRAINTS & THINGS TO RESOLVE EARLY

- **Is `cv2` (OpenCV) / `onnxruntime` available on QNX?** UNKNOWN and important.
  `ai_triage.py` uses `cv2.dnn.readNetFromONNX(...)`. Verify on the Pi:
  `python3 -c "import cv2, numpy; print(cv2.__version__)"`. If cv2 is missing,
  decide early: (a) do the debayer/resize with **numpy only** and run the model
  **off-device** (POST frames to a backend that has cv2/onnxruntime), or (b)
  install/port cv2 on QNX. This choice shapes the whole frame-format layer.
- **Throughput:** the sensor runs ~56 fps internally, but the polling read +
  numpy unpack of 3.7 MB/frame is the bottleneck (tens of ms each). "1 frame
  every few seconds" is trivial; smooth real-time video will need measuring and
  possibly the ISR/double-buffer driver upgrade (out of scope here).
- **Single buffer / polling:** each read gives the latest complete frame; there's
  no queue. Don't assume frame-perfect continuity — fine for crack detection.
- **Exposure/Bayer:** unconfirmed (see §3) — settle both on a lit scene before
  declaring frames "model-ready".
- **Git / OneDrive gotcha:** the repo lives in a OneDrive folder on Windows, and
  git commits frequently fail with `.git/index.lock: File exists`. Commits are
  made **manually by the user** (`del .git\index.lock .git\HEAD.lock` then
  `git add ... && git commit`). Don't rely on automated commits succeeding; write
  files and let the user commit. Active branch: **`camera_driver`**.
- **Code → Pi sync:** files are edited on the Windows side; they must be synced
  to the Pi (`~/berkai/...`) before `make`/run. Confirm the file is on the Pi
  (e.g. `grep` a known new line) before building.

---

## 7. REPOSITORY STRUCTURE

```
berkai/                         (git repo; active branch: camera_driver)
├── app.py                      Flask backend: auth, /api/* endpoints, /video_feed MJPEG, AI-triage loop
├── sensor_reader.py            Simulated + real (DHT11) sensor data thread
├── ai_triage.py                Crack detection: YOLO/ONNX via cv2.dnn + OpenCV Canny fallback; detect_defects()
├── config.py                   SEGMENTS dict (segment IDs, thresholds, names)
├── route_planner.py            placeholder (unused)
├── requirements.txt
├── static/
│   ├── index.html              Leaflet map dashboard (polls /api/*)
│   └── login.html
├── camera_driver/              <-- main working area for this session
│   ├── Makefile                QNX native build (gcc) -> camera_resmgr, rp1_clk_dump
│   ├── camera_resmgr.c         THE DRIVER: maps RP1, inits IMX708, DMA, exposes /dev/video0
│   ├── dphy.h / dphy.c         DW CSI-2 Host + D-PHY register map + bring-up (HSFREQRANGE 0x14)
│   ├── csi2.h / csi2.c         CSI-2 DMA channel: AUTO_ARM, CH_DEBUG frame counter, DT 0x2b
│   ├── imx708_regs.h           Sensor I2C register tables (exposure 0x0202/3, gain 0x0204/5=0x0380, frame_len 0x0340/1)
│   ├── rp1_clk_dump.c          Diagnostic: dump RP1 clock/GPIO register regions
│   ├── raw10_unpack.py         numpy: unpack MIPI RAW10 -> 16-bit bayer + grayscale PGM (REUSE THIS)
│   ├── clk_dump.txt            saved RP1 CLOCKS dump
│   ├── gpio_dump.txt           saved RP1 GPIO dump
│   └── (build outputs: camera_resmgr, rp1_clk_dump, *.o, frame.png)
├── dht11_reader.c              QNX C binary: DHT11 GPIO edge capture -> JSON (decode jitter-limited)
├── yolo26n.pt                  YOLO crack-detection weights (.pt); an .onnx version exists too
├── runs/                       YOLO training artifacts
├── pipe-monitoring-handoff.md  original project design doc
└── progress/
    ├── PROGRESS.md             full project log (read Sections 9, 15, 16 for camera details)
    └── HANDOFF_video_stream.md this file
```

**Overall project:** "Distributed Pipe Monitoring Digital Twin" — a Flask +
Leaflet dashboard showing a pipe network, fed by a real Pi node (camera + DHT11
sensor) plus simulated nodes, with AI crack/anomaly detection. The camera feed
from this session ultimately replaces the placeholder/USB-webcam feed and
becomes the input to `ai_triage.detect_defects()`.

---

## 8. SENSOR REGISTER QUICK-REFERENCE (in `imx708_regs.h`, if you tune exposure)

```
0x0100        mode select (0x00 standby, 0x01 streaming)
0x0202/0x0203 coarse integration / exposure time (lines)   <- raise for brightness
0x0204/0x0205 analogue gain (currently 0x0380 = 8x; gain = 1024/(1024-code))
0x020E/0x020F digital gain (0x0100 = 1.0x)
0x0340/0x0341 frame_length_lines (currently 0x0538; raise to allow longer exposure)
0x0005        FRM_CNT  (live frame counter, advances while streaming; read to confirm)
```
The driver power-cycles the sensor (gpio34) and rewrites these tables on every
start, so changes take effect after `make` + restarting `camera_resmgr`.

---

## 9. SUGGESTED FIRST STEPS

1. Sanity-check the camera: build, run `camera_resmgr` as root, `dd` one frame,
   unpack + view — confirm `/dev/video0` is alive and producing data.
2. Resolve the **cv2/onnxruntime-on-QNX** question (§6) — it decides whether
   debayer + the model run on-device or off-device.
3. Confirm **Bayer order** and acceptable **exposure** on a lit scene (§3).
4. Write `imx708_stream.py`: read loop → unpack → debayer → 8-bit (+resize) →
   emit at a chosen cadence (start with "save a JPEG every N seconds").
5. Demonstrate a few consecutive saved/streamed frames that clearly show the
   scene — that's the deliverable. Leave actual model inference for the next
   session.
