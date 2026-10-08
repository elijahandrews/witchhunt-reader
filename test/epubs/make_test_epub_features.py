#!/usr/bin/env python3
"""Original synthetic code-layout, caps-inheritance and MathML fallback specimens."""
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED, ZIP_STORED
import struct
import zlib
from typography_font_fixture import static_font

def png():
    width, height = 80, 24
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    # Original diagnostic bitmap: an equals sign flanked by two hollow squares.
    pixels = bytearray()
    for y in range(height):
        pixels.append(0)
        for x in range(width):
            ink = ((8 <= x <= 24 or 56 <= x <= 72) and y in (4, 20)) or (x in (8,24,56,72) and 4 <= y <= 20) or (34 <= x <= 46 and y in (9,15))
            pixels.append(0 if ink else 255)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width,height,8,0,0,0,0)) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND",b"")

def marker_png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = bytearray()
    for y in range(15):
        pixels.append(0)
        pixels.extend(0 if abs(x - 7) + abs(y - 7) <= 6 else 255 for x in range(15))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB",15,15,8,0,0,0,0)) + chunk(b"IDAT",zlib.compress(pixels)) + chunk(b"IEND",b"")

CSS = """
p, pre { margin:0; text-indent:0; }
.wrap {white-space:pre-wrap}
.preline {white-space:pre-line}
.caps {font-variant-caps:small-caps}
.all {font-variant-caps:all-small-caps}
.reset {font-variant-caps:normal}
.inherit {font-variant-caps:inherit}
.shared {font-style:italic}
.fromglobal {font-family:'Chapter Face',serif}
#priority {font-weight:bold}
"""
CHAPTERS = [
("Preserved spaces", '''<pre>
  LEAD  GAP
A\tTAB
<span>AB </span><b> CD</b>
<span>XY</span><i>  </i>Z

LAST
</pre><p>AFTER   COLLAPSE</p>'''),
("Wrapping", '''<pre>NOWRAP alpha beta gamma delta epsilon zeta eta theta iota kappa lambda</pre>
<pre class="wrap">WRAP alpha beta gamma delta epsilon zeta eta theta iota kappa lambda</pre>
<pre class="wrap">TOKEN ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 END</pre>\n<div class="wrap">  CSSLEAD  CSSGAP
CSSNEXT
</div><p class="preline">PRELINE  COLLAPSED
NEXTLINE</p>
<p><code>CODE</code> <kbd>KEY</kbd> <samp>OUTPUT</samp> PROSE</p>'''),
("Caps inheritance", '''<div class="caps"><p>CapsBlock <span class="inherit">CapsInherited</span></p>
<section><p>CapsNested <span class="all">AllInline</span> CapsAgain</p></section>
<div class="reset"><p>NormalNested</p></div><p>CapsRestored</p></div>
<div class="all"><p>AllBlock <a href="#target">AllLink</a></p>
<div style="font-variant:inherit"><p>AllInherited</p></div>
<p class="reset">NormalOverride</p><p>AllRestored</p></div>
<p id="target">OutsideCaps</p>'''),
("Math image fallback", '''<p>BeforeMath</p>
<math xmlns="http://www.w3.org/1998/Math/MathML" altimg="equation.png" alttext="diagnostic equation"><mi>HiddenMathFallback</mi></math>
<p>AfterMath</p><math xmlns="http://www.w3.org/1998/Math/MathML" altimg="missing.png" altimg-width="80" altimg-height="24"><mi>MissingMathVisible</mi></math>
<math xmlns="http://www.w3.org/1998/Math/MathML" altimg="unsupported.svg"><mi>UnsupportedMathVisible</mi></math>
<math xmlns="http://www.w3.org/1998/Math/MathML"><mi>NoImageMathVisible</mi></math>
<table><tr><td><math xmlns="http://www.w3.org/1998/Math/MathML" altimg="equation.png"><mi>HiddenCellMath</mi></math></td><td><math xmlns="http://www.w3.org/1998/Math/MathML" altimg="missing.png"><mi>MissingCellMathVisible</mi></math></td></tr></table>'''),
("Local head styles one", '<p class="shared">SharedOne</p><p class="fromglobal">Nn</p><p style="font-family:Chapter Face,serif">Hh</p><p id="priority">PriorityOne</p>'),
("Local head styles two", '<p class="shared">SHAREDTWO</p><p class="fromglobal">Tt</p><p style="font-family:Chapter Face,serif">Nn</p><p id="priority">PriorityTwo</p>'),
]

# Original on-device navigation specimens. These deliberately use long paragraphs
# and inline boundaries so search and reflow can share stable source positions.
def numbered_paragraph():
    sentences = []
    for index in range(1, 97):
        number = f"<em>{index:04d}</em>" if index % 8 == 0 else f"{index:04d}"
        sentences.append(
            f"Marker {number}: a clear path crosses the orchard before reaching a low stone wall."
        )
    return "<p>" + " ".join(sentences) + "</p>"

