#!/usr/bin/env python3
"""Original synthetic EPUB specimens. All prose, graphics and font outlines are invented here."""
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


CSS += r"""
.rule-top {border-top:1px solid #000;padding-top:.5em;margin-top:1em}
.rule-span {border-bottom:1px solid #000}
.shaded {background-color:#e7e7e9;padding:.5em;margin:1em 0}
.keep {page-break-inside:avoid;break-inside:avoid}
.spacer {min-height:1.65em;clear:both}
.empty-height {height:1.4em}
.opener {min-height:12em;text-align:center}
.bracketed::before {content:"[\00a0"}
.bracketed::after {content:"\00a0]"}
.attribution:before {content:"- "}
.first-line::first-line {font-variant:small-caps;letter-spacing:.04em;font-variant-ligatures:none}
.no-ligatures {font-variant-ligatures:none}
.no-hyphens {hyphens:none;-webkit-hyphens:none;adobe-hyphenate:none}
.manual-hyphens {hyphens:manual}
.limited-hyphens {hyphens:auto;-webkit-hyphenate-limit-before:4;-webkit-hyphenate-limit-after:4;hyphenate-limit-lines:2}
.right-float {float:right}
.small-icon {display:inline-block;width:25%;max-width:100%}
.small-icon img {width:100%;max-height:100%}
.ornament {width:11.2em;text-align:center;margin:1em auto}
.ornament span {display:inline-block;width:28.1%}
.ornament img {width:100%}
"""
CHAPTERS += [
    ("Rules and shaded blocks", """<p class="rule-top">A thin rule appears above this paragraph, with half an em of padding.</p>
<p>The words <span class="rule-span">under this rule</span> have a thin border below them.</p>
<div class="shaded"><p>A pale gray rectangle contains this original specimen. Padding separates the text from its edges.</p></div>
<p>Outside the shaded box, the background returns to white.</p>
<p><span class="right-float">Right edge label</span>Left edge label</p><div style="clear:both"></div>
<p>The next paragraph begins beneath both labels.</p>"""),
    ("Generated decorations and first line", """<h1 class="bracketed">A sample heading</h1>
<p class="first-line">A small lantern hangs beside the gate while the evening breeze moves through the leaves. Only the first displayed line uses small capitals and extra letter spacing. Change the reader size and watch the styled boundary follow the first line.</p>
<p class="attribution">A fictional observer</p>
<p class="no-ligatures">Ligatures disabled: office affinity difficult final.</p>
<p>Reader default ligatures: office affinity difficult final.</p>"""),
    ("Empty spacers and heading height", """<div class="opener"><h1>Room above the prose</h1></div>
<p>This paragraph begins below the minimum height of the heading wrapper.</p>
<div class="spacer"></div><p>A visible gap precedes this paragraph.</p>
<p class="empty-height"></p><p>A second empty element adds its explicit height.</p>
<div style="height:0;min-height:0"></div><p>A zero-height element adds no blank line.</p>
<div style="min-height:4em;padding:1em;background-color:#eee"><div style="min-height:inherit;background-color:inherit"><p>The inner box inherits the immediate parent minimum height and shade.</p></div></div>"""),
    ("SVG ornaments and percentage icons", """<p>These images use original vector paths, including curves and an elliptical arc.</p>
<div class="ornament"><span><img src="../ornament.svg" alt="Synthetic curved ornament"/></span></div>
<p>The ornament is a small fraction of its containing block.</p>
<div style="text-align:center"><span class="small-icon"><img src="../icon.svg" alt="Synthetic ring icon"/></span></div>
<p>The ring occupies one quarter of the available width.</p>
<p><span><img src="../icon.svg" style="width:1.8em;height:1.8em" alt="Synthetic prefix icon"/></span> An icon taller than ordinary text stays beside this label. The following line clears its lower edge without overlap.</p>"""),
    ("Figures kept with captions", """<p>Change the font size to move the following figure toward a page boundary.</p>""" +
     "<p>An original line of filler precedes the illustration.</p>" * 8 +
     """<div class="keep"><p style="text-align:center"><img src="../ornament.svg" width="120" height="48"/></p>
<p>A short caption stays with its illustration when the group fits on one page.</p></div>
<p>Longer groups may split so that all their contents remain readable.</p>"""),
    ("Hyphenation policies", """<p class="no-hyphens">No discretionary hyphens: inter&#173;connected, trans&#173;formation, extra&#173;ordinary. A very long token may wrap at the edge without a printed hyphen.</p>
<p class="manual-hyphens">Manual discretionary breaks: inter&#173;connected, trans&#173;formation, extra&#173;ordinary.</p>
<p class="limited-hyphens">Automatic hyphenation uses four-letter minimum pieces and at most two consecutive hyphenated lines. Interconnected observations describe extraordinary transformations across a hypothetical landscape.</p>
<p>Optional breaks in this invented address add no visible spaces or hyphens: https://<wbr/>example.<wbr/>invalid/<wbr/>specimens/<wbr/>a-long-path/<wbr/>sample.</p>"""),
]

