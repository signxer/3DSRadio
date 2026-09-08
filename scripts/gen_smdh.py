#!/usr/bin/env python3
"""Generate a minimal valid SMDH file for 3DS homebrew."""
import struct
import sys

def make_smdh(path, short_title, long_title, publisher):
    # SMDH icon data starts at 0x24C0 and contains a 48x48 RGB565 image.
    # The previous minimal file stopped before the icon block, leaving the
    # Homebrew Launcher with a generic/letter-like placeholder.
    data = bytearray(0x36C0)
    data[0:4] = b'SMDH'
    struct.pack_into('<H', data, 4, 0x0100)

    st = short_title.encode('utf-16-le')
    for i in range(min(len(st), 0x200)):
        data[0x200 + i] = st[i]

    lt = long_title.encode('utf-16-le')
    for i in range(min(len(lt), 0x200)):
        data[0x400 + i] = lt[i]

    pb = publisher.encode('utf-16-le')
    for i in range(min(len(pb), 0x200)):
        data[0x600 + i] = pb[i]

    # All regions
    struct.pack_into('<I', data, 0x1F04, 0xFFFFFFFF)
    # Visible + Audio
    struct.pack_into('<I', data, 0x1F08, 0x00000003)

    icon_offset = 0x24C0
    for y in range(48):
        for x in range(48):
            dx, dy = x - 24, y - 24
            distance = dx * dx + dy * dy
            if distance < 20 * 20:
                rgb = (14, 21, 30)
            elif distance < 22 * 22:
                rgb = (89, 208, 216)
            else:
                rgb = (8, 10, 15)

            # Three tuning bars and a coral play mark echo the in-app radio
            # mark while remaining legible at 48 pixels.
            for bx, height in ((15, 8), (19, 13), (23, 18)):
                if x == bx and 28 - height <= y < 28:
                    rgb = (89, 208, 216)
            if 20 <= x <= 28 and abs(y - 24) <= (x - 20) * 2:
                rgb = (235, 91, 117)

            r, g, b = rgb
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            struct.pack_into('<H', data, icon_offset + (y * 48 + x) * 2,
                             rgb565)

    with open(path, 'wb') as f:
        f.write(data)

if __name__ == '__main__':
    if len(sys.argv) < 5:
        print(f"Usage: {sys.argv[0]} <output.smdh> <short> <long> <publisher>")
        sys.exit(1)
    make_smdh(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
    print(f"Generated {sys.argv[1]}")
