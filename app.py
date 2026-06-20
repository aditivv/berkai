from flask import Flask, jsonify, request, send_from_directory
from datetime import datetime
import os
from dotenv import load_dotenv

load_dotenv()

from config import SEGMENTS
from sensor_reader import get_segment_status, start_sensor_thread, force_anomaly

app = Flask(__name__, static_folder='static')

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

@app.route('/api/config')
def config():
    return jsonify({
        "camera_url": os.environ.get("PI_CAMERA_URL", "")
    })

@app.route('/api/demo-anomaly', methods=['POST'])
def demo_anomaly():
    data = request.json
    segment_id = int(data.get('segment_id', 0))
    force_anomaly(segment_id)
    return jsonify({"ok": True, "segment_id": segment_id})

if __name__ == '__main__':
    start_sensor_thread(use_arduino=False)  # flip to True once Arduino is wired
    app.run(host='0.0.0.0', port=5000, debug=True)