"""Trickster Automation Studio app icon -> res/tas.ico (drawn here: no game art).

A rounded tile in the app's accent blue, a gear and "TAS". Sizes 16..256 in one .ico (res/tas.rc, id 1).

  python tools/make_icon.py [--out res/tas.ico]
"""
import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFont

ACCENT = (14, 99, 156, 255)      # ui_theme kAccent (#0E639C)
DARK = (24, 24, 24, 255)
WHITE = (255, 255, 255, 255)


def gear(d, cx, cy, r_out, r_in, teeth, width, color):
    pts = []
    for i in range(teeth * 2):
        a0 = math.pi * 2 * i / (teeth * 2)
        a1 = math.pi * 2 * (i + 1) / (teeth * 2)
        r = r_out if i % 2 == 0 else r_in
        for a in (a0, a1):
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=color)
    d.ellipse([cx - r_in + width, cy - r_in + width, cx + r_in - width, cy + r_in - width], fill=ACCENT)


def draw(size):
    s = 1024
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([24, 24, s - 24, s - 24], radius=180, fill=ACCENT)
    gear(d, s * 0.5, s * 0.40, 300, 245, 12, 70, DARK)
    try:
        font = ImageFont.truetype("arialbd.ttf", 230)
    except OSError:
        font = ImageFont.load_default()
    text = "TAS"
    box = d.textbbox((0, 0), text, font=font)
    d.text(((s - (box[2] - box[0])) / 2 - box[0], s * 0.70 - box[1]), text, font=font, fill=WHITE)
    return img.resize((size, size), Image.LANCZOS)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Trickster Automation Studio app icon")
    ap.add_argument("--out", default=os.path.join(root, "res", "tas.ico"))
    a = ap.parse_args()
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    big = draw(256)
    big.save(a.out, format="ICO", sizes=[(n, n) for n in sizes])
    print("wrote", a.out)


if __name__ == "__main__":
    main()
