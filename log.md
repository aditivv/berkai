# Session Log

A running log of what was done in each Claude Code session, prompt by prompt.
Newest session last. Kept short and readable — deep technical history lives in
`progress/PROGRESS.md`; this file is for quickly answering "where were we and
why" at the start of a session, for any collaborator.

**Maintenance rule (for future sessions):** append an entry per prompt as you
work — what was asked, what was done, what was verified, and anything a future
session must know. Update the **Future Goals** section at the bottom whenever
the roadmap changes.

---

## Before this log existed (through 2026-06-26)

Covered in detail by `progress/PROGRESS.md` (Phases 1–7): Flask + Leaflet
dashboard scaffolding, QNX port on the Pi 5, DHT11 first attempt (qnx2 branch —
decode unreliable, root-caused to `gpio_event_get()` jitter), YOLO crack
detection (`ai_triage.py`), and the custom QNX camera driver `camera_resmgr`
publishing `/dev/video0` (RAW10 from the IMX708).

A follow-up session (branch `video_stream_extraction`) built the Python
capture/stream layer in `camera_driver/`: `imx708_stream.py` (RAW10 unpack →
debayer → 8-bit frames, `fast8` fast path), `mjpeg_server.py` (standalone
~14 fps browser stream, FrameHub pattern), `bench_stream.py`. Key finding:
pipeline is capture-bound at ~14 fps — that's the ceiling, and it's fine.
Handoff doc: `progress/HANDOFF_stream_merge.md`.

---

## Session 2026-07-03 — camera merge into app.py (branch `video_stream_merge`)

**Prompt 1 — "Merge the camera video stream into app.py" (handoff doc).**
Built the integration layer:
- New `camera_source.py`: `CameraHub` — the ONE capture thread on `/dev/video0`
  (single-buffer driver → single reader), broadcasts latest annotated JPEG to
  all `/video_feed` clients + latest raw BGR frame to the inference loop.
  Config: `fast8 + auto debayer + bgr + 960x540`. Missing device → "NO CAMERA
  SIGNAL" card, 3 s retry, off-Pi webcam fallback for dev.
- `camera_driver/imx708_stream.py`: added `color="bgr"` (exact channel swap,
  verified) so app/ai_triage get cv2's convention with no per-frame conversion.
- `app.py`: dropped the `qnx_apis` placeholder; `/video_feed` relays hub JPEGs;
  inference decoupled (every 2 s on the latest frame). Added `threaded=True`
  (one stream client would otherwise block every other route).
- New `start.sh` (starts driver if root, then app) and `.gitignore` hygiene.
- Verified on Windows with a fake stream: multipart frames flow end-to-end via
  Flask test client. Code-review pass found 2 HIGH issues (capture-thread
  exception scope, unthreaded Flask) — fixed. Also narrowed
  `imx708_stream.frames()` to catch only `ShortReadError` so a dead driver
  propagates to the hub's retry logic instead of spinning.

