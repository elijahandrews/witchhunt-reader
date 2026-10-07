"""Original geometric type specimens for reproducible EPUB/runtime-font tests.

These are diagnostic outlines drawn here, not derived from an existing typeface.
Only H, N, T, I, F, their lowercase forms, fi, space, and replacement are covered.
Requires fonttools; does not read system or downloaded fonts.
"""
from io import BytesIO
from pathlib import Path
from tempfile import TemporaryDirectory

from fontTools.designspaceLib import AxisDescriptor, DesignSpaceDocument, SourceDescriptor
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont
from fontTools.varLib import build as build_variable

STYLE_NAMES = ('Regular', 'Bold', 'Italic', 'BoldItalic')
COVERAGE = {32: 'space', 70: 'F', 72: 'H', 73: 'I', 78: 'N', 84: 'T',
            102: 'f', 104: 'h', 105: 'i', 110: 'n', 116: 't', 0xFB01: 'fi', 0xFFFD: '.notdef'}


def _rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def _outlines(letter, height, width, stroke):
    half = width / 2
    if letter == 'H':
        return [_rect(0, 0, stroke, height), _rect(width - stroke, 0, width, height),
                _rect(0, height * .46, width, height * .46 + stroke)]
    if letter == 'N':
        return [_rect(0, 0, stroke, height), _rect(width - stroke, 0, width, height),
                [(0, height), (stroke, height), (width, 0), (width - stroke, 0)]]
    if letter == 'T':
        return [_rect(half - stroke / 2, 0, half + stroke / 2, height),
                _rect(-20, height - stroke, width + 20, height)]
    if letter == 'I':
        return [_rect(half - stroke / 2, 0, half + stroke / 2, height)]
    if letter == 'F':
        return [_rect(0, 0, stroke, height), _rect(0, height - stroke, width, height),
                _rect(0, height * .5, width * .8, height * .5 + stroke)]
    return []


def static_font(style=0, wide=False, optical=12, weight=None, family='Fixture Forms'):
    """Return one original TTF; topology is stable for variable-font masters."""
    bold, italic = bool(style & 1), bool(style & 2)
    weight = (700 if bold else 400) if weight is None else weight
    # Larger optical designs are narrower and have finer strokes. At equal
    # final size this is visibly different from simply enlarging the default.
    stroke = 85 + (weight - 400) * .17 - (optical - 12) * .55
    width = (840 if wide else 580) - (optical - 12) * .9
    names = ['.notdef', 'space'] + list('HNTIFhntif') + ['fi'] + [c + '.sc' for c in 'HNTIF']
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(names)
    builder.setupCharacterMap(COVERAGE)
    glyphs, metrics = {}, {}
    for name in names:
        paths, advance = [], width + 90
        if name == '.notdef':
            paths = [_rect(0, 0, 50, 700), _rect(width - 50, 0, width, 700),
                     _rect(0, 0, width, 50), _rect(0, 650, width, 700)]
        elif name == 'space':
            advance = 300
        elif name == 'fi':
            paths = _outlines('F', 540, width * .65, stroke)
            paths += [[(x + width * .7, y) for x, y in path] for path in _outlines('I', 540, width * .25, stroke)]
            advance = width + 60
        elif name.endswith('.sc'):
            # Authored small capitals use stronger stems, not scaled large caps.
            paths = _outlines(name[0], 535, width * .82, stroke * 1.04)
            advance = width * .82 + 85
        else:
            lower = name.islower()
            paths = _outlines(name.upper(), 500 if lower else 700, width * (.8 if lower else 1), stroke)
            advance = width * (.8 if lower else 1) + 90
        pen = TTGlyphPen(None)
        for path in paths:
            transformed = [(round(x + (y * .18 if italic else 0)), round(y)) for x, y in path]
            pen.moveTo(transformed[0])
            for point in transformed[1:]:
                pen.lineTo(point)
            pen.closePath()
        glyphs[name] = pen.glyph()
        metrics[name] = (round(advance), -20 if name in ('T', 'T.sc', 't') else 0)
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=850, descent=-200)
    label = STYLE_NAMES[style]
    builder.setupNameTable({'familyName': family, 'styleName': label,
        'uniqueFontIdentifier': family + '-' + label, 'fullName': family + ' ' + label,
        'psName': family.replace(' ', '') + '-' + label})
    builder.setupOS2(sTypoAscender=850, sTypoDescender=-200, usWinAscent=850,
                    usWinDescent=200, usWeightClass=round(weight))
    builder.setupPost(italicAngle=-10 if italic else 0)
    builder.setupMaxp()
    addOpenTypeFeaturesFromString(builder.font, """
      feature smcp { sub h by H.sc; sub n by N.sc; sub t by T.sc; sub i by I.sc; sub f by F.sc; } smcp;
      feature c2sc { sub H by H.sc; sub N by N.sc; sub T by T.sc; sub I by I.sc; sub F by F.sc; } c2sc;
      feature liga { sub f i by fi; } liga;
      feature kern { pos H T -80; pos H.sc T.sc -60; pos H T.sc -40; pos H.sc T -50; } kern;
    """)
    builder.font['head'].created = builder.font['head'].modified = 2082844800
    builder.font.recalcTimestamp = False
    output = BytesIO()
    builder.font.save(output)
    return output.getvalue()


