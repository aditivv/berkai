from flask import Flask, jsonify, request, send_from_directory, Response
from datetime import datetime
import threading
import time
import cv2
from dotenv import load_dotenv

load_dotenv()

from config import SEGMENTS
from sensor_reader import get_segment_status, start_sensor_thread, force_anomaly, set_visual_flag
from ai_triage import detect_defects, annotate_frame, is_enabled, disabled_reason

_TRIAGE_INTERVAL = 10   # seconds between model inference passes
_TRIAGE_SEGMENT  = 0    # segment id monitored by the physical camera

app = Flask(__name__, static_folder='static')

# ── AI triage state ─────────────────────────────────────────────────────────
# Latest detections are cached here so /api/detections can report them
# without re-running inference; _generate_frames() updates this as it goes.

_latest_detections = []
_detections_lock = threading.Lock()

# ── Camera ────────────────────────────────────────────────────────────────────
# Camera Module 3 is read through QNX's Sensor Framework via qnx_apis, a
# wrapper that mirrors cv2's VideoCapture API. Falls back to cv2.VideoCapture
# if qnx_apis isn't importable (e.g. running this off the Pi for other testing) —
# that fallback will NOT see the Camera Module 3 under QNX, it's just so the
# rest of the app doesn't crash on import.

try:
    import qnx_apis
    _VideoCapture = qnx_apis.VideoCapture
    print(_VideoCapture)
except ImportError:
    print("qnx_apis not found — falling back to cv2.VideoCapture (won't see the Pi camera on QNX)")
    _VideoCapture = cv2.VideoCapture

_camera = None

def _get_camera():
    global _camera
    # list all cameras available
    
    if _camera is None or not _camera.isOpened():
        _camera = _VideoCapture(1)
    return _camera

def _generate_frames():
    global _latest_detections
    cam = _get_camera()
    while True:
        ok, frame = cam.read()
        if not ok:
            break

        detections = detect_defects(frame)
        with _detections_lock:
            _latest_detections = detections
        frame = annotate_frame(frame, detections)

        _, buf = cv2.imencode('.jpg', frame)
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + buf.tobytes() + b'\r\n')

@app.route('/video_feed')
def video_feed():
    return Response(_generate_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/api/detections')
def detections():
    with _detections_lock:
        current = list(_latest_detections)
    return jsonify({
        "enabled": is_enabled(),
        "disabled_reason": disabled_reason(),
        "detections": current,
        "timestamp": datetime.now().isoformat()
    })

# ── API ───────────────────────────────────────────────────────────────────────

@app.route('/')
def index():
    return send_from_directory('static', 'index.html')

@app.route('/api/twin-state')
def twin_state():
    status = get_segment_status()
    anomalies = [k for k, v in status.items() if v["state"] == "anomaly"]
    warnings = [k for k, v in status.items() if v["state"] == "warning"]
    rising = [k for k, v in status.items() if v["trend"] == "rising"]

    return jsonify({
        "segments": status,
        "anomalies": anomalies,
        "warnings": warnings,
        "rising_trends": rising,
        "timestamp": datetime.now().isoformat()
    })

@app.route('/api/demo-anomaly', methods=['POST'])
def demo_anomaly():
    data = request.json
    segment_id = int(data.get('segment_id', 0))
    force_anomaly(segment_id)
    return jsonify({"ok": True, "segment_id": segment_id})

def _run_triage_updates():
    """Every _TRIAGE_INTERVAL seconds, apply latest YOLO detections to the segment state."""
    while True:
        time.sleep(_TRIAGE_INTERVAL)
        with _detections_lock:
            detections = list(_latest_detections)
        labels = [d['label'] for d in detections]
        set_visual_flag(_TRIAGE_SEGMENT, labels)
        if labels:
            print(f"[triage] segment {_TRIAGE_SEGMENT} flagged: {labels}")

if __name__ == '__main__':
    start_sensor_thread()
    threading.Thread(target=_run_triage_updates, daemon=True).start()
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False)
