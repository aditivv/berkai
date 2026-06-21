import threading
import random
import time
import os
import subprocess
from datetime import datetime
from config import SEGMENTS

segment_readings = {}
reading_history = {}
lock = threading.RLock()

# ── DHT11 TEMPERATURE/HUMIDITY SENSOR ───────────────────
# Reads come from the compiled dht11_reader C binary (see dht11_reader.c —
# it talks to the QNX GPIO resource manager directly; this is just a thin
# subprocess wrapper around it). On the laptop, where that binary doesn't
# exist, DHT11_ENABLED is False and DHT11_SEGMENT_ID just stays simulated —
# no code changes needed to run this file off the Pi.

DHT11_BINARY_PATH = os.environ.get("DHT11_BINARY_PATH", "./dht11_reader")
DHT11_GPIO_PIN = int(os.environ.get("DHT11_GPIO_PIN", "17"))
DHT11_SEGMENT_ID = int(os.environ.get("DHT11_SEGMENT_ID", "0"))
DHT11_POLL_INTERVAL = int(os.environ.get("DHT11_POLL_INTERVAL", "2"))  # seconds; DHT11 wants >=1s between reads

DHT11_ENABLED = os.path.exists(DHT11_BINARY_PATH)


def read_dht11_once():
    """Run the compiled dht11_reader binary once. Returns (humidity, temp) or None on failure."""
    try:
        result = subprocess.run(
            [DHT11_BINARY_PATH, str(DHT11_GPIO_PIN)],
            capture_output=True, text=True, timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError) as e:
        print(f"[dht11] could not run {DHT11_BINARY_PATH}: {e}")
        return None

    if result.returncode != 0:
        if result.stderr:
            print(f"[dht11] read failed: {result.stderr.strip()}")
        return None

    try:
        humidity_str, temp_str = result.stdout.strip().split(",")
        return int(humidity_str), int(temp_str)
    except ValueError:
        print(f"[dht11] unexpected output: {result.stdout!r}")
        return None


def dht11_polling_loop():
    """Background thread: poll the physical DHT11 sensor, feed DHT11_SEGMENT_ID."""
    while True:
        reading = read_dht11_once()
        if reading is not None:
            humidity, temp = reading
            record_segment_reading(DHT11_SEGMENT_ID, temp, humidity)
        time.sleep(DHT11_POLL_INTERVAL)

# ── VISUAL DETECTION FLAGS ────────────────────────────────
# Set by the AI triage thread in app.py every 10 s.
# Maps segment_id -> list of detected defect label strings.
# Empty list means no active visual defect.

_DEFECT_LABELS = {'crack', 'hole', 'rupture'}
_visual_flags = {}   # segment_id -> [label, ...]


def set_visual_flag(segment_id, detected_labels):
    """
    Update the visual anomaly flag for a segment.
    detected_labels: iterable of label strings from the YOLO model.
    Only labels in _DEFECT_LABELS are recorded; others are ignored.
    """
    defects = [l for l in detected_labels if l in _DEFECT_LABELS]
    with lock:
        _visual_flags[segment_id] = defects

# ── SIMULATED MODE ──────────────────────────────────────

def simulate_sensors():
    with lock:
        for seg_id, seg in SEGMENTS.items():
            if DHT11_ENABLED and seg_id == DHT11_SEGMENT_ID:
                continue  # real DHT11 readings own this segment; see dht11_polling_loop
            mid = (seg["temp_normal"][0] + seg["temp_normal"][1]) / 2
            segment_readings[seg_id] = {"temp": mid, "humidity": None, "last_seen": datetime.now().isoformat()}

    while True:
        with lock:
            for seg_id in SEGMENTS:
                if DHT11_ENABLED and seg_id == DHT11_SEGMENT_ID:
                    continue
                current = segment_readings[seg_id]["temp"]
                drift = random.uniform(-0.5, 0.5)
                new_temp = current + drift
                record_segment_reading(seg_id, new_temp)
        time.sleep(3)

# ── SHARED LOGIC ─────────────────────────────────────────

def record_segment_reading(segment_id, temp, humidity=None):
    with lock:
        now = datetime.now().isoformat()
        existing = segment_readings.get(segment_id, {})
        segment_readings[segment_id] = {
            "temp": temp,
            "humidity": humidity if humidity is not None else existing.get("humidity"),
            "last_seen": now,
        }
        reading_history.setdefault(segment_id, []).append({"temp": temp, "time": now})
        reading_history[segment_id] = reading_history[segment_id][-50:]

def get_trend(segment_id):
    history = reading_history.get(segment_id, [])
    if len(history) < 5:
        return "insufficient_data"
    recent = [h["temp"] for h in history[-5:]]
    older = [h["temp"] for h in history[:-5]] or recent
    recent_avg = sum(recent) / len(recent)
    older_avg = sum(older) / len(older)
    if recent_avg - older_avg > 2:
        return "rising"
    elif older_avg - recent_avg > 2:
        return "falling"
    return "stable"

def get_segment_status():
    status = {}
    with lock:
        for seg_id, seg in SEGMENTS.items():
            reading = segment_readings.get(seg_id)
            temp = reading["temp"] if reading else None

            if temp is None:
                state = "unknown"
            elif temp > seg["temp_normal"][1]:
                state = "anomaly"
            elif temp > seg["temp_normal"][1] - 3:
                state = "warning"
            else:
                state = "normal"

            # Visual detections override to anomaly regardless of temperature
            visual_defects = _visual_flags.get(seg_id, [])
            if visual_defects:
                state = "anomaly"

            status[seg_id] = {
                **seg,
                "temp": round(temp, 1) if temp else None,
                "humidity": reading.get("humidity") if reading else None,
                "state": state,
                "trend": get_trend(seg_id),
                "last_seen": reading["last_seen"] if reading else None,
                "visual_defects": visual_defects,
            }
    return status

def force_anomaly(segment_id):
    """Manually spike a segment's temperature — for testing/demo"""
    with lock:
        spike = SEGMENTS[segment_id]["temp_normal"][1] + 15
        record_segment_reading(segment_id, spike)

def start_sensor_thread():
    t = threading.Thread(target=simulate_sensors, daemon=True)
    t.start()
    if DHT11_ENABLED:
        dht_thread = threading.Thread(target=dht11_polling_loop, daemon=True)
        dht_thread.start()
        print(f"[dht11] polling enabled on segment {DHT11_SEGMENT_ID} via {DHT11_BINARY_PATH} (GPIO {DHT11_GPIO_PIN})")
    else:
        print(f"[dht11] disabled — binary not found at {DHT11_BINARY_PATH} (expected when running off the Pi)")
