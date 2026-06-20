import threading
import random
import time
import serial
from datetime import datetime
from config import SEGMENTS

segment_readings = {}
reading_history = {}
lock = threading.RLock()

# ── SIMULATED MODE ──────────────────────────────────────

def simulate_sensors():
    with lock:
        for seg_id, seg in SEGMENTS.items():
            mid = (seg["temp_normal"][0] + seg["temp_normal"][1]) / 2
            segment_readings[seg_id] = {"temp": mid, "last_seen": datetime.now().isoformat()}

    while True:
        with lock:
            for seg_id in SEGMENTS:
                current = segment_readings[seg_id]["temp"]
                drift = random.uniform(-0.5, 0.5)
                new_temp = current + drift
                record_segment_reading(seg_id, new_temp)
        time.sleep(3)

# ── REAL ARDUINO MODE (use this once hardware is wired) ──

def read_arduino(port='/dev/ttyACM0'):
    try:
        ser = serial.Serial(port, 9600, timeout=2)
        print(f"Arduino connected on {port}")
    except Exception as e:
        print(f"Arduino not found ({e}) — falling back to simulation")
        simulate_sensors()
        return

    while True:
        try:
            line = ser.readline().decode('utf-8').strip()
            if line.startswith("SEGMENT:"):
                parts = line.split(":")
                segment_id = int(parts[1])
                temp = float(parts[2])
                record_segment_reading(segment_id, temp)
        except Exception as e:
            print(f"Read error: {e}")

# ── SHARED LOGIC ─────────────────────────────────────────

def record_segment_reading(segment_id, temp):
    with lock:
        now = datetime.now().isoformat()
        segment_readings[segment_id] = {"temp": temp, "last_seen": now}
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

            status[seg_id] = {
                **seg,
                "temp": round(temp, 1) if temp else None,
                "state": state,
                "trend": get_trend(seg_id),
                "last_seen": reading["last_seen"] if reading else None,
            }
    return status

def force_anomaly(segment_id):
    """Manually spike a segment's temperature — for testing/demo"""
    with lock:
        spike = SEGMENTS[segment_id]["temp_normal"][1] + 15
        record_segment_reading(segment_id, spike)

def start_sensor_thread(use_arduino=False):
    target = read_arduino if use_arduino else simulate_sensors
    t = threading.Thread(target=target, daemon=True)
    t.start()