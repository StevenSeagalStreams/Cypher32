#!/usr/bin/env python3
"""Scan the join QR exactly as the panel draws it.

test_qr.py proves the encoder is correct and that an idealised render of it
decodes. This asks the question that actually matters on the bench: at three
pixels per module on a 250x122 e-ink panel, laid out beside two lines of text
by displaySetup(), does a phone camera still get it? The framebuffer here is
the one the panel would be sent, produced by the sketch's own drawing code.
"""
import pathlib, sys

PBM = pathlib.Path(__file__).parent / "out" / "eink-setup-qr.pbm"
WANT = "WIFI:T:nopass;S:C32_B_HexByte;;"

try:
    import cv2, numpy as np
except ImportError:
    print("opencv not installed — skipping panel QR scan")
    sys.exit(0)

if not PBM.exists():
    print(f"{PBM} missing — run render_eink first")
    sys.exit(1)

lines = [l for l in PBM.read_text().split("\n") if l.strip()]
w, h = map(int, lines[1].split())
rows = lines[2:]

SCALE, QUIET = 4, 8
img = np.full(((h + QUIET * 2) * SCALE, (w + QUIET * 2) * SCALE), 255, np.uint8)
for y, row in enumerate(rows):
    for x, c in enumerate(row[:w]):
        if c == "1":
            yy, xx = (y + QUIET) * SCALE, (x + QUIET) * SCALE
            img[yy:yy + SCALE, xx:xx + SCALE] = 0

got, _, _ = cv2.QRCodeDetector().detectAndDecode(img)
if got != WANT:
    print(f"FAIL panel QR: decoded {got!r}, wanted {WANT!r}")
    sys.exit(1)
print(f"QR panel: the join code decodes off the rendered 250x122 panel ({got})")
