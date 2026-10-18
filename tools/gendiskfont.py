#!/usr/bin/env python3
"""Build an Amiga bitmap disk font from the AmigaMac 8x8 font, rows doubled:
System/Fonts/amigamac.font (FontContentsHeader) + System/Fonts/amigamac/16
(a hunk file: MOVEQ #0,D0 / RTS, DiskFontHeader, TextFont, glyph data)."""
import os, re, struct
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..')
glyphs = []
for line in open(os.path.join(root, 'Sources/CAmiga/font8.c')):
    m = re.match(r'\s*\{((?:0x[0-9A-Fa-f]{2},?\s*){8})\}', line)
    if m:
        glyphs.append([int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', m.group(1))])
assert len(glyphs) == 256
off = {}
for line in open(os.path.join(root, 'Sources/CAmiga/include/ndk_offsets.h')):
    m = re.match(r'#define (\w+) (\d+)', line)
    if m: off[m.group(1)] = int(m.group(2))

LO, HI, H = 32, 255, 16
chars = list(range(LO, HI + 1)) + [None]            # last: the glyph for undefined characters
n = len(chars)
modulo = (n + 1) & ~1
chardata = bytearray(modulo * H)
for i, ch in enumerate(chars):
    g = glyphs[ch] if ch is not None else [0x7E, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7E, 0]
    for r in range(H):
        chardata[r * modulo + i] = g[r // 2]
charloc = b''.join(struct.pack('>HH', i * 8, 8) for i in range(n))

DFH = 4                                             # after MOVEQ/RTS
TF = DFH + off['DiskFontHeader_dfh_TF']
NAME = DFH + off['DiskFontHeader_dfh_Name']
data = bytearray(TF + off['TextFont_SIZEOF'])
data[0:4] = b'\x70\x00\x4e\x75'
relocs = []
def ptr(at, target):
    struct.pack_into('>I', data, at, target)
    relocs.append(at)
data[DFH + 8] = 12                                  # ln_Type NT_FONT
ptr(DFH + 10, NAME)
struct.pack_into('>HHI', data, DFH + off['DiskFontHeader_dfh_FileID'], 0x0f80, 1, 0)
data[NAME:NAME + 9] = b'amigamac\0'
data[TF + 8] = 12
ptr(TF + 10, NAME)
struct.pack_into('>H', data, TF + off['TextFont_tf_YSize'], H)
data[TF + off['TextFont_tf_Style']] = 0
data[TF + off['TextFont_tf_Flags']] = 0x42           # FPF_DISKFONT | FPF_DESIGNED
struct.pack_into('>H', data, TF + off['TextFont_tf_XSize'], 8)
struct.pack_into('>H', data, TF + off['TextFont_tf_Baseline'], 13)
struct.pack_into('>H', data, TF + off['TextFont_tf_BoldSmear'], 1)
data[TF + off['TextFont_tf_LoChar']] = LO
data[TF + off['TextFont_tf_HiChar']] = HI
struct.pack_into('>H', data, TF + off['TextFont_tf_Modulo'], modulo)
cd = len(data)
data += chardata
cl = len(data)
data += charloc
ptr(TF + off['TextFont_tf_CharData'], cd)
ptr(TF + off['TextFont_tf_CharLoc'], cl)
while len(data) % 4: data.append(0)

hunk = struct.pack('>IIIIII', 0x3F3, 0, 1, 0, 0, len(data) // 4)
hunk += struct.pack('>II', 0x3E9, len(data) // 4) + bytes(data)
hunk += struct.pack('>III', 0x3EC, len(relocs), 0) + b''.join(struct.pack('>I', r) for r in sorted(relocs)) + struct.pack('>I', 0)
hunk += struct.pack('>I', 0x3F2)
fonts = os.path.join(root, 'System/Fonts')
os.makedirs(os.path.join(fonts, 'amigamac'), exist_ok=True)
open(os.path.join(fonts, 'amigamac', '16'), 'wb').write(hunk)
fc = b'amigamac/16'.ljust(256, b'\0') + struct.pack('>HBB', H, 0, 0x42)
open(os.path.join(fonts, 'amigamac.font'), 'wb').write(struct.pack('>HH', 0x0f00, 1) + fc)
print('amigamac.font: %d glyphs, %d bytes' % (n, len(hunk)))
