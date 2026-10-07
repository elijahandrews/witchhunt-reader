#!/usr/bin/env python3
"""Regression EPUB for typography found in the user's book collection.

Contains original test text only. The same file can be used for on-device QA.
"""
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_STORED, ZIP_DEFLATED
from html import escape
import argparse
import importlib.util
import json
import tempfile

from typography_font_fixture import embedded_fonts, STYLE_NAMES

CSS = """
@font-face { font-family: "Fixture Forms"; src: url("fonts/FixtureForms-Regular.ttf") format("truetype"); font-weight: 400; font-style: normal; }
@font-face { font-family: "Fixture Forms"; src: url("fonts/FixtureForms-Bold.ttf") format("truetype"); font-weight: 700; font-style: normal; }
@font-face { font-family: "Fixture Forms"; src: url("fonts/FixtureForms-Italic.ttf") format("truetype"); font-weight: 400; font-style: italic; }
@font-face { font-family: "Fixture Forms"; src: url("fonts/FixtureForms-BoldItalic.ttf") format("truetype"); font-weight: 700; font-style: italic; }
@font-face { font-family: "Fixture Wide"; src: url("fonts/FixtureWide-Regular.ttf") format("truetype"); }
@font-face { font-family: "Fixture Optical"; src: url("fonts/FixtureOptical-Variable.ttf") format("truetype"); font-weight: 400; }
@font-face { font-family: "Fixture Optical"; src: url("fonts/FixtureOptical-Variable.ttf") format("truetype"); font-weight: 700; }
@font-face { font-family: "Fixture Optical"; src: url("fonts/FixtureOptical-Variable.ttf") format("truetype"); font-weight: 400; font-style: italic; }
@font-face { font-family: "Fixture Optical"; src: url("fonts/FixtureOptical-Variable.ttf") format("truetype"); font-weight: 700; font-style: italic; }
@font-face { font-family: "Fixture CFF"; src: url("fonts/FixtureCff-Regular.otf") format("opentype"); }
.fixture-cff { font-family: "Fixture CFF", serif; }
.fixture { font-family: "Fixture Forms", serif; }
.fixture-wide { font-family: "Fixture Wide", serif; }
.fixture-fallback { font-family: "Missing Fixture", "Fixture Forms", serif; }
.fixture-optical { font-family: "Fixture Optical", serif; }
.genuine-small { font-variant-caps: small-caps; }
.genuine-all { font-variant-caps: all-small-caps; }
p { margin: 0 0 0.5em; text-indent: 0; }
.small_caps { font-size: 75%; text-transform: uppercase; }
.heading { font-size: 120%; }
.upper { text-transform: uppercase; }
.lower { text-transform: lowercase; }
.cancel { text-transform: none; }
.inherit { text-transform: inherit; }
.fv-allsmallcaps { font-variant: small-caps; text-transform: lowercase; }
.tight { line-height: 1.2em; }
.loose { line-height: 2.25; }
.normal { line-height: normal; }
.serif { font-family: "Bembo Std", Georgia, serif; }
.sans { font-family: "Eurostile LT Std Ext Two", Helvetica, sans-serif; }
.tracked { letter-spacing: 0.1em; }
.tracking-reset { letter-spacing: normal; }

"""
CHAPTERS = [
("Embedded original fonts", """<h1>Embedded original fonts</h1>
<p>These diagnostic outlines were drawn for this test. Only the specimens use the embedded font; labels use the reader font.</p>
<p>Regular: <span class="fixture">HNT IF hnt if</span></p>
<p>Bold: <b class="fixture">HNT IF hnt if</b></p>
<p>Italic: <i class="fixture">HNT IF hnt if</i></p>
<p>Bold italic: <b class="fixture"><i>HNT IF hnt if</i></b></p>
<p>The bold stems should be heavier; italic stems should lean to the right. Bold italic must do both.</p>
<p>Forms: <span class="fixture">HNT HNT</span></p>
<p>Same outlines in CFF: <span class="fixture-cff">HNT HNT</span></p>
<p>The CFF sample must match the Forms size; a much larger sample indicates a raster scaling fault.</p>
<p>Wide family: <span class="fixture-wide">HNT HNT</span></p>
<p>Missing first family: <span class="fixture-fallback">HNT HNT</span></p>
<p>The wide sample should be wider. The missing-family sample should match Forms.</p>"""),
("Authored small capitals", """<h1>Authored small capitals</h1>
<p>Normal: <span class="fixture">HNT hnt IF if</span></p>
<p>Small caps: <span class="fixture genuine-small">HNT hnt IF if</span></p>
<p>All small caps: <span class="fixture genuine-all">HNT hnt IF if</span></p>
<p>Small caps keep the initial capitals tall. All small caps make both groups use the same authored small-cap outlines. Their stems must stay stronger than reduced full capitals.</p>
<p>Legacy CSS: <span class="fixture" style="font-variant:small-caps">HNT hnt IF if</span></p>
<p>Bold: <b class="fixture genuine-small">HNT hnt IF if</b></p>
<p>Italic: <i class="fixture genuine-all">HNT hnt IF if</i></p>
<p>Normal ligature: <span class="fixture">fi fi</span></p>
<p>Small-cap letters: <span class="fixture genuine-small">fi fi</span></p>
<p>Small caps must resolve the individual letters before the ordinary fi ligature.</p>
<p>Spacing: <span class="fixture genuine-small">HT ht Ht hT</span></p>"""),
("Variable optical size", """<h1>Variable optical size</h1>
<p>This embedded original variable font has optical-size and weight axes. Optical size follows the actual displayed point size, including CSS scaling.</p>
<p><span class="fixture-optical" style="font-size:80%">HNT hnt IF if</span></p>
<p><span class="fixture-optical">HNT hnt IF if</span></p>
<p><span class="fixture-optical" style="font-size:160%">HNT hnt IF if</span></p>
<p><span class="fixture-optical" style="font-size:200%">HNT hnt IF if</span></p>
<p>At larger optical sizes the design has relatively finer strokes and narrower letters; it should not just enlarge the default design. Raster density must not double the optical design size.</p>
<p>Regular variable: <span class="fixture-optical">HNT hnt</span></p>
<p>Bold variable: <b class="fixture-optical">HNT hnt</b></p>
<p>Italic variable: <i class="fixture-optical">HNT hnt</i></p>
<p>Bold italic variable: <b class="fixture-optical"><i>HNT hnt</i></b></p>"""),
("Percentage sizes", """<h1>External CSS percentages</h1><p class="heading">A heading at 120 percent</p>
<p class="small_caps">Monday, the fifth of October</p>
<p>The heading should be larger. The date should be smaller and ALL UPPERCASE.</p>
<h2>Inline CSS percentages</h2><p style="font-size:120%">An inline heading at 120 percent</p>
<p style="font-size:75%;text-transform:uppercase">Monday, the fifth of October</p>
<p>This is normal body text for comparison.</p>"""),
("Case and Unicode", """<h1>Case and Unicode</h1>
<p class="upper">Monday café Straße ﬃ</p><p>Expected: MONDAY CAFÉ STRASSE FFI</p>
<p class="lower">MONDAY CAFÉ STRASSE</p><p>Expected: monday café strasse</p>
<p class="fv-allsmallcaps">Mixed CASE Small CAPS</p><p>The preceding line should appear as small capitals, all the same height.</p>
<p style="text-transform:uppercase">Entity: caf&#233; &amp; stra&#223;e</p>
<p>Expected: ENTITY: CAFÉ &amp; STRASSE</p>"""),
("Inheritance", """<h1>Inheritance and resets</h1>
<div class="upper"><p>outer Alpha <span class="cancel">Keep Mixed</span> outer Beta</p>
<p class="lower">LOWER GAMMA <b>DELTA</b></p><p>outer Epsilon</p>
<p><span class="cancel">Case <i style="text-transform:inherit">Stay Mixed</i></span> outer Zeta</p>
<p><b>bold Alpha</b> <i>italic Beta</i> <u>underlined Gamma</u></p>
<p><a href="#target">linked Delta</a></p><table><tr><td>table Alpha</td><td class="cancel">Table Mixed</td></tr></table>
</div><p id="target">Outside Mixed must stay Mixed.</p>
<p class="upper">pre<span class="cancel">Mixed</span>post</p>
<p>Expected single word: PREMixedPOST</p>
<p><span style="float:left;font-size:3em;text-transform:uppercase">d</span>rop cap should start with a capital D.</p>
<p style="text-transform:lowercase">UPPER <span style="text-transform:initial">Keep Mixed</span> UPPER</p>"""),
("Line spacing", """<h1>Line spacing</h1><p>Each group has three explicit lines. Tight should have a smaller line advance than normal; loose should be larger.</p>
<div class="tight"><p>Tight one<br/>Tight two<br/>Tight three</p></div>
<div class="normal"><p>Normal one<br/>Normal two<br/>Normal three</p></div>
<div class="loose"><p>Loose one<br/>Loose two<br/>Loose three</p><p class="normal">Reset one<br/>Reset two<br/>Reset three</p></div>
<p>Outside one<br/>Outside two<br/>Outside three</p>
<div class="loose"><p class="tight" style="line-height:inherit">Inherited one<br/>Inherited two<br/>Inherited three</p></div>"""),
("Review regressions", """<h1>Review regressions</h1>
<p class="loose"></p><p>Sibling one<br/>Sibling two<br/>Sibling three</p>
<p class="loose">Inside<br/></p><p>Trailing one<br/>Trailing two<br/>Trailing three</p>
<div class="loose"><div></div></div><p>Nested one<br/>Nested two<br/>Nested three</p>
<section class="loose">Section one<br/>Section two<br/>Section three</section>
<p><span style="float:left;font-size:3em"><i class="upper">d</i></span>rop</p>
<p><span style="float:left;font-size:3em;text-transform:uppercase"><i class="cancel">q</i></span>uiet</p>
<p><span style="float:left;font-size:3em;text-transform:uppercase">ΐΐΐ</span>rest</p>
<p><span style="float:left;font-size:3em;text-transform:uppercase">abcdefghijklmnééé</span>rest</p>
<p>End review regressions.</p>"""),
("Blank lines", """<h1>Blank line spacing</h1>
<p class="loose">BlankLoose one<br/><br/>BlankLoose two<br/><br/>BlankLoose three</p>
<p class="tight">BlankTight one<br/><br/>BlankTight two<br/><br/>BlankTight three</p>"""),
("Font families", """<h1>Serif and sans serif</h1>
<p class="serif">SerifBody with <span class="sans">SansInline <b>SansBold</b> <i>SansItalic</i></span> SerifAgain.</p>
<p class="sans">SansLetter should look distinctly different from the serif body.</p>
<div class="sans"><p>SansInherited <span class="serif">SerifOverride</span> SansRestored</p></div>
<p>ReaderDefault follows the selected reader font.</p>
<p class="sans">Joined<span class="serif">Middle</span>End</p>
<p><span style="float:left;font-size:3em"><span class="sans">N</span></span>ested cap uses sans serif.</p>
<p><span style="float:left;font-size:3em">“<span class="sans">Q</span></span>uoted cap preserves each family as inline runs.</p>
<table><tr><td class="sans">SansCell</td><td class="serif">SerifCell</td></tr></table>"""),
("Letter spacing", """<h1>Letter spacing</h1>
<p style="text-align:center">CHAPTER FORTY TWO</p>
<p class="tracked" style="text-align:center;text-transform:uppercase">chapter forty two</p>
<p>Above: same words, with the second line visibly spaced farther apart.</p>
<p>NormalBefore <span class="tracked">TrackedInline <span class="tracking-reset">ResetInline</span> TrackedAgain</span> NormalAfter</p>
<div style="letter-spacing:2px"><p>FixedTracking <span style="font-size:150%">LargerInherited</span> SameTracking</p></div>
<p style="letter-spacing:1pt">PointSpacing</p><p style="letter-spacing:-0.05em">NegativeSpacing</p>
<p style="letter-spacing:.1em">Joined<span style="letter-spacing:normal">Center</span>Tail</p>
<p class="tracked">Wrapping text must retain its spacing all the way across each line and down to the next line without crossing the right margin.</p>
<p class="tracked">intercontinentalabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz</p>
<p class="sans tracked">SansTracked office affinity café</p>"""),
("Family line heights", """<h1>Family line heights</h1>
<p style="font-family:sans-serif;margin:0;text-indent:0">BlankSans one<br/><br/>BlankSans two<br/><br/>BlankSans three</p>
<p style="font-family:serif;margin:0;text-indent:0">BlankSerif one<br/><br/>BlankSerif two<br/><br/>BlankSerif three</p>
<p style="margin:0;text-indent:0"><span style="font-family:sans-serif">HeldSans one<br/><br/></span><span style="font-family:serif">HeldSans two</span></p>
<p style="margin:0;text-indent:0"><span style="font-family:serif">HeldSerif one<br/><br/></span><span style="font-family:sans-serif">HeldSerif two</span></p>
<p style="font-family:sans-serif;margin:0;text-indent:0">CrossSans one<br/></p><p style="font-family:serif;margin:0;text-indent:0">CrossSans two</p>
<p style="font-family:serif;margin:0;text-indent:0">CrossSerif one<br/></p><p style="font-family:sans-serif;margin:0;text-indent:0">CrossSerif two</p>"""),
("Family and CSS spacing", """<h1>Family and CSS spacing</h1>
<p style="font-family:sans-serif;line-height:2.25;margin:0;text-indent:0">LooseSans one<br/><br/>LooseSans two<br/><br/>LooseSans three</p>
<p style="font-family:serif;line-height:2.25;margin:0;text-indent:0">LooseSerif one<br/><br/>LooseSerif two<br/><br/>LooseSerif three</p>"""),
("Streaming", '<h1>Long transformed word</h1><p class="upper">' + 'caféß' * 180 + '</p><p>All letters above should be uppercase, with no broken UTF-8 or missing text.</p>'),
("Chunk boundaries", '<h1>Drop cap overflow and UTF-8</h1>' + ''.join(
    '<p><span style="float:left;font-size:3em;text-transform:uppercase">' + 'a' * count + suffix + 'rest</span></p>'
    for count, suffix in ((255, 'é'), (254, 'ḿ'), (253, '🦊')))),
("Reduced uppercase strokes", """<h1>Reduced uppercase strokes</h1>
<p>Compare this page with Bookerly and Adobe Garamond Pro. Thin strokes in A, M, N, W and R should stay continuous.</p>
<p>Normal body: capitals and thin strokes.</p>
<p><span class="small_caps">capitals and thin strokes</span></p>
<p><span class="small_caps"><b>capitals and thin strokes</b></span></p>
<p><span class="small_caps"><i>capitals and thin strokes</i></span></p>
<p><span class="small_caps"><b><i>capitals and thin strokes</i></b></span></p>
<p><span class="small_caps">abcdefghijklmnopqrstuvwxyz</span></p>
<p><span class="small_caps">Tuesday, the eighth day of September</span></p>
<p style="font-variant:small-caps">Small capitals and dates</p>
<p style="font-size:50%;text-transform:uppercase">Half size: capitals and thin strokes</p>
<p>All samples should preserve their strokes, with smooth edges when antialiasing is enabled.</p>
<p>Baseline reference: HNT HINTING NTHHE ANMT</p>
<p style="font-size:75%">HNT HINTING NTHHE ANMT</p>
<p style="font-size:90%">HNT HINTING NTHHE ANMT</p>
<p style="font-size:120%">HNT HINTING NTHHE ANMT</p>
<p style="font-size:75%;letter-spacing:.08em">HNT HINTING NTHHE ANMT</p>
<p>Stroke joins: fT office affinity AVATAR P. P,</p>
<p style="font-size:75%;font-style:italic">fT office affinity AVATAR P. P,</p>
<p style="font-size:75%;font-style:italic;letter-spacing:-.05em">fT office affinity AVATAR</p>
<p>Wide punctuation: — ⸻ —</p>
<p>Solid strokes must stay dark where letters overlap.</p>
<p>Garamond has contrasting strokes and a T that extends above the usual capital height. Preserve these shapes without extra pixel shifts.</p>"""),
]

