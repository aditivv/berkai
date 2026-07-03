# Berkai — Full Project Progress Log

**Project:** Distributed Pipe Monitoring Digital Twin  
**Team:** Shrujan Sriram, Aditi Varia  
**Repo:** github.com/aditivv/berkai  
**Document created:** 2026-06-24  
**Covers:** Project inception → context handoff (2026-06-24) → camera driver fully working, live RAW10 capture over `/dev/video0` (2026-06-26)

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Architecture & Design Decisions](#2-architecture--design-decisions)
3. [Branch Map](#3-branch-map)
4. [Phase 1 — Project Scaffolding & Backend (2026-06-20)](#4-phase-1--project-scaffolding--backend-2026-06-20)
5. [Phase 2 — Frontend Map Dashboard (2026-06-20)](#5-phase-2--frontend-map-dashboard-2026-06-20)
6. [Phase 3 — QNX Port & Sensor Integration (2026-06-20 to 06-21)](#6-phase-3--qnx-port--sensor-integration-2026-06-20-to-06-21)
7. [Phase 4 — DHT11 Sensor on QNX (qnx2 branch, 2026-06-21)](#7-phase-4--dht11-sensor-on-qnx-qnx2-branch-2026-06-21)
8. [Phase 5 — AI Triage / YOLO Crack Detection (2026-06-21)](#8-phase-5--ai-triage--yolo-crack-detection-2026-06-21)
9. [Phase 6 — Camera Driver (camera_driver branch, 2026-06-22 to 06-24)](#9-phase-6--camera-driver-camera_driver-branch-2026-06-22-to-06-24)
10. [Eliminated Approaches & Why](#10-eliminated-approaches--why)
11. [Current State at Handoff](#11-current-state-at-handoff)
12. [Open Work & Next Steps](#12-open-work--next-steps)
13. [Key Files Reference](#13-key-files-reference)
14. [Hardware Reference](#14-hardware-reference)
15. [Phase 7 — Camera Driver Resolution (2026-06-25 to 06-26)](#15-phase-7--camera-driver-resolution-2026-06-25-to-06-26)
16. [Updated State — Working Camera](#16-updated-state--working-camera)

---

## 1. Project Overview

**Goal:** Build a distributed pipe monitoring system with a physical Raspberry Pi node that captures live camera footage and environmental sensor data, runs AI-based anomaly detection, and feeds a real-time web dashboard (the "digital twin"). Additional nodes are simulated in software to demonstrate scale.

**One-line pitch:** A network of fixed camera+sensor nodes mirrors a pipe network in real time, detects cracks/buildup/anomalies, and flags abnormal segments on a live map.

**Hardware in play:**
- Raspberry Pi 5 (BCM2712 + RP1 I/O chip) running QNX SDP 8.0.4
- Sony IMX708 (Raspberry Pi Camera Module 3) on the Pi's CSI-2 connector
- DHT11 temperature/humidity sensor on Pi GPIO
- Arduino (originally planned as sensor node, later replaced by direct Pi GPIO)
- NexiGo USB webcam (for interim testing before IMX708 driver was ready)

---

## 2. Architecture & Design Decisions

The initial design was documented in `pipe-monitoring-handoff.md` (committed before the project started, as a planning artifact).

### Original plan (from handoff doc)
- Arduino reads DS18B20/DHT22 → sends JSON over USB serial to Pi
- Pi captures USB webcam → POSTs frames + sensor data to FastAPI backend
- FastAPI + SQLite stores readings, runs OpenCV frame-diff for anomaly detection
- React frontend shows pipe topology, live snapshots, temp trends, alerts
- 4–8 simulated nodes run alongside the real node to show scale

### What actually shipped (divergences from plan)
| Decision | Original plan | What was built |
|---|---|---|
| Sensor | DS18B20 via Arduino | DHT11 directly on Pi GPIO (no Arduino in final) |
| Camera | USB webcam | Attempted IMX708 via native QNX driver (ongoing) |
| Backend | FastAPI | Flask |
| Anomaly detection | OpenCV frame diff | OpenCV Canny edge detection → then YOLO |
| Frontend | React/Vite | Plain HTML + Leaflet.js (MapLibre dropped, see below) |
| OS | Linux assumed | QNX SDP 8.0.4 (significant added complexity) |

---

## 3. Branch Map

```
main              ← stable frontend/backend, no QNX-specific code
├── browserbase   ← login/email flow experiments (Aditi)
├── browserbase2  ← further browserbase iteration
├── qnx           ← first QNX port: USB webcam attempt, Arduino removal
├── qnx2          ← DHT11 C driver on QNX GPIO
├── opencv        ← OpenCV-based crack detection experiments
├── camera_driver ← IMX708 QNX resource manager (active)
└── 3DPipe        ← 3D frontend experiment
```

**Active branch as of handoff:** `camera_driver`

---

## 4. Phase 1 — Project Scaffolding & Backend (2026-06-20)

### 2026-06-20 12:22 — `85eef40` — Aditi
**"feat: created empty files indicated by claude"**

Project skeleton created. Empty files committed:
- `app.py`, `sensor_reader.py`, `config.py`, `ai_triage.py`
- `route_planner.py` (placeholder, never filled)
- `static/index.html`
- `.gitignore`

### 2026-06-20 12:35 — `cda31d9` — Aditi
**"test code"**

First working code: `app.py` gets a basic Flask route, `sensor_reader.py` gets initial structure.

### 2026-06-20 12:53 — `ce03f3c` — Aditi
**"feat: hard-coded sensor data that displays on localhost:5000 in JSON format"**

First real milestone: Flask backend running at `localhost:5000` returning hardcoded sensor data as JSON. `sensor_reader.py` gets a full simulated sensor thread with temperature readings per segment. `config.py` defines segment metadata.

**Files added/modified:** `app.py` (+38 lines), `config.py` (+5 lines), `sensor_reader.py` (+108 lines), `static/index.html` (+17 lines)

---

## 5. Phase 2 — Frontend Map Dashboard (2026-06-20)

### 2026-06-20 12:55 — `972c5fc` — Shrujan
**"context file"**

Context/planning markdown committed to the repo.

### 2026-06-20 13:45 — `8c5fa07` — Shrujan
**"feat: add MapLibre map with hardcoded pipeline and node markers"**

First real frontend: replaces placeholder `index.html` with a full MapLibre GL JS dashboard. Features:
- OpenStreetMap raster basemap, no API key required
- Pipeline drawn as green glowing line
- Three monitoring node markers
- Click-to-popup showing name, temperature, state, trend
- Dark header + dark-themed popup styling

**Why MapLibre:** Free, open-source, no API key, runs in-browser.

### 2026-06-20 13:47 — `e315d79` — Shrujan
**"feat: split pipeline into state-colored segments with legend"**

Pipeline segments colored by state (normal/warning/critical), legend added.

### 2026-06-20 13:51 — `15a4290` — Shrujan
**"feat: integrate live API polling with real-time state updates"**

Frontend now polls `/api/segments` and `/api/readings` from the Flask backend every few seconds, updating marker colors and popup data in real time.

### 2026-06-20 13:53 — `6c3fcfe` — Shrujan
**"feat: add node detail panel with camera placeholder and force anomaly button"**

Side panel added: clicking a node shows a detail panel with a camera preview placeholder and a "Force Anomaly" button for demo purposes.

### 2026-06-20 14:03 — `5d55778` — Shrujan
**"feat: change normal pipeline color from green to blue"**

Visual polish: normal state changed from green → blue (green reserved for "healthy" indicators).

### 2026-06-20 14:06 — `ec24de5` — Shrujan
**"feat: add continuous pipeline backbone under colored segments"**

Continuous grey backbone line drawn under the colored segments so the pipeline topology is always visible even when no anomaly coloring is applied.

### 2026-06-20 16:44 — Aditi
**"feat: integrated live camera footage from the pi in the frontend"**

Attempt to embed Pi's webcam feed (MJPEG stream from `/video_feed`) into the frontend node detail panel.

### 2026-06-20 16:52 — `d6107b5` — Shrujan
**"index html"**

Larger `index.html` rewrite/restructure (333 lines changed).

---

## 6. Phase 3 — QNX Port & Sensor Integration (2026-06-20 to 06-21)

This phase began when the Pi was confirmed to be running **QNX SDP 8.0.4** rather than Linux, requiring all hardware interfaces to be re-approached.

### 2026-06-20 17:17 — `ae3e9a5` — Shrujan
**"claude settings json"**

Claude project context file committed (`.claude/` directory).

### 2026-06-20 17:19 — `7d8fa83` — Shrujan
**"chore: add requirements.txt"**

Python dependencies pinned.

### 2026-06-20 17:36 — `1d0e5c2` — Aditi
**Merge from browserbase branch**

Browserbase login/email flow work merged. Browserbase was being used for automated login flow testing.

### 2026-06-20 18:38–18:50 — `e900383`, `e0beb03` — Shrujan
**"use_arduino = true"** / **"use adruino"**

Toggled `use_arduino` flag in `sensor_reader.py`. At this point the plan was still to use Arduino for sensor data; these commits were experimenting with switching between Arduino serial and simulated mode.

### 2026-06-20 23:33 — `bde11e5` — Shrujan
**"qnx"**

First QNX-specific changes to `app.py` and `sensor_reader.py`. Arduino serial dependency removed from sensor_reader (stripped to simulated mode for QNX), `requirements.txt` updated.

### 2026-06-20 23:43 — `081c43e` — Shrujan
**"rewired camera to go to qnx camera"**

`app.py` updated to point camera feed at QNX-accessible endpoint rather than direct OpenCV USB capture. First recognition that QNX does not support standard OpenCV `VideoCapture`.

### 2026-06-20 23:47 — `bab9b0d` — Shrujan
**"added test file for camera"**

`camera_test.py` added — a standalone test script for camera connectivity on QNX.

### 2026-06-21 00:28 — `b1a5fe7` — Shrujan
**"console logging"**

Debug logging added.

### 2026-06-21 01:17 — `86aeb00` — Shrujan
**"change 0 to 1"**

Minor config toggle.

---

## 7. Phase 4 — DHT11 Sensor on QNX (qnx2 branch, 2026-06-21)

Since QNX does not support the standard Linux GPIO sysfs or libgpiod interfaces, reading the DHT11 required writing a **custom C program** using QNX's `gpio_chip_open()` / `gpio_event_*` API on the `qnx2` branch.

### 2026-06-21 05:56 — `0d0bf31` — Shrujan
**"Add DHT11 sensor reader and wire into sensor_reader.py/dashboard"**

First working DHT11 implementation. Three files added/modified:

**`dht11_reader.c`** (259 lines, new):
- Pure C QNX program using `gpio_chip_open()`, `gpio_set_dir()`, `gpio_write()`, `gpio_event_open()`
- Sends 18ms LOW start pulse, then switches to input and captures all edges (RISING + FALLING) via `gpio_event_get()`
- Decodes 40-bit DHT11 protocol: 16 bits humidity + 16 bits temperature + 8 bit checksum
- Outputs JSON to stdout: `{"temperature": N, "humidity": N}`
- `sensor_reader.py` spawns this binary as a subprocess and parses its stdout

**`sensor_reader.py`** (+72 lines):
- New `read_dht11()` function launches `./dht11_reader` subprocess
- Thread polls every 30s and updates segment 0's temperature with real readings
- Falls back to simulated data if binary not found or read fails

**`static/index.html`** (+19 lines):
- Dashboard updated to show humidity alongside temperature
- Real sensor data indicator added

### DHT11 debugging marathon (2026-06-21 06:15–09:25) — 9 commits in ~3 hours

The DHT11 protocol is timing-sensitive (microsecond-level pulse widths). QNX's GPIO event API introduced unexpected behavior. Each commit was a targeted hypothesis test:

| Time | Commit | Hypothesis tested |
|---|---|---|
| 06:15 | `cc8c416` | Added verbose edge-timing diagnostics — print every pulse width in µs |
| 06:24 | `8c270bb` | Fix: register RISING+FALLING in a single `gpio_event_open()` call (previously two separate calls, missing edges) |
| 06:27 | `b1ab6ae` | Fix: register GPIO event channel BEFORE driving the start signal — avoids missing the first response edge |
| 06:31 | `cbe92f1` | Hypothesis: pulse capacity exhausted after N edges → re-arm event detection after every edge |
| 06:35 | `575c4d0` | Hypothesis: 3ms stall at bit 24 → truncate capture to 24 bits, skip last byte |
| 06:39 | `cbfaa8f` | Try multiple bit-alignment offsets (0–16); use `humidity_decimal == 0x00` as correctness signal |
| 06:43 | `6870746` | Widen offset search to 16, add plausibility-based selection |
| 07:11 | `12a64d3` | Replace fixed-offset decode with sliding 8-bit window scan; use known real readings (12°C/92%) as ground truth |
| 07:16 | `9a40039` | Match exact bit patterns for known reference values instead of approximate numeric closeness |
| 09:25 | `4cd230b` | Decode temperature only from fixed bits 16-23/24-31; drop humidity entirely |

**Status at end of DHT11 work:** Partial — raw edges are captured, decoding remains unreliable due to QNX GPIO timing jitter. Temperature reads sometimes produce plausible values; humidity decode is abandoned. The sensor reader falls back to simulated data when decode fails.

**Root cause (diagnosed but not fully fixed):** QNX's `gpio_event_get()` introduces non-deterministic latency when de-queuing edges. The DHT11's data pulses are ~26µs (bit=0) vs ~70µs (bit=1) — too close together for reliable discrimination under kernel scheduling jitter without a real-time GPIO capture mechanism or hardware timer capture.

---

## 8. Phase 5 — AI Triage / YOLO Crack Detection (2026-06-21)

### 2026-06-21 04:02–04:07 — `04d4e0c`, `84b8306` — Shrujan
**"yolo model"** / **"removed dataset"**

YOLO model files downloaded/committed then dataset removed (too large for repo). Model binary `yolo26n.pt` and `ziQWRZxR` (Roboflow dataset archive) staged.

### 2026-06-21 07:30 — `1bda857` — Shrujan
**"onnx model"**

YOLO model converted to ONNX format for deployment (`.pt` → `.onnx`). ONNX is lighter and doesn't require PyTorch at inference time, important for Pi resource constraints.

### 2026-06-21 08:27 — `79b06d8` — Shrujan
**"opencv"**

OpenCV integration added to `ai_triage.py`. The module now supports both:
- **YOLO/ONNX path:** runs `cv2.dnn.readNetFromONNX()` on each frame, extracts bounding boxes for `crack`, `hole`, `rupture` classes
- **Fallback path:** Canny edge detection + contour geometry (aspect ratio ≥ 3.5, min area 200px) to find elongated crack-like features

`app.py` updated to call `detect_defects()` every 10 seconds (configurable `_TRIAGE_INTERVAL`) on frames from `_TRIAGE_SEGMENT = 0`. Detections are cached and served at `/api/detections`.

**Key design:** `ai_triage.py` exports `is_enabled()` / `disabled_reason()` so the frontend can show why inference is or isn't running.

### 2026-06-21 09:58 — `977a192` — Shrujan
**"revived login"**

Login system re-enabled in `app.py` — session-based auth with `ADMIN_EMAIL`/`ADMIN_PASSWORD` from `.env`. All routes except `/login` and `/static/` redirect to login if unauthenticated.

### 2026-06-21 10:09 — `2963af9` — Shrujan
**"fixed moving nodes"**

Frontend bug fix: map nodes were moving/jittering on data update. Fixed by separating marker creation (once) from data update (poll).

### 2026-06-21 later — `d0696fe` — Aditi
**"browserbase login + email flow"**

Browserbase automated email/login flow for the demo.

### 2026-06-23 21:12–22:28 — Aditi (multiple commits)

Frontend refinements on the frontend/map:
- `229433f` — Altered 2D pipe map to display anomalies by depth rather than left-to-right
- `3964d39` — First attempt at 2D render of node section in frontend
- `8796f7a` — Switch from MapLibre to Leaflet (MapLibre had rendering issues on Pi 3, Leaflet is lighter)
- `d75f3ef` — Reduced maxArea contour threshold (closer cracks in frame weren't registering because contour areas were smaller than expected)
- `9476285` — Replaced OpenCV crack detection implementation with YOLO model as primary
- `9856cfb` — Convert YOLO model to `.onnx` to reduce size

---

## 9. Phase 6 — Camera Driver (camera_driver branch, 2026-06-22 to 06-24)

This is the most technically complex work in the project. Because the Pi runs QNX (not Linux), the standard `libcamera` / V4L2 / `cv2.VideoCapture` stack is unavailable. The solution is to write a **custom QNX resource manager** — a kernel-level driver — that speaks directly to the RP1 chip's MIPI CSI-2 DMA engine and exposes `/dev/video0`.

### Hardware context
- **SoC:** BCM2712 (ARM Cortex-A76 × 4)
- **I/O chip:** RP1 (Raspberry Pi's own ASIC, connected via PCIe 2.0 ×4)
- **RP1 BAR0 physical base:** `0x1f00000000` (fixed on all RPi5 boards)
- **Sensor:** Sony IMX708 (Camera Module 3), RAW10 Bayer, 2x2bin mode: 2304×1296 @ ~30fps, 450 Mbps/lane over 2-lane MIPI CSI-2
- **Camera connector:** CAM/DISP 0 (22-pin FPC) → maps to CSI1 hardware block in RP1 (naming inverted: CAM/DISP 0 → CSI1, CAM/DISP 1 → CSI0)
- **I2C bus:** `/dev/i2c6` (confirmed by probing; sensor ACKs at address 0x1A)

### 2026-06-22 16:36 — `39210d0` — Shrujan
**"camera driver"** — 1535 lines, 7 new files

First complete camera driver skeleton committed. All 7 files created in `camera_driver/`:

| File | Purpose |
|---|---|
| `Makefile` | Cross-compile for QNX aarch64le |
| `dphy.h` | MIPI D-PHY register definitions |
| `dphy.c` | D-PHY bring-up sequence |
| `csi2.h` | CSI-2 DMA register definitions |
| `csi2.c` | CSI-2 DMA channel management |
| `imx708_regs.h` | IMX708 I2C register tables (common init, 2x2bin mode, test pattern) |
| `camera_resmgr.c` | QNX resource manager main — maps registers, inits sensor, starts DMA, exposes `/dev/video0` |

**QNX-specific techniques used:**
- `mmap_device_memory()` for register-mapped I/O (no `/dev/mem` on QNX)
- `posix_typed_mem_open("/memory/below4G")` for physically contiguous DMA buffer (RP1 DMA requires 32-bit physical addresses)
- `mem_offset64()` to get physical address of the allocated buffer
- `DCMD_I2C_SEND` / `DCMD_I2C_SENDRECV` for I2C sensor register access
- `resmgr_attach()` + `dispatch_block()` for the `/dev/video0` device node
- `_RESMGR_NPARTS(1)` for replying with iov data in `io_read()`

### 2026-06-22 16:49 — `3c7c152` — Shrujan
**"camera_driver: fix build errors"**

First compile attempt on QNX SDP revealed several issues, all fixed:
- Broken comment causing parse error
- `off64_t` not declared (needed `#include <sys/types.h>`)
- `_RESMGR_NPARTS` macro signature wrong for QNX SDP 8
- `iofunc_ocb_attach()` argument count mismatch

### 2026-06-22 16:44 — `42cfb66` — Shrujan
**"pci file"**

Investigated using QNX PCI library to locate the RP1 BAR dynamically. Rejected in favour of hardcoded `0x1f00000000` since this address is fixed on all RPi5 boards and PCI server setup was complex.

### 2026-06-22 18:07 — `19791a0` — Shrujan
**"rp1 offsets"**

First runtime test. Initially used CSI0 offsets (`0x110000` DMA, `0x114000` DPHY, `0x120000` MIPI_CFG). These were the wrong block — camera is wired to CAM/DISP 0 which maps to CSI1, not CSI0.

### 2026-06-22 18:55 — `edb07b0` — Shrujan
**"init via i2c"**

IMX708 I2C initialization confirmed working. Sensor probe at `/dev/i2c6` returns ACK. Chip ID register `0x0016` reads `0x0708` (correct for IMX708). Sensor streaming registers written successfully. **I2C layer is fully functional.**

Key discovery: `DCMD_I2C_SENDRECV` places received data at offset 0 of the data buffer, not at offset `send_len` as might be expected. This is a QNX-specific behavior documented in the code.

### 2026-06-22 19:56 — `51a664b` — Shrujan
**"i2s probe"**

Diagnostic probing of I2S/I2C addresses to confirm sensor bus.

### 2026-06-22 19:59 — `b539eb7` — Shrujan
**"changed reading positions"**

Adjusted DPHY register read positions in diagnostics to track stop-state.

### 2026-06-22 20:27 — `e2e0fd9` — Shrujan
**"rewrite camera driver with right offsets"**

Switched from CSI0 to CSI1 offsets after confirming camera is on CAM/DISP 0:
- `RP1_CSI0_DMA_OFFSET`: `0x110000` → `0x128000`
- `RP1_CSI0_DPHY_OFFSET`: `0x114000` → `0x12C000`
- `RP1_CSI0_MIPICFG_OFFSET`: `0x120000` → `0x138000`
- Also fixed `RP1_CSI0_DMA_SIZE`: `0x100` → `0x200` (CH_FRAME_SIZE(3) at offset 0x100 was past the end of the mapping)

Also added critical MIPI_CFG initialization: must write `SEL_CSI = 1` to the MIPI CFG register BEFORE any CSI2 or DPHY access. Without this, both blocks are in DSI mode and return `0xFFFFFFFF` on all reads.

### 2026-06-22 20:42 — `7ee015b` — Shrujan
**"remove unused variable"**

Cleanup.

### 2026-06-22 21:14 — `00e683d` — Shrujan
**"fixed more bugs"**

Multiple runtime fixes based on first test output with CSI1 offsets:
- Removed `CSI2_STATUS_PHY_ERRORS` check (macro doesn't exist in the rewritten `csi2.h`)
- Fixed `csi2_open_rx()` calling `dphy_start()` a second time internally (double-reset bug)
- Added diagnostic dumps: DPHY STOPSTATE, DPHY RX, all four DISCARD counters

### 2026-06-22 21:36 — `e0d6d5d` — Shrujan
**"more testing"**

Added before/after CTRL0 readback logging to diagnose why DPHY wasn't asserting stop-state. Discovery: CTRL0 at offset `0x00` always reads `0x3132302a` regardless of what is written. This is the first clue that the register map is wrong.

### 2026-06-22 21:46 — `565be9f` — Shrujan
**"changed cam port"**

Experimented with reverting to CSI0 port to compare behavior. Confirmed both blocks show the same CTRL0 mystery value.

### 2026-06-22 21:58 — `8a07444` — Shrujan
**"rewrite camera_driver"** — 293 lines changed across dphy.h, dphy.c, camera_resmgr.c

Major intermediate rewrite based on first round of debugging:
- Added `DPHY_BASEDIR_PERIPHERAL` (bit 2 of CTRL1) — hypothesis that DPHY needed to be explicitly set to RX mode
- Moved TESTCLR pulse before CTRL0 lane-enable write
- Added detailed diagnostic logging throughout
- Fixed lane-enable bitmap (was writing N_LANES count instead of bit mask)
- Switched init ordering: MIPI_CFG → dphy_start → sensor I2C → dphy_wait_stop → csi2_open_rx

**Runtime output from this version:**
```
[dphy] CTRL0 before write: 0x3132302a
[dphy] CTRL0 after  write: 0x3132302a (wrote 0x07)
[dphy] CTRL1 = 0x00000007
PHY_STOPSTATE = 0x00000000
```
CTRL1 writes correctly (0x07), CTRL0 writes are silently ignored. STOPSTATE still 0.

### 2026-06-24 07:54 — `9196518` — Shrujan
**"Add rp1_clk_dump diagnostic tool"** — 193 lines, new file

`camera_driver/rp1_clk_dump.c`: standalone QNX diagnostic tool that maps the RP1 clock controller region (`BAR0 + 0x18000`, size 0x11000) and dumps every 32-bit register as `offset: value`. Designed to inspect which clocks QNX has (or has not) configured, specifically to investigate whether the 24 MHz MCLK to the IMX708 is being driven.

Usage:
```
./rp1_clk_dump                    # default: CLOCKS block at 0x18000
./rp1_clk_dump 0x18000 0x11000    # explicit base + size
./rp1_clk_dump -a 0x18000 0x2000  # print ALL regs (including zero)
./rp1_clk_dump 0x0d0000 0x4000    # GPIO/pinmux bank (when identified)
```

---

## 10. Eliminated Approaches & Why

### Camera driver register map errors (all discovered through runtime testing)

| What we believed | What it actually is | Discovered |
|---|---|---|
| DPHY offset 0x00 = CTRL0 (lane enables) | VERSION register (read-only, always `0x3132302a` = DW DPHY Host **v1.20**) | 2026-06-22, after seeing writes not stick |
| DPHY offset 0x04 = CTRL1 (SHUTDOWNZ/RSTZ/BASEDIR) | N_LANES (we were writing 0x07 = "7 data lanes") | 2026-06-23, after fetching Linux source |
| DPHY offset 0x08 = PHY_RX | RESETN register | 2026-06-23 |
| DPHY offset 0x0C = PHY_STOPSTATE | Unnamed register (always 0) | 2026-06-23 |
| Test interface at 0x010 / 0x014 | Actual TST_CTRL0/1 at 0x050 / 0x054 | 2026-06-23 |
| BASEDIR_PERIPHERAL bit needed | Not present in RP1's DW DPHY. Linux never sets it. | 2026-06-23, from Linux source |
| HSFREQRANGE data byte 0x0C for 450 Mbps | Code 0x06 = ≤449 Mbps. Correct for 450 Mbps: 0x2C (code 0b010110) | 2026-06-23 |
| Whole-register writes to test interface | Linux uses read-modify-write to toggle individual bits | 2026-06-23 |
| PHY_STOPSTATE watchdog catches streaming | dphy_stop() is a no-op in Linux — resetting mid-stream corrupts IDI bus | 2026-06-23 |
| Camera on CAM/DISP 0 → CSI0 | CAM/DISP 0 → **CSI1** (RPi5 naming is inverted) | 2026-06-22 runtime |
| DMA size 0x100 for CSI2 | 0x200 needed: CH_FRAME_SIZE(3) is at offset 0x100 | 2026-06-22 |
| dphy_start() called inside csi2_open_rx() | Double-reset bug. DPHY was reset again after already running | 2026-06-22 |
| Wait for STOPSTATE before sensor init | Sensor drives LP-11; sensor must be streaming first | 2026-06-22 |

### Frontend/mapping

| Approach | Why dropped |
|---|---|
| MapLibre GL JS | Rendering failures on Raspberry Pi 3 browser (WebGL limited). Switched to Leaflet.js |
| Arduino as sensor node | Eliminated complexity. DHT11 wired directly to Pi GPIO instead |
| FastAPI backend | Used Flask instead (faster to iterate, lighter) |
| React/Vite frontend | Used plain HTML+JS+Leaflet (no build step needed on Pi) |
| USB webcam as primary camera | Replaced by IMX708 native driver (in progress) |
| OpenCV frame-diff anomaly detection | Replaced by YOLO crack detection model |
| YOLO `.pt` model | Converted to ONNX (lighter, no PyTorch dependency at inference) |

---

## 11. Current State at Handoff

### What works
| Component | Status |
|---|---|
| Flask backend (`app.py`) | ✅ Running on Pi, serves `/api/segments`, `/api/readings`, `/api/detections` |
| Simulated multi-node data | ✅ 4-8 virtual nodes generating synthetic temperature drift |
| Leaflet frontend | ✅ Map with pipe segments, node markers, real-time polling |
| Login/auth | ✅ Session-based, credentials in `.env` |
| IMX708 I2C init | ✅ Chip ID `0x0708` confirmed, all register tables write successfully |
| DPHY register map | ✅ Correct offsets from Linux `rpi-6.12.y` source |
| DPHY bring-up sequence | ✅ Matches Linux `rp1_cfe/dphy.c` exactly |
| MIPI_CFG SEL_CSI | ✅ Writes and reads back correctly (`0x00000001`) |
| DW CSI-2 Host version | ✅ Confirmed v1.20 (`0x3132302a`) |
| PHY_RX bit 16 (RXULPSCLKNOT) | ✅ = 1 after DPHY power-up (clock lane not in ULPS — correct) |
| rp1_clk_dump tool | ✅ Built and committed, ready to run on QNX |

### What is not yet working
| Component | Status |
|---|---|
| PHY_STOPSTATE | ❌ = 0x00000000 (no LP-11 on data lanes) |
| MIPI data arriving at CSI2 DMA | ❌ All DISCARD counters = 0, frame_count = 0 |
| `/dev/video0` frame capture | ❌ No frames — DMA never triggers |
| DHT11 decode | ⚠️ Partial — raw edges captured, decoding unreliable due to QNX GPIO jitter |
| NexiGo USB webcam on QNX | ⚠️ In progress (different task) |

### Root cause hypothesis (high confidence)
The IMX708 sensor requires a **24 MHz master clock (MCLK/XCLK)** on pin 11 of the FPC ribbon cable, provided by the Raspberry Pi. Without MCLK the sensor's internal PLL cannot lock, and MIPI output (both LP-11 and HS bursts) never activates. I2C continues to work because it uses an internal RC oscillator.

In Linux, the RP1 clock generator (at `BAR0 + 0x18000`) is programmed by `clk-rp1.c` and `pinctrl-rp1.c` (via device tree `cam0_clk`) to output 24 MHz on a specific GPIO routed to the FPC MCLK pin. **QNX has no equivalent driver.** Nothing programs this clock.

Evidence:
- I2C works perfectly (internal oscillator independent of MCLK)
- PHY_STOPSTATE = 0 consistently across all runs and both CSI ports
- All DISCARD counters = 0 (zero MIPI packets arriving, not just wrong packets)
- PHY_RX = `0x00010000` is constant before and after sensor streaming (status doesn't change = sensor not transmitting)
- DPHY configuration now matches Linux exactly — software path is correct

---

## 12. Open Work & Next Steps

### Immediate (camera_driver branch)

**Step 1 — Confirm MCLK hypothesis (no code)**  
Boot Linux on the Pi, run:
```bash
libcamera-still --camera 0 -o /tmp/test.jpg
```
If successful:
```bash
sudo cat /sys/kernel/debug/clk/rp1/summary | grep -i cam
sudo cat /sys/kernel/debug/gpio | grep -i cam
```
These commands identify which RP1 clock output drives CAM0 MCLK and at what rate.

**Step 2 — Program RP1 clock generator for 24 MHz MCLK**  
The RP1 clock controller at `BAR0 + 0x18000` (size ~0x11000). Must:
- Map the region with `rp1_map(0x18000, 0x11000)`
- Identify the clock output register for CAM0 MCLK from step 1 or `rp1_clk_dump` output
- Set PLL/divider for 24 MHz and enable the output
- Also configure RP1 GPIO function-select to route the clock to the FPC MCLK pin
- This code must run **before** `imx708_init()` in `camera_resmgr.c`

**Step 3 — End-to-end frame capture**  
After MCLK fix, expected:
```
[dphy] LP-11 stop-state confirmed (PHY_STOPSTATE=0x00000003)
[step3] frame 1 received (frame_count=1)
```
Then:
```bash
dd if=/dev/video0 of=/tmp/frame.raw bs=3732480 count=1
```
On PC:
```bash
ffmpeg -f rawvideo -pixel_format bayer_rggb10 -video_size 2304x1296 -i frame.raw frame.png
```

### Remaining work (broader project)

- Wire live IMX708 frame into `app.py`'s MJPEG stream (replacing USB webcam)
- Run YOLO inference on IMX708 frames instead of USB webcam
- Fix DHT11 decode on QNX (or replace with a hardware-timer-based capture approach)
- NexiGo USB webcam fallback path (Task #1, in progress on qnx branch)
- 3D pipe visualization (3DPipe branch, started by Aditi)
- Email alerting (browserbase branch, started by Aditi)

---

## 13. Key Files Reference

```
berkai/
├── app.py                     Flask backend, auth, MJPEG stream, AI triage loop
├── sensor_reader.py           Simulated + real sensor data, DHT11 subprocess call
├── ai_triage.py               YOLO/ONNX + OpenCV crack detection
├── config.py                  SEGMENTS dict (segment IDs, temp thresholds, names)
├── sensor_reader.py           Real + simulated sensor thread
├── static/
│   ├── index.html             Leaflet map frontend (main dashboard)
│   └── login.html             Login page
├── camera_driver/
│   ├── Makefile               QNX cross-compile (aarch64le-qnx800-gcc)
│   ├── dphy.h                 DW CSI-2 Host + DPHY register map (CORRECT offsets)
│   ├── dphy.c                 DPHY bring-up, matches Linux rpi-6.12.y exactly
│   ├── csi2.h                 CSI-2 DMA registers, channel stride 0x40
│   ├── csi2.c                 DMA channel arm/disarm, frame counter
│   ├── imx708_regs.h          IMX708 I2C register tables, /dev/i2c6, addr 0x1A
│   ├── camera_resmgr.c        QNX resource manager main, /dev/video0
│   └── rp1_clk_dump.c         Diagnostic: dump RP1 clock/GPIO registers
├── dht11_reader.c             QNX C binary: DHT11 GPIO edge capture + JSON output
├── yolo26n.pt                 YOLO model weights (.pt)
├── runs/                      YOLO training run artifacts
└── pipe-monitoring-handoff.md Original project design document
```

---

## 14. Hardware Reference

### RP1 CSI1 register map (camera on CAM/DISP 0)
```
RP1 BAR0 physical base: 0x1f00000000

CAM/DISP 0 → CSI1 (IMPORTANT: RPi5 naming is inverted)

  DMA base:      BAR0 + 0x00128000  (size 0x200)
  DPHY base:     BAR0 + 0x0012C000  (size 0x200, this is DW CSI-2 Host + DPHY)
  MIPI CFG base: BAR0 + 0x00138000  (size 0x100, must write SEL_CSI=1 first)
  Clock ctrl:    BAR0 + 0x00018000  (size 0x11000, cam MCLK setup needed here)

For reference — CSI0 (camera on CAM/DISP 1):
  DMA base:      BAR0 + 0x00110000
  DPHY base:     BAR0 + 0x00114000
  MIPI CFG base: BAR0 + 0x00120000
```

### DW CSI-2 Host v1.20 register offsets (within DPHY base)
```
VERSION        0x000  read-only, = 0x3132302a (v1.20)
N_LANES        0x004  write (nlanes-1): 0=1 lane, 1=2 lanes
RESETN         0x008  host soft reset: 0=reset, 0xffffffff=run
PHY_SHUTDOWNZ  0x040  PHY shutdown: 0=off, 1=active
PHY_RSTZ       0x044  PHY reset: 0=reset, 1=active
PHY_RX         0x048  RX status (bit 16 = RXULPSCLKNOT)
PHY_STOPSTATE  0x04C  LP-11 stop state bits[1:0] per data lane
TST_CTRL0      0x050  test interface (TESTCLK=bit1, TESTCLR=bit0) — read-modify-write
TST_CTRL1      0x054  test interface (TESTEN=bit16, TESTDOUT=[15:8], TESTDIN=[7:0])
```

### IMX708 sensor constants
```
I2C bus:       /dev/i2c6
I2C address:   0x1A
Chip ID reg:   0x0016
Chip ID value: 0x0708
Mode:          2x2 binned, 2304×1296, RAW10
Line stride:   2880 bytes (= 2304 × 10/8)
Frame size:    3,732,480 bytes (= 2880 × 1296)
MIPI:          2 data lanes, 450 Mbps/lane
HSFREQRANGE:   testcode=0x44, data=0x2C (for 450 Mbps)
```

### DPHY test interface transaction sequence (from Linux rpi-6.12.y)
```
Address phase (TESTEN=1 + TESTCLK falling edge):
  1. set TESTCLK=1 (read-modify-write bit 1 of TST_CTRL0)
  2. set TESTEN=0  (read-modify-write bit 16 of TST_CTRL1)
  3. set TESTDIN=testcode (bits[7:0] of TST_CTRL1)
  4. set TESTEN=1
  5. set TESTCLK=0  ← address latched on falling edge

Data phase (TESTEN=0 + TESTCLK rising edge):
  6. set TESTEN=0
  7. set TESTDIN=data
  8. set TESTCLK=1  ← data written on rising edge
```

---

## 15. Phase 7 — Camera Driver Resolution (2026-06-25 to 06-26)

This session took the camera driver from "STOPSTATE stuck at 0, zero frames" all the way to **live RAW10 capture on `/dev/video0`**. The big surprise: the handoff root-cause hypothesis (Section 11 — missing 24 MHz MCLK) was **wrong**. The Pi 5 never supplies a clock to the camera over the CSI connector; the IMX708 module has its **own onboard 24 MHz oscillator** (powered by `cam0_reg`). The real blockers were a *stack* of separate QNX-vs-Linux infrastructure gaps — things Linux's device tree / clk / regulator / dma-ranges frameworks do implicitly that QNX does not — each found and fixed in turn.

> **Commit note:** OneDrive holds a `.git/index.lock` on the repo, so several commits in this phase were made manually by Shrujan (the hashes below that are confirmed come from `git log`; the final DMA / gain / unpack commits were committed by hand and may carry different messages).

### Diagnostic groundwork — `rp1_clk_dump` runs

Ran the diagnostic tool from Phase 6 as **root** (the physical-memory mapping needs `PROCMGR_AID_MEM_PHYS`; default `qnxuser` cannot map BAR0 — root login/password is `root`/`root` on the stock QNX Pi image). Two dumps were captured:
- **CLOCKS block (`0x18000`):** housekeeping clocks (DMA, UART, ETH, PCIE_AUX, PWM) enabled, but **all GPCLK outputs disabled** and **`CLK_MIPI0_CFG`/`CLK_MIPI1_CFG` disabled** (`0x180c4`/`0x180d4` = 0).
- **GPIO bank (`0xd0000`, size `0x30000`):** decoded against `clk-rp1.c` and the Pi5 device tree. `gpio34` (cam0_reg) already driven high by firmware → analog power on; `gpio38/39` muxed to i2c6 (funcsel 3); `gpio40/41` idle.

### 2026-06-25 — `70a41ca` — Shrujan
**"change from csi1 to csi0"**

Device-tree cross-check (`rp1.dtsi` + `bcm2712-rpi-5-b.dts`, rpi-6.12.y) disproved the "inverted naming → CSI1" belief. The Pi5 DT bundles each connector's I2C **and** CSI block as a matched pair: a camera answering on `/dev/i2c6` is on the **CD0** connector, which routes to **CSI0** (`0x110000`), with regulator `gpio34`. Switched the offsets back to CSI0 (`DMA 0x110000`, `DPHY 0x114000`, `MIPICFG 0x120000`). Registers read back correctly on CSI0 — but `STOPSTATE` was still 0, eliminating "wrong block" as the *sole* cause and pointing upstream.

### 2026-06-25 — `574a2a1` — Shrujan
**"Final Bug Hopefully"** — the breakthrough

The clock dump showed `CLK_MIPI0_CFG_CTRL` (`0x180c4`) = 0. The DW D-PHY's **functional logic — including the LP-11 stop-state detection that drives `PHY_STOPSTATE`** — runs on this 25 MHz config clock; APB register reads work on the always-on system clock, which masked the problem. Added code to enable `RP1_CLK_MIPI0_CFG` before DPHY bring-up (parent xosc 50 MHz ÷ 2 = 25 MHz; write `DIV_INT=2`, then `CTRL` bit 11 `ENABLE`, AUXSRC=0), per `clk-rp1.c`.

**Result — packets finally arriving:**
```
[clk] MIPI0_CFG: CTRL=0x10000800 DIV_INT=0x00000002
[dphy] LP-11 stop-state confirmed (PHY_STOPSTATE=0x00000003)
PHY_RX = 0x00030000          (HS clock lane now active)
DISCARDS_UNMATCHED = 0x12000018   (embedded-data packets received)
```

### 2026-06-25 — `3261723` — Shrujan
**"Double hsfreqrange(450->900)"**

Confirmed from `imx708.c` + `cfe.c` that IMX708 `link_freq` = 450 MHz and the D-PHY data rate = **2 × link_freq = 900 Mbps/lane** (DDR). The handoff value (450 Mbps → data `0x2C`) was half the real rate. Corrected `dphy_set_hsfreqrange()` to 900 Mbps → table code `0b001010` → data byte **`0x14`**. (LP-11 detection is unaffected by HSFREQRANGE, but correct HS reception of the long RAW10 packets requires it.)

### 2026-06-25 — `c15dc77` — Shrujan
**"IMX708 frame count"**

Added sensor-side readback (`FRM_CNT` 0x0005, `MODE` 0x0100) to settle whether the sensor was actually streaming. Result: `MODE=0x01` (stream-on stuck) but **`FRM_CNT` frozen** and DISCARD counters static — the sensor accepted config and emitted only a brief burst, then stalled. The register tables were verified **byte-identical to Linux**, ruling out a config/transcription error.

### 2026-06-25 — `7728e0b` — Shrujan
**"Add camera power-on reset via gpio34 before sensor init"**

Found the gap: Linux's `imx708_power_on` power-cycles the regulator and resets the sensor on every bring-up; QNX inherited a firmware-enabled regulator and **never reset the sensor**, leaving it in a bad boot state. Added a power-cycle before I2C init — drive `gpio34` (cam0_reg; RIO bank2 `0xe8000`, bit 0) low → 30 ms → high → 70 ms (delays from the camera overlay).

**Result — the sensor streams and the first frame is captured:**
```
[imx708:post-stream-on] FRM_CNT=0x05
[imx708:post-wait]      FRM_CNT=0xa7      (counter now advancing!)
[step3] frame 1 received (frame_count=1)
```

### 2026-06-25 — `a547147` — Shrujan
**"continuous capture"**

Two fixes for usable capture: (1) set the **`AUTO_ARM`** bit in `CH_CTRL` so the channel re-arms after each frame (without it, exactly one frame is captured and the rest land in `DISCARDS_INACTIVE`); (2) `io_read()` now polls the **CH_DEBUG** frame counter instead of the interrupt-only global `frame_count` (interrupts aren't wired up, so that counter never moved and `dd` would block forever). Result: frames 1, 2, 3… counted continuously, and `dd if=/dev/video0` returns a full frame without hanging.

### 2026-06-25 — DMA PCIe inbound-window fix — Shrujan *(committed manually)*

Captured frames were counted but the buffer came back **all zeros** — the pixel bytes weren't landing. Root cause: RP1 reaches system RAM through the BCM2712 PCIe inbound window. Per the Pi5 `dma-ranges`, the address RP1 must emit is **CPU-physical + `0x10_00000000`** (RP1 `0x10_xxxxxxxx` → PCIe `0x10_xxxxxxxx` → RAM `0x0`). Added this base to the DMA buffer address (`ADDR1` now `0x1`). Linux hides this behind `videobuf2`/`dma-ranges`; QNX needs it explicit.

**Result — real pixel data lands in the buffer:**
```
[csi2] ch0 armed: ADDR0=0x00735000 ADDR1=0x00000001
[diag] DMA buffer: 3676640 / 3732480 non-zero bytes (first at offset 0)
```
`od` confirmed genuine MIPI RAW10 Bayer (varied pixel values + the packed-LSB byte every 5th position).

### 2026-06-25 to 06-26 — exposure tuning + viewing tools — Shrujan *(committed manually)*

- Bumped IMX708 **analog gain** (`0x0204/0x0205`) from ~1.1× (code `0x0070`) to **8×** (code `0x0380`) — default captures were ~6 % of full scale.
- Added **`raw10_unpack.py`**: unpacks MIPI RAW10 (4 pixels / 5 bytes) into a 16-bit Bayer file (for ffmpeg `bayer_rggb16le` debayer) plus a percentile-stretched, gamma-corrected grayscale PGM for instant viewing. (ffmpeg has no `bayer_*10` format, so MIPI RAW10 must be unpacked first.)
- Verified end-to-end on the Pi: capture → unpack → ffmpeg → viewable image, served over `python3 -m http.server` for browser viewing. Current frames are real but **underexposed** (low ambient light); gain helped, exposure-time tuning remains.

### Corrected understanding (supersedes Section 11 hypothesis)

| Handoff belief (Section 11) | Reality found this session |
|---|---|
| Missing 24 MHz MCLK from the Pi is the root cause | Pi 5 never sends a camera clock; the IMX708 module self-clocks from its own 24 MHz oscillator. Hypothesis was wrong. |
| Camera on CAM/DISP 0 → CSI1 (inverted naming) | Camera on i2c6 = CD0 connector → **CSI0** (`0x110000`). Confirmed via DT + GPIO funcsel dump. |
| HSFREQRANGE `0x2C` (450 Mbps) | **`0x14`** (900 Mbps/lane = 2 × 450 MHz link_freq). |
| Software path is fully correct, only MCLK missing | Several gaps remained: MIPI_CFG config clock disabled, missing sensor power-on reset, no AUTO_ARM, and the PCIe DMA inbound-window translation. |
| Sensor analog rail may need a power-enable GPIO | `gpio34` (cam0_reg) is already driven high by firmware — but the sensor still needs an explicit **power-cycle reset** before config. |

**The actual chain of blockers, in the order they were cleared:** wrong CSI block → MIPI_CFG config clock disabled (the one that unstuck STOPSTATE) → HSFREQRANGE half the real rate → sensor never power-on-reset → single-shot capture (no AUTO_ARM) → read path on a dead counter → DMA address missing the PCIe window base.

---

## 16. Updated State — Working Camera

*(Supersedes the camera rows of Section 11 "Current State at Handoff".)*

### What works now
| Component | Status |
|---|---|
| `RP1_CLK_MIPI0_CFG` 25 MHz config clock | ✅ Enabled by the driver before DPHY bring-up |
| Correct CSI block (CSI0, CD0 connector) | ✅ `0x110000`/`0x114000`/`0x120000` |
| HSFREQRANGE for 900 Mbps/lane | ✅ data byte `0x14` |
| Sensor power-on reset (gpio34 cycle) | ✅ low → 30 ms → high → 70 ms before init |
| `PHY_STOPSTATE` | ✅ = `0x00000003` (both data lanes LP-11) |
| Sensor streaming | ✅ `FRM_CNT` advances; `MODE`=0x01 |
| MIPI packets arriving | ✅ DISCARD counters climbing; RAW10 (DT 0x2b) matched on ch0 |
| Continuous capture | ✅ `AUTO_ARM` — frames counted continuously |
| DMA delivering real pixels | ✅ ~99.6 % non-zero buffer (PCIe window base `+0x10_00000000`) |
| `/dev/video0` read path | ✅ polls CH_DEBUG; `dd` returns a full frame, no hang |
| Off-target viewing | ✅ `raw10_unpack.py` + ffmpeg debayer / grayscale PGM |

### What remains
| Item | Status |
|---|---|
| Exposure | ⚠️ Frames underexposed in low light; analog gain bumped to 8×, exposure-time (frame-length + integration) tuning still open |
| Bayer order confirmation | ⬜ `bayer_rggb16le` assumed; confirm vs grbg/bggr/gbrg on a lit scene |
| Interrupt-driven capture | ⬜ Still polling (works; not latency-optimized). ISR + double-buffer (ping-pong ADDR0/ADDR1) is the upgrade |
| Integrate IMX708 into `app.py` MJPEG stream | ⬜ Replace USB webcam feed; run YOLO on IMX708 frames |
| DHT11 decode on QNX | ⚠️ Unchanged from Phase 4 (still jitter-limited) |

### Corrected hardware reference (camera path)
```
Camera connector:  CD0 (i2c6, gpio38/39) → CSI0 hardware block
  DMA base:      BAR0 + 0x00110000
  DPHY base:     BAR0 + 0x00114000   (DW CSI-2 Host + DPHY)
  MIPI CFG base: BAR0 + 0x00120000   (write SEL_CSI=1 first)

Config clock:  RP1_CLK_MIPI0_CFG @ CLOCKS(0x18000)+0x0c4 (CTRL)/+0x0c8 (DIV_INT)
               parent xosc 50 MHz ÷ 2 = 25 MHz; enable = CTRL bit 11. MUST be on.
Sensor clock:  onboard 24 MHz oscillator on the camera module (Pi supplies none)
Power/reset:   cam0_reg = RP1 gpio34 = RIO bank2 (0xe8000) bit 0; power-cycle to reset
HSFREQRANGE:   testcode 0x44, data 0x14  (900 Mbps/lane = 2 × 450 MHz link_freq)
DMA address:   CPU-physical + 0x10_00000000 (PCIe inbound window), then >> 4
```

---

*Document generated 2026-06-24 from git history and session context.*
*Updated 2026-06-26 — Sections 15–16 added for the camera-driver resolution session (camera now capturing live RAW10 frames).*
*Authors: Shrujan Sriram, Aditi Varia*

---

## 17. Phase 9 — Stream Merge into app.py (video_stream_merge branch, 2026-07-03)

Replaced the `qnx_apis` / `cv2.VideoCapture` placeholder in `app.py` with the
real IMX708 stream. Integration layer only — capture pipeline untouched.

**New/changed files:**
- `camera_source.py` (new) — `CameraHub`: the ONE capture thread on
  `/dev/video0` (single-buffer driver → single reader), FrameHub-style
  latest-JPEG broadcast to all `/video_feed` clients + latest raw BGR frame
  for inference. Config: `fast8 + auto debayer + bgr + 960x540` (the proven
  ~14 fps colour config). If the device is missing it publishes a
  "NO CAMERA SIGNAL" card, retries every 3 s, and (off-Pi dev only) falls
  back to a local webcam.
- `camera_driver/imx708_stream.py` — added `color="bgr"` (exact channel swap,
  verified in both cv2 and numpy debayer paths), so app.py/ai_triage get
  their native cv2 convention with no per-frame conversion.
- `app.py` — dropped `qnx_apis`/`_VideoCapture`/`_get_camera`/`_capture_loop`;
  `/video_feed` now relays hub JPEGs (event-driven, no FPS timer), inference
  runs decoupled every 2 s on `hub.latest_frame()`. Annotation happens once
  per captured frame in the hub via a callback reading `_latest_detections`.
- `start.sh` (new) — starts `camera_resmgr` (if root) then `python3 app.py`;
  prints instructions if `/dev/video0` is missing and not root.
- `.gitignore` — now covers `__pycache__`, `node_modules`, camera_driver
  build outputs, capture artifacts.

**Verified locally (Windows, fake stream):** debayer bgr==rgb[..., ::-1] both
paths; hub JPEG broadcast + annotate hook + status card; full app end-to-end
via Flask test client (3 multipart frames from `/video_feed`, `/api/detections`
200 with ONNX model loaded). **Not yet run on the Pi** — needs file sync +
`./start.sh`.