CHAPTERS += [
    ("Search passages", """<p>A silver lantern stood beside the gate.</p>
<p>We carried a <em>silver</em> <strong>lantern</strong> through the garden.</p>
<p>At dusk, the SILVER&#160;LANTERN was lit.</p>
<p>The cabinet was labeled copper &amp; stone.</p>
<p>The sign read Café Straße beside a quiet courtyard.</p>
<p>A note described inter<span>leaved</span> characters and a soft&#173;hyphen.</p>
<pre>At the window, rain
returns to the glass.</pre>
<p>End of the short search specimens.</p>"""),
    ("Long paragraph navigation", """<p>Choose a numbered passage below, change font size several
 times, then return to the original size. The same passage should remain visible.</p>"""
     + numbered_paragraph()),
    ("Nested paragraph navigation", """<p>This paragraph is inside nested containers. Its numbered
 passages should remain reachable after changing font size or selecting a search result.</p>
<section><div>""" + numbered_paragraph() + "</div></section>"),
]
CHAPTERS += [
    ("Media and image markers", """<p class="media-check">Screen stylesheet active</p>
<p>This chapter has an extra body inset on both sides. The diamond below shares the first line of its paragraph.</p>
<p class="image-marker"><img src="../marker.png" width="15" height="15"/>&#160;&#160;&#160;&#160;<strong>Orchard path.</strong>
The text continues beside the marker and wraps at the hanging inset. This is original diagnostic text for checking paragraph shape.</p>
<p>After the marker, this paragraph returns to the normal body inset.</p>"""),
    ("Scaled spacing and plain links", """<p class="scaled-heading">Scaled heading</p>
<p>The gap above this line is two em at the heading size.</p>
<p><a class="plain-link" href="#destination">This link has no underline.</a></p>
<p id="destination">The link remains navigable. Its neighboring text keeps the usual body style.</p>"""),
    ("Small font strokes", """<p>Choose Bookerly HD at 10 pt with Normal darkness and anti-aliasing enabled. Let the page settle before comparing the strokes.</p>
<p>Regular: Tt Hh Nn Ii Ll</p>
<p><strong>Bold: Tt Hh Nn Ii Ll</strong></p>
<p><em>Italic: Tt Hh Nn Ii Ll</em></p>
<p><strong><em>Bold italic: Tt Hh Nn Ii Ll</em></strong></p>
<p>TALL TENTS TEST TINY TILES. Tall tents shelter the trail at twilight.</p>
<p>The thin thread turns twice, then settles beside a little lantern.</p>
<p>Combining accents: T&#x301; t&#x301; Cafe&#x301;.</p>
<p>The T crossbars should meet their stems evenly. Compare the upright strokes with H, N, I, and L in each style.</p>"""),
]

def build(output):
    files = {
      "mimetype":"application/epub+zip",
      "META-INF/container.xml":'<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
      "OPS/style.css":CSS, "OPS/equation.png":png(),
      "OPS/marker.png":marker_png(),
      "OPS/alternate-device.css":".media-check {text-transform:uppercase;font-size:60%;margin-left:0}",
      "OPS/Fonts/One.ttf":static_font(), "OPS/Fonts/Two.ttf":static_font(wide=True),
    }
    manifest='<item id="css" href="style.css" media-type="text/css"/><item id="image" href="equation.png" media-type="image/png"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
    manifest += '<item id="font1" href="Fonts/One.ttf" media-type="font/ttf"/><item id="font2" href="Fonts/Two.ttf" media-type="font/ttf"/>'
    manifest += '<item id="marker" href="marker.png" media-type="image/png"/><item id="alternate" href="alternate-device.css" media-type="text/css"/>'
    spine='';nav=''
    for i,(title,body) in enumerate(CHAPTERS):
        path = f"Heads/ch{i}.xhtml" if i >= 4 else f"ch{i}.xhtml"
        sheet = "../style.css" if i >= 4 else "style.css"
        head = ""
        if 4 <= i <= 8:
            source = "One" if i == 4 else "Two"
            transform = "uppercase" if i == 4 else "lowercase"
            # Declaration deliberately precedes @font-face; document-local aliases are discovered first.
            head = f"""<style type="text/css"><![CDATA[
              .shared {{font-family:'Chapter Face',serif;font-style:normal;text-transform:{transform}}}
              p {{font-weight:normal}}
              @font-face {{font-family:'Chapter Face';src:url('../Fonts/{source}.ttf') format('truetype')}}
            ]]></style>"""
        if i == 9:
            head = '''<link rel="stylesheet" href="../alternate-device.css" media="amzn-mobi"/>
              <style>body {margin:1em} .media-check {font-size:120%;text-align:center}
              .image-marker {margin-left:2em;text-indent:-2em;text-align:left}</style>
              <style media="amzn-mobi">.media-check {text-decoration:line-through}</style>'''
        if i == 10:
            head = '''<style>.scaled-heading {font-size:170%;margin-bottom:2em;text-align:center}
              .plain-link {text-decoration:none}</style>'''
        files[f"OPS/{path}"]=f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>{title}</title><link rel="stylesheet" type="text/css" href="{sheet}"/>{head}</head><body>{body}</body></html>'
        manifest+=f'<item id="c{i}" href="{path}" media-type="application/xhtml+xml"/>'
        spine+=f'<itemref idref="c{i}"/>'
        nav+=f'<navPoint id="c{i}" playOrder="{i+1}"><navLabel><text>{title}</text></navLabel><content src="{path}"/></navPoint>'
    files["OPS/content.opf"]=f'<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">witch-feature-fixture-v2</dc:identifier><dc:title>EPUB Feature Specimens</dc:title><dc:creator>Test Suite</dc:creator><dc:language>en</dc:language></metadata><manifest>{manifest}</manifest><spine toc="ncx">{spine}</spine></package>'
    files["OPS/toc.ncx"]=f'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>EPUB Feature Specimens</text></docTitle><navMap>{nav}</navMap></ncx>'
    with ZipFile(output,"w") as archive:
        for path,data in files.items():
            info=ZipInfo(path,(2026,10,6,0,0,0));info.compress_type=ZIP_STORED if path=="mimetype" else ZIP_DEFLATED
            archive.writestr(info,data.encode() if isinstance(data,str) else data)
if __name__=="__main__":build(Path(__file__).with_name("test_epub_features.epub"))
