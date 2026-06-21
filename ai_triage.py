"""
AI triage — OpenCV-based crack detection in pipe camera frames.

Uses edge detection + contour geometry to find elongated, crack-like features.
No ML model or internet connection required — works with cv2 only.

Tunable via environment variables:
  CRACK_MIN_AREA       minimum contour area in pixels     (default 200)
  CRACK_ASPECT_RATIO   min length/width ratio for a crack (default 3.5)
  CRACK_CANNY_LOW      Canny lower threshold              (default 50)
  CRACK_CANNY_HIGH     Canny upper threshold              (default 150)
"""

import os
import cv2
import numpy as np

CRACK_MIN_AREA     = int(os.environ.get("CRACK_MIN_AREA",     "200"))
CRACK_ASPECT_RATIO = float(os.environ.get("CRACK_ASPECT_RATIO", "3.5"))
CRACK_CANNY_LOW    = int(os.environ.get("CRACK_CANNY_LOW",    "50"))
CRACK_CANNY_HIGH   = int(os.environ.get("CRACK_CANNY_HIGH",   "150"))


def is_enabled():
    return True


def disabled_reason():
    return None


def detect_defects(frame):
    """
    Run OpenCV crack detection on a single BGR frame.

    Returns a list of dicts: [{"label": "crack", "confidence": float,
    "bbox": (x1, y1, x2, y2)}, ...].
    Empty list means no cracks detected.
    """
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

    # Enhance local contrast so cracks stand out against the pipe surface
    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    enhanced = clahe.apply(gray)

    # Smooth to reduce sensor noise before edge detection
    blurred = cv2.GaussianBlur(enhanced, (5, 5), 0)

    # Detect edges
    edges = cv2.Canny(blurred, CRACK_CANNY_LOW, CRACK_CANNY_HIGH)

    # Dilate to join nearby edge fragments into continuous crack lines
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
    dilated = cv2.dilate(edges, kernel, iterations=2)

    contours, _ = cv2.findContours(dilated, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

    detections = []
    for cnt in contours:
        if cv2.contourArea(cnt) < CRACK_MIN_AREA:
            continue

        # minAreaRect gives the true aspect ratio regardless of orientation
        _, (w, h), _ = cv2.minAreaRect(cnt)
        if min(w, h) == 0:
            continue
        aspect = max(w, h) / min(w, h)
        if aspect < CRACK_ASPECT_RATIO:
            continue

        x, y, bw, bh = cv2.boundingRect(cnt)

        # Confidence: normalise aspect ratio; higher aspect = more crack-like
        confidence = round(min(aspect / 10.0, 1.0), 3)

        detections.append({
            "label": "crack",
            "confidence": confidence,
            "bbox": (x, y, x + bw, y + bh),
        })

    return detections


def annotate_frame(frame, detections):
    """
    Draw bounding boxes + labels onto a copy of the frame.
    Returns frame unchanged if detections is empty.
    """
    if not detections:
        return frame

    annotated = frame.copy()
    for det in detections:
        x1, y1, x2, y2 = det["bbox"]
        color = (0, 0, 255) if det["confidence"] > 0.7 else (0, 165, 255)
        cv2.rectangle(annotated, (x1, y1), (x2, y2), color, 2)
        text = f'crack {det["confidence"]:.0%}'
        cv2.putText(annotated, text, (x1, max(y1 - 8, 0)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)
    return annotated
