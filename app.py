from flask import (Flask, jsonify, request, send_from_directory,
                   Response, session, redirect, url_for, render_template_string)
from datetime import datetime
from functools import wraps
import cv2
import os
import smtplib
import threading
from email.mime.text import MIMEText
from dotenv import load_dotenv

load_dotenv()

from config import SEGMENTS
from sensor_reader import get_segment_status, start_sensor_thread, force_anomaly

app = Flask(__name__, static_folder='static')
app.secret_key = os.environ.get('SECRET_KEY', 'change-me-in-production')

# ── Auth ──────────────────────────────────────────────────────────────────────

def login_required(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if 'user_email' not in session:
            return redirect(url_for('login_page'))
        return f(*args, **kwargs)
    return decorated

LOGIN_HTML = """
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Sign In — Pipe Digital Twin</title>
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    html, body { height: 100%; font-family: 'Segoe UI', Tahoma, sans-serif; background: #0f1117; color: #e0e0e0; }
    body { display: flex; align-items: center; justify-content: center; }
    .card {
      background: #1a1d27;
      border: 1px solid #2a2d3e;
      border-radius: 12px;
      padding: 40px 36px;
      width: 360px;
    }
    .logo { font-size: 28px; margin-bottom: 6px; }
    h1 { font-size: 18px; font-weight: 600; color: #fff; margin-bottom: 4px; }
    .subtitle { font-size: 12px; color: #555; margin-bottom: 28px; }
    label { display: block; font-size: 11px; text-transform: uppercase; letter-spacing: .6px; color: #666; margin-bottom: 6px; }
    input {
      width: 100%;
      background: #131620;
      border: 1px solid #2a2d3e;
      border-radius: 6px;
      padding: 10px 12px;
      font-size: 14px;
      color: #e0e0e0;
      outline: none;
      margin-bottom: 16px;
      transition: border-color .15s;
    }
    input:focus { border-color: #4a6fa5; }
    .error {
      background: rgba(244,67,54,.12);
      border: 1px solid rgba(244,67,54,.4);
      border-radius: 6px;
      color: #f44336;
      font-size: 13px;
      padding: 9px 12px;
      margin-bottom: 16px;
    }
    button {
      width: 100%;
      padding: 11px;
      background: #2a4a7f;
      border: none;
      border-radius: 6px;
      color: #fff;
      font-size: 14px;
      font-weight: 600;
      cursor: pointer;
      transition: background .15s;
    }
    button:hover { background: #3a5a9f; }
  </style>
</head>
<body>
  <div class="card">
    <div class="logo">🔧</div>
    <h1>Pipe Digital Twin</h1>
    <p class="subtitle">Sign in to access the pipeline monitor</p>
    <form method="POST">
      <label>Email</label>
      <input type="email" name="email" placeholder="you@example.com" required autofocus>
      <label>Password</label>
      <input type="password" name="password" placeholder="••••••••" required>
      {% if error %}<div class="error">{{ error }}</div>{% endif %}
      <button type="submit">Sign In</button>
    </form>
  </div>
</body>
</html>
"""

@app.route('/login', methods=['GET', 'POST'])
def login_page():
    if 'user_email' in session:
        return redirect('/')
    error = ''
    if request.method == 'POST':
        email    = request.form.get('email', '').strip().lower()
        password = request.form.get('password', '')
        if email == os.environ.get('USER_EMAIL', '').lower() and \
           password == os.environ.get('USER_PASSWORD', ''):
            session['user_email'] = email
            return redirect('/')
        error = 'Invalid email or password.'
    return render_template_string(LOGIN_HTML, error=error)

@app.route('/logout')
def logout():
    session.clear()
    return redirect(url_for('login_page'))

# ── Anomaly email ─────────────────────────────────────────────────────────────

_notified_anomalies = set()
_notify_lock = threading.Lock()

def _send_email(recipient, segment_name):
    def _send():
        smtp_host = os.environ.get('SMTP_SERVER', 'smtp.gmail.com')
        smtp_port = int(os.environ.get('SMTP_PORT', '465'))
        smtp_user = os.environ.get('SMTP_EMAIL', '')
        smtp_pass = os.environ.get('SMTP_PASSWORD', '')
        if not smtp_user or not smtp_pass:
            print('Email not configured — skipping anomaly notification')
            return
        try:
            body = (
                f"An anomaly has been detected in {segment_name}.\n\n"
                f"Please check the Pipe Digital Twin dashboard immediately.\n\n"
                f"Time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}"
            )
            msg = MIMEText(body)
            msg['Subject'] = f'⚠ Pipe Anomaly Alert: {segment_name}'
            msg['From']    = smtp_user
            msg['To']      = recipient
            with smtplib.SMTP_SSL(smtp_host, smtp_port) as server:
                server.login(smtp_user, smtp_pass)
                server.send_message(msg)
            print(f'Anomaly email sent to {recipient} for {segment_name}')
        except Exception as e:
            print(f'Email error: {e}')
    threading.Thread(target=_send, daemon=True).start()

# ── Camera ────────────────────────────────────────────────────────────────────

_camera = None

def _get_camera():
    global _camera
    if _camera is None or not _camera.isOpened():
        _camera = cv2.VideoCapture(0)
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
@login_required
def video_feed():
    return Response(_generate_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

# ── API ───────────────────────────────────────────────────────────────────────

@app.route('/')
@login_required
def index():
    return send_from_directory('static', 'index.html')

@app.route('/api/twin-state')
@login_required
def twin_state():
    global _notified_anomalies
    status    = get_segment_status()
    anomalies = [k for k, v in status.items() if v["state"] == "anomaly"]
    warnings  = [k for k, v in status.items() if v["state"] == "warning"]
    rising    = [k for k, v in status.items() if v["trend"] == "rising"]

    current = set(anomalies)
    with _notify_lock:
        new_anomalies      = current - _notified_anomalies
        _notified_anomalies = (_notified_anomalies | new_anomalies) & current

    for seg_id in new_anomalies:
        seg_name = status[seg_id].get('name', f'Segment {seg_id}')
        _send_email(session['user_email'], seg_name)

    return jsonify({
        "segments":     status,
        "anomalies":    anomalies,
        "warnings":     warnings,
        "rising_trends": rising,
        "timestamp":    datetime.now().isoformat()
    })

@app.route('/api/demo-anomaly', methods=['POST'])
@login_required
def demo_anomaly():
    data = request.json
    segment_id = int(data.get('segment_id', 0))
    force_anomaly(segment_id)
    return jsonify({"ok": True, "segment_id": segment_id})

if __name__ == '__main__':
    start_sensor_thread(use_arduino=True)
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False)
