#!/usr/bin/env python3
"""mkilbm.py out.iff width height depth [mode]: a test picture as IFF ILBM
(ByteRun1), colour bars, a disc and a grid, drawn here for AmigaMac tests."""
import math, struct, sys
out, w, h, depth = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
camg = int(sys.argv[5], 0) if len(sys.argv) > 5 else (0x8000 if w > 400 else 0)
n = 1 << depth
pal = []
for i in range(n):
    t = i / max(1, n - 1)
    r = int(255 * (0.5 + 0.5 * math.cos(6.283 * t)))
    g = int(255 * (0.5 + 0.5 * math.cos(6.283 * (t - 1 / 3))))
    b = int(255 * (0.5 + 0.5 * math.cos(6.283 * (t - 2 / 3))))
    pal.append((r & 0xF0, g & 0xF0, b & 0xF0))
pal[0] = (0, 0, 0)
pal[1] = (255, 255, 255)
def pixel(x, y):
    cx, cy, rad = w / 2, h / 2, min(w, h) / 3
    if (x % 32 == 0) or (y % 32 == 0): return 1
    if (x - cx) ** 2 + ((y - cy) * (w / h / 1.6)) ** 2 < rad * rad: return 2 + ((x + y) // 8) % (n - 2)
    return 2 + (x * (n - 2) // w)
def byterun1(row):
    out, i = bytearray(), 0
    while i < len(row):
        j = i
        while j + 1 < len(row) and row[j + 1] == row[i] and j - i < 126: j += 1
        if j - i >= 2:
            out += bytes([257 - (j - i + 1)]) + bytes([row[i]]); i = j + 1
        else:
            k = i
            while k < len(row) and k - i < 127 and not (k + 2 < len(row) and row[k] == row[k + 1] == row[k + 2]): k += 1
            out += bytes([k - i - 1]) + row[i:k]; i = k
    return out
rb = ((w + 15) // 16) * 2
body = bytearray()
for y in range(h):
    px = [pixel(x, y) for x in range(w)]
    for p in range(depth):
        row = bytearray(rb)
        for x, v in enumerate(px):
            if (v >> p) & 1: row[x >> 3] |= 0x80 >> (x & 7)
        body += byterun1(bytes(row))
def chunk(i, d): return i + struct.pack('>I', len(d)) + d + (b'\0' if len(d) & 1 else b'')
bmhd = struct.pack('>HHhhBBBBHBBhh', w, h, 0, 0, depth, 0, 1, 0, 0, 10, 11, w, h)
cmap = b''.join(bytes(c) for c in pal)
form = b'ILBM' + chunk(b'BMHD', bmhd) + chunk(b'CMAP', cmap) + chunk(b'CAMG', struct.pack('>I', camg)) + chunk(b'BODY', bytes(body))
open(out, 'wb').write(b'FORM' + struct.pack('>I', len(form)) + form)