# Original spacing specimens for comparing glyph origins at small reading sizes.
CHAPTERS += [("Small text spacing", """<p>Compare these samples at a small reader font size with antialiasing enabled.</p>
<p style="font-size:80%">still ill little sitting minimum tilt</p>
<p>still ill little sitting minimum tilt</p>
<p style="font-size:120%">still ill little sitting minimum tilt</p>
<p>The tall tree stood still.</p>
<p><em>The little mill is still quiet.</em></p>
<p><strong>A thin twig rests beside the window.</strong></p>
<p style="font-variant:small-caps">Still little letters and small capitals</p>
<p>AV office ffi 123 café e&#x301;</p>
<p style="letter-spacing:.03125em">Fine positive spacing: still little letters.</p>
<p style="letter-spacing:-.03125em">Fine negative spacing: still little letters.</p>""")]

def build(output):
    files = {
      "mimetype":"application/epub+zip",
      "META-INF/container.xml":'<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
      "OPS/style.css":CSS, "OPS/equation.png":png(),
      "OPS/marker.png":marker_png(),
      "OPS/ornament.svg":'<svg xmlns="http://www.w3.org/2000/svg" width="144" height="64" viewBox="0 0 144 64"><path fill="#606060" fill-rule="evenodd" d="M4 32 Q36 4 72 32 T140 32 Q108 60 72 32 T4 32Z"/></svg>',
      "OPS/icon.svg":'<svg xmlns="http://www.w3.org/2000/svg" width="90" height="90" viewBox="0 0 90 90"><g fill="#101010" fill-rule="evenodd"><path d="M45 5 A40 40 0 1 1 44.99 5Z M45 20 A25 25 0 1 0 45.01 20Z"/></g></svg>',

      "OPS/alternate-device.css":".media-check {text-transform:uppercase;font-size:60%;margin-left:0}",
      "OPS/Fonts/One.ttf":static_font(), "OPS/Fonts/Two.ttf":static_font(wide=True),
    }
    manifest='<item id="css" href="style.css" media-type="text/css"/><item id="image" href="equation.png" media-type="image/png"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
    manifest += '<item id="font1" href="Fonts/One.ttf" media-type="font/ttf"/><item id="font2" href="Fonts/Two.ttf" media-type="font/ttf"/>'
    manifest += '<item id="marker" href="marker.png" media-type="image/png"/><item id="alternate" href="alternate-device.css" media-type="text/css"/>'
    manifest += '<item id="ornament" href="ornament.svg" media-type="image/svg+xml"/><item id="icon" href="icon.svg" media-type="image/svg+xml"/>'
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
    files["OPS/content.opf"]=f'<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">witch-feature-fixture-v3</dc:identifier><dc:title>EPUB Feature Specimens</dc:title><dc:creator>Test Suite</dc:creator><dc:language>en</dc:language></metadata><manifest>{manifest}</manifest><spine toc="ncx">{spine}</spine></package>'
    files["OPS/toc.ncx"]=f'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>EPUB Feature Specimens</text></docTitle><navMap>{nav}</navMap></ncx>'
    with ZipFile(output,"w") as archive:
        for path,data in files.items():
            info=ZipInfo(path,(2026,10,6,0,0,0));info.compress_type=ZIP_STORED if path=="mimetype" else ZIP_DEFLATED
            archive.writestr(info,data.encode() if isinstance(data,str) else data)
if __name__=="__main__":build(Path(__file__).with_name("test_epub_features.epub"))
