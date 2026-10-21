#!/usr/bin/env python3
"""Write the app icon (a white "H" on indigo) as an RGB PNG: icon.py OUT.png SIZE"""
import struct, sys, zlib

out, size = sys.argv[1], int(sys.argv[2])
bg, fg = (88, 86, 214), (255, 255, 255)
rows = []
for y in range(size):
    row = bytearray(b'\0')
    for x in range(size):
        u, v = x / size, y / size
        h = 0.25 < v < 0.75 and (0.28 < u < 0.38 or 0.62 < u < 0.72 or (0.45 < v < 0.55 and 0.28 < u < 0.72))
        row += bytes(fg if h else bg)
    rows.append(bytes(row))


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
       chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))
open(out, 'wb').write(png)
