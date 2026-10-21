#!/usr/bin/env python3
"""Build the production backend and compare variable kerning with FontTools."""
import argparse
from pathlib import Path
import subprocess
import sys
from make_variation_fixture import make, oracle

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--sanitize',action='store_true')
p.add_argument('--real-font',type=Path,help='Optional variable 1000 UPEM font, e.g. local EB Garamond')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[2]
fixture=out/'variable-spacing.ttf';make(fixture)
build=out/'build'
command=[sys.executable,str(repo/'test/outline_font/run.py'),'--out',str(build),'--raster-only']
if a.sanitize:command.append('--sanitize')
fonts=[fixture]
if a.real_font:fonts.append(a.real_font.resolve())
subprocess.run([*command,*map(str,fonts)],check=True)
flags=['-std=c++17','-g','-O1' if a.sanitize else '-Os','-DWITCH_FONT_AUTOHINT=1',
       '-DFT_CONFIG_OPTIONS_H=<witch_ftoption.h>','-DFT_CONFIG_MODULES_H=<witch_ftmodule.h>']
flags+=['-I'+str(repo/d) for d in ['lib/FreeType/config','lib/FreeType/third_party/freetype/include','lib/EpdFont','lib/OutlineFont']]
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
exe=build/'GposVariationTest'
subprocess.run(['clang++',*flags,str(repo/'test/outline_font/GposVariationTest.cpp'),
                *[str(obj) for obj in sorted(build.glob('*.o'))],'-o',str(exe)],check=True)
arguments=[]
for index,font in enumerate(fonts):
    coordinates=out/f'coordinates-{index}.csv';expected=out/f'expected-{index}.csv'
    subprocess.run([str(exe),'--coordinates',str(font),str(coordinates)],check=True)
    oracle(font,expected,coordinates)
    arguments.extend([str(font),str(expected)])
for name,delta in [('positive',2147483647),('negative',-2147483648)]:
    font=out/f'extreme-{name}.ttf';make(font,delta)
    arguments.extend([f'--extreme-{name}',str(font)])
result=subprocess.run([str(exe),*arguments],capture_output=True,text=True)
(out/'variation-test.log').write_text(result.stdout+result.stderr)
print(result.stdout+result.stderr,end='')
if result.returncode:raise SystemExit(result.returncode)
