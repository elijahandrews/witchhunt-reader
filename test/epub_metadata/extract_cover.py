"""Compile the production cover-selection block and document resolver unchanged."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
output = Path(sys.argv[2])
start = source.index('  const auto wrapper = [](const std::string& path) {')
end = source.index('  bookMetadata.textReferenceHref =', start)
(output / 'CoverSelection.inc').write_text(source[start:end])
start = source.index('std::string Epub::findCoverImageInDocument(')
start = source.index('{', start) + 1
end = source.index('\n}\n', start)
(output / 'CoverDocument.inc').write_text(source[start:end])
