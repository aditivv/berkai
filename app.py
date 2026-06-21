from flask import Flask, jsonify, request, send_from_directory, Response
from datetime import datetime
import cv2
from dotenv import load_dotenv

load_dotenv()

from config import SEGMENTS
from sensor_reader import get_segment_status, start_sensor_thread, force_anomaly

app = Flask(__name__, static_folder='static')

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
        _camera = _VideoCapture(0)
    return _camera

def _generate_frames():
    cam = _get_camera()
    while True:
        ok, frame = cam.read()
        if not ok:
            break
        _, buf = cv2.imencode('.jpg', frame)
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + buf.tobytes() + b'\r\n')

@app.route('/video_feed')
def video_feed():
    return Response(_generate_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

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

if __name__ == '__main__':
    start_sensor_thread()
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False)
