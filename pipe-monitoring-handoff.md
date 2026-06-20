# Distributed Pipe Monitoring Digital Twin — Build Handoff

## Overview

A network of fixed monitoring nodes (camera + temperature sensor) streams data into a software layer that mirrors the pipe network in real time, detects changes, and flags abnormal segments. For the hackathon, we build one physical node and simulate 4–8 more in software so the dashboard shows a realistic distributed deployment.

## Role split

**Software / robotics lead** — node-to-server data flow, digital twin backend, image change detection, dashboards, multi-node simulation, alerting/risk-scoring logic.

**Environmental engineer** — defines which visual patterns matter (buildup, residue, discoloration, bio-growth), sets temperature thresholds and trend logic, shapes severity scoring, frames the real-world relevance for the pitch.

## Hardware stack

We already have a Raspberry Pi, a USB webcam, and an Arduino — no need to buy a separate camera module or microcontroller.

**Decision: Pi as gateway, Arduino as sensor node.**

| Role | Device | Job |
|---|---|---|
| Sensor node | Arduino | Reads the temperature sensor, sends readings over USB serial |
| Gateway | Raspberry Pi | Captures webcam frames, receives Arduino's serial data, posts both to the backend over WiFi |

This mirrors a real sensor-node/gateway architecture and uses everything we already own. It also gives us a fallback: if the Arduino serial link flakes during judging, the Pi can read the temp sensor directly off its own GPIO instead.

**To buy:** one DS18B20 (or DHT22) temperature sensor (~$1–3) and jumper wires. That's the entire hardware budget.

**Wiring:**
- Temp sensor → Arduino digital pin.
- Arduino sketch reads the sensor and prints a JSON or CSV line over Serial (9600 baud).
- Webcam → Pi via USB.

## Software stack

| Layer | Tool | Why |
|---|---|---|
| Node-side capture | `pyserial` (read Arduino) + `opencv-python` (`cv2.VideoCapture`) for webcam frames | Free, minimal setup, runs directly on the Pi |
| Backend | Python + FastAPI | Fast to write, handles ingestion, storage, anomaly logic |
| Storage | SQLite | Zero setup, fine at hackathon scale |
| Anomaly detection | OpenCV (frame differencing / structural similarity) | No trained model needed for the MVP |
| Digital twin layer | Custom Python module on top of the backend | Stores baselines, current state, and active alerts per segment |
| Frontend | React (Vite) + Recharts or Chart.js | Pipe diagram, live snapshots, temperature trends, alerts |
| Simulation layer | Python script generating 4–8 virtual nodes | Synthetic temperature drift + perturbed camera frames, posted to the same ingestion endpoint as the real node |
| Hosting | Local laptop for the demo; free tier on Render/Railway if remote access is needed | No paid infra required |

## Architecture & data flow

1. **Node layer** — Arduino (temp sensor) + Pi (webcam + gateway), plus 4–8 simulated nodes.
2. **Transport layer** — Arduino → Pi over USB serial. Pi → backend over WiFi via HTTP POST (JSON payload: node ID, timestamp, temperature, JPEG frame).
3. **Backend layer** — FastAPI ingestion endpoint, SQLite storage, OpenCV-based image diffing, anomaly/risk scoring.
4. **Digital twin layer** — Pipe/segment model holding each node's baseline, current state, and active alerts; compares incoming readings against baseline.
5. **Frontend layer** — React dashboard: pipe topology, per-node live snapshot, temperature trend charts, baseline-vs-current image comparison, risk level per segment, historical playback, ranked alert list.
6. **Simulation layer** — Generates synthetic node traffic so the backend and frontend can demonstrate full network scale even though only one physical node exists.

The real node and simulated nodes hit the same ingestion endpoint, so the backend and frontend don't need to know which is which.

## Setup steps

1. **Arduino**: wire the DS18B20 to a digital pin, flash a sketch that reads it every few seconds and prints a JSON line over Serial.
2. **Pi**: install `pyserial` and `opencv-python`; write a script that reads the Arduino's serial line, grabs a webcam frame, and POSTs both to the backend.
3. **Backend**: scaffold FastAPI with one ingestion endpoint (`POST /nodes/{id}/reading`) that writes to SQLite and runs the OpenCV diff against the node's stored baseline frame.
4. **Digital twin logic**: on each new reading, update the segment's current state, compute a risk score (image-change evidence + temperature trend), and write an alert if thresholds are crossed.
5. **Simulator**: write a script that spins up 4–8 fake node IDs, generates synthetic temperature drift and slightly perturbed frames, and POSTs to the same endpoint on a timer.
6. **Frontend**: scaffold React + Vite, build the pipe diagram, per-node snapshot view, temperature chart, and alert list, polling the backend for updates.
7. **Demo run**: start backend → start simulator → start Pi node script → open frontend → trigger one real or simulated anomaly (e.g., cover the webcam or apply heat near the sensor) → confirm the dashboard flags the segment and updates risk scores in real time.
