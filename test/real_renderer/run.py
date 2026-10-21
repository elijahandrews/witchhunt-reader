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


def write_png(path, width, height, pixels):
    scanlines = b"".join(b"\0" + pixels[y * width:(y + 1) * width] for y in range(height))
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(scanlines)) + png_chunk(b"IEND", b"")
    path.write_bytes(png)


def read_frame(path):
    pgm = path.read_bytes()
    header = b"P5\n480 800\n255\n"
    if not pgm.startswith(header) or len(pgm) != len(header) + 480 * 800:
        raise RuntimeError("Unexpected framebuffer dimensions or PGM encoding: " + str(path))
    return pgm[len(header):]


def export_images(output, failed=False):
    """Keep original screen pixels and side-by-side, nearest-neighbor comparisons."""
    for path in sorted(output.glob("*.pgm")):
        try:
            pixels = read_frame(path)
        except RuntimeError:
            if not failed:
                raise
            print("Skipping incomplete framebuffer from failed run:", path, file=sys.stderr)
            continue
        write_png(path.with_suffix(".png"), 480, 800, pixels)
        print("Screenshot:", path.with_suffix(".png"))
    modes = ("legacy", "oversample", "final")
    for phases in (4, 8):
        paths = [output / f"outline-quality-{mode}-p{points}-q{phases}.pgm"
                 for points in (8, 10, 12) for mode in modes]
        if not all(path.exists() for path in paths):
            continue
        frames = [read_frame(path) for path in paths]
        width, height = 1440, 1500
        pixels = bytearray([255]) * (width * height)
        for i, frame in enumerate(frames):
            for y in range(500):
                at = (i // 3 * 500 + y) * width + (i % 3) * 480
                pixels[at:at + 480] = frame[y * 480:(y + 1) * 480]
        write_png(output / f"outline-comparison-q{phases}.png", width, height, pixels)
        # 8pt detail, 2x nearest neighbor: no smoothing that might hide a pixel defect.
        crop_x, crop_y, crop_w, crop_h, zoom = 8, 35, 392, 280, 2
        width, height = 3 * crop_w * zoom, crop_h * zoom
        pixels = bytearray([255]) * (width * height)
        for col, frame in enumerate(frames[:3]):
            for y in range(crop_h):
                row = b"".join(bytes([value]) * zoom for value in frame[(crop_y + y) * 480 + crop_x:
                                                                       (crop_y + y) * 480 + crop_x + crop_w])
                for dy in range(zoom):
                    at = (y * zoom + dy) * width + col * crop_w * zoom
                    pixels[at:at + len(row)] = row
        write_png(output / f"outline-detail-p8-q{phases}.png", width, height, pixels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("--out", type=Path, default=Path("render-smoke-output"))
    parser.add_argument("--cpfont", type=Path, help="Local four-style v4/v5/v6 font; never copied into the repository")
    parser.add_argument("--outline", type=Path, help="Local TTF/OTF rendered by the actual embedded FreeType backend")
    parser.add_argument("--native-fonts", nargs=4, type=Path, metavar=("REGULAR", "BOLD", "ITALIC", "BOLD_ITALIC"),
                        help="Four local TTF files tested through the actual flash-store/native outline adapter")
    parser.add_argument("--sanitize", action="store_true", help="Run with AddressSanitizer and UndefinedBehaviorSanitizer")
    args = parser.parse_args()
    sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
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

    include_dirs = [output, source, repo / "test/zip_entry_reader", repo / "test/shims"] + [
        repo / "lib" / name
        for name in (
            "GfxRenderer", "EpdFont", "OutlineFont", "Memory", "Logging", "Utf8",
            "InflateReader", "uzlib/src",
        )
    ]
    c_command = [
        "clang", *sanitizers, "-c", str(repo / "lib/uzlib/src/tinflate.c"),
        "-o", str(output / "tinflate.o"),
    ]
    files = [source / "main.cpp", source / "support.cpp", source / "downscale.cpp", source / "overlap.cpp", source / "fractional.cpp", source / "stem_weight.cpp", output / "GfxRenderer.cpp"]
    files += [
        repo / "lib" / name
        for name in (
            "GfxRenderer/TextTruncation.cpp", "EpdFont/EpdFont.cpp",
            "EpdFont/EpdFontFamily.cpp", "EpdFont/SdCardFont.cpp", "EpdFont/FontDecompressor.cpp",
            "EpdFont/GlyphFallback.cpp", "Utf8/Utf8.cpp",
            "InflateReader/InflateReader.cpp",
        )
    ]
    files.append(repo / "test/zip_entry_reader/LoggingStub.cpp")
    files.append(output / "tinflate.o")
    command = ["clang++", *sanitizers, "-std=c++20", "-O1", "-ffunction-sections", "-fdata-sections"]
    command.append("-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections")
    if args.outline or args.native_fonts:
        # Pixel/sanitizer tests on a large host stack cannot catch RTOS overflows.
        # Use a separate optimized build; sanitizer instrumentation changes frames.
        subprocess.run([sys.executable, str(source / "stack_budget.py"), "--repo-root", str(repo),
                        "--out", str(output / "stack-budget"), "--static-only"], check=True)
        ft = repo / "lib/FreeType"
        ftflags = [*sanitizers, "-O1", "-DWITCH_FONT_AUTOHINT=1", "-I" + str(ft / "config"),
                   "-I" + str(ft / "third_party/freetype/include")]
        backendflags = ftflags + ["-I" + str(repo / "lib/EpdFont"), "-I" + str(repo / "lib/OutlineFont")]
        for src in sorted((ft / "src").glob("*.c")):
            obj = output / (src.stem + ".o")
            subprocess.run(["clang", *ftflags, "-c", str(src), "-o", str(obj)], check=True)
            files.append(obj)
        for src in sorted((repo / "lib/OutlineFont").glob("*.cpp")):
            obj = output / ("outline_" + src.stem + ".o")
            subprocess.run(["clang++", "-std=c++20", *backendflags, "-c", str(src), "-o", str(obj)], check=True)
            files.append(obj)
        files += [source / "outline.cpp", source / "outline_quality.cpp"]
        if args.native_fonts:
            native_includes = [repo / "lib/hal", repo / "lib/EpdFont/ttf", repo / "test/glyph_store",
                               repo / "test/ttf_font", repo / "freeink-sdk/libs/font/FreeInkOpenType/include"]
            include_dirs[4:4] = native_includes
            files += [source / "native_outline.cpp"]
            files += [repo / "lib/EpdFont/ttf" / (name + ".cpp") for name in
                      ("TtfFamily", "TtfFont", "TtfOpenType", "FlashGlyphStore", "GlyphStoreFormat", "TtfFace", "TtfRasterizer")]
            files += [repo / "freeink-sdk/libs/font/FreeInkFont/src" / (name + ".cpp") for name in
                      ("Gpos", "Gsub", "KernTable")]
            gray_obj = output / "native_ftgrays.o"
            subprocess.run(["clang", *sanitizers, "-O1", "-c", str(repo / "lib/EpdFont/ttf/third_party/freetype/ftgrays.c"),
                            "-o", str(gray_obj)], check=True)
            files.append(gray_obj)
            command.append("-DWITCH_TEST_NATIVE_OUTLINE=1")
        command.append("-DWITCH_TEST_OUTLINE=1")
    command += ["-I" + str(path) for path in include_dirs]
    command += [str(path) for path in files] + ["-o", str(output / "real_render")]
    (output / "build-command.json").write_text(json.dumps([c_command, command], indent=2) + "\n")
    subprocess.run(c_command, check=True)
    subprocess.run(command, check=True)
    run_args = [str(args.cpfont.resolve()) if args.cpfont else "-"] if (args.cpfont or args.outline or args.native_fonts) else []
    if args.outline or args.native_fonts: run_args.append(str(args.outline.resolve()) if args.outline else "-")
    if args.native_fonts: run_args.extend(str(path.resolve()) for path in args.native_fonts)
    render = subprocess.run(
        [str(output / "real_render")] + run_args,
        cwd=output, check=False,
    )

    export_images(output, failed=render.returncode != 0)
    render.check_returncode()
    import driver_audit
    driver_audit.run(repo, output / "driver-audit", sanitize=args.sanitize)


if __name__ == "__main__":
    main()
