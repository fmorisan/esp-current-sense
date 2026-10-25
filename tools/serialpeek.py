#!/usr/bin/env python3
"""Peek at the board's serial output for a few seconds and exit.

Usage:
    uv run --with pyserial python tools/serialpeek.py \
        --port /dev/cu.usbserial-0001 --seconds 6

(`make monitor` gives the interactive version; this one works non-TTY,
e.g. from scripts/agents.)
"""
import argparse
import sys
import time

import serial


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbserial-0001")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=6.0)
    args = parser.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.5
    # Deassert handshake lines BEFORE open so the auto-reset circuit never
    # sees a transition — opening the port then left the chip running.
    ser.dtr = False
    ser.rts = False
    ser.open()

    deadline = time.time() + args.seconds
    while time.time() < deadline:
        data = ser.read(512)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
    ser.close()


if __name__ == "__main__":
    main()
