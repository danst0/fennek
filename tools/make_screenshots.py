#!/usr/bin/env python3
# make_screenshots.py — wandelt die vom Host-Renderer erzeugten PGMs in
# hochskalierte PNGs mit dezentem Rahmen um (für die README).
#
#   python3 tools/make_screenshots.py <pgm-dir> <png-dir> [scale]
import sys
from pathlib import Path
from PIL import Image, ImageOps

SCALE_DEFAULT = 2
BORDER = 6          # heller Rahmen ums "Display"
BORDER_COLOR = (210, 210, 210)
FRAME = (90, 90, 90)  # 1px dunkle Kante

def convert(pgm: Path, png: Path, scale: int):
    img = Image.open(pgm).convert("L")
    img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    img = img.convert("RGB")
    img = ImageOps.expand(img, border=1, fill=FRAME)
    img = ImageOps.expand(img, border=BORDER, fill=BORDER_COLOR)
    img.save(png)
    print(f"{png.name}  ({png.stat().st_size // 1024} KB)")

def main():
    if len(sys.argv) < 3:
        print("usage: make_screenshots.py <pgm-dir> <png-dir> [scale]")
        return 1
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else SCALE_DEFAULT
    dst.mkdir(parents=True, exist_ok=True)
    for pgm in sorted(src.glob("*.pgm")):
        convert(pgm, dst / (pgm.stem + ".png"), scale)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
