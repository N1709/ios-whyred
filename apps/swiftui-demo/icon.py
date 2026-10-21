#!/usr/bin/env python3
"""Write the app icon (a white "S" on orange) as an RGB PNG: icon.py OUT.png SIZE"""
import struct, sys, zlib

out, size = sys.argv[1], int(sys.argv[2])
bg, fg = (255, 149, 0), (255, 255, 255)
rows = []
for y in range(size):
    row = bytearray(b'\0')
    for x in range(size):
        u, v = x / size, y / size
        bar = 0.28 < u < 0.72 and (0.22 < v < 0.32 or 0.45 < v < 0.55 or 0.68 < v < 0.78)
        side = (0.28 < u < 0.38 and 0.22 < v < 0.55) or (0.62 < u < 0.72 and 0.45 < v < 0.78)
        row += bytes(fg if bar or side else bg)
    rows.append(bytes(row))


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
       chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))
open(out, 'wb').write(png)
