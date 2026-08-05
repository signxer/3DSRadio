#!/usr/bin/env python3
"""Generate a minimal valid SMDH file for 3DS homebrew."""
import struct
import sys

def make_smdh(path, short_title, long_title, publisher):
    data = bytearray(0x3000)
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

    with open(path, 'wb') as f:
        f.write(data)

if __name__ == '__main__':
    if len(sys.argv) < 5:
        print(f"Usage: {sys.argv[0]} <output.smdh> <short> <long> <publisher>")
        sys.exit(1)
    make_smdh(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
    print(f"Generated {sys.argv[1]}")
