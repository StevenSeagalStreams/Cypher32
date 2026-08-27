#!/usr/bin/env python3
"""Turn the raw e-ink framebuffers into images fit for a README.

The panel is 250x122 monochrome, which is a postage stamp on a laptop, so each
is scaled up with nearest-neighbour (no smoothing — the pixel grid IS the
aesthetic) and set in a bezel the colour of the real board's frame.
"""
import sys, pathlib
from PIL import Image

SCALE, PAD, RADIUS = 3, 14, 6
INK, PAPER, BEZEL = (26, 28, 26), (232, 232, 228), (36, 40, 38)


def read_pbm(path):
    tok, data = [], path.read_text().split("\n")
    assert data[0].strip() == "P1", f"{path}: not a P1 pbm"
    w, h = map(int, data[1].split())
    rows = [r for r in data[2:] if r.strip()]
    assert len(rows) == h, f"{path}: {len(rows)} rows, expected {h}"
    return w, h, rows


def render(src, dst):
    w, h, rows = read_pbm(src)
    panel = Image.new("RGB", (w, h), PAPER)
    px = panel.load()
    for y, row in enumerate(rows):
        for x, c in enumerate(row[:w]):
            if c == "1":
                px[x, y] = INK
    panel = panel.resize((w * SCALE, h * SCALE), Image.NEAREST)

    out = Image.new("RGB", (w * SCALE + PAD * 2, h * SCALE + PAD * 2), BEZEL)
    out.paste(panel, (PAD, PAD))
    # Soften the bezel corners so it reads as a device, not a screenshot.
    for cx, cy in ((0, 0), (out.width - 1, 0), (0, out.height - 1),
                   (out.width - 1, out.height - 1)):
        for dy in range(RADIUS):
            for dx in range(RADIUS):
                if (dx - RADIUS) ** 2 + (dy - RADIUS) ** 2 > RADIUS ** 2:
                    out.putpixel((cx + dx * (1 if cx == 0 else -1),
                                  cy + dy * (1 if cy == 0 else -1)), (13, 15, 14))
    out.save(dst, optimize=True)
    return out.size


if __name__ == "__main__":
    src_dir, dst_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    dst_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for pbm in sorted(src_dir.glob("eink-*.pbm")):
        size = render(pbm, dst_dir / (pbm.stem + ".png"))
        n += 1
    print(f"{n} e-ink screens -> {dst_dir}/ at {SCALE}x ({size[0]}x{size[1]})")