**Prompt 2 — `pip install onnxruntime` fails on the Pi.**
Expected: onnxruntime has no QNX wheels. Added a cv2.dnn fallback backend to
`ai_triage.py` (`cv2.dnn.readNetFromONNX`; cv2 IS on the Pi). Verified locally:
cv2.dnn output matches onnxruntime to max abs diff 1.6e-3 on the real model
(`runs/segment/pipedown_crack_seg/weights/best.onnx`, det head 1×10×8400),
identical detections through both backends. Class names hardcoded for the
cv2.dnn path (matches the model's embedded names). No pip install needed on Pi.

**Prompt 3 — how to run start.sh.** `sh start.sh` or `chmod +x` once. Verified
the file has LF endings.

**Prompt 4 — `ModuleNotFoundError: flask` on the Pi.** Cause: an active
`.venv` hiding the system-site packages (venvs can't pip-install cv2/numpy on
QNX). Advice: `deactivate` and use the system Python.

**Prompt 5 — dashboard loads but `/static/leaflet.*` 404.** Leaflet 1.9.4 was
npm-installed but never copied to `static/`. Copied `leaflet.css`, `leaflet.js`
and `images/` from `node_modules/leaflet/dist/` into `static/`. User confirmed:
**live video working on the dashboard.** (User committed: "Merge video stream",
"changed model to cv2 dnn", "added leaflet to static".)

**Prompt 6 — plan DHT11 integration (planner agent, no repo changes).**
Produced an 8-phase incremental plan (each phase = test gate + commit) on a new
branch `feat/dht11-regread`. Core decision: the qnx2 failure was the *capture
mechanism* (`gpio_event_get()` dequeue jitter can't discriminate 26 µs vs 70 µs
pulses), so the new driver busy-waits on RP1 GPIO registers directly
(`camera_resmgr.c` mmap pattern) with `ClockCycles()` timestamps at FIFO
priority, ending as a root resmgr publishing `/dev/dht11` (0666) with a ≥2 s
internal rate limit. Phases: 0 wiring+pin sanity → 1 rp1_gpio module → 2
busy-wait capture + pulse histogram (**go/no-go gate: bimodal 26/70 µs, 4 h
time-box, else pivot to I2C sensor contingency**) → 3 offline decode → 4 live
reliability (50 reads ≥90% checksum-valid) → 5 resmgr → 6 sensor_reader.py +
humidity data model → 7 dashboard humidity → 8 optional thresholds/alerts.

**Prompt 7 — wiring question.** DHT11: VCC→3.3 V (pin 1), DATA→GPIO17 (header
pin 11), GND→pin 9; 10 kΩ pull-up (built into 3-pin breakout modules). The
Phase 0 register test itself only needs a jumper wire. User wired it up.

## Session 2026-07-04 — DHT11 Phase 0 (branch `feat/dht11-regread`)

**Prompt 1 — proceed with Phase 0 (Pi still powered off).**
Created branch `feat/dht11-regread` off `video_stream_merge`. Wrote
`dht11_driver/gpio_peek.c` (~200 lines) + `dht11_driver/Makefile`:
- mmaps IO_BANK0 (0xD0000), SYS_RIO0 (0xE0000), PADS_BANK0 (0xF0000) with the
  proven `PROT_NOCACHE` pattern; muxes GPIO17 to SYS_RIO (funcsel 5), sets pad
  IE + pull-up; polls ~10×/s printing the level from BOTH candidate input paths
  (`RIO_IN` @ +0x08 and the pin's STATUS word) to localise any wrong-offset
  assumption; restores original pin config on Ctrl-C.
- The register READ path is the unverified part — `camera_resmgr.c` only ever
  writes OUT/OE. That's exactly what the Phase 0 jumper test validates.
- Gitignored `dht11_driver` build outputs; added `.gitattributes` LF rules for
  `*.sh/Makefile/*.c/*.h/*.py` (CRLF would break them on QNX).
- Commits: `chore(dht11): add gpio_peek...`, `chore: force LF endings...`;
  branch pushed to origin.
- **Phase 0 gate still open (needs the Pi):** build in `~/berkai/dht11_driver`,
  run `./gpio_peek` as root; idle must read HIGH (sensor pull-up), and ~20
  jumper touches to GND must all track. On failure, capture the register dump
  lines for diagnosis.

**Prompt 2 — this file.** Created `log.md` (session/prompt log + future goals),
to be maintained every session.

**Prompt 3 — first gpio_peek run on the Pi.** Everything it exercised passed:
config writes read back (funcsel 31→5, pad IE/PUE set), line reads 0 with
input disabled → 1 once the pull-up engaged (STATUS bits 17-19 high) — wiring
and read path respond to pad config. To close the gate without a manual
jumper, added `--selftest` to gpio_peek: the pin drives itself LOW and watches
its own input, 20 cycles (drive-low → IN must read 0; release → pull-up must
restore 1). Only drives low (the DHT11 start-signal pattern — no contention
risk). This also pre-validates Phase 1's output-drive requirement.

---

## Future Goals

**Active: DHT11 integration (branch `feat/dht11-regread`)** — see Prompt 6
above for the full plan. Status:
- [ ] Phase 0: on-Pi jumper test of `gpio_peek` (code ready, awaiting hardware run)
- [ ] Phase 1: `rp1_gpio.c/h` reusable GPIO module (input + output drive tests)
- [ ] Phase 2: busy-wait capture + raw pulse-width dump — **go/no-go histogram
      gate** (bimodal ~26 µs vs ~70 µs; 4 h time-box, else I2C AHT20/SHT3x
      contingency on /dev/i2c6)
- [ ] Phase 3: offline 40-bit decode + checksum from saved dumps (restores humidity)
- [ ] Phase 4: live CLI, 50 consecutive reads ≥90% checksum-valid vs reference
- [ ] Phase 5: `/dev/dht11` resource manager (root, 0666, ≥2 s internal rate limit)
- [ ] Phase 6: `sensor_reader.py` — humidity through the data model, segment 0
      real reads, sim fallback; extend `start.sh`
- [ ] Phase 7: dashboard humidity (stat box, detail view, poll wiring)
- [ ] Phase 8 (optional): humidity thresholds + alert email wording

**Backlog / later:**
- Merge `video_stream_merge` → `main` once the demo is stable.
- Camera fps beyond ~14: sensor registers (`0x0340/0x0341` frame length) or
  driver ISR/double-buffering — explicitly deferred, capture-bound today.
- Scene brightness: `fast8` does no tone-mapping; if the monitored scene is
  dim, light it or raise exposure in `imx708_regs.h` (`0x0202/0x0203`).
- Password-less SSH key for the Pi would let Claude sync/test on-device
  directly (today: password-only, so all Pi steps are manual).
- Model: current ONNX is a 6-class det head (Deformation/Obstacle/Rupture/
  Disconnect/Misalignment/Deposition); revisit training/conf threshold after
  real-camera footage is observed.