def variable_font():
    """Original opsz/wght/ital font with optical-dependent outlines and advances."""
    document = DesignSpaceDocument()
    for tag, low, default, high in [('opsz', 8, 12, 72), ('wght', 400, 400, 700), ('ital', 0, 0, 1)]:
        axis = AxisDescriptor()
        axis.name = axis.tag = tag
        axis.minimum, axis.default, axis.maximum = low, default, high
        document.addAxis(axis)
    with TemporaryDirectory(prefix='typography-variable-') as directory:
        for index, (optical, weight, italic) in enumerate([(12, 400, 0), (8, 400, 0), (72, 400, 0), (12, 700, 0), (12, 400, 1)]):
            path = Path(directory) / f'master{index}.ttf'
            path.write_bytes(static_font(style=2 if italic else 0, optical=optical, weight=weight, family='Fixture Optical'))
            source = SourceDescriptor()
            source.path, source.name = str(path), f'master{index}'
            source.familyName, source.styleName = 'Fixture Optical', f'Master{index}'
            source.location = {'opsz': optical, 'wght': weight, 'ital': italic}
            if index == 0:
                source.copyInfo = source.copyLib = source.copyFeatures = True
            document.addSource(source)
        font, _, _ = build_variable(document)
        font['head'].created = font['head'].modified = 2082844800
        font.recalcTimestamp = False
        output = BytesIO()
        font.save(output)
        font.close()
        return output.getvalue()


def cff_font():
    """The same original Regular design in CFF, with an explicit 1000-unit em."""
    with TTFont(BytesIO(static_font())) as source:
        builder = FontBuilder(1000, isTTF=False)
        names = source.getGlyphOrder()
        builder.setupGlyphOrder(names)
        builder.setupCharacterMap(COVERAGE)
        glyph_set = source.getGlyphSet()
        strings = {}
        for name in names:
            pen = T2CharStringPen(source['hmtx'][name][0], glyph_set)
            glyph_set[name].draw(pen)
            strings[name] = pen.getCharString()
        builder.setupCFF('FixtureCff-Regular', {'FullName': 'Fixture CFF Regular',
                         'FamilyName': 'Fixture CFF', 'Weight': 'Regular'}, strings, {})
        builder.setupHorizontalMetrics(dict(source['hmtx'].metrics))
        builder.setupHorizontalHeader(ascent=850, descent=-200)
        builder.setupNameTable({'familyName': 'Fixture CFF', 'styleName': 'Regular',
                               'psName': 'FixtureCff-Regular', 'fullName': 'Fixture CFF Regular'})
        builder.setupOS2(sTypoAscender=850, sTypoDescender=-200, usWinAscent=850, usWinDescent=200)
        builder.setupPost()
        builder.setupMaxp()
        builder.font['head'].created = builder.font['head'].modified = 2082844800
        builder.font.recalcTimestamp = False
        output = BytesIO()
        builder.font.save(output)
        return output.getvalue()


def embedded_fonts():
    result = {f'FixtureForms-{style}.ttf': static_font(i) for i, style in enumerate(STYLE_NAMES)}
    result['FixtureWide-Regular.ttf'] = static_font(wide=True, family='Fixture Wide')
    result['FixtureOptical-Variable.ttf'] = variable_font()
    result['FixtureCff-Regular.otf'] = cff_font()
    return result
