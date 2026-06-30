#!/usr/bin/env python3
"""
mjpeg_server.py — Step 2: live MJPEG stream from /dev/video0.

Standalone HTTP server (Python stdlib only), deliberately separate from app.py
so watching the feed can't disturb the dashboard. A background thread captures
frames via imx708_stream (default: fast8 unpack + cv2 + colour) and encodes them
to JPEG; HTTP clients are always served the *latest* frame, so capture runs at
full speed no matter how many browsers are connected (this is also a first taste
of decoupling capture from consumers — the model can tap the same source later).

Endpoints:
    /          HTML page with the live image
    /stream    multipart/x-mixed-replace MJPEG (point an <img> at this)
    /snapshot  a single current JPEG

Run on the Pi (with camera_resmgr already streaming):
    python3 mjpeg_server.py                      # -> http://<pi-ip>:8091/
    python3 mjpeg_server.py --resize 1280x720    # bigger view (slower encode)
    python3 mjpeg_server.py --color gray         # grayscale feed
    python3 mjpeg_server.py --resize none         # full 2304x1296 (slowest)

Default resize is 960x540 — 16:9 to match the sensor's 2304x1296 so the image
isn't stretched. View from your PC's browser at http://<pi-ip>:8091/.
"""

from __future__ import annotations

import argparse
import logging
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

from imx708_stream import (
    DEFAULT_DEVICE,
    DEFAULT_HEIGHT,
    DEFAULT_WIDTH,
    Imx708Stream,
    StreamConfig,
    _BAYER_CELL,
    encode_jpeg_bytes,
)

log = logging.getLogger("mjpeg_server")

INDEX_HTML = b"""<!doctype html><html><head><meta charset="utf-8">
<title>IMX708 live</title><style>
body{background:#111;color:#ddd;font-family:sans-serif;text-align:center;margin:0}
.bar{padding:8px;font-size:14px;letter-spacing:.5px}
img{max-width:100%;height:auto;border-top:1px solid #333}
</style></head><body>
<div class="bar">IMX708 &middot; /dev/video0 &middot; live MJPEG</div>
<img src="/stream" alt="connecting to stream...">
</body></html>"""


class FrameHub:
    """Captures frames in a background thread; serves the latest to all clients."""

    def __init__(self, cfg: StreamConfig, quality: int) -> None:
        self.cfg = cfg
        self.quality = quality
        self._cond = threading.Condition()
        self._jpeg: Optional[bytes] = None
        self._seq = 0
        self._stopped = False
        self._fps = 0.0
        self._thread = threading.Thread(target=self._run, name="capture", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def _run(self) -> None:
        frames, t0 = 0, time.monotonic()
        try:
            with Imx708Stream(self.cfg) as cam:
                log.info("capture started: %s", self.cfg)
                for img in cam.frames():
                    jpeg = encode_jpeg_bytes(img, self.quality)
                    with self._cond:
                        self._jpeg = jpeg
                        self._seq += 1
                        self._cond.notify_all()
                    frames += 1
                    if frames % 30 == 0:
                        now = time.monotonic()
                        self._fps = 30.0 / (now - t0)
                        t0 = now
                        log.info("capture %.1f fps  (%d KB/frame)",
                                 self._fps, len(jpeg) // 1024)
        except Exception as e:  # device gone, encoder missing, etc.
            log.error("capture thread stopped: %s", e)
        finally:
            with self._cond:
                self._stopped = True
                self._cond.notify_all()

    def wait_next(self, last_seq: int, timeout: float = 5.0):
        """Block until a frame newer than last_seq, or stop. Returns (jpeg, seq)."""
        with self._cond:
            if not self._stopped and self._seq == last_seq:
                self._cond.wait(timeout)
            if self._stopped:
                return None, last_seq
            return self._jpeg, self._seq

    def latest(self) -> Optional[bytes]:
        with self._cond:
            return self._jpeg


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args) -> None:  # keep the console for capture stats
        pass

    def do_GET(self) -> None:
        if self.path in ("/", "/index.html"):
            self._send_bytes(INDEX_HTML, "text/html; charset=utf-8")
        elif self.path == "/snapshot":
            jpeg = self.server.hub.latest()  # type: ignore[attr-defined]
            if jpeg is None:
                self.send_error(503, "no frame yet")
            else:
                self._send_bytes(jpeg, "image/jpeg")
        elif self.path == "/stream":
            self._stream()
        else:
            self.send_error(404)

    def _send_bytes(self, data: bytes, ctype: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _stream(self) -> None:
        self.send_response(200)
        self.send_header("Cache-Control", "no-cache, private")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Type",
                         "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        seq = -1
        try:
            while True:
                jpeg, seq = self.server.hub.wait_next(seq)  # type: ignore[attr-defined]
                if jpeg is None:
                    break
                self.wfile.write(b"--frame\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(jpeg))
                self.wfile.write(jpeg)
                self.wfile.write(b"\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass  # client closed the tab — normal


def _parse_resize(s: str) -> Optional[tuple[int, int]]:
    if not s or s.lower() in ("none", "full", "0"):
        return None
    w, _, h = s.lower().partition("x")
    return (int(w), int(h))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8091)
    p.add_argument("--device", default=DEFAULT_DEVICE)
    p.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    p.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    p.add_argument("--bayer", choices=list(_BAYER_CELL), default="rggb")
    p.add_argument("--color", choices=["rgb", "gray"], default="rgb")
    p.add_argument("--debayer", choices=["auto", "cv2", "bin"], default="auto")
    p.add_argument("--resize", default="960x540",
                   help="WxH (16:9 keeps aspect); 'none' for full res (default %(default)s)")
    p.add_argument("--quality", type=int, default=80, help="JPEG quality 1-100")
    return p


def main() -> int:
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s",
                        datefmt="%H:%M:%S")
    args = build_parser().parse_args()
    cfg = StreamConfig(
        device=args.device, width=args.width, height=args.height,
        bayer=args.bayer, color=args.color,
        unpack="fast8", debayer=args.debayer,
        resize=_parse_resize(args.resize),
    )
    hub = FrameHub(cfg, args.quality)
    hub.start()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.hub = hub  # type: ignore[attr-defined]
    log.info("serving on http://%s:%d/  — open / in a browser (Ctrl-C to stop)",
             args.host, args.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("shutting down")
    finally:
        server.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
