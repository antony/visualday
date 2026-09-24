#!/usr/bin/env python3
"""Grab what the Paper Color is drawing, as a PNG, over USB serial.

Usage: ~/.platformio/penv/bin/python tools/screenshot.py OUT.png [COMMAND] [--reset] [--wait=SECONDS]

COMMAND is sent to the device first (default "s", the current screen); several
can be joined with ";" (e.g. "d;t1010"), and the last screen is kept. Useful:
  t1430   the screen as it would look at 14:30 today (no panel refresh)
  d       toggle demo data
--reset restarts the device first and waits (--wait, default 60 s) for it to boot
and draw before sending the command.

Colours are an approximation of the e-paper inks.
"""
import struct
import sys
import time
import zlib

import serial

PORT = "/dev/cu.usbmodem101"
INKS = [(20, 20, 20), (250, 250, 245), (235, 200, 0), (200, 35, 35), (35, 65, 160), (40, 125, 70)]


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    out = args[0] if args else "shot.png"
    cmd = args[1] if len(args) > 1 else "s"
    wait = 60
    for f in flags:
        if f.startswith("--wait="):
            wait = float(f.split("=", 1)[1])

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = PORT, 115200, 1
    s.dtr = s.rts = False   # opening must not toggle the reset lines
    s.open()
    if "--reset" in flags:
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        end = time.time() + wait
        while time.time() < end:
            line = s.readline().decode("utf-8", "replace")
            if line:
                sys.stdout.write(line)
            if "[schedule]" in line:
                break

    s.reset_input_buffer()
    for c in cmd.split(";"):
        w, h, rows = grab(s, c)
    s.close()
    if not w:
        sys.exit("no screenshot received")
    blank = [INKS[1]] * w
    write_png(out, w, h, [(rows.get(y, blank) + blank)[:w] for y in range(h)])
    print(f"wrote {out} ({w}x{h}, {len(rows)} rows)")


def grab(s, cmd):
    s.write((cmd + "\n").encode())
    w = h = 0
    rows = {}
    deadline = time.time() + 90
    while time.time() < deadline:
        line = s.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if line.startswith("SHOT END"):
            break
        if line.startswith("SHOT "):
            w, h = map(int, line.split()[1:3])
        elif line.startswith("R") and w:
            head, _, runs = line.partition(" ")
            px = []
            for run in filter(None, runs.split(",")):
                px += [INKS[int(run[0])]] * int(run[1:])
            rows[int(head[1:])] = px
        else:
            print(line)
    return w, h, rows


if __name__ == "__main__":
    main()
