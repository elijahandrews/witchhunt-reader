#!/usr/bin/env python3
"""Build actual vendored runtime backend on the host; font fixtures are external."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument("fonts",nargs="+",help="TTF/CFF/CFF2 fixtures with Latin smcp, c2sc and fi ligature")
parser.add_argument("--out",type=Path)
parser.add_argument("--sanitize",action="store_true")
args=parser.parse_args()
repo=Path(__file__).resolve().parents[2]
out=args.out or Path(tempfile.mkdtemp(prefix="witch-outline-"))
out.mkdir(parents=True,exist_ok=True)
ft=repo/"lib/FreeType"
backend=repo/"lib/OutlineFont"
flags=["-g","-O1" if args.sanitize else "-Os","-DWITCH_FONT_AUTOHINT=1",
 "-DFT_CONFIG_OPTIONS_H=<witch_ftoption.h>","-DFT_CONFIG_MODULES_H=<witch_ftmodule.h>",
 "-I"+str(ft/"config"),"-I"+str(ft/"third_party/freetype/include"),
 "-I"+str(repo/"lib/EpdFont"),"-I"+str(backend)]
if args.sanitize:flags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer"]
objects=[]
for src in sorted((ft/"src").glob("*.c")):
 obj=out/(src.stem+".o")
 subprocess.run(["clang",*flags,"-c",str(src),"-o",str(obj)],check=True)
 objects.append(str(obj))
for src in sorted(backend.glob("*.cpp")):
 obj=out/(src.stem+".o")
 subprocess.run(["clang++","-std=c++17",*flags,"-c",str(src),"-o",str(obj)],check=True)
 objects.append(str(obj))
exe=out/"OutlineFontTest"
subprocess.run(["clang++","-std=c++17",*flags,str(repo/"test/outline_font/OutlineFontTest.cpp"),*objects,"-o",str(exe)],check=True)
subprocess.run([str(exe),*[str(Path(f).resolve()) for f in args.fonts]],check=True)
