#!/usr/bin/env python3
"""
raw_viewer.py — display NV12/RGB frames from nvunixfdsrc with FPS overlay.

The publisher must be started with --raw-stream <socket-path>.
Requires OpenCV built with GStreamer support (default on Jetson/JetPack).

nvunixfdsink always transmits NVMM (CUDA device) memory regardless of platform,
so nvvidconv is always needed to download to system memory before videoconvert.

Usage:
  python3 raw_viewer.py --socket /tmp/cam0_raw.sock
"""

import argparse
import time
import cv2


def build_pipeline(socket: str) -> str:
    src  = f"nvunixfdsrc socket-path={socket}"
    sink = "appsink sync=false drop=true"
    # nvvidconv: NVMM NV12 → system-memory NV12; videoconvert: NV12 → BGR
    return f"{src} ! nvvideoconvert ! video/x-raw,format=NV12 ! videoconvert ! video/x-raw,format=BGR ! {sink}"


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Display raw frames from nvunixfdsrc with FPS overlay"
    )
    ap.add_argument("--socket", required=True, metavar="PATH",
                    help="Unix socket matching --raw-stream on the publisher")
    ap.add_argument("--title", default="Raw Viewer")
    args = ap.parse_args()

    pipe = build_pipeline(args.socket)
    print(f"[viewer] {pipe}")

    cap = cv2.VideoCapture(pipe, cv2.CAP_GSTREAMER)
    if not cap.isOpened():
        print("[viewer] ERROR: could not open pipeline — check socket path and GStreamer plugins")
        return

    fps_count = 0
    fps_t0 = time.monotonic()
    fps = 0.0

    print(f"[viewer] Running — press 'q' to quit")
    while True:
        ret, frame = cap.read()
        if not ret:
            print("[viewer] No frame — pipeline ended or socket closed")
            break

        fps_count += 1
        now = time.monotonic()
        elapsed = now - fps_t0
        if elapsed >= 1.0:
            fps = fps_count / elapsed
            fps_count = 0
            fps_t0 = now

        cv2.putText(frame, f"FPS: {fps:.1f}", (10, 34),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 0), 2, cv2.LINE_AA)
        cv2.imshow(args.title, frame)

        if cv2.waitKey(1) & 0xFF == ord("q"):
            break

    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
