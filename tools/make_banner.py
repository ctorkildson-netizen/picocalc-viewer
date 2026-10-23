#!/usr/bin/env python3
"""Draws the viewer's banner (320x44) and writes it as src/banner.h (RGB565).
   Text in DejaVu Sans Bold; the icon is drawn here, shape by shape."""
import sys
from PIL import Image, ImageDraw, ImageFont

W, H = 320, 44
im = Image.new("RGB", (W, H))
d = ImageDraw.Draw(im)
for y in range(H):                      # night-blue gradient
    t = y / (H - 1)
    d.line([(0, y), (W, y)], fill=(int(10 + 20 * t), int(16 + 30 * t), int(48 + 50 * t)))

# icon: a framed picture of hills under a sun
x0, y0, x1, y1 = 8, 6, 52, 38
d.rectangle([x0, y0, x1, y1], fill=(235, 225, 200))          # mat
d.rectangle([x0 + 3, y0 + 3, x1 - 3, y1 - 3], fill=(120, 180, 235))  # sky
d.ellipse([x1 - 17, y0 + 6, x1 - 8, y0 + 15], fill=(255, 210, 60))   # sun
d.polygon([(x0 + 3, y1 - 3), (x0 + 15, y0 + 15), (x0 + 27, y1 - 3)], fill=(60, 140, 70))
d.polygon([(x0 + 16, y1 - 3), (x0 + 30, y0 + 19), (x1 - 3, y1 - 3)], fill=(40, 110, 55))
d.rectangle([x0, y0, x1, y1], outline=(150, 110, 60), width=2)       # frame

big = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 24)
small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
d.text((64, 3), "Picture Viewer", font=big, fill=(255, 255, 255))
d.text((66, 29), "for the PicoCalc", font=small, fill=(150, 190, 255))
im.save(sys.argv[1] if len(sys.argv) > 1 else "banner.png")

with open(sys.argv[2] if len(sys.argv) > 2 else "banner.h", "w") as f:
    f.write("/* banner.h -- made by tools/make_banner.py; don't edit */\n")
    f.write("#pragma once\n#include <stdint.h>\n")
    f.write(f"#define BANNER_W {W}\n#define BANNER_H {H}\n")
    f.write("static const uint16_t banner[BANNER_W * BANNER_H] = {\n")
    px = list(im.getdata())
    for i in range(0, len(px), 12):
        f.write("\t" + ", ".join("0x%04X" % (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
                                for r, g, b in px[i:i + 12]) + ",\n")
    f.write("};\n")
