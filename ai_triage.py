"""
AI triage — runs a YOLO defect-detection model (cracks / corrosion / leaks)
against camera frames and reports back what it finds.

Weights path is configurable via the YOLO_WEIGHTS_PATH env var, defaulting to
runs/detect/train/weights/best.pt (ultralytics' default training output
location). If ultralytics isn't installed, or the weights file doesn't exist
yet, detection is silently disabled — the rest of the app (camera feed,
temperature monitoring) keeps working without it.
"""

import os
import threading

WEIGHTS_PATH = os.environ.get("YOLO_WEIGHTS_PATH", "runs/detect/train/weights/best.pt")
CONFIDENCE_THRESHOLD = float(os.environ.get("YOLO_CONFIDENCE", "0.4"))

_model = None
_model_lock = threading.Lock()
_load_attempted = False
_disabled_reason = None


def _try_load_model():
    """Lazily load the YOLO model on first use. Thread-safe, idempotent."""
    global _model, _load_attempted, _disabled_reason

    with _model_lock:
        if _load_attempted:
            return
        _load_attempted = True

        try:
            from ultralytics import YOLO
        except ImportError:
            _disabled_reason = "ultralytics not installed — pip install ultralytics"
            print(f"[ai_triage] disabled: {_disabled_reason}")
            return

        if not os.path.exists(WEIGHTS_PATH):
            _disabled_reason = f"no weights found at {WEIGHTS_PATH} — train a model first"
            print(f"[ai_triage] disabled: {_disabled_reason}")
            return

        try:
            _model = YOLO(WEIGHTS_PATH)
            print(f"[ai_triage] loaded model from {WEIGHTS_PATH}")
        except Exception as e:
            _disabled_reason = f"failed to load model: {e}"
            print(f"[ai_triage] disabled: {_disabled_reason}")


def is_enabled():
    """Whether detection is actually available right now."""
    _try_load_model()
    return _model is not None


def disabled_reason():
    """Human-readable reason detection is unavailable, or None if it's working."""
    _try_load_model()
    return _disabled_reason


def detect_defects(frame):
    """
    Run detection on a single frame (numpy BGR array, as returned by
    cv2.VideoCapture/qnx_apis.VideoCapture .read()).

    Returns a list of dicts: [{"label": str, "confidence": float,
    "bbox": (x1, y1, x2, y2)}, ...]. Returns [] if detection is disabled
    or no defects are found above the confidence threshold.
    """
    _try_load_model()
    if _model is None:
        return []

    results = _model.predict(frame, conf=CONFIDENCE_THRESHOLD, verbose=False)
    detections = []
    for result in results:
        for box in result.boxes:
            x1, y1, x2, y2 = box.xyxy[0].tolist()
            label = result.names[int(box.cls[0])]
            confidence = float(box.conf[0])
            detections.append({
                "label": label,
                "confidence": round(confidence, 3),
                "bbox": (round(x1), round(y1), round(x2), round(y2)),
            })
    return detections


def annotate_frame(frame, detections):
    """
    Draw bounding boxes + labels onto a copy of the frame for display.
    Safe to call with an empty detections list (returns frame unchanged).
    """
    if not detections:
        return frame

    import cv2
    annotated = frame.copy()
    for det in detections:
        x1, y1, x2, y2 = det["bbox"]
        color = (0, 0, 255) if det["confidence"] > 0.7 else (0, 165, 255)
        cv2.rectangle(annotated, (x1, y1), (x2, y2), color, 2)
        text = f'{det["label"]} {det["confidence"]:.0%}'
        cv2.putText(annotated, text, (x1, max(y1 - 8, 0)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)
    return annotated
