from flask import Flask, jsonify, request, send_from_directory, Response, session, redirect, url_for, render_template_string
from datetime import datetime
import threading
import time
import smtplib
import os
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
import cv2
from dotenv import load_dotenv

load_dotenv()

from config import SEGMENTS
from sensor_reader import get_segment_status, start_sensor_thread, force_anomaly, set_visual_flag
from ai_triage import detect_defects, annotate_frame, is_enabled, disabled_reason

_TRIAGE_INTERVAL = 10   # seconds between model inference passes
_TRIAGE_SEGMENT  = 0    # segment id monitored by the physical camera

app = Flask(__name__, static_folder='static', template_folder='static')
app.secret_key = os.environ.get('SECRET_KEY', 'pipe-monitor-secret-key')

# ── Auth ──────────────────────────────────────────────────────────────────────

_PUBLIC_PATHS = {'/login'}

@app.before_request
def require_login():
    if request.path not in _PUBLIC_PATHS and not request.path.startswith('/static/'):
        if 'user_email' not in session:
            return redirect(url_for('login'))

@app.route('/login', methods=['GET', 'POST'])
def login():
    # Read credentials fresh each request so .env changes take effect without restart
    admin_email    = os.environ.get('ADMIN_EMAIL', '')
    admin_password = os.environ.get('ADMIN_PASSWORD', '')
    error = None
    if request.method == 'POST':
        email    = request.form.get('email', '').strip()
        password = request.form.get('password', '')
        if email == admin_email and password == admin_password:
            session['user_email'] = email
            return redirect(url_for('index'))
        error = 'Invalid email or password.'
    return render_template_string(
        open('static/login.html').read(),
        error=error
    )

@app.route('/logout')
def logout():
    session.clear()
    return redirect(url_for('login'))

# ── AI triage state ─────────────────────────────────────────────────────────

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
_latest_frame = None
_frame_lock = threading.Lock()
_STREAM_INFER_INTERVAL = 1.0  # seconds between inference passes on the stream

def _get_camera():
    global _camera
    if _camera is None or not _camera.isOpened():
        _camera = _VideoCapture(0)
    return _camera

def _capture_and_infer():
    """Background thread: capture frames at full speed, run inference every second."""
    global _latest_frame, _latest_detections
    cam = _get_camera()
    last_infer = 0
    while True:
        ok, frame = cam.read()
        if not ok:
            time.sleep(0.05)
            continue
        with _frame_lock:
            _latest_frame = frame
        now = time.time()
        if now - last_infer >= _STREAM_INFER_INTERVAL:
            detections = detect_defects(frame)
            with _detections_lock:
                _latest_detections = detections
            last_infer = now

def _generate_frames():
    """Stream thread: encode and send latest frame as fast as possible."""
    while True:
        with _frame_lock:
            frame = _latest_frame
        if frame is None:
            time.sleep(0.01)
            continue
        with _detections_lock:
            detections = list(_latest_detections)
        annotated = annotate_frame(frame, detections)
        _, buf = cv2.imencode('.jpg', annotated, [cv2.IMWRITE_JPEG_QUALITY, 70])
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
    resp = send_from_directory('static', 'index.html')
    resp.headers['Cache-Control'] = 'no-store'
    return resp

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

# ── Email alerts ──────────────────────────────────────────────────────────────

def _send_anomaly_email(segment_name, visual_defects):
    smtp_host  = os.environ.get('SMTP_HOST', '')
    smtp_port  = int(os.environ.get('SMTP_PORT', '587'))
    smtp_user  = os.environ.get('SMTP_USER', '')
    smtp_pass  = os.environ.get('SMTP_PASSWORD', '')
    alert_to   = os.environ.get('ALERT_EMAIL', os.environ.get('ADMIN_EMAIL', ''))

    if not all([smtp_host, smtp_user, smtp_pass, alert_to]):
        print('[email] alert skipped — SMTP not configured in .env')
        return

    defect_str = ', '.join(visual_defects) if visual_defects else 'temperature threshold exceeded'
    subject = f'[Pipe Monitor] Anomaly detected — {segment_name}'
    body = (
        f'An anomaly has been detected on the pipe monitoring system.\n\n'
        f'Segment  : {segment_name}\n'
        f'Detected : {defect_str}\n'
        f'Time     : {datetime.now().strftime("%Y-%m-%d %H:%M:%S")}\n\n'
        f'Open the dashboard to inspect the live feed and sensor data.'
    )

    msg = MIMEMultipart()
    msg['From']    = smtp_user
    msg['To']      = alert_to
    msg['Subject'] = subject
    msg.attach(MIMEText(body, 'plain'))

    try:
        with smtplib.SMTP(smtp_host, smtp_port) as server:
            server.starttls()
            server.login(smtp_user, smtp_pass)
            server.sendmail(smtp_user, alert_to, msg.as_string())
        print(f'[email] anomaly alert sent to {alert_to}')
    except Exception as e:
        print(f'[email] failed to send alert: {e}')

_prev_anomalies = set()

def _monitor_anomalies():
    """Every 15 s, check for new anomalies and send an email alert."""
    global _prev_anomalies
    while True:
        time.sleep(15)
        try:
            status = get_segment_status()
            current_anomalies = {k for k, v in status.items() if v['state'] == 'anomaly'}
            new_anomalies = current_anomalies - _prev_anomalies
            for seg_id in new_anomalies:
                seg = status[seg_id]
                _send_anomaly_email(
                    seg.get('name', f'Segment {seg_id}'),
                    seg.get('visual_defects', [])
                )
            _prev_anomalies = current_anomalies
        except Exception as e:
            print(f'[email monitor] error: {e}')

# ── Triage ────────────────────────────────────────────────────────────────────

def _run_triage_updates():
    """Every _TRIAGE_INTERVAL seconds, apply latest detections to the segment state."""
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
    threading.Thread(target=_capture_and_infer, daemon=True).start()
    threading.Thread(target=_run_triage_updates, daemon=True).start()
    threading.Thread(target=_monitor_anomalies, daemon=True).start()
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False)