def build(out, preview_dir=None):
    files = {
      "mimetype": "application/epub+zip",
      "META-INF/container.xml": '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
      "OEBPS/style.css": CSS,
    }
    manifest = '<item id="css" href="style.css" media-type="text/css"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
    fonts = embedded_fonts()
    for i, (name, data) in enumerate(fonts.items()):
      files['OEBPS/fonts/' + name] = data
      media_type = "font/otf" if name.endswith(".otf") else "font/ttf"
      manifest += f'<item id="font{i}" href="fonts/{name}" media-type="{media_type}"/>'
    spine = ''; nav = ''
    for i, (title, body) in enumerate(CHAPTERS):
      files[f"OEBPS/ch{i}.xhtml"] = f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>{escape(title)}</title><link rel="stylesheet" type="text/css" href="style.css"/></head><body>{body}</body></html>'
      manifest += f'<item id="c{i}" href="ch{i}.xhtml" media-type="application/xhtml+xml"/>'
      spine += f'<itemref idref="c{i}"/>'
      nav += f'<navPoint id="c{i}" playOrder="{i+1}"><navLabel><text>{escape(title)}</text></navLabel><content src="ch{i}.xhtml"/></navPoint>'
    files['OEBPS/content.opf'] = f'<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">witch-css-typography-test-v3</dc:identifier><dc:title>Witch CSS Style Test</dc:title><dc:creator>Test Suite</dc:creator><dc:language>en</dc:language></metadata><manifest>{manifest}</manifest><spine toc="ncx">{spine}</spine></package>'
    files['OEBPS/toc.ncx'] = f'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>Witch CSS Style Test</text></docTitle><navMap>{nav}</navMap></ncx>'
    with ZipFile(out, 'w') as z:
      for name, text in files.items():
        info = ZipInfo(name, (2026, 10, 5, 0, 0, 0)); info.compress_type = ZIP_STORED if name == 'mimetype' else ZIP_DEFLATED
        z.writestr(info, text if isinstance(text, bytes) else text.encode('utf-8'))

    if preview_dir is not None:
      write_v6_preview(Path(preview_dir), fonts)


