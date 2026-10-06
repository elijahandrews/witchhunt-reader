#!/usr/bin/env python3
"""Build the production renderer and save its framebuffer as a PNG."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib


def png_chunk(kind, data):
    return (
        struct.pack(">I", len(data))
        + kind
        + data
        + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("--out", type=Path, default=Path("render-smoke-output"))
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    source = Path(__file__).parent.resolve()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)

    # Adapt only the device cache-budget assertion for pointers on a 64-bit host.
    # Copy the .cpp byte-for-byte to retain its adjacent quoted header include.
    for name in ("GfxRenderer.h", "GfxRenderer.cpp"):
        data = (repo / "lib/GfxRenderer" / name).read_bytes()
        if name.endswith(".h"):
            assertion = b"sizeof(ScaledGlyphEntry) <= 16"
            if data.count(assertion) != 1:
                raise RuntimeError("Review the renderer's host size-assertion adaptation")
            data = data.replace(assertion, b"sizeof(void*) > 4 || " + assertion)
        (output / name).write_bytes(data)

    include_dirs = [output, source, repo / "test/shims"] + [
        repo / "lib" / name
        for name in (
            "GfxRenderer", "EpdFont", "Memory", "Logging", "Utf8",
            "InflateReader", "uzlib/src",
        )
    ]
    c_command = [
        "clang", "-c", str(repo / "lib/uzlib/src/tinflate.c"),
        "-o", str(output / "tinflate.o"),
    ]
    files = [source / "main.cpp", source / "support.cpp", output / "GfxRenderer.cpp"]
    files += [
        repo / "lib" / name
        for name in (
            "GfxRenderer/TextTruncation.cpp", "EpdFont/EpdFont.cpp",
            "EpdFont/EpdFontFamily.cpp", "EpdFont/FontDecompressor.cpp",
            "EpdFont/GlyphFallback.cpp", "Utf8/Utf8.cpp",
            "InflateReader/InflateReader.cpp",
        )
    ]
    files.append(output / "tinflate.o")
    command = ["clang++", "-std=c++20", "-O1", "-ffunction-sections", "-fdata-sections"]
    command.append("-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections")
    command += ["-I" + str(path) for path in include_dirs]
    command += [str(path) for path in files] + ["-o", str(output / "real_render")]
    (output / "build-command.json").write_text(json.dumps([c_command, command], indent=2) + "\n")
    subprocess.run(c_command, check=True)
    subprocess.run(command, check=True)
    subprocess.run([str(output / "real_render")], cwd=output, check=True)

    # Standard-library PNG encoding preserves every production framebuffer pixel.
    pgm = (output / "frame.pgm").read_bytes()
    header = b"P5\n480 800\n255\n"
    if not pgm.startswith(header) or len(pgm) != len(header) + 480 * 800:
        raise RuntimeError("Unexpected framebuffer dimensions or PGM encoding")
    pixels = pgm[len(header):]
    scanlines = b"".join(b"\0" + pixels[y * 480:(y + 1) * 480] for y in range(800))
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", 480, 800, 8, 0, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(scanlines)) + png_chunk(b"IEND", b"")
    (output / "frame.png").write_bytes(png)
    print("Screenshot:", output / "frame.png")


if __name__ == "__main__":
    main()
