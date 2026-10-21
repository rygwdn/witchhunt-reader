#!/usr/bin/env python3
"""Write the language/hyphenation fixture EPUB.

The book carries no dc:language, so the reader falls back to whatever the
Book language setting says. Its text is one long word ("representation")
repeated, which the English patterns break and no pattern set leaves alone:
the difference between the two layouts is visible on the page.

    python3 scripts/make_language_fixture.py /tmp/sd/books/language.epub
"""

import sys
import zipfile
from pathlib import Path

TITLE = "Language fixture"
# "representation" is broken by the English patterns (rep-re-sen-ta-tion) and by
# the German ones (re-prä-sen-ta-tion), and left alone when no pattern set
# applies, so the three layouts differ visibly.
PARAGRAPH = " ".join(["representation extraordinary"] * 12)

CONTENT_OPF = f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:language-fixture</dc:identifier>
    <dc:title>{TITLE}</dc:title>
    <dc:language></dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="chapter"/>
  </spine>
</package>
"""

CONTAINER_XML = """<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

NAV_XHTML = f"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>{TITLE}</title></head>
  <body>
    <nav epub:type="toc" id="toc">
      <ol><li><a href="chapter.xhtml">Chapter</a></li></ol>
    </nav>
  </body>
</html>
"""

CHAPTER_XHTML = f"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>{TITLE}</title></head>
  <body>
    <h1>{TITLE}</h1>
    <p>{PARAGRAPH}</p>
  </body>
</html>
"""


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    out.parent.mkdir(parents=True, exist_ok=True)
    # The mimetype entry must be first and stored uncompressed.
    with zipfile.ZipFile(out, "w") as zf:
        zf.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", CONTAINER_XML)
        zf.writestr("content.opf", CONTENT_OPF)
        zf.writestr("nav.xhtml", NAV_XHTML)
        zf.writestr("chapter.xhtml", CHAPTER_XHTML)
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
