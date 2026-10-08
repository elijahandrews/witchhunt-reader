#!/usr/bin/env python3
"""Exercise the production Direct HAL with the complete SDK and its recording bus."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def definition(source, signature):
    """Extract a definition verbatim; fail instead of testing a stale manual copy."""
    matches = list(re.finditer(r"(?m)^" + re.escape(signature), source))
    if len(matches) != 1:
        raise RuntimeError(f"Expected one production definition: {signature}")
    start = matches[0].start()
    opening = source.index("{", matches[0].end())
    # Ignore comment/string braces when locating the function's closing brace.
    tokens = re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
                         source[opening:])
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:opening + token.end()]
    raise RuntimeError(f"Unclosed production definition: {signature}")


def run(repo, output, sanitize=False):
    repo, output = Path(repo).resolve(), Path(output).resolve()
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

    # Same complete facade/driver setup as the SDK's test/host/run_pro.py.
    # The only bus replacement is the SDK's own recording bus, copied unchanged.
    drivers = ("Ssd1677", "Uc8179", "Uc8279X4")
    paths = ["src/FreeInkDisplay.cpp", "src/driver/PanelDriver.h",
             "include/FreeInkDisplay.h", "include/EInkDisplay.h", "include/GrayscaleCapabilities.h"]
    paths += [f"src/driver/{driver}Driver.{ext}" for driver in drivers for ext in ("h", "cpp")]
    paths += [f"src/lut/{name}" for name in ("Ssd1677Luts.h", "Uc8279X3Luts.h", "UltraChipDirectGrayLuts.h")]
    for name in paths:
        copy(sdk / name, output / "sdk" / name)
    for name in ("Arduino.h", "BoardConfig.h", "SPI.h", "esp_heap_caps.h"):
        copy(sdk / "test/host/pro_stubs" / name, output / name)
    copy(sdk / "test/host/pro_stubs/EpdBus.h", output / "sdk/src/bus/EpdBus.h")
    copy(repo / "lib/hal/HalDisplay.h", output / "HalDisplay.h")

    hal = (repo / "lib/hal/HalDisplay.cpp").read_text()
    signatures = ("HalDisplay::HalDisplay()", "HalDisplay::~HalDisplay()",
                  "static uint8_t refreshModeToByte(",
                  "bool HalDisplay::supportsDirectGrayPanel() const",
                  "bool HalDisplay::supportsDirectGrayPlanes() const",
                  "bool HalDisplay::displayDirectGrayPlanes(")
    definitions = [definition(hal, signature) for signature in signatures]
    (output / "HalDirectProduction.cpp").write_text(
        '#include <HalDisplay.h>\n'
        '// Only the RTOS SPI mutex and logging are absent on this single-threaded host.\n'
        'namespace HalSpiBus { struct Lock { Lock() {} ~Lock() {} }; }\n'
        '#define LOG_INF(...) ((void)0)\n\n' + "\n\n".join(definitions) + "\n")
    (output / "source-provenance.json").write_text(json.dumps({
        "copied_unchanged": copied,
        "hal_cpp_sha256": hashlib.sha256(hal.encode()).hexdigest(),
        "extracted_definitions": list(signatures),
    }, indent=2) + "\n")

    sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else []
    env = os.environ.copy()
    if sanitize:
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    configs = {"dual_psram": ["-DBOARD_HAS_PSRAM=1", "-DFREEINK_FB_PSRAM=1"],
               "single_psram": ["-DBOARD_HAS_PSRAM=1", "-DFREEINK_FB_PSRAM=1",
                                "-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1"],
               "no_psram_undefined": ["-DFREEINK_FB_PSRAM=0"],
               "no_psram_zero": ["-DBOARD_HAS_PSRAM=0", "-DFREEINK_FB_PSRAM=0"]}
    commands = []
    for config, flags in configs.items():
        exe = output / config
        command = [os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-g", *sanitizers,
                   "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function", "-DARDUINO=1",
                   "-DFREEINK_DRIVER_SSD1677=1", "-DFREEINK_DRIVER_UC8179=1", "-DFREEINK_DRIVER_UC8279_X4=1",
                   *flags, "-I" + str(output), "-I" + str(output / "sdk/include"),
                   "-I" + str(output / "sdk/src"), str(source / "direct_hal.cpp"),
                   str(output / "HalDirectProduction.cpp"), str(output / "sdk/src/FreeInkDisplay.cpp")]
        command += [str(output / f"sdk/src/driver/{name}Driver.cpp") for name in drivers]
        command += ["-o", str(exe)]
        commands.append(command)
        (output / "build-commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        subprocess.run(command, check=True)
        print(f"Direct HAL configuration: {config}", flush=True)
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("--out", type=Path, default=Path("direct-hal-audit-output"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    run(args.repo_root, args.out, args.sanitize)
