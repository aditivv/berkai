"""
camera_source.py — single shared camera source for app.py.

Wraps camera_driver/imx708_stream.py in a FrameHub-style broadcaster (the
pattern proven in camera_driver/mjpeg_server.py): exactly ONE background
thread reads /dev/video0 — the QNX driver is single-buffer/polling with no
queue, so concurrent readers would fight over the one buffer — and every
consumer (each /video_feed client, the inference loop) is handed the *latest*
frame instead of doing its own capture.

Frames are produced in BGR (cv2 / ai_triage convention) at 960x540, the best
measured colour config (~14 fps, capture-bound — see HANDOFF_stream_merge.md).

If /dev/video0 is missing (camera_resmgr not started as root) the hub retries
every few seconds, publishes a status card so /video_feed shows the error
instead of hanging, and — for off-Pi development only — falls back to a local
webcam via cv2.VideoCapture(0) when one exists.
"""

from __future__ import annotations

import os
import sys
import threading
import time
from typing import Callable, Optional

import cv2
import numpy as np

_CAMERA_DRIVER_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'camera_driver')
if _CAMERA_DRIVER_DIR not in sys.path:
    sys.path.insert(0, _CAMERA_DRIVER_DIR)

from imx708_stream import Imx708Stream, StreamConfig  # noqa: E402

STREAM_RESIZE = (960, 540)   # 16:9, matches the sensor's 2304x1296
JPEG_QUALITY = 80
_RETRY_DELAY = 3.0           # seconds between attempts when the device is missing

# The proven streaming config from the extraction session — do not re-tune here.
DEFAULT_CONFIG = StreamConfig(
    unpack="fast8", debayer="auto", color="bgr", resize=STREAM_RESIZE,
)

# annotate(frame_bgr) -> frame_bgr; must not mutate its input (annotate_frame copies)
AnnotateFn = Callable[[np.ndarray], np.ndarray]


class CameraHub:
    """One capture thread; broadcasts the latest frame/JPEG to all consumers."""

    def __init__(self, cfg: StreamConfig = DEFAULT_CONFIG,
                 annotate: Optional[AnnotateFn] = None,
                 jpeg_quality: int = JPEG_QUALITY) -> None:
        self.cfg = cfg
        self.annotate = annotate
        self.jpeg_quality = jpeg_quality
        self._cond = threading.Condition()
        self._jpeg: Optional[bytes] = None
        self._frame: Optional[np.ndarray] = None   # raw (un-annotated) BGR frame
        self._seq = 0
        self._error: Optional[str] = None
        self._fps = 0.0
        self._thread = threading.Thread(target=self._run, name="camera-hub", daemon=True)

    # ── consumer API ────────────────────────────────────────────────────────

    def start(self) -> None:
        self._thread.start()

    def latest_frame(self) -> Optional[np.ndarray]:
        """Latest raw BGR frame (for inference). May be None before first frame."""
        with self._cond:
            return self._frame

    def wait_jpeg(self, last_seq: int, timeout: float = 5.0):
        """Block until a JPEG newer than last_seq exists. Returns (jpeg, seq);
        jpeg is None on timeout (caller should just loop)."""
        with self._cond:
            if self._seq == last_seq or self._jpeg is None:
                self._cond.wait(timeout)
            if self._seq == last_seq or self._jpeg is None:
                return None, last_seq
            return self._jpeg, self._seq

    @property
    def error(self) -> Optional[str]:
        return self._error

    @property
    def fps(self) -> float:
        return self._fps

    # ── capture thread ──────────────────────────────────────────────────────

    def _run(self) -> None:
        # Nothing in this loop may raise, or the (daemon) capture thread dies
        # silently and the feed never recovers — guard every phase.
        while True:
            try:
                self._stream_imx708()
            except FileNotFoundError:
                self._error = (f"{self.cfg.device} not found — start camera_resmgr "
                               f"as root first (see camera_driver/STREAM_USAGE.md)")
            except Exception as e:
                self._error = f"camera capture failed: {e}"
            print(f"[camera] {self._error} — retrying in {_RETRY_DELAY:.0f}s")
            try:
                if self._stream_webcam_fallback():
                    continue  # webcam ended (unplugged?) — go around and retry
                self._publish_status_card(self._error)
            except Exception as e:
                print(f"[camera] fallback/status-card failed: {e}")
            time.sleep(_RETRY_DELAY)

    def _stream_imx708(self) -> None:
        with Imx708Stream(self.cfg) as cam:
            print(f"[camera] IMX708 capture started: {self.cfg}")
            self._error = None
            frames, t0 = 0, time.monotonic()
            for frame in cam.frames():
                self._publish(frame)
                frames += 1
                if frames % 60 == 0:
                    now = time.monotonic()
                    self._fps = 60.0 / (now - t0)
                    t0 = now

    def _stream_webcam_fallback(self) -> bool:
        """Off-Pi development only: serve a local webcam if cv2 can open one.
        Returns True if a webcam was streamed (and then stopped)."""
        cap = cv2.VideoCapture(0)
        if not cap.isOpened():
            cap.release()
            return False
        print("[camera] dev fallback: streaming local webcam via cv2.VideoCapture(0)")
        self._error = None
        try:
            while True:
                ok, frame = cap.read()
                if not ok:
                    return True
                self._publish(cv2.resize(frame, STREAM_RESIZE, interpolation=cv2.INTER_AREA))
        finally:
            cap.release()

    def _publish(self, frame_bgr: np.ndarray) -> None:
        shown = self.annotate(frame_bgr) if self.annotate else frame_bgr
        ok, buf = cv2.imencode('.jpg', shown, [cv2.IMWRITE_JPEG_QUALITY, self.jpeg_quality])
        if not ok:
            print("[camera] cv2.imencode failed — frame dropped")
            return
        with self._cond:
            self._frame = frame_bgr
            self._jpeg = buf.tobytes()
            self._seq += 1
            self._cond.notify_all()

    def _publish_status_card(self, message: Optional[str]) -> None:
        """Publish a dark card with the error text so /video_feed never hangs."""
        card = np.full((STREAM_RESIZE[1], STREAM_RESIZE[0], 3), 24, np.uint8)
        cv2.putText(card, "NO CAMERA SIGNAL", (40, 250),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.4, (0, 0, 220), 3)
        for i, line in enumerate(_wrap_text(message or "unknown error", 70)):
            cv2.putText(card, line, (40, 310 + 30 * i),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (200, 200, 200), 1)
        ok, buf = cv2.imencode('.jpg', card, [cv2.IMWRITE_JPEG_QUALITY, self.jpeg_quality])
        if not ok:
            return
        with self._cond:
            self._jpeg = buf.tobytes()   # leave self._frame alone: no real frame
            self._seq += 1
            self._cond.notify_all()


def _wrap_text(text: str, width: int) -> list[str]:
    words, lines, cur = text.split(), [], ""
    for w in words:
        cand = f"{cur} {w}".strip()
        if len(cand) > width and cur:
            lines.append(cur)
            cur = w
        else:
            cur = cand
    if cur:
        lines.append(cur)
    return lines
