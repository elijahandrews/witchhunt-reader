"""Original geometric variable font plus independent FontTools kerning oracles."""
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables import otTables
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.varLib.builder import buildVarRegionList, buildVarData, buildVarStore
from fontTools.varLib.varStore import VarStoreInstancer
from fontTools.misc.roundTools import otRound


def make(path, delta=-100):
    names=['.notdef']+[f'uni{cp:04X}' for cp in range(32,127)]
    for cp in [65,72,84,86,89]:names[1+cp-32]=chr(cp)
    fb=FontBuilder(1000,isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({cp:names[1+cp-32] for cp in range(32,127)})
    glyphs={}
    for i,name in enumerate(names):
        pen=TTGlyphPen(None)
        def rectangle(x,y,w,h):
            pen.moveTo((x,y));pen.lineTo((x+w,y));pen.lineTo((x+w,y+h));pen.lineTo((x,y+h));pen.closePath()
        if name=='H':
            rectangle(60,0,70,700);rectangle(470,0,70,700);rectangle(130,310,340,70)
        elif name=='T':
            rectangle(265,0,70,630);rectangle(60,630,480,70)
        elif i>1:
            pen.moveTo((60,0));pen.lineTo((140,0));pen.lineTo((360,700));pen.lineTo((280,700));pen.closePath()
        glyphs[name]=pen.glyph()
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({name:(600,60) for name in names})
    fb.setupHorizontalHeader(ascent=800,descent=-200)
    fb.setupNameTable({'familyName':'Variable Spacing Fixture','styleName':'Regular','psName':'VariableSpacingFixture','fullName':'Variable Spacing Fixture'})
    fb.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
    fb.setupPost();fb.setupMaxp()
    fb.setupFvar([('wght',100,400,900,'Weight')],[])
    font=fb.font
    font['avar']=newTable('avar');font['avar'].segments={'wght':{-1.0:-1.0,-0.5:-0.75,0.0:0.0,0.5:0.25,1.0:1.0}}
    font['gvar']=newTable('gvar');font['gvar'].variations={name:[] for name in names}
    addOpenTypeFeaturesFromString(font,'languagesystem DFLT dflt; languagesystem latn dflt; feature kern { pos A V -20; pos T Y -30; pos H T -20; pos T H -30; } kern;')
    for lookup in font['GPOS'].table.LookupList.Lookup:
        for table in lookup.SubTable:
            assert table.Format==1
            table.ValueFormat1 |= 0x40
            for pair_set in table.PairSet:
                for record in pair_set.PairValueRecord:
                    device=otTables.Device();device.StartSize=0;device.EndSize=0;device.DeltaFormat=0x8000
                    record.Value1.XAdvDevice=device
    font['GDEF']=newTable('GDEF');gdef=font['GDEF'].table=otTables.GDEF()
    gdef.Version=0x00010003
    gdef.GlyphClassDef=gdef.AttachList=gdef.LigCaretList=gdef.MarkAttachClassDef=gdef.MarkGlyphSetsDef=None
    gdef.VarStore=buildVarStore(buildVarRegionList([{'wght':(0,1,1)}],['wght']),[buildVarData([0],[[delta]],optimize=False)])
    font['head'].created=font['head'].modified=2082844800;font.recalcTimestamp=False
    font.save(path)


def pair_adjustment(font,left,right,variation):
    gpos=font['GPOS'].table
    scripts={r.ScriptTag:r.Script for r in gpos.ScriptList.ScriptRecord}
    script=scripts.get('latn') or scripts.get('DFLT')
    language=script.DefaultLangSys
    indices=list(language.FeatureIndex)
    if language.ReqFeatureIndex!=0xffff:indices.insert(0,language.ReqFeatureIndex)
    total=0
    for index in indices:
        feature=gpos.FeatureList.FeatureRecord[index]
        if feature.FeatureTag!='kern':continue
        for lookup_index in feature.Feature.LookupListIndex:
            lookup=gpos.LookupList.Lookup[lookup_index]
            for table in lookup.SubTable:
                if lookup.LookupType==9:
                    if table.ExtensionLookupType!=2:continue
                    table=table.ExtSubTable
                elif lookup.LookupType!=2:continue
                if left not in table.Coverage.glyphs:continue
                if table.Format==1:
                    pair_set=table.PairSet[table.Coverage.glyphs.index(left)]
                    record=next((r for r in pair_set.PairValueRecord if r.SecondGlyph==right),None)
                    if record is None:continue
                elif table.Format==2:
                    a=table.ClassDef1.classDefs.get(left,0);b=table.ClassDef2.classDefs.get(right,0)
                    record=table.Class1Record[a].Class2Record[b]
                else:continue
                if record.Value1:
                    total+=getattr(record.Value1,'XAdvance',0)
                    device=getattr(record.Value1,'XAdvDevice',None)
                    if device and device.DeltaFormat==0x8000:
                        total+=otRound(variation[(device.StartSize<<16)|device.EndSize])
                break
    return total


def oracle(path,out,coordinates):
    font=TTFont(path)
    assert font['head'].unitsPerEm==1000
    cmap=font.getBestCmap();chars=[c for c in range(32,127) if c in cmap]
    locations={}
    for line in coordinates.read_text().splitlines():
        weight,tag,value=line.split(',');locations.setdefault(int(weight),{})[tag]=int(value)/65536
    with out.open('w') as f:
        for weight,location in locations.items():
            variation=VarStoreInstancer(font['GDEF'].table.VarStore,font['fvar'].axes,location)
            for left in chars:
                for right in chars:
                    a,b=cmap[left],cmap[right]
                    f.write(f'{weight},{font.getGlyphID(a)},{font.getGlyphID(b)},{pair_adjustment(font,a,b,variation)}\n')


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('out',type=Path)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    fixture=a.out/'variable-spacing.ttf';make(fixture)
