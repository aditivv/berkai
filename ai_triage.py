"""
AI triage — ONNX-based YOLO pipe defect segmentation.

Runs the ONNX model with the first available backend:
  1. onnxruntime  (laptop / anywhere pip can install it)
  2. cv2.dnn      (the QNX Pi: onnxruntime has no QNX wheels, but cv2 is
                   present — verified numerically equivalent to onnxruntime
                   on this model, max abs diff ~1.6e-3)
Requires: runs/segment/pipedown_crack_seg/weights/best.onnx
Falls back to OpenCV edge detection if the ONNX file or both backends are missing.

Tunable via environment variables:
  YOLO_CONF          confidence threshold (default 0.25)
  YOLO_IOU           NMS IoU threshold    (default 0.45)
"""

import os
import ast
import threading
import cv2
import numpy as np

ONNX_PATH = os.path.join(os.path.dirname(__file__),
                         'runs', 'segment', 'pipedown_crack_seg', 'weights', 'best.onnx')
CONF_THRESHOLD = float(os.environ.get('YOLO_CONF', '0.25'))
NMS_IOU        = float(os.environ.get('YOLO_IOU',  '0.45'))
INPUT_SIZE     = 640

_session     = None   # onnxruntime backend
_net         = None   # cv2.dnn backend (QNX Pi)
_net_lock    = threading.Lock()  # cv2.dnn setInput/forward is stateful
_class_names = None
_disabled_reason = None

# cv2.dnn can't read ONNX custom metadata, and also the safety net when
# onnxruntime metadata is missing/corrupt.
_FALLBACK_CLASS_NAMES = {
    0: 'Deformation', 1: 'Obstacle', 2: 'Rupture',
    3: 'Disconnect',  4: 'Misalignment', 5: 'Deposition',
}


def _load_model():
    global _session, _net, _class_names, _disabled_reason
    if not os.path.exists(ONNX_PATH):
        _disabled_reason = (f'ONNX model not found at {ONNX_PATH} — '
                            'export it on the laptop first, then git pull on the Pi')
        return
    try:
        import onnxruntime as ort
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = 4
        opts.inter_op_num_threads = 1
        opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        _session = ort.InferenceSession(ONNX_PATH, sess_options=opts,
                                        providers=['CPUExecutionProvider'])
        meta = _session.get_modelmeta().custom_metadata_map
        raw  = meta.get('names', '')
        try:
            _class_names = ast.literal_eval(raw)
        except Exception:
            _class_names = _FALLBACK_CLASS_NAMES
        print(f'[ai_triage] ONNX model loaded (onnxruntime) — classes: {list(_class_names.values())}')
        return
    except ImportError:
        print('[ai_triage] onnxruntime not installed — trying cv2.dnn backend')
    except Exception as e:
        _disabled_reason = f'Failed to load ONNX model: {e}'
        print(f'[ai_triage] {_disabled_reason}')
        return
    # cv2.dnn fallback — the path taken on the QNX Pi (no onnxruntime wheels).
    try:
        _net = cv2.dnn.readNetFromONNX(ONNX_PATH)
        _class_names = _FALLBACK_CLASS_NAMES
        print(f'[ai_triage] ONNX model loaded (cv2.dnn) — classes: {list(_class_names.values())}')
    except Exception as e:
        _disabled_reason = (f'no ONNX backend: onnxruntime not installed and '
                            f'cv2.dnn failed to load the model: {e}')
        print(f'[ai_triage] {_disabled_reason}')


threading.Thread(target=_load_model, daemon=True).start()


def is_enabled():
    return _session is not None or _net is not None


def disabled_reason():
    return _disabled_reason


def detect_defects(frame):
    """
    Run defect detection on a single BGR frame.
    Returns [{"label": str, "confidence": float, "bbox": (x1, y1, x2, y2)}]
    """
    if _session is None and _net is None:
        return _opencv_fallback(frame)
    try:
        orig_h, orig_w = frame.shape[:2]

        # Preprocess: resize → RGB → normalise → BCHW
        blob = cv2.resize(frame, (INPUT_SIZE, INPUT_SIZE))
        blob = blob[:, :, ::-1].astype(np.float32) / 255.0
        blob = blob.transpose(2, 0, 1)[np.newaxis]

        # Inference (onnxruntime if present, else cv2.dnn)
        if _session is not None:
            input_name = _session.get_inputs()[0].name
            raw = _session.run(None, {input_name: blob})
        else:
            with _net_lock:
                _net.setInput(blob)
                raw = list(_net.forward(_net.getUnconnectedOutLayersNames()))
        # Seg model: raw[0]=(1,4+nc+32,8400), raw[1]=(1,32,160,160) protos
        # Det model: raw[0]=(1,4+nc,8400)
        # We only need raw[0]; ignore protos and mask coefficients
        pred_tensor = raw[0]
        preds = pred_tensor[0].T  # (8400, 4+nc[+32])

        n_extra = 32 if len(raw) == 2 else 0
        nc = preds.shape[1] - 4 - n_extra
        boxes_xywh   = preds[:, :4]
        class_scores = preds[:, 4:4 + nc]
        class_ids    = class_scores.argmax(axis=1)
        confidences  = class_scores.max(axis=1)

        mask = confidences >= CONF_THRESHOLD
        if not mask.any():
            return []

        boxes_xywh  = boxes_xywh[mask]
        confidences = confidences[mask]
        class_ids   = class_ids[mask]

        # Scale from 640-space to original image space
        sx, sy = orig_w / INPUT_SIZE, orig_h / INPUT_SIZE
        x1 = (boxes_xywh[:, 0] - boxes_xywh[:, 2] / 2) * sx
        y1 = (boxes_xywh[:, 1] - boxes_xywh[:, 3] / 2) * sy
        bw =  boxes_xywh[:, 2] * sx
        bh =  boxes_xywh[:, 3] * sy

        nms_boxes = [[float(x), float(y), float(w), float(h)]
                     for x, y, w, h in zip(x1, y1, bw, bh)]
        indices = cv2.dnn.NMSBoxes(nms_boxes, confidences.tolist(),
                                   CONF_THRESHOLD, NMS_IOU)

        detections = []
        for i in (indices.flatten() if len(indices) else []):
            x, y, w, h = nms_boxes[i]
            label = _class_names.get(int(class_ids[i]), str(int(class_ids[i])))
            detections.append({
                'label':      label,
                'confidence': round(float(confidences[i]), 3),
                'bbox':       (int(x), int(y), int(x + w), int(y + h)),
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


# ── OpenCV fallback (no ONNX file / no onnxruntime) ──────────────────────────

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
    contours, _ = cv2.findContours(dilated, cv2.RETR_EXTERNAL,
                                   cv2.CHAIN_APPROX_SIMPLE)
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
