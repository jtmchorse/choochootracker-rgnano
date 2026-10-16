"""Bake a faint 'DONG' constellation into snes_sky.bmp (the default title sky).
Usage: python3 -I dong_sky.py <in.bmp> <out.bmp> [preview.png]"""
import sys
from PIL import Image

BG = (0, 0, 97)
DIM, MID, BRIGHT = (0, 58, 132), (0, 90, 165), (66, 156, 181)  # sampled from the sky's own stars
GLYPHS = {
    'D': ["XX.", "X.X", "X.X", "X.X", "XX."],
    'O': [".X.", "X.X", "X.X", "X.X", ".X."],
    'N': ["X..X", "XX.X", "X.XX", "X..X", "X..X"],
    'G': [".XXX", "X...", "X.XX", "X..X", ".XX."],
}
PITCH, GAP, Y0 = 3, 2, 5

def dots():
    cx = 0
    for ch in "DONG":
        rows = GLYPHS[ch]
        for r, row in enumerate(rows):
            for c, v in enumerate(row):
                if v == 'X':
                    yield (cx + c) * PITCH, Y0 + r * PITCH
        cx += len(rows[0]) + GAP

def main():
    im = Image.open(sys.argv[1]).convert('RGB')
    pts = list(dots())
    w = max(x for x, _ in pts) + 1
    # leftmost x where the whole box (+2px margin) is empty sky
    box = lambda x0: [((x0 + x) % im.width, y) for x in range(-2, w + 2)
                      for y in range(Y0 - 2, Y0 + 4 * PITCH + 3)]
    # the stretch with the fewest existing stars; blank those so DONG reads cleanly
    x0 = min(range(260, im.width - w - 2), key=lambda x: sum(im.getpixel(q) != BG for q in box(x)))
    for q in box(x0):
        im.putpixel(q, BG)
    for i, (x, y) in enumerate(pts):
        col = BRIGHT if i in (0, len(pts) - 1) else (MID if i % 5 == 0 else DIM)
        im.putpixel((x0 + x, y), col)
    im.save(sys.argv[2])
    print("placed at x0", x0, "width", w, "stars", len(pts))
    if len(sys.argv) > 3:
        crop = im.crop((x0 - 40, 0, x0 + w + 40, 40))
        crop.resize((crop.width * 8, crop.height * 8), Image.NEAREST).save(sys.argv[3])

main()
