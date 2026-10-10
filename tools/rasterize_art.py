# Copyright (c) 2026 Martial Systems LLC. All rights reserved.
"""Rasterize the LIFT SVG art into the bitmaps the plug-in embeds.

The art (assets/art-src, from LIFT-art-svg) uses SVG filters (feTurbulence
grain, drop shadows) that JUCE's Drawable cannot render, so it is rasterized
here, ahead of the build, with resvg (MPL-2.0; a build tool only, nothing of it
ships). The app never parses SVG.

    python3 tools/rasterize_art.py [--resvg PATH]

Writes assets/art/case@2x.jpg and case@3x.jpg: the v3.1 case (LIFT-v3.html)
with its recessed black patch panel and the screen well, on the page colour,
drawn on the panel canvas itself (1360 x 1106; the face sits at (16, 76)). The app
picks the smallest that covers the window's device scale and area-averages
it down once per window size.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "art-src", "case-v31.svg")
OUT = os.path.join(ROOT, "assets", "art")
CANVAS_W, CANVAS_H = 1360, 1106
PAGE = "#c2bdb3"


def framed_svg(scale):
    s = open(SRC, encoding="utf-8").read()
    root = re.search(r"<svg[^>]*>", s).group(0)
    return s.replace(root, '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
                     % (CANVAS_W * scale, CANVAS_H * scale, CANVAS_W, CANVAS_H), 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--resvg", default="resvg")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    for scale in (2, 3):
        with tempfile.TemporaryDirectory() as tmp:
            svg = os.path.join(tmp, "case.svg")
            png = os.path.join(tmp, "case.png")
            open(svg, "w", encoding="utf-8").write(framed_svg(scale))
            subprocess.run([a.resvg, svg, png], check=True)
            im = Image.open(png).convert("RGB")
            assert im.size == (CANVAS_W * scale, CANVAS_H * scale), im.size
            dst = os.path.join(OUT, "case@%dx.jpg" % scale)
            im.save(dst, quality=92, subsampling=0, optimize=True)
            print("wrote", dst, im.size, os.path.getsize(dst), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
