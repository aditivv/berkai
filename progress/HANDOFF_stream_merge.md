# HANDOFF — merge the camera video stream into app.py

## 0. MISSION FOR THIS SESSION (scope-limited on purpose)

The camera extraction + frame-format layer is **done and working**. A previous
session produced a live MJPEG video stream from the QNX IMX708 driver at **~14
fps colour**, viewable in a browser via a standalone server (`mjpeg_server.py`).

This session's only goal: **replace the placeholder / `qnx_apis` camera source in
`app.py` with the real IMX708 stream**, so that the dashboard's `/video_feed`
endpoint shows the live camera and `ai_triage.detect_defects()` runs on real
frames. Do **not** re-tune the capture pipeline, touch the C driver, or chase
higher fps — that is settled and out of scope. Build the **integration layer only**.

Definition of done: `python3 app.py` on the Pi serves the real camera at
`/video_feed`, the Leaflet dashboard shows live motion, and `/api/detections`
reflects crack/anomaly detection run on those real frames — demonstrated in a
browser.

Active branch for this work: **`video_stream_merge`** (create it from
`video_stream_extraction`, which holds the streaming layer described below).

---

## 1. WHAT THE PREVIOUS SESSION ACHIEVED (the streaming layer)

Starting from a working driver (`/dev/video0` streaming RAW10), we built a
Python capture + frame-format + streaming layer, all in `camera_driver/`:

* **`imx708_stream.py`** — the core module. Reads `/dev/video0` in a loop,
  unpacks MIPI RAW10, debayers to 8-bit RGB/gray, optional resize, emits frames.
* **`bench_stream.py`** — per-phase benchmark harness (read / unpack / debayer /
  encode timing across configs). This is how the bottlenecks below were found.
* **`mjpeg_server.py`** — standalone HTTP MJPEG server (stdlib only), separate
  from `app.py`. This is what currently produces the working 14 fps browser feed.
* **`STREAM_USAGE.md`** — run/verify notes.

**Key wins:**

1. **`fast8` unpack.** The full 10-bit RAW10 unpack cost ~53 ms/frame (the
   dominant bottleneck). `unpack_raw10_fast8()` keeps the 4 MSB bytes of each
   5-byte MIPI group and drops the LSB byte, producing an 8-bit Bayer array
   directly. It is **bit-identical to `unpack_raw10() >> 2`** (the discarded 2
   LSBs are below the sensor noise floor) and dropped unpack to **~15 ms**.
2. **Working colour MJPEG stream at ~14 fps**, decoupled capture thread, viewable
   at `http://<pi-ip>:8091/`.