def write_v6_preview(directory, fonts):
    """Optional original-font cpfont and summary for the production render harness."""
    converter_path = Path(__file__).resolve().parents[2] / 'lib/EpdFont/scripts/fontconvert_sdcard.py'
    spec = importlib.util.spec_from_file_location('fixture_fontconvert', converter_path)
    converter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(converter)
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='typography-preview-') as temporary:
      sources = {}
      for style, name in enumerate(STYLE_NAMES):
        path = Path(temporary) / f'{name}.ttf'
        path.write_bytes(fonts[f'FixtureForms-{name}.ttf'])
        sources[style] = str(path)
      output = directory / 'FixtureForms_14.cpfont'
      converter.generate_cpfont_multistyle(sources, 14, [(32, 126), (0xFB01, 0xFB01), (0xFFFD, 0xFFFD)],
        str(output), raster_density=2, small_caps=True)
      (directory / 'expected.json').write_text(json.dumps({
        'format': 6, 'density': 2, 'styles': 4, 'smcp_sources': list('fhint'),
        'c2sc_sources': list('FHINT'), 'alternate_count_per_style': 5,
        'sample': 'HNT hnt IF if', 'source': 'Original geometric outlines generated by typography_font_fixture.py'
      }, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('test_css_typography.epub'))
    parser.add_argument('--preview-dir', type=Path, help='Optional original-font v6 cpfont and expected-data JSON.')
    args = parser.parse_args()
    build(args.output, args.preview_dir)
