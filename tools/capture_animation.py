#!/usr/bin/env python3
"""Capture a Tokenmeter animation from its USB framebuffer protocol."""

from __future__ import annotations

import argparse
import subprocess
import time
from pathlib import Path

import serial


def read_line(port: serial.Serial, deadline: float) -> str:
    while time.monotonic() < deadline:
        line = port.readline()
        if line:
            return line.decode(errors="replace").strip()
    raise TimeoutError("timed out waiting for device response")


def read_exact(port: serial.Serial, size: int, deadline: float) -> bytes:
    data = bytearray()
    while len(data) < size and time.monotonic() < deadline:
        chunk = port.read(size - len(data))
        if chunk:
            data.extend(chunk)
    if len(data) != size:
        raise TimeoutError(f"received {len(data)} of {size} framebuffer bytes")
    return bytes(data)


def rgb565_to_rgb(raw: bytes, width: int, height: int) -> bytes:
    rgb = bytearray(width * height * 3)
    for pixel_index in range(width * height):
        offset = pixel_index * 2
        pixel = raw[offset] | (raw[offset + 1] << 8)
        red = (pixel >> 11) & 0x1F
        green = (pixel >> 5) & 0x3F
        blue = pixel & 0x1F
        out = pixel_index * 3
        rgb[out] = (red << 3) | (red >> 2)
        rgb[out + 1] = (green << 2) | (green >> 4)
        rgb[out + 2] = (blue << 3) | (blue >> 2)
    return bytes(rgb)


def capture_frame(port: serial.Serial) -> tuple[bytes, int, int, float]:
    started = time.monotonic()
    port.write(b"anim keepalive\n")
    port.flush()
    while read_line(port, started + 5) != "ANIM_KEEPALIVE_OK":
        pass
    port.write(b"screenshot\n")
    port.flush()
    deadline = started + 30
    while True:
        line = read_line(port, deadline)
        if line.startswith("SCREENSHOT_START "):
            _, width, height, size = line.split()
            break
        if line in {"SCREENSHOT_ERR", "SCREENSHOT_UNSUPPORTED"}:
            raise RuntimeError(line)

    raw = read_exact(port, int(size), deadline)
    while read_line(port, deadline) != "SCREENSHOT_END":
        pass
    width_i = int(width)
    height_i = int(height)
    return (
        rgb565_to_rgb(raw, width_i, height_i),
        width_i,
        height_i,
        time.monotonic() - started,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument(
        "--screen", required=True, choices=("codex-splash", "cursor-splash")
    )
    parser.add_argument("--frames", type=int, default=24)
    parser.add_argument("--fps", type=int, default=10)
    parser.add_argument(
        "--advance",
        type=int,
        default=0,
        help="Advance the active service animation this many times before capture",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    frame_dir = args.output.parent / f".{args.output.stem}-frames"
    frame_dir.mkdir(parents=True, exist_ok=True)
    for stale_frame in frame_dir.glob("frame-*.ppm"):
        stale_frame.unlink()

    timings: list[float] = []
    with serial.Serial(args.port, 115200, timeout=0.25, write_timeout=2) as port:
        port.reset_input_buffer()
        port.write(f"screen {args.screen}\n".encode())
        port.flush()
        deadline = time.monotonic() + 5
        while read_line(port, deadline) != f"SCREEN_SELECTED {args.screen}":
            pass
        for _ in range(args.advance):
            port.write(b"anim next\n")
            port.flush()
            deadline = time.monotonic() + 5
            while read_line(port, deadline) != "ANIM_NEXT_OK":
                pass

        for index in range(args.frames):
            for attempt in range(3):
                try:
                    rgb, width, height, elapsed = capture_frame(port)
                    break
                except TimeoutError:
                    if attempt == 2:
                        raise
                    port.reset_input_buffer()
                    time.sleep(0.5)
            frame_path = frame_dir / f"frame-{index:03d}.ppm"
            frame_path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)
            timings.append(elapsed)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-loglevel",
            "error",
            "-framerate",
            str(args.fps),
            "-i",
            str(frame_dir / "frame-%03d.ppm"),
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            "-movflags",
            "+faststart",
            str(args.output),
        ],
        check=True,
    )
    average = sum(timings) / len(timings)
    print(
        f"Captured {len(timings)} frames to {args.output} "
        f"(average USB capture {average:.3f}s/frame)"
    )


if __name__ == "__main__":
    main()
