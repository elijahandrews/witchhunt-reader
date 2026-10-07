#!/usr/bin/env python3
"""Bridge production-renderer planes through the actual UC8279 X4 Pro driver."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def run(repo, output, sanitize=False):
    sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else []
    repo, output = Path(repo).resolve(), Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).parent.resolve()

    # Keep production behavior unchanged; only adapt the host pointer-size guard.
    for name in ("GfxRenderer.h", "GfxRenderer.cpp"):
        data = (repo / "lib/GfxRenderer" / name).read_bytes()
        if name.endswith(".h"):
            assertion = b"sizeof(ScaledGlyphEntry) <= 16"
            if data.count(assertion) != 1:
                raise RuntimeError("Review host size assertion adaptation")
            data = data.replace(assertion, b"sizeof(void*) > 4 || " + assertion)
        (output / name).write_bytes(data)

    # Preserve relative includes and compile the SDK driver byte-for-byte.
    sdk = repo / "freeink-sdk/libs/display/FreeInkDisplay"
    for name in (
        "src/driver/Uc8279X4Driver.cpp", "src/driver/Uc8279X4Driver.h",
        "src/driver/PanelDriver.h", "src/lut/Uc8279X3Luts.h",
        "include/GrayscaleCapabilities.h",
    ):
        destination = output / "sdk" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(sdk / name, destination)
    (output / "sdk/src/bus").mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "driver_shims/bus/EpdBus.h", output / "sdk/src/bus/EpdBus.h")

    includes = [output, source, repo / "test/zip_entry_reader", repo / "test/shims"] + [
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
    files = [source / "driver_fixtures.cpp", source / "support.cpp", output / "GfxRenderer.cpp"]
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
    renderer = ["clang++", *sanitizers, "-std=c++20", "-O1", "-ffunction-sections", "-fdata-sections"]
    renderer.append("-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections")
    renderer += ["-I" + str(path) for path in includes]
    renderer += [str(path) for path in files] + ["-o", str(output / "driver_fixtures")]
    wire = [
        "clang++", *sanitizers, "-std=c++20", "-O1",
        "-I" + str(source / "driver_shims"), "-I" + str(output / "sdk/src"),
        str(source / "driver_wire.cpp"), str(output / "sdk/src/driver/Uc8279X4Driver.cpp"),
        "-o", str(output / "driver_wire"),
    ]
    (output / "driver-build-commands.json").write_text(
        json.dumps([c_command, renderer, wire], indent=2) + "\n")
    for command in (c_command, renderer, wire):
        subprocess.run(command, check=True)
    for stale in output.glob("driver-fixture-*.bin"):
        stale.unlink()
    subprocess.run([str(output / "driver_fixtures")], cwd=output, check=True)
    subprocess.run([str(output / "driver_wire"), str(output)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("--out", type=Path, default=Path("driver-audit-output"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    run(args.repo_root, args.out, sanitize=args.sanitize)
