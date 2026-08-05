#!/bin/bash
# Download Chinese font and convert to 3DS BCFNT format
# Requires devkitPro tools (mkbcfnt) installed

set -e

ROMFS_DIR="$(dirname "$0")/../romfs"
mkdir -p "$ROMFS_DIR"

FONT_URL="https://github.com/googlefonts/noto-cjk/releases/download/Sans2.004/03_NotoSansCJKsc.zip"
FONT_NAME="NotoSansSC-Regular.otf"
OUTPUT_FONT="${ROMFS_DIR}/chinese.bcfnt"

echo "=== 3DSRadio Chinese Font Setup ==="

# Check if mkbcfnt is available
if command -v mkbcfnt &>/dev/null; then
    echo "mkbcfnt found"
else
    echo "mkbcfnt not found. Checking devkitPro..."
    if [ -n "$DEVKITPRO" ] && [ -f "$DEVKITPRO/tools/bin/mkbcfnt" ]; then
        export PATH="$DEVKITPRO/tools/bin:$PATH"
        echo "Found mkbcfnt in devkitPro tools"
    else
        echo "WARNING: mkbcfnt not available."
        echo "To install: dkp-pacman -S devkitpro-devtools"
        echo "Skipping font generation (app will use system font for ASCII)"
        exit 0
    fi
fi

# Download Noto Sans CJK SC
echo "Downloading Noto Sans CJK SC font..."
TMP_DIR=$(mktemp -d)
curl -sSL -o "$TMP_DIR/noto.zip" "$FONT_URL"
unzip -q -o "$TMP_DIR/noto.zip" -d "$TMP_DIR/fonts/"

# Find the OTF/TTF file
FONT_FILE=$(find "$TMP_DIR/fonts" -name "$FONT_NAME" 2>/dev/null | head -1)
if [ -z "$FONT_FILE" ]; then
    # Try to find any regular weight OTF
    FONT_FILE=$(find "$TMP_DIR/fonts" -name "*SC*Regular*.otf" 2>/dev/null | head -1)
fi

if [ -z "$FONT_FILE" ]; then
    echo "Could not find font file in downloaded package"
    rm -rf "$TMP_DIR"
    exit 1
fi

echo "Converting $FONT_FILE to BCFNT..."
mkbcfnt -o "$OUTPUT_FONT" "$FONT_FILE" 2>&1

if [ -f "$OUTPUT_FONT" ]; then
    SIZE=$(stat -f%z "$OUTPUT_FONT" 2>/dev/null || stat -c%s "$OUTPUT_FONT" 2>/dev/null)
    echo "Chinese font generated: $OUTPUT_FONT ($((SIZE/1024)) KB)"
else
    echo "Font conversion failed"
    rm -rf "$TMP_DIR"
    exit 1
fi

rm -rf "$TMP_DIR"
echo "=== Done ==="
