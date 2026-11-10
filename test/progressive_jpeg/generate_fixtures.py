#!/usr/bin/env python3
"""Original synthetic JPEGs and libjpeg luma references, generated only in a build directory."""
import random
import sys
from pathlib import Path
from PIL import Image, ImageDraw

output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)

def picture(width, height, mode="RGB"):
    rng = random.Random(7)
    image = Image.new("RGB", (width, height), (250, 248, 240))
    draw = ImageDraw.Draw(image)
    for x in range(width):
        draw.line([(x, height * 2 // 3), (x, height - 1)], fill=(x * 255 // width, 90, 255 - x * 255 // width))
    for _ in range(12):
        x, y = rng.randrange(width), rng.randrange(height * 2 // 3)
        draw.rectangle([x, y, x + rng.randrange(8, 40), y + rng.randrange(4, 24)],
                       outline=(0, 0, 0), fill=tuple(rng.randrange(256) for _ in range(3)))
    for x in range(3, width, 7):
        draw.line([(x, 0), (x, height // 4)], fill=(0, 0, 0))
    pixels = image.load()
    for _ in range(width * height // 20):
        pixels[rng.randrange(width), rng.randrange(height)] = tuple(rng.randrange(256) for _ in range(3))
    return image.convert(mode)

for name, width, height, mode, subsampling, restart in [
    ("prog_full_420", 203, 141, "RGB", 2, 0),
    ("prog_full_gray", 157, 99, "L", 0, 0),
    ("prog_full_444_rst", 150, 90, "RGB", 0, 3),
]:
    source = picture(width, height, mode)
    source.save(output / f"{name}.jpg", "JPEG", quality=90, subsampling=subsampling,
                progressive=True, optimize=True, restart_marker_blocks=restart)
    reference = Image.open(output / f"{name}.jpg")
    if reference.mode != "L":
        reference.draft("YCbCr", reference.size)
        reference = reference.getchannel(0)
    reference.save(output / f"{name}.y.pgm")
picture(96, 64).save(output / "contrast_420.jpg", "JPEG", quality=90, subsampling=2, progressive=False)
# Fine bars establish that the native converter uses AC detail, not a DC preview.
lines = Image.new("L", (96, 64), 255)
draw = ImageDraw.Draw(lines)
for x in range(3, 96, 7):
    draw.line([(x, 0), (x, 63)], fill=0)
lines.save(output / "lines.jpg", "JPEG", quality=95, progressive=True)
reference = Image.open(output / "lines.jpg")
reference.save(output / "lines.y.pgm")
