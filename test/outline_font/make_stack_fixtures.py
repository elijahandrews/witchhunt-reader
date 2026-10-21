"""Original geometric charstrings; no downloaded fonts or book contents."""
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString

def make(path, nested=False):
    names=['.notdef','space','A','I','acute','Aacute']
    fb=FontBuilder(1000,isTTF=False)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({32:'space',65:'A',73:'I',0xB4:'acute',0xC1:'Aacute'})
    programs={
      '.notdef':['endchar'], 'space':['endchar'],
      'A':[0,50,250,50,300,50,'hstem','cntrmask',b'\xe0',50,0,'rmoveto',0,700,80,0,0,-700,'rlineto','endchar'],
      'I':[250,0,'rmoveto',0,700,80,0,0,-700,'rlineto','endchar'],
      'acute':[0,0,'rmoveto',80,140,70,0,-80,-140,'rlineto','endchar'],
      'Aacute':[150,700,65,194,'endchar'],
    }
    if nested:programs['A']=[0,0,73,194,'endchar']
    strings={n:T2CharString(program=v) for n,v in programs.items()}
    fb.setupCFF('StackFixture',{'FullName':'Stack Fixture','FamilyName':'Stack Fixture','Weight':'Regular'},strings,{'defaultWidthX':600,'nominalWidthX':0})
    fb.setupHorizontalMetrics({n:(600,0) for n in names})
    fb.setupHorizontalHeader(ascent=1000,descent=-200)
    fb.setupNameTable({'familyName':'Stack Fixture','styleName':'Regular','psName':'StackFixture','fullName':'Stack Fixture'})
    fb.setupOS2(sTypoAscender=1000,sTypoDescender=-200,usWinAscent=1000,usWinDescent=200)
    fb.setupPost();fb.setupMaxp();fb.font['head'].created=fb.font['head'].modified=2082844800;fb.font.recalcTimestamp=False
    fb.save(path)
if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='Generate original CFF counter-mask/seac stack fixtures.')
    parser.add_argument('out', type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    make(args.out / 'stack-cff.otf')
    make(args.out / 'stack-nested-cff.otf', True)
