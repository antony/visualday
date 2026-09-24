#!/usr/bin/env python3
"""Reset the M5Stack Paper Color and capture its serial log for N seconds.

Usage: /Users/ant/.platformio/penv/bin/python tools/serial_capture.py [seconds] [--no-reset] [--send=AT:CHARS ...]

--send=AT:CHARS writes CHARS to the device AT seconds in (n = new article,
k = prev page, j = next page). Repeatable. Note that on macOS merely opening the
port resets the chip, so do everything you need within one run.
"""
import sys
import time

import serial

PORT = "/dev/cu.usbmodem101"
BAUD = 115200


def main() -> None:
    seconds = 60
    reset = True
    sends = []  # (at_seconds, bytes)
    for arg in sys.argv[1:]:
        if arg == "--no-reset":
            reset = False
        elif arg.startswith("--send="):
            at, _, chars = arg[len("--send="):].partition(":")
            sends.append((float(at), chars.encode()))
        else:
            seconds = int(arg)
    sends.sort()

    # The S3's USB-Serial-JTAG watches DTR/RTS transitions: an RTS pulse with DTR
    # low resets into normal boot, while DTR high at reset release means download
    # mode. Open with both lines low so opening the port never toggles anything,
    # then pulse RTS only when a reset is wanted.
    s = serial.Serial()
    s.port = PORT
    s.baudrate = BAUD
    s.timeout = 1
    s.dtr = False
    s.rts = False
    s.open()
    if reset:
        s.rts = True
        time.sleep(0.1)
        s.rts = False

    start = time.time()
    end = start + seconds
    while time.time() < end:
        if sends and time.time() - start >= sends[0][0]:
            s.write(sends[0][1])
            sends.pop(0)
        chunk = s.read(4096)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    s.close()


if __name__ == "__main__":
    main()
