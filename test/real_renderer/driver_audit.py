#!/usr/bin/env python3
"""Bridge production-renderer planes through the actual UC8279 X4 Pro driver."""

import argparse
import hashlib
import json
import os
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
    # Driver capability checks use the real board/controller enum contract.
    # The renderer still uses its orientation/viewable-inset shim separately.
    (output / "driver-board").mkdir(exist_ok=True)
    shutil.copy2(sdk / "test/host/pro_stubs/BoardConfig.h", output / "driver-board/BoardConfig.h")

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
        "-I" + str(output / "driver-board"),
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


def run_dark_sdk(repo, output, fixtures, sanitize=False):
    """Replay actual real-renderer glyph planes through the complete SDK facade."""
    repo, output, fixtures = Path(repo).resolve(), Path(output).resolve(), Path(fixtures).resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = repo / "freeink-sdk/libs/display/FreeInkDisplay"
    source = Path(__file__).resolve().parent
    copied = []

    def copy(src, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, destination)
        assert destination.read_bytes() == src.read_bytes()
        copied.append({"path": str(src.relative_to(repo)),
                       "sha256": hashlib.sha256(src.read_bytes()).hexdigest()})

    drivers = ("Ssd1677", "Uc8179", "Uc8279X4")
    paths = ["src/FreeInkDisplay.cpp", "src/driver/PanelDriver.h", "include/FreeInkDisplay.h",
             "include/EInkDisplay.h", "include/GrayscaleCapabilities.h"]
    paths += [f"src/driver/{driver}Driver.{ext}" for driver in drivers for ext in ("h", "cpp")]
    paths += [f"src/lut/{name}" for name in ("Ssd1677Luts.h", "Uc8279X3Luts.h", "UltraChipDirectGrayLuts.h")]
    for name in paths:
        copy(sdk / name, output / "sdk" / name)
    # Same electrical stubs and unchanged recording bus as SDK run_pro.py.
    for name in ("Arduino.h", "BoardConfig.h", "SPI.h", "esp_heap_caps.h"):
        copy(sdk / "test/host/pro_stubs" / name, output / name)
    copy(sdk / "test/host/pro_stubs/EpdBus.h", output / "sdk/src/bus/EpdBus.h")
    (output / "source-provenance.json").write_text(json.dumps(copied, indent=2) + "\n")
    sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else []
    command = [os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-g", *sanitizers,
               "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function", "-DARDUINO=1",
               "-DFREEINK_DRIVER_SSD1677=1", "-DFREEINK_DRIVER_UC8179=1", "-DFREEINK_DRIVER_UC8279_X4=1",
               "-DBOARD_HAS_PSRAM=1", "-DFREEINK_FB_PSRAM=1", "-I" + str(output),
               "-I" + str(output / "sdk/include"), "-I" + str(output / "sdk/src"),
               str(source / "dark_sdk_wire.cpp"), str(output / "sdk/src/FreeInkDisplay.cpp")]
    command += [str(output / f"sdk/src/driver/{driver}Driver.cpp") for driver in drivers]
    command += ["-o", str(output / "dark_sdk_wire")]
    (output / "build-command.json").write_text(json.dumps(command, indent=2) + "\n")
    subprocess.run(command, check=True)
    env = os.environ.copy()
    if sanitize:
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    subprocess.run([str(output / "dark_sdk_wire"), str(fixtures)], check=True, env=env)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("--out", type=Path, default=Path("driver-audit-output"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    run(args.repo_root, args.out, sanitize=args.sanitize)
