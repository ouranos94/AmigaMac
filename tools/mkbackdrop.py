#!/usr/bin/env python3
"""mkbackdrop.py: AmigaMac's Workbench backdrop (an original design) and the
wbpattern.prefs that names it.

A light grey gradient with the "AmigaMac" name drawn from the system font,
italic and smoothed, written as an IFF ILBM (ByteRun1). Run from the
repository root: python3 tools/mkbackdrop.py
"""
import os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
SYS = os.path.join(HERE, '..', 'System')
W, H = 1280, 720

def load_font():
    glyphs, cur, rows = {}, None, []
    for line in open(os.path.join(HERE, '..', 'fonts', 'amiga8.txt'), encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('# ') or not line:
            continue
        if line.startswith('= '):
            if cur is not None:
                glyphs[cur] = rows
            name = line[2:]
            cur = ' ' if name == 'space' else name
            rows = []
        else:
            rows.append(line)
    if cur is not None:
        glyphs[cur] = rows
    return glyphs

FONT = load_font()

def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))

# ---- the picture, in RGB ----------------------------------------------------------------

TOP, BOTTOM = (0xE8, 0xE8, 0xEA), (0xC8, 0xC9, 0xCC)
GRADIENT_STEPS = 40

def background(x, y):
    t = int(y / (H - 1) * (GRADIENT_STEPS - 1)) / (GRADIENT_STEPS - 1)
    return mix(TOP, BOTTOM, t)

img = [[background(x, y) for x in range(W)] for y in range(H)]

def coverage_text(text, scale, slant, ss=4):
    """how much of each pixel the text covers (0..1), drawn at `scale` pixels per font pixel"""
    gw = 8 * scale
    width, height = gw * len(text) + int(8 * scale * slant) + 2, 8 * scale
    cov = [[0.0] * width for _ in range(height)]
    for py in range(height):
        for px in range(width):
            hits = 0
            for sy in range(ss):
                for sx in range(ss):
                    fy = (py + (sy + 0.5) / ss) / scale
                    fx = (px + (sx + 0.5) / ss) / scale - (8 - fy) * slant   # italic: lean right
                    ci = int(fx // 8)
                    if 0 <= ci < len(text) and 0 <= fy < 8:
                        rows = FONT.get(text[ci], FONT[' '])
                        gx, gy = int(fx - ci * 8), int(fy)
                        if gx < 7 and rows[gy][gx] == '#':
                            hits += 1
            cov[py][px] = hits / (ss * ss)
    return cov

def paint(cov, x0, y0, colour, levels=8, dx=0, dy=0):
    for py, row in enumerate(cov):
        for px, a in enumerate(row):
            if a <= 0:
                continue
            a = round(a * levels) / levels                 # a few levels: few colours
            x, y = x0 + px + dx, y0 + py + dy
            if 0 <= x < W and 0 <= y < H:
                img[y][x] = mix(background(x, y), colour, a)

def glow(cx, cy, rx, ry, colour, strength):
    for y in range(max(0, cy - ry), min(H, cy + ry)):
        for x in range(max(0, cx - rx), min(W, cx + rx)):
            d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
            if d < 1:
                a = round((1 - d) * strength * 6) / 6
                img[y][x] = mix(img[y][x], colour, a)

NAVY, RED, SHADOW, GREY = (0x1E, 0x2A, 0x58), (0xD0, 0x2C, 0x28), (0xA8, 0xAA, 0xB2), (0x70, 0x74, 0x80)

amiga, mac = coverage_text('Amiga', 7, 0.28), coverage_text('Mac', 7, 0.28)
x0, y0 = 1280 - 8 * 56 - 90, 330
paint(amiga, x0, y0, SHADOW, dx=4, dy=4)
paint(mac, x0 + 5 * 56, y0, SHADOW, dx=4, dy=4)
paint(amiga, x0, y0, NAVY)
paint(mac, x0 + 5 * 56, y0, RED)
sub = coverage_text('Workbench', 2, 0.25)
paint(sub, x0 + 8 * 56 - 9 * 16, y0 + 70, GREY, levels=4)
for x in range(x0 + 10, x0 + 8 * 56 + 30):                     # a thin red line under the name
    for y in (y0 + 62, y0 + 63):
        t = (x - x0) / (8 * 56 + 20)
        img[y][x] = mix(img[y][x], RED, max(0.0, 1 - abs(t - 0.5) * 2) * 0.9)

# ---- ILBM ---------------------------------------------------------------------------------

# at most 96 colours (the icons need pens too): the most frequent ones, the others go to their nearest
from collections import Counter
freq = Counter(c for row in img for c in row)
keep = [c for c, _ in freq.most_common(96)]
near = {}
for c in freq:
    if c not in near:
        near[c] = c if c in set(keep) else min(keep, key=lambda k: sum((k[i] - c[i]) ** 2 for i in range(3)))
img = [[near[c] for c in row] for row in img]

palette, index = [], {}
pens = []
for row in img:
    for c in row:
        if c not in index:
            index[c] = len(palette)
            palette.append(c)
        pens.append(index[c])
assert len(palette) <= 256, len(palette)
depth = max(1, (len(palette) - 1).bit_length())

def byterun1(data):
    out, i, n = bytearray(), 0, len(data)
    while i < n:
        run = 1
        while i + run < n and run < 128 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += bytes([(257 - run) & 0xFF, data[i]]); i += run
            continue
        j = i
        while j < n and j - i < 128 and not (j + 2 < n and data[j] == data[j + 1] == data[j + 2]):
            j += 1
        out += bytes([j - i - 1]) + data[i:j]; i = j
    return bytes(out)

rowbytes = (W + 15) // 16 * 2
body = bytearray()
for y in range(H):
    rowpens = pens[y * W:(y + 1) * W]
    for plane in range(depth):
        bits = bytearray(rowbytes)
        for x, p in enumerate(rowpens):
            if (p >> plane) & 1:
                bits[x >> 3] |= 0x80 >> (x & 7)
        body += byterun1(bytes(bits))

def chunk(name, data):
    return name + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')

bmhd = struct.pack('>HHhhBBBBHBBhh', W, H, 0, 0, depth, 0, 1, 0, 0, 1, 1, W, H)
cmap = bytes(c for p in palette for c in p)
form = b'ILBM' + chunk(b'BMHD', bmhd) + chunk(b'CMAP', cmap) + chunk(b'CAMG', struct.pack('>I', 0x50031000)) + chunk(b'BODY', bytes(body))
pic = os.path.join(SYS, 'Prefs', 'Presets', 'Backdrops', 'AmigaMac.iff')
os.makedirs(os.path.dirname(pic), exist_ok=True)
open(pic, 'wb').write(b'FORM' + struct.pack('>I', len(form)) + form)

# wbpattern.prefs: the desktop shows that picture, scaled to the screen
name = b'SYS:Prefs/Presets/Backdrops/AmigaMac.iff'
ptrn = struct.pack('>IIIIHHbbH', 0, 0, 0, 0, 0, 0x2000, 0, 8, len(name)) + name   # WBP_ROOT, WBPF_PLACEMENT_SCALE
prhd = struct.pack('>BBI', 0, 0, 0)
form = b'PREF' + chunk(b'PRHD', prhd) + chunk(b'PTRN', ptrn)
prefs = os.path.join(SYS, 'Prefs', 'Env-Archive', 'Sys', 'wbpattern.prefs')
open(prefs, 'wb').write(b'FORM' + struct.pack('>I', len(form)) + form)
print('%s : %dx%d, %d couleurs, %d octets' % (os.path.relpath(pic, SYS), W, H, len(palette), os.path.getsize(pic)))
