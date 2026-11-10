"""Run native caps checks on original generated outlines; retain no font binary."""
from io import BytesIO
from pathlib import Path
import subprocess
import sys
import tempfile
from fontTools.ttLib import TTFont
from typography_font_fixture import static_font

with tempfile.TemporaryDirectory(prefix='crossink-caps-') as directory:
    path = Path(directory) / 'fixture.ttf'
    data = static_font()
    path.write_bytes(data)
    font = TTFont(BytesIO(data))
    subprocess.run([sys.argv[1], str(path), str(font.getGlyphID('H.sc')), str(font.getGlyphID('T.sc'))], check=True)
