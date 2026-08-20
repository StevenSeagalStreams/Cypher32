#!/usr/bin/env python3
"""Verify the QR encoder two ways.

1. STRUCTURE — for every mask 0-7, our matrix must be byte-identical to
   python-qrcode's for the same payload. That checks the bitstream, the
   Reed-Solomon, the fixed patterns, the data zigzag and the format bits,
   exhaustively rather than for whichever mask happened to be chosen.

2. SCANNABILITY — the matrix we actually emit is rendered to an image and
   decoded with OpenCV. This is the only check that answers the real question.
   A QR that renders but does not scan looks completely fine on the device.

Mask *selection* is deliberately not compared. Any of the eight is a valid,
scannable code; the penalty score is a heuristic for choosing among them, and
python-qrcode's differs from the specification in places. Correctness is
established by (1) and (2), not by agreeing on a preference.
"""
import subprocess, sys, pathlib

here = pathlib.Path(__file__).parent
BIN, BIN_MASK = here / "test_qr", here / "test_qr_mask"

CASES = [
    "WIFI:T:nopass;S:Cypher32;;",
    "WIFI:T:nopass;S:C32_B_HexByte;;",
    "WIFI:T:nopass;S:C32_G_SpecterGrid;;",
    "WIFI:T:nopass;S:C32_W_A;;",
    "HELLO",
    "A" * 17,      # exactly fills version 1
    "B" * 32,      # exactly fills version 2
    "C" * 53,      # exactly fills version 3
]

def run(binary, args):
    out = subprocess.run([str(binary)] + args, capture_output=True, text=True,
                         check=True).stdout.splitlines()
    grids, i = {}, 0
    while i < len(out):
        if not out[i].startswith("QR "):
            i += 1; continue
        _, text, size = out[i].split(" ", 2)
        size = int(size)
        grids[text] = out[i + 1:i + 1 + size]
        i += 1 + size
    return grids

failures = 0

# ── 1. structure, every mask ──
try:
    import qrcode
    from qrcode.constants import ERROR_CORRECT_L
    from qrcode.util import QRData, MODE_8BIT_BYTE
except ImportError:
    print("python 'qrcode' not installed — skipping structural comparison")
else:
    checked = 0
    for mask in range(8):
        grids = run(BIN_MASK, [str(mask)] + CASES)
        for text in CASES:
            ours = grids[text]
            q = qrcode.QRCode(error_correction=ERROR_CORRECT_L, box_size=1,
                              border=0, mask_pattern=mask)
            # Byte mode explicitly: python-qrcode would otherwise pick the most
            # compact mode ("HELLO" becomes alphanumeric) and we only speak byte,
            # which is what a Wi-Fi join string needs anyway.
            q.add_data(QRData(text.encode(), mode=MODE_8BIT_BYTE)); q.make(fit=True)
            ref = ["".join("#" if c else "." for c in row) for row in q.get_matrix()]
            checked += 1
            if ref != ours:
                bad = sum(a != b for ra, rb in zip(ref, ours) for a, b in zip(ra, rb))
                print(f"FAIL structure: mask {mask}, {text!r}: {bad} modules differ")
                failures += 1
    print(f"QR structure: {checked} matrices identical to python-qrcode "
          f"({len(CASES)} payloads x 8 masks)")

# ── 2. does it actually scan ──
try:
    import cv2, numpy as np
except ImportError:
    print("opencv not installed — skipping scan test")
else:
    grids, scanned = run(BIN, CASES), 0
    for text in CASES:
        g = grids[text]
        n, quiet, scale = len(g), 4, 8
        side = (n + quiet * 2) * scale
        img = np.full((side, side), 255, dtype=np.uint8)
        for r in range(n):
            for c in range(n):
                if g[r][c] == "#":
                    y, x = (r + quiet) * scale, (c + quiet) * scale
                    img[y:y + scale, x:x + scale] = 0
        got, _, _ = cv2.QRCodeDetector().detectAndDecode(img)
        if got != text:
            print(f"FAIL scan: {text!r} decoded as {got!r}")
            failures += 1
        else:
            scanned += 1
    print(f"QR scan: {scanned}/{len(CASES)} payloads decoded correctly by OpenCV")

sys.exit(1 if failures else 0)
