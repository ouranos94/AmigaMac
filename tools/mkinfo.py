#!/usr/bin/env python3
"""mkinfo.py art.txt out.info [tool|project]: a Workbench icon (.info) from a text drawing.
Pens: '.' 0 (grey), '#' 1 (black), 'o' 2 (white), '*' 3 (blue); lines starting with '#'
and a space are comments."""
import struct, sys

art, out = sys.argv[1], sys.argv[2]
kind = {'tool': 3, 'project': 4}[sys.argv[3] if len(sys.argv) > 3 else 'tool']
rows = [l.rstrip('\n') for l in open(art, encoding='utf-8') if l.strip() and not l.startswith('# ')]
w, h = len(rows[0]), len(rows)
if any(len(r) != w for r in rows):
    sys.exit('rangées de longueurs différentes')
pens = {'.': 0, '#': 1, 'o': 2, '*': 3}
words = (w + 15) // 16
planes = b''
for plane in range(2):
    for r in rows:
        bits = [(pens[c] >> plane) & 1 for c in r] + [0] * (words * 16 - w)
        planes += bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))

gadget = struct.pack('>IhhhhHHHIIIIIHI', 0, 0, 0, w, h + 1, 0x0004, 0x0003, 0x0001, 1, 0, 0, 0, 0, 0, 1)
disk = struct.pack('>HH', 0xE310, 1) + gadget + struct.pack('>BBIIiiIII', kind, 0, 0, 0,
       -0x80000000, -0x80000000, 0, 0, 4096)
image = struct.pack('>hhhhhIBBI', 0, 0, w, h, 2, 1, 3, 0, 0)
open(out, 'wb').write(disk + image + planes)
print('%s : %dx%d, %d octets' % (out, w, h, len(disk + image + planes)))
