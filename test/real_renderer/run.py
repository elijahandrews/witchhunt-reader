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
    parser.add_argument("--cpfont", type=Path, help="Local four-style v4/v5/v6 font; never copied into the repository")
    parser.add_argument("--outline", type=Path, help="Local TTF/OTF rendered by the actual embedded FreeType backend")
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
    files = [source / "main.cpp", source / "support.cpp", source / "downscale.cpp", source / "overlap.cpp", output / "GfxRenderer.cpp"]
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
    if args.outline:
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
        files.append(source / "outline.cpp")
        command.append("-DWITCH_TEST_OUTLINE=1")
    command += ["-I" + str(path) for path in include_dirs]
    command += [str(path) for path in files] + ["-o", str(output / "real_render")]
    (output / "build-command.json").write_text(json.dumps([c_command, command], indent=2) + "\n")
    subprocess.run(c_command, check=True)
    subprocess.run(command, check=True)
    run_args = [str(args.cpfont.resolve()) if args.cpfont else "-"] if (args.cpfont or args.outline) else []
    if args.outline: run_args.append(str(args.outline.resolve()))
    render = subprocess.run(
        [str(output / "real_render")] + run_args,
        cwd=output, check=False,
    )

    # Preserve the framebuffer pixels, including diagnostic images from failing runs.
    for pgm_path in sorted(output.glob("*.pgm")):
        pgm = pgm_path.read_bytes()
        header = b"P5\n480 800\n255\n"
        if not pgm.startswith(header) or len(pgm) != len(header) + 480 * 800:
            raise RuntimeError("Unexpected framebuffer dimensions or PGM encoding")
        pixels = pgm[len(header):]
        scanlines = b"".join(b"\0" + pixels[y * 480:(y + 1) * 480] for y in range(800))
        png = b"\x89PNG\r\n\x1a\n"
        png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", 480, 800, 8, 0, 0, 0, 0))
        png += png_chunk(b"IDAT", zlib.compress(scanlines)) + png_chunk(b"IEND", b"")
        png_path = pgm_path.with_suffix(".png")
        png_path.write_bytes(png)
        print("Screenshot:", png_path)
    render.check_returncode()
    import driver_audit
    driver_audit.run(repo, output / "driver-audit", sanitize=args.sanitize)


if __name__ == "__main__":
    main()
