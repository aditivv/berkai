"""
AI triage — YOLO-based pipe defect detection.

Loads runs/detect/train-2/weights/best.pt (trained on the pipe defects dataset).
Falls back to OpenCV edge detection if the model file is missing or ultralytics
is not installed.

Tunable via environment variables:
  YOLO_CONF          detection confidence threshold (default 0.25)
  CRACK_MIN_AREA     fallback OpenCV: min contour area (default 200)
  CRACK_ASPECT_RATIO fallback OpenCV: min length/width ratio (default 3.5)
  CRACK_CANNY_LOW    fallback OpenCV: Canny lower threshold (default 50)
  CRACK_CANNY_HIGH   fallback OpenCV: Canny upper threshold (default 150)
"""

import os
import cv2
import numpy as np

MODEL_PATH = os.path.join(os.path.dirname(__file__), 'runs', 'detect', 'train-2', 'weights', 'best.pt')
CONF_THRESHOLD = float(os.environ.get('YOLO_CONF', '0.25'))

_model = None
_disabled_reason = None


def _load_model():
    global _model, _disabled_reason
    if not os.path.exists(MODEL_PATH):
        _disabled_reason = f'Model not found at {MODEL_PATH} — using OpenCV fallback'
        return
    try:
        from ultralytics import YOLO
        _model = YOLO(MODEL_PATH)
        _model.overrides['verbose'] = False
        print(f'[ai_triage] YOLO model loaded — classes: {list(_model.names.values())}')
    except Exception as e:
        _disabled_reason = f'Failed to load YOLO model: {e}'
        print(f'[ai_triage] {_disabled_reason}')


_load_model()


def is_enabled():
    return _model is not None


def disabled_reason():
    return _disabled_reason


def detect_defects(frame):
    """
    Run defect detection on a single BGR frame.
    Returns [{"label": str, "confidence": float, "bbox": (x1, y1, x2, y2)}]
    """
    if _model is None:
        return _opencv_fallback(frame)
    try:
        results = _model(frame, conf=CONF_THRESHOLD, verbose=False)[0]
        detections = []
        for box in results.boxes:
            cls_id = int(box.cls[0])
            label  = _model.names[cls_id]
            conf   = float(box.conf[0])
            x1, y1, x2, y2 = map(int, box.xyxy[0])
            detections.append({
                'label':      label,
                'confidence': round(conf, 3),
                'bbox':       (x1, y1, x2, y2),
            })
        return detections
    except Exception as e:
        print(f'[ai_triage] inference error: {e}')
        return []


def annotate_frame(frame, detections):
    """Draw bounding boxes + labels onto a copy of the frame."""
    if not detections:
        return frame
    annotated = frame.copy()
    for det in detections:
        x1, y1, x2, y2 = det['bbox']
        color = (0, 0, 255) if det['confidence'] > 0.7 else (0, 165, 255)
        cv2.rectangle(annotated, (x1, y1), (x2, y2), color, 2)
        text = f'{det["label"]} {det["confidence"]:.0%}'
        cv2.putText(annotated, text, (x1, max(y1 - 8, 0)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)
    return annotated


# ── OpenCV fallback (no model file / no ultralytics) ─────────────────────────

CRACK_MIN_AREA     = int(os.environ.get('CRACK_MIN_AREA',     '200'))
CRACK_ASPECT_RATIO = float(os.environ.get('CRACK_ASPECT_RATIO', '3.5'))
CRACK_CANNY_LOW    = int(os.environ.get('CRACK_CANNY_LOW',    '50'))
CRACK_CANNY_HIGH   = int(os.environ.get('CRACK_CANNY_HIGH',   '150'))


def _opencv_fallback(frame):
    gray     = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    clahe    = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    enhanced = clahe.apply(gray)
    blurred  = cv2.GaussianBlur(enhanced, (5, 5), 0)
    edges    = cv2.Canny(blurred, CRACK_CANNY_LOW, CRACK_CANNY_HIGH)
    kernel   = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
    dilated  = cv2.dilate(edges, kernel, iterations=2)
    contours, _ = cv2.findContours(dilated, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    detections = []
    for cnt in contours:
        if cv2.contourArea(cnt) < CRACK_MIN_AREA:
            continue
        _, (w, h), _ = cv2.minAreaRect(cnt)
        if min(w, h) == 0:
            continue
        aspect = max(w, h) / min(w, h)
        if aspect < CRACK_ASPECT_RATIO:
            continue
        x, y, bw, bh = cv2.boundingRect(cnt)
        confidence = round(min(aspect / 10.0, 1.0), 3)
        detections.append({
            'label':      'crack',
            'confidence': confidence,
            'bbox':       (x, y, x + bw, y + bh),
        })
    return detections
