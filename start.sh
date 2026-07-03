#!/bin/sh
# start.sh — bring up the pipe-monitor dashboard on the QNX Pi.
#
# The camera driver (camera_resmgr) must run as ROOT — it needs
# PROCMGR_AID_MEM_PHYS — while the app runs fine as qnxuser (/dev/video0 is
# mode 0666). Two supported flows:
#
#   1. Two-step (recommended):
#        su                                # password: root
#        ./camera_driver/camera_resmgr >/tmp/cam.log 2>&1 &
#        exit
#        ./start.sh                        # as qnxuser
#
#   2. One-step as root: ./start.sh will start the driver itself, then run
#      the app (as root — fine for a demo).
#
# If /dev/video0 is missing and you are not root, this script exits with
# instructions. app.py itself also survives a missing camera: /video_feed
# shows a "NO CAMERA SIGNAL" card and recovers once the driver is up.

set -e
cd "$(dirname "$0")"

if [ ! -e /dev/video0 ]; then
    if [ "$(id -u)" = "0" ]; then
        echo "[start] /dev/video0 missing — starting camera_resmgr..."
        ./camera_driver/camera_resmgr >/tmp/cam.log 2>&1 &
        # wait for the driver to publish the node
        for i in 1 2 3 4 5 6 7 8 9 10; do
            [ -e /dev/video0 ] && break
            sleep 1
        done
        if [ ! -e /dev/video0 ]; then
            echo "[start] camera_resmgr did not come up — see /tmp/cam.log" >&2
            exit 1
        fi
        echo "[start] camera driver ready."
    else
        echo "[start] /dev/video0 missing and not running as root." >&2
        echo "        Start the driver first:  su  (password: root)" >&2
        echo "        ./camera_driver/camera_resmgr >/tmp/cam.log 2>&1 &" >&2
        echo "        exit, then re-run ./start.sh" >&2
        exit 1
    fi
fi

echo "[start] launching dashboard on http://$(hostname):5000/"
exec python3 app.py
