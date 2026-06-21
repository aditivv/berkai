"""
Standalone camera diagnostic — bypasses Flask entirely.

Run directly on the Pi: python3 camera_test.py

Tells you whether qnx_apis is actually pulling real sensor data (varying
pixel values) vs. a hardware/driver-level black frame (all zeros) vs. a
capture that fails to open at all.
"""

import sys
import time

try:
    import qnx_apis
    backend = qnx_apis
    backend_name = "qnx_apis"
except ImportError as e:
    print(f"[FAIL] qnx_apis not importable: {e}")
    print("Falling back to cv2 for this test (won't see the Pi camera under QNX).")
    import cv2
    backend = cv2
    backend_name = "cv2"

print(f"Using backend: {backend_name}")

cam = backend.VideoCapture(0)

if not cam.isOpened():
    print("[FAIL] cam.isOpened() == False — camera did not open at all.")
    print("This points to a hardware/driver issue, not the Flask app.")
    print("Check: ribbon cable seating/orientation, sensor service process running,")
    print("and system logs (slog2info on QNX) for camera driver init errors.")
    sys.exit(1)

print("[OK] cam.isOpened() == True. Grabbing 5 frames...")

import numpy as np

for i in range(5):
    ok, frame = cam.read()
    if not ok:
        print(f"  frame {i}: [FAIL] read() returned ok=False")
        continue
    arr = np.asarray(frame)
    print(f"  frame {i}: shape={arr.shape} dtype={arr.dtype} "
          f"min={arr.min()} max={arr.max()} mean={arr.mean():.2f}")
    time.sleep(0.3)

# Save the last frame so it can be pulled off the Pi and viewed.
try:
    import cv2 as _cv2
    _cv2.imwrite("camera_test_frame.jpg", frame)
    print("Saved last frame to camera_test_frame.jpg — scp it off and look at it.")
except Exception as e:
    print(f"Could not save frame with cv2.imwrite: {e}")

print()
print("Interpretation:")
print("  - min/max/mean all 0          -> camera opens but sends no real signal")
print("                                    (cable, sensor not actually streaming,")
print("                                    or Sensor Framework returning a stub buffer)")
print("  - min/max/mean vary, nonzero  -> real data is coming through; the bug is")
print("                                    downstream in app.py / index.html, not the camera")
