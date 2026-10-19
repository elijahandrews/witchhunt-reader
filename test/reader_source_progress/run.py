#!/usr/bin/env python3
"""Check production source capture/restore statements without device services."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument("--out", type=Path, required=True)
p.add_argument("--sanitize", action="store_true")
a = p.parse_args()
repo, out = a.repo.resolve(), a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
s = (repo / "src/activities/reader/EpubReaderActivity.cpp").read_text()
def extract(start, end):
    assert s.count(start) == 1 and s.count(end) == 1
    return s.split(start, 1)[1].split(end, 1)[0]
start = "std::optional<EpubProgressRecord::Source> EpubReaderActivity::sourceForProgress("
# Preserve the entire implementation including the signature after its unique prefix.
capture = start + extract(start, "void EpubReaderActivity::captureSourceBeforeReflow() {")
restore = extract("      currentSpineIndex = record->spineIndex;", "      hadSavedProgress = true;")
close = extract("  // 100% record; leave that alone.\n", "\n  // Save bookmarks before exit")
generated = out / "probe.cpp"
shell = (Path(__file__).with_name("Probe.cpp.in")).read_text()
header = (repo / "src/activities/reader/EpubReaderActivity.h").read_text()
nav_decl = "  struct NavigationTarget {" + header.split("  struct NavigationTarget {", 1)[1].split(
    "  // Phase lifecycle for memory management", 1)[0]
nav_start = "std::optional<int> EpubReaderActivity::NavigationTarget::relaidOutRestorePage("
nav_impl = nav_start + extract(nav_start, "// navTarget is the anchor a section (re)build resolves into.")

generated.write_text(shell.replace("// NAVIGATION_DECLARATION", nav_decl)
                          .replace("// NAVIGATION_IMPLEMENTATION", nav_impl)
                          .replace("// CAPTURE_IMPLEMENTATION", capture)
                          .replace("// RESTORE_STATEMENTS", "currentSpineIndex = record->spineIndex;" + restore)
                          .replace("// CLOSE_STATEMENTS", close))
binary = out / "reader-source-progress"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if a.sanitize else []
subprocess.run(["c++", "-std=c++20", "-O1", "-g", *flags, "-I" + str(repo / "src"),
                "-I" + str(repo / "lib/Epub/Epub"), str(generated), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
