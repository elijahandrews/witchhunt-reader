#!/usr/bin/env python3
"""Regression EPUB for typography found in the user's book collection.

Contains original test text only. The same file can be used for on-device QA.
"""
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_STORED, ZIP_DEFLATED
from html import escape

CSS = """
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
"""
CHAPTERS = [
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
("Streaming", '<h1>Long transformed word</h1><p class="upper">' + 'caféß' * 180 + '</p><p>All letters above should be uppercase, with no broken UTF-8 or missing text.</p>'),
("Chunk boundaries", '<h1>Drop cap overflow and UTF-8</h1>' + ''.join(
    '<p><span style="float:left;font-size:3em;text-transform:uppercase">' + 'a' * count + suffix + 'rest</span></p>'
    for count, suffix in ((255, 'é'), (254, 'ḿ'), (253, '🦊')))),
]

def build(out):
    files = {
      "mimetype": "application/epub+zip",
      "META-INF/container.xml": '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
      "OEBPS/style.css": CSS,
    }
    manifest = '<item id="css" href="style.css" media-type="text/css"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
    spine = ''; nav = ''
    for i, (title, body) in enumerate(CHAPTERS):
      files[f"OEBPS/ch{i}.xhtml"] = f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>{escape(title)}</title><link rel="stylesheet" type="text/css" href="style.css"/></head><body>{body}</body></html>'
      manifest += f'<item id="c{i}" href="ch{i}.xhtml" media-type="application/xhtml+xml"/>'
      spine += f'<itemref idref="c{i}"/>'
      nav += f'<navPoint id="c{i}" playOrder="{i+1}"><navLabel><text>{escape(title)}</text></navLabel><content src="ch{i}.xhtml"/></navPoint>'
    files['OEBPS/content.opf'] = f'<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">witch-css-typography-test-v1</dc:identifier><dc:title>Witch CSS Style Test</dc:title><dc:creator>Test Suite</dc:creator><dc:language>en</dc:language></metadata><manifest>{manifest}</manifest><spine toc="ncx">{spine}</spine></package>'
    files['OEBPS/toc.ncx'] = f'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>Witch CSS Style Test</text></docTitle><navMap>{nav}</navMap></ncx>'
    with ZipFile(out, 'w') as z:
      for name, text in files.items():
        info = ZipInfo(name, (2026, 10, 5, 0, 0, 0)); info.compress_type = ZIP_STORED if name == 'mimetype' else ZIP_DEFLATED
        z.writestr(info, text.encode('utf-8'))

if __name__ == '__main__':
    build(Path(__file__).with_name('test_css_typography.epub'))