**Key finding — we are now CAPTURE-BOUND, not processing-bound.** After `fast8`,
most configs converge on ~71 ms/frame total (~14 fps) regardless of processing
load — the signature of a frame-cadence wall (sensor exposure/`frame_length` or
the driver's polling read), not CPU. **This matters for the merge:** ~14 fps is
the ceiling the dashboard will get, and it's fine for crack detection. Going
faster later means the sensor registers or the driver (ISR + double buffering),
both **out of scope** here.

Measured phase costs at the streaming config (`fast8`, cv2, colour):

| phase   | cost      | notes |
|---------|-----------|-------|
| read    | 8–20 ms   | includes wait-for-fresh-frame (absorbs cadence slack) |
| unpack  | ~15 ms    | fast8 strided byte-gather (was ~53 ms full) |
| debayer | ~4–36 ms  | cv2 full-res + resize; gray cheaper than rgb |
| encode  | ~5–78 ms  | JPEG; tiny at 640/960, ~78 ms at full-res rgb |

Best colour config: `fast8 + cv2 + rgb + 960x540` ≈ 14 fps. `mjpeg_server.py`
defaults to exactly this.

---

## 2. HARDWARE / OS CONTEXT (unchanged, still critical)

* **Board:** Raspberry Pi 5 (BCM2712 SoC + RP1 I/O chip over PCIe).
* **OS:** QNX SDP 8.0.4 — **NOT Linux.** No V4L2, no libcamera, no
  `cv2.VideoCapture` for the CSI camera, no access to the Pi's hardware ISP or
  H.264 encoder. All debayer/encode is CPU-side (this is why QNX buys
  determinism, not throughput — see the earlier session's notes).
* **Sensor:** Sony IMX708 (Camera Module 3) on CD0 CSI, control bus `/dev/i2c6`,
  addr `0x1A`. Runs in a 2304×1296 2×2-binned RAW10 mode.
* **The camera driver is a separate root process.** `camera_resmgr` (a QNX
  resource manager) must be started **as root** (needs `PROCMGR_AID_MEM_PHYS`)
  and left running; it publishes `/dev/video0` (mode `crw-rw-rw-`, so `qnxuser`
  can read it, but the node only exists while the driver runs). **If the driver
  isn't running, any reader fails with `No such file or directory: /dev/video0`.**
* **Pi access:** SSH as `qnxuser`; root is `root`/`root` (`su`, password `root`).
* **cv2 IS available on the Pi** (confirmed: `cv2=True` in the benchmark). This
  is important — `ai_triage.py` uses `cv2.dnn.readNetFromONNX(...)` and OpenCV
  Canny, so the model path can run **on-device**. `PIL` is NOT present; ffmpeg 8.0
  is. numpy is present (Python 3.14).
* **No display.** View by serving over HTTP and opening in a browser on your PC
  (`ifconfig | grep inet` for the Pi IP).

---

## 3. FRAME FORMAT (recap — you likely won't touch this)

`/dev/video0` delivers MIPI CSI-2 RAW10, packed:

```
Resolution   2304 x 1296   (16:9)
Line stride  2880 bytes    (= 2304 * 10 / 8)
Frame size   3,732,480 bytes   <- exactly one read() per frame
```

`imx708_stream` handles all of this. Relevant facts for the merge:

* **Colour order is RGGB** (confirmed to look right this session). The
  `--bayer` knob (`rggb|grbg|bggr|gbrg`) is there if a future scene proves it
  wrong; both the cv2 and numpy debayer paths agree on a given label.
* **`imx708_stream` produces RGB**, not BGR. `app.py`/`ai_triage` are written for
  **cv2's BGR** convention (`cv2.cvtColor(frame, COLOR_BGR2GRAY)`, box colours).
  This is the single most important integration detail — see §6.
* **`fast8` does no brightening** (it equals the `shift` tonemap). On a dim scene
  frames look dark. If the monitored scene is dark, either light it, raise sensor
  exposure in `imx708_regs.h` (`0x0202/0x0203` exposure, `0x0340/0x0341` frame
  length — the latter also affects the fps wall), or use the slower `stretch`
  tonemap. For crack detection, good lighting is the cheap fix.

---

## 4. imx708_stream.py — THE API YOU'LL BUILD ON

Public surface (all in `camera_driver/imx708_stream.py`):

```python
@dataclass(frozen=True)
class StreamConfig:
    device="/dev/video0"; width=2304; height=1296
    bayer="rggb"; color="rgb"|"gray"
    tonemap="stretch"|"shift"; gamma=0.5
    unpack="full"|"fast8"                 # use "fast8" for streaming
    debayer="auto"|"cv2"|"bin"            # auto = cv2 if present, else numpy 2x2-bin
    resize=None|(w,h); jpeg_quality=90

class Imx708Stream:                        # context manager
    def open(self) / close(self)
    def read_raw_frame(self) -> bytes       # one full frame (blocks)
    def capture(self) -> np.ndarray         # read+unpack+debayer(+resize) -> 8-bit HxWx3 (rgb) or HxW (gray)
    def frames(self, interval=0.0, count=0) -> Iterator[np.ndarray]

def encode_jpeg_bytes(img, quality=90) -> bytes   # in-memory JPEG (cv2 or PIL)
def save_jpeg(path, img, quality)                 # file JPEG (cv2 -> PIL -> ffmpeg)
```

Recommended streaming config (what the merge should use):

```python
StreamConfig(unpack="fast8", debayer="auto", color="rgb", resize=(960, 540))
```

**The FrameHub pattern (in `mjpeg_server.py`) is the reference for the merge.** It
runs ONE background capture thread and broadcasts the latest JPEG to all HTTP
clients via a `threading.Condition`. Reuse this idea in `app.py` — see §6.

---

## 5. HOW TO RUN (verified)

```bash
# On the Pi:
cd ~/berkai/camera_driver
su                                    # password: root
./camera_resmgr >/tmp/cam.log 2>&1 &  # wait for "=== camera_resmgr ready ==="
exit                                  # capture runs as qnxuser (node is 0666)
ls -l /dev/video0                     # confirm crw-rw-rw- exists

# Current working stream (the thing we're merging INTO app.py):
python3 mjpeg_server.py               # -> http://<pi-ip>:8091/
#   knobs: --resize 1280x720 | --color gray | --resize none | --bayer grbg

# Benchmark / snapshots (diagnostics):
python3 bench_stream.py
python3 imx708_stream.py --unpack fast8 --color rgb --resize 960x540 --interval 1 --count 4 --out ./frames
```

Common failure: `No such file or directory: /dev/video0` == the driver isn't
running. Start `camera_resmgr` first, then start the reader.

---

## 6. THE INTEGRATION TASK (what to actually build)

`app.py` today (see the `# ── Camera ──` block, ~line 64+):

* `import cv2`; `from ai_triage import detect_defects, annotate_frame, ...`
* `try: import qnx_apis; _VideoCapture = qnx_apis.VideoCapture; except: cv2.VideoCapture`
  — a placeholder that does **not** see the Camera Module 3 under QNX.
* `_get_camera()` opens `_VideoCapture(0)`.
* `_generate_frames()`: `ok, frame = cam.read()` → `detect_defects(frame)` →
  `annotate_frame(frame, detections)` → `cv2.imencode('.jpg', frame)` → yield
  multipart. Stores results in `_latest_detections` (under `_detections_lock`).
* `/video_feed` returns `Response(_generate_frames(), multipart/x-mixed-replace)`.
* `/api/detections` serves `_latest_detections`.
* `ai_triage.detect_defects(frame)` and `annotate_frame(frame, dets)` assume a
  **BGR** numpy image (they call `cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)` and
  draw coloured boxes). `_TRIAGE_INTERVAL=10` and `_TRIAGE_SEGMENT=0` suggest
  inference is meant to run periodically, not every frame.

### Recommended approach (single shared capture source)

1. **Adopt a FrameHub-style single capture thread** (lift/adapt from
   `mjpeg_server.py`) so there is exactly ONE reader of `/dev/video0`. The
   single-buffer polling driver has no queue — multiple concurrent `read()`ers
   (e.g. two `/video_feed` clients, or `/video_feed` + a triage loop) will fight
   over the one buffer. One capture thread + latest-frame broadcast avoids this.
2. **Feed the hub with the real camera:**
   `Imx708Stream(StreamConfig(unpack="fast8", color="rgb", resize=(960,540)))`.
3. **Fix the colour convention.** `imx708_stream` yields **RGB**; cv2/ai_triage
   want **BGR**. Convert once at the boundary (`cv2.cvtColor(rgb, COLOR_RGB2BGR)`),
   OR add a `color="bgr"` option to `imx708_stream` (small, clean). Pick one and
   be consistent, or the detections/annotations will have R/B swapped and gray
   conversion will be subtly off.
4. **Rewire `/video_feed`** to pull the latest frame from the hub, annotate, and
   `imencode` — dropping `qnx_apis`/`_VideoCapture` entirely (or keeping the cv2
   fallback only for off-Pi dev).
5. **Decouple inference from the video framerate.** Run `detect_defects` (cv2.dnn
   ONNX — potentially slower than 14 fps) on a timer (`_TRIAGE_INTERVAL`) or a
   separate thread against the latest frame, not inline on every streamed frame,
   so the video stays smooth. Publish results to `_latest_detections`.
6. **Startup ordering / ops.** `app.py` (as `qnxuser`) needs `camera_resmgr`
   already running as root. Decide: document the two-step start, or have `app.py`
   detect a missing `/dev/video0` and surface a clear error (don't try to launch
   the root driver from the Flask process). A small `start.sh` that launches the
   driver then the app is a reasonable deliverable.

### Alternative (lighter, less clean)

Leave `mjpeg_server.py` running as a separate process and have the dashboard
`<img>` point at `http://<pi-ip>:8091/stream` directly, with `app.py` only doing
detection via its own hub. Works, but splits the video and the API across two
servers and two camera readers — not recommended unless the single-process merge
proves awkward.

### Decisions to make early
* **BGR at the boundary** vs adding `color="bgr"` to `imx708_stream` (recommended:
  the latter, it's a 3-line change and keeps app.py clean).
* **Inference cadence** (every-N-seconds timer vs thread) and target **resize**
  for the model (YOLO commonly 640×640 — note that squares the 16:9 frame;
  consider 640×360 or letterboxing).
* **Whether `/video_feed` shows annotated or raw frames** (annotated is nicer for
  the dashboard; keep a raw source for the model).

---

## 7. REPOSITORY STRUCTURE (branch `video_stream_merge`, off `video_stream_extraction`)

```
berkai/
├── app.py                      Flask backend: auth, /api/*, /video_feed MJPEG, triage  <-- EDIT HERE
├── ai_triage.py                detect_defects() / annotate_frame() — cv2.dnn ONNX + Canny (BGR)
├── sensor_reader.py            sensor data thread (simulated + DHT11)
├── config.py                   SEGMENTS dict
├── static/index.html           Leaflet dashboard (polls /api/*, shows /video_feed)
├── static/login.html
├── camera_driver/              <-- streaming layer from last session (STABLE, reuse)
│   ├── camera_resmgr.c         THE DRIVER (run as root; publishes /dev/video0) — do not touch
│   ├── imx708_stream.py        capture module: Imx708Stream, StreamConfig, frames(), encode_jpeg_bytes  <-- BUILD ON THIS
│   ├── mjpeg_server.py         standalone MJPEG server + FrameHub pattern  <-- REFERENCE / LIFT FrameHub
│   ├── bench_stream.py         per-phase benchmark (diagnostic)
│   ├── raw10_unpack.py         original numpy RAW10 reference
│   ├── STREAM_USAGE.md         run/verify notes
│   ├── imx708_regs.h           sensor registers (exposure/gain/frame_length) — only if tuning brightness
│   ├── dphy.*/csi2.*           driver internals (do not touch)
│   └── (build outputs: camera_resmgr, *.o — DO NOT COMMIT, see §8)
├── yolo26n.pt / .onnx          crack-detection weights (ai_triage loads the ONNX via cv2.dnn)
└── progress/
    ├── PROGRESS.md
    ├── HANDOFF_video_stream.md original camera-extraction handoff
    └── HANDOFF_stream_merge.md this file
```

---

## 8. CONSTRAINTS & GOTCHAS (carry-over, still true)

* **Driver first, always.** `/dev/video0` exists only while `camera_resmgr` runs
  as root. This bit us this session (the exact error above). `app.py` should fail
  loudly and clearly if the node is missing.
* **Single buffer / polling, no queue.** One capture thread only — don't open two
  readers on the device. (This is why the merge should use one FrameHub.)
* **~14 fps ceiling (capture-bound).** Don't expect more from software; it's fine
  for crack detection. Faster = sensor registers or driver ISR/double-buffer, both
  out of scope.
* **cv2 present, PIL absent, ffmpeg present** on the Pi. `encode_jpeg_bytes` uses
  cv2 here. ai_triage's cv2.dnn model runs on-device.
* **RGB vs BGR** — the #1 integration trap (see §6.3).
* **fast8 = no brightening** — ensure adequate light/exposure or detections suffer.
* **Git / OneDrive lock.** The repo lives in a OneDrive folder on Windows; commits
  frequently fail with `.git/index.lock: File exists`. The user commits manually
  (`del .git\index.lock .git\HEAD.lock` then `git add … && git commit`). Don't rely
  on automated commits; write files and let the user commit. **Never commit build
  outputs** (`camera_resmgr`, `*.o`, `*.pyc`, `frames/`, `frame.png`, `dht11_reader`)
  — gitignore them.
* **Code → Pi sync.** Files are edited on Windows and must be synced to the Pi
  (`~/berkai/...`) before running. Confirm the file is on the Pi (`grep` a known
  new line, or `python3 -c "import <module>"`) before trusting a run. Note: the
  Windows working copy is under a OneDrive path and files may be cloud-only until
  opened.

---

## 9. SUGGESTED FIRST STEPS

1. Create branch `video_stream_merge` from `video_stream_extraction`; confirm the
   `camera_driver/` streaming files are present on the Pi.
2. Sanity-check the source: start `camera_resmgr` (root), run `mjpeg_server.py`,
   confirm the 14 fps browser feed still works. That's your known-good baseline.
3. Decide the colour convention (add `color="bgr"` to `imx708_stream`, recommended)
   and the inference cadence (§6).
4. Lift `FrameHub` into `app.py` (or a shared `camera_source.py`), point it at
   `Imx708Stream`, and rewire `/video_feed` + the triage loop to consume it.
5. Prove it: `python3 app.py`, open the dashboard, confirm live video at
   `/video_feed` and real detections at `/api/detections`. Leave any fps chasing
   or driver work for a later session.
```
