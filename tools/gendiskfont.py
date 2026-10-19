#!/usr/bin/env python3
"""AmigaMac's own bitmap disk fonts, built from its 8x8 font (Sources/CAmiga/font8.c):

  amigamac  8, 16   fixed width (16: rows doubled), for the system text and Shells
  amigasans 8, 16   proportional: each glyph trimmed to its ink, one pixel between

Each family is System/Fonts/<name>.font (a FontContentsHeader listing the sizes)
and System/Fonts/<name>/<size>, a hunk file: MOVEQ #0,D0 / RTS, a DiskFontHeader
holding the TextFont, then the glyph data. Run from anywhere: python3 tools/gendiskfont.py
"""
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

LO, HI = 32, 255
UNDEF = [0x7E, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7E, 0]          # the glyph for characters the font lacks
FPF_PROPORTIONAL, FPF_DISKFONT, FPF_DESIGNED = 0x20, 0x02, 0x40


def build(name, height, proportional):
    rows_per = height // 8                                       # 16: each row twice
    chars = list(range(LO, HI + 1)) + [None]
    cells = []                                                   # (rows of bit strings, advance)
    for ch in chars:
        g = glyphs[ch] if ch is not None else UNDEF
        cols = [x for x in range(8) if any(r & (0x80 >> x) for r in g)]
        if proportional:
            if not cols:                                         # space and the like
                left, width, adv = 0, 0, 4
            else:
                left, right = min(cols), max(cols)
                width, adv = right - left + 1, right - left + 2
        else:
            left, width, adv = 0, 8, 8
        bits = [''.join('1' if r & (0x80 >> (left + x)) else '0' for x in range(width)) for r in g]
        cells.append((bits, width, adv))
    total = sum(c[1] for c in cells)
    modulo = ((total + 15) // 16) * 2                            # bytes a row, even
    chardata = bytearray(modulo * height)
    charloc, charspace, at = b'', b'', 0
    for bits, width, adv in cells:
        for r in range(height):
            src = bits[r // rows_per]
            for x, b in enumerate(src):
                if b == '1':
                    p = at + x
                    chardata[r * modulo + p // 8] |= 0x80 >> (p % 8)
        charloc += struct.pack('>HH', at, width)
        charspace += struct.pack('>h', adv)
        at += width

    DFH = 4                                                      # after MOVEQ/RTS
    TF = DFH + off['DiskFontHeader_dfh_TF']
    NAME = DFH + off['DiskFontHeader_dfh_Name']
    data = bytearray(TF + off['TextFont_SIZEOF'])
    data[0:4] = b'\x70\x00\x4e\x75'
    relocs = []
    def ptr(at, target):
        struct.pack_into('>I', data, at, target)
        relocs.append(at)
    flags = FPF_DISKFONT | FPF_DESIGNED | (FPF_PROPORTIONAL if proportional else 0)
    data[DFH + 8] = 12                                           # ln_Type NT_FONT
    ptr(DFH + 10, NAME)
    struct.pack_into('>HHI', data, DFH + off['DiskFontHeader_dfh_FileID'], 0x0f80, 1, 0)
    data[NAME:NAME + len(name) + 1] = name.encode() + b'\0'
    data[TF + 8] = 12
    ptr(TF + 10, NAME)
    struct.pack_into('>H', data, TF + off['TextFont_tf_YSize'], height)
    data[TF + off['TextFont_tf_Style']] = 0
    data[TF + off['TextFont_tf_Flags']] = flags
    struct.pack_into('>H', data, TF + off['TextFont_tf_XSize'], 8)
    struct.pack_into('>H', data, TF + off['TextFont_tf_Baseline'], 6 if height == 8 else 13)
    struct.pack_into('>H', data, TF + off['TextFont_tf_BoldSmear'], 1)
    data[TF + off['TextFont_tf_LoChar']] = LO
    data[TF + off['TextFont_tf_HiChar']] = HI
    struct.pack_into('>H', data, TF + off['TextFont_tf_Modulo'], modulo)
    cd = len(data); data += chardata
    cl = len(data); data += charloc
    ptr(TF + off['TextFont_tf_CharData'], cd)
    ptr(TF + off['TextFont_tf_CharLoc'], cl)
    if proportional:
        cs = len(data); data += charspace
        ptr(TF + off['TextFont_tf_CharSpace'], cs)
    while len(data) % 4: data.append(0)
    hunk = struct.pack('>IIIIII', 0x3F3, 0, 1, 0, 0, len(data) // 4)
    hunk += struct.pack('>II', 0x3E9, len(data) // 4) + bytes(data)
    hunk += struct.pack('>III', 0x3EC, len(relocs), 0) + b''.join(struct.pack('>I', r) for r in sorted(relocs)) + struct.pack('>I', 0)
    hunk += struct.pack('>I', 0x3F2)
    return hunk, flags


fonts = os.path.join(root, 'System/Fonts')
for name, prop in (('amigamac', False), ('amigasans', True)):
    os.makedirs(os.path.join(fonts, name), exist_ok=True)
    entries = b''
    sizes = (8, 16)
    for h in sizes:
        hunk, flags = build(name, h, prop)
        open(os.path.join(fonts, name, str(h)), 'wb').write(hunk)
        entries += ('%s/%d' % (name, h)).encode().ljust(256, b'\0') + struct.pack('>HBB', h, 0, flags)
        print('%s/%d : %d octets%s' % (name, h, len(hunk), ', proportionnelle' if prop else ''))
    open(os.path.join(fonts, name + '.font'), 'wb').write(struct.pack('>HH', 0x0f00, len(sizes)) + entries)
