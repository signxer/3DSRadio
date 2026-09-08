#!/usr/bin/env python3
"""Build the BCFNT character list used by both CI and local builds.

The radio API can return arbitrary UTF-8 station names.  GB2312 is not a
complete Unicode Chinese set, so keep the full CJK Unified Ideographs BMP
range in addition to characters found in the application and the previous
curated list.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CHARLIST = ROOT / "scripts" / "charlist.txt"


def add_text(codepoints, text):
    codepoints.update(ord(ch) for ch in text)


def main():
    codepoints = set()

    # Preserve existing non-CJK coverage (Latin accents, Greek, Cyrillic,
    # kana and symbols) while rebuilding the Chinese portion deterministically.
    if CHARLIST.exists():
        for token in CHARLIST.read_text(encoding="ascii").split():
            if token.lower().startswith("0x"):
                try:
                    codepoints.add(int(token, 16))
                except ValueError:
                    pass

    for directory in (ROOT / "source", ROOT / "include"):
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if path.suffix not in (".c", ".h"):
                continue
            add_text(codepoints, path.read_text(encoding="utf-8"))

    # CJK Unified Ideographs contains both simplified and traditional Chinese
    # used by radio-browser station names.  It is intentionally BMP-only:
    # BCFNT/Citro2D support and 3DS memory remain predictable.
    codepoints.update(range(0x4E00, 0xA000))
    codepoints.update(range(0x3400, 0x4DC0))
    codepoints.update(range(0xF900, 0xFB00))

    # Punctuation and full-width forms commonly returned alongside CJK text.
    codepoints.update(range(0x3000, 0x3040))
    codepoints.update(range(0xFF00, 0xFFEF + 1))
    add_text(codepoints, "·—…“”‘’《》、。•°±×÷")

    CHARLIST.write_text(
        " ".join(f"0x{cp:04X}" for cp in sorted(codepoints)) + "\n",
        encoding="ascii",
    )
    print(f"Character list: {len(codepoints)} unique codepoints")


if __name__ == "__main__":
    main()
