#!/usr/bin/env python3
"""Capture all Tokenmeter screens from the connected device."""

from __future__ import annotations

import argparse
import subprocess
import time
from pathlib import Path

import serial

from capture_animation import capture_frame, read_line


SCREENS = (
    "selector",
    "claude-splash",
    "claude-usage",
    "codex-splash",
    "codex-usage",
    "cursor-splash",
    "cursor-usage",
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    with serial.Serial(args.port, 115200, timeout=0.25, write_timeout=2) as port:
        for screen in SCREENS:
            port.reset_input_buffer()
            port.write(f"screen {screen}\n".encode())
            port.flush()
            deadline = time.monotonic() + 5
            while read_line(port, deadline) != f"SCREEN_SELECTED {screen}":
                pass
            time.sleep(0.25)

            for attempt in range(3):
                try:
                    rgb, width, height, _ = capture_frame(port)
                    break
                except TimeoutError:
                    if attempt == 2:
                        raise
                    port.reset_input_buffer()
                    time.sleep(0.5)
            ppm = args.output_dir / f".{screen}.ppm"
            png = args.output_dir / f"{screen}.png"
            ppm.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)
            subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error", "-i", str(ppm), str(png)],
                check=True,
            )
            ppm.unlink()
            print(png)


if __name__ == "__main__":
    main()
