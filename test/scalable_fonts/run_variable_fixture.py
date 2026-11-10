"""Compare runtime variable instances against fontTools-frozen original faces."""
from io import BytesIO
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont, OverlapMode
from fontTools.cffLib.CFFToCFF2 import convertCFFToCFF2
from typography_font_fixture import variable_font, cff_font
with TemporaryDirectory(prefix="crossink-variable-") as directory:
    folder = Path(directory)
    data = variable_font()
    (folder / "variable.ttf").write_bytes(data)
    for style in range(4):
        for points in (8, 12, 22, 44):
            font = instantiateVariableFont(TTFont(BytesIO(data)), {"opsz": points, "wght": 700 if style & 1 else 400,
                                                                   "ital": 1 if style & 2 else 0}, inplace=True, overlap=OverlapMode.KEEP_AND_DONT_SET_FLAGS)
            font.save(folder / f"frozen-{style}-{points}.ttf")
    font = TTFont(BytesIO(cff_font()))
    convertCFFToCFF2(font)
    font.save(folder / "cff2.otf")
    subprocess.run([sys.argv[1], str(folder)], check=True)
