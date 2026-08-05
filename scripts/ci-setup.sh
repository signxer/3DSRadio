#!/bin/bash
# CI setup script for 3DSRadio GitHub Actions
# Installs tools and generates placeholder assets for CIA build

set -e

echo "=== 3DSRadio CI Setup ==="

# Install devkitPro tools for CIA building
echo "Installing bannertool and makerom..."
sudo dkp-pacman -S --noconfirm \
    devkitARM \
    3ds-libcurl \
    3ds-mbedtls \
    3ds-libpng \
    3ds-libjpeg-turbo \
    bannertool \
    makerom \
    2>&1 || true

# Generate placeholder banner image (256x128 gradient)
echo "Generating placeholder assets..."
python3 -c "
from PIL import Image
import struct

# Banner image (256x128)
banner = Image.new('RGB', (256, 128), (28, 28, 46))
for y in range(128):
    r = int(28 + (92 - 28) * y / 128)
    g = int(28 + (158 - 28) * y / 128)
    b = int(46 + (255 - 46) * y / 128)
    for x in range(256):
        banner.putpixel((x, y), (r, g, b))

# Add text area
from PIL import ImageDraw, ImageFont
draw = ImageDraw.Draw(banner)
draw.text((40, 40), '3DS Radio', fill=(255, 255, 255))
draw.text((40, 70), 'Internet Radio Player', fill=(180, 180, 200))
banner.save('romfs/banner.png')

# Icon image (48x48)
icon = Image.new('RGBA', (48, 48), (0, 0, 0, 0))
for y in range(48):
    for x in range(48):
        dx, dy = x - 24, y - 24
        dist = (dx*dx + dy*dy) ** 0.5
        if dist < 22:
            r = int(92 * (1 - dist/22))
            g = int(158 * (1 - dist/22))
            b = int(255 * (1 - dist/22))
            a = 255
            icon.putpixel((x, y), (r, g, b, a))
icon.save('romfs/icon.png')

# Banner WAV (100ms silent WAV)
import wave
with wave.open('romfs/banner.wav', 'w') as wav:
    wav.setnchannels(1)
    wav.setsampwidth(2)
    wav.setframerate(8000)
    wav.writeframes(b'\\x00\\x00' * 800)  # 100ms silence

print('Assets generated successfully!')
" 2>&1 || echo "Warning: Pillow not available, using fallback"

# Fallback if PIL is not available - create minimal valid PNGs
if [ ! -f "romfs/banner.png" ]; then
    echo "Creating minimal PNG files..."
    # Minimal 1x1 red PNG (valid header + IDAT)
    python3 -c "
import struct, zlib
def make_png(w, h, r, g, b):
    def chunk(ctype, data):
        c = ctype + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    header = b'\\x89PNG\\r\\n\\x1a\\n'
    ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    raw = b''
    for y in range(h):
        raw += b'\\x00'
        for x in range(w):
            raw += struct.pack('BBB', r, g, b)
    idat = chunk(b'IDAT', zlib.compress(raw))
    iend = chunk(b'IEND', b'')
    return header + ihdr + idat + iend

with open('romfs/banner.png', 'wb') as f:
    f.write(make_png(256, 128, 28, 28, 46))
with open('romfs/icon.png', 'wb') as f:
    f.write(make_png(48, 48, 92, 158, 255))
print('Fallback PNGs created!')
"
fi

# Download CA certificates
echo "Downloading CA certificates..."
bash scripts/get_cacert.sh

echo "=== CI Setup Complete ==="
