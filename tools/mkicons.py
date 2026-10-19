#!/usr/bin/env python3
"""mkicons.py: AmigaMac's own Workbench icons, drawn here (original designs).

Each .info holds a classic 4-colour image (for old programs) and the colour
images of AmigaOS 3.5+ (an IFF FORM ICON: FACE + IMAG, RLE-packed pens and a
palette). Run from the repository root: python3 tools/mkicons.py
"""
import os, struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'System')

# ---- a tiny raster ------------------------------------------------------------------

class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[None] * w for _ in range(h)]

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h and c is not None:
            self.px[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.put(x, y, c)

    def vgrad(self, x0, y0, x1, y1, c0, c1):
        for y in range(y0, y1 + 1):
            t = (y - y0) / max(1, y1 - y0)
            self.rect(x0, y, x1, y, mix(c0, c1, t))

    def hgrad(self, x0, y0, x1, y1, c0, c1):
        for x in range(x0, x1 + 1):
            t = (x - x0) / max(1, x1 - x0)
            self.rect(x, y0, x, y1, mix(c0, c1, t))

    def frame(self, x0, y0, x1, y1, c):
        self.rect(x0, y0, x1, y0, c); self.rect(x0, y1, x1, y1, c)
        self.rect(x0, y0, x0, y1, c); self.rect(x1, y0, x1, y1, c)

    def bevel(self, x0, y0, x1, y1, light, dark):
        self.rect(x0, y0, x1, y0, light); self.rect(x0, y0, x0, y1, light)
        self.rect(x0, y1, x1, y1, dark); self.rect(x1, y0, x1, y1, dark)

    def ellipse(self, cx, cy, rx, ry, c, c2=None):
        for y in range(int(cy - ry), int(cy + ry) + 1):
            for x in range(int(cx - rx), int(cx + rx) + 1):
                d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
                if d <= 1:
                    self.put(x, y, c if c2 is None else mix(c, c2, min(1, (y - (cy - ry)) / (2 * ry))))

    def poly(self, pts, c):
        ys = [p[1] for p in pts]
        for y in range(min(ys), max(ys) + 1):
            xs = []
            for i in range(len(pts)):
                (xa, ya), (xb, yb) = pts[i], pts[(i + 1) % len(pts)]
                if (ya <= y < yb) or (yb <= y < ya):
                    xs.append(xa + (y - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(round(xs[i])), int(round(xs[i + 1])) + 1):
                    self.put(x, y, c)

    def line(self, x0, y0, x1, y1, c):
        n = max(abs(x1 - x0), abs(y1 - y0), 1)
        for i in range(n + 1):
            self.put(round(x0 + (x1 - x0) * i / n), round(y0 + (y1 - y0) * i / n), c)

    def shadow(self, c=(0x30, 0x38, 0x50), dx=2, dy=2):
        """a soft drop shadow under what is drawn"""
        under = [[self.px[y][x] is not None for x in range(self.w)] for y in range(self.h)]
        for y in range(self.h):
            for x in range(self.w):
                sx, sy = x - dx, y - dy
                if self.px[y][x] is None and 0 <= sx < self.w and 0 <= sy < self.h and under[sy][sx]:
                    self.px[y][x] = c

def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))

def rgb(h):
    return ((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF)

OUT = rgb(0x18223C)          # outlines
WHITE = rgb(0xF8FBFF)

# ---- the icons ----------------------------------------------------------------------

def drawer(badge=None):
    c = Canvas(52, 40) if badge else Canvas(48, 36)
    c.poly([(4, 8), (40, 8), (45, 3), (9, 3)], rgb(0xB8CCF4))                 # top, in perspective
    c.poly([(40, 8), (45, 3), (45, 28), (40, 33)], rgb(0x3A58B0))             # side
    c.vgrad(4, 8, 40, 33, rgb(0x82A2F0), rgb(0x3F62C8))                      # front
    c.frame(4, 8, 40, 33, OUT)
    c.line(9, 3, 45, 3, OUT); c.line(4, 8, 9, 3, OUT); c.line(45, 3, 45, 28, OUT); c.line(40, 33, 45, 28, OUT)
    c.rect(5, 9, 39, 9, rgb(0xC8D8FF))
    c.vgrad(14, 17, 30, 23, rgb(0xF4F6FA), rgb(0x9AA4B4))                    # handle
    c.frame(14, 17, 30, 23, OUT)
    c.rect(17, 20, 27, 20, rgb(0x404858))
    if badge:
        badge(c, 37, 26)                                       # an emblem on the corner
    c.shadow()
    return c

# ---- emblems for the system drawers (centre cx, cy, about 15 pixels) ----------------------

def badge_disc(c, cx, cy, top, bottom):
    c.ellipse(cx, cy, 7.5, 7.5, OUT)
    c.ellipse(cx, cy, 6.5, 6.5, top, bottom)

def b_prefs(c, cx, cy):                                         # sliders
    badge_disc(c, cx, cy, rgb(0x90B0FF), rgb(0x3050C0))
    for i, k in enumerate((-2, 1, -1)):
        x = cx - 3 + i * 3
        c.rect(x, cy - 4, x, cy + 4, WHITE)
        c.rect(x - 1, cy + k, x + 1, cy + k + 1, rgb(0xFFD040))

def b_system(c, cx, cy):                                        # a chip
    c.rect(cx - 6, cy - 6, cx + 6, cy + 6, OUT)
    c.vgrad(cx - 5, cy - 5, cx + 5, cy + 5, rgb(0x505868), rgb(0x20242C))
    for k in range(-4, 5, 3):
        for (x, y) in ((cx + k, cy - 7), (cx + k, cy + 7), (cx - 7, cy + k), (cx + 7, cy + k)):
            c.put(x, y, rgb(0xE8C040))
    c.rect(cx - 2, cy - 2, cx + 2, cy + 2, rgb(0x80B0FF))

def b_tools(c, cx, cy):                                         # a wrench
    badge_disc(c, cx, cy, rgb(0xFFD070), rgb(0xD08820))
    c.line(cx - 4, cy + 4, cx + 3, cy - 3, rgb(0x404650)); c.line(cx - 3, cy + 4, cx + 4, cy - 3, rgb(0x404650))
    c.ellipse(cx + 3, cy - 3, 2.2, 2.2, rgb(0x404650)); c.put(cx + 4, cy - 4, rgb(0xFFD070))

def b_utilities(c, cx, cy):                                     # a calculator
    c.rect(cx - 6, cy - 7, cx + 6, cy + 7, OUT)
    c.vgrad(cx - 5, cy - 6, cx + 5, cy + 6, rgb(0xF0ECE0), rgb(0xC0B8A4))
    c.rect(cx - 4, cy - 5, cx + 4, cy - 3, rgb(0x60A070))
    for y in (cy, cy + 3):
        for x in (cx - 4, cx - 1, cx + 2):
            c.rect(x, y, x + 1, y + 1, rgb(0x505868))

def b_wbstartup(c, cx, cy):                                     # power on
    badge_disc(c, cx, cy, rgb(0x80F090), rgb(0x20A040))
    c.ellipse(cx, cy + 1, 4, 4, WHITE); c.ellipse(cx, cy + 1, 2.6, 2.6, rgb(0x40C060))
    c.rect(cx - 1, cy - 5, cx + 1, cy + 1, rgb(0x40C060)); c.rect(cx, cy - 5, cx, cy, WHITE)

def b_storage(c, cx, cy):                                       # a box
    c.poly([(cx - 7, cy - 2), (cx + 7, cy - 2), (cx + 7, cy + 7), (cx - 7, cy + 7)], rgb(0xC08A50))
    c.poly([(cx - 7, cy - 2), (cx - 3, cy - 6), (cx + 10, cy - 6), (cx + 7, cy - 2)], rgb(0xE0B078))
    c.frame(cx - 7, cy - 2, cx + 7, cy + 7, OUT)
    c.rect(cx - 2, cy - 2, cx + 1, cy + 2, rgb(0xF0D8A8))

def b_expansion(c, cx, cy):                                     # a plus
    badge_disc(c, cx, cy, rgb(0xFFF080), rgb(0xE0A820))
    c.rect(cx - 4, cy - 1, cx + 4, cy + 1, WHITE); c.rect(cx - 1, cy - 4, cx + 1, cy + 4, WHITE)

def b_devs(c, cx, cy):                                          # a plug
    badge_disc(c, cx, cy, rgb(0xD0D6E0), rgb(0x8890A0))
    c.rect(cx - 3, cy - 5, cx - 2, cy - 2, rgb(0x404650)); c.rect(cx + 2, cy - 5, cx + 3, cy - 2, rgb(0x404650))
    c.rect(cx - 4, cy - 2, cx + 4, cy + 2, rgb(0x404650)); c.rect(cx - 1, cy + 2, cx + 1, cy + 5, rgb(0x404650))

def b_fonts(c, cx, cy):                                         # an A
    badge_disc(c, cx, cy, WHITE, rgb(0xC8D0E0))
    c.line(cx - 4, cy + 4, cx, cy - 5, rgb(0xC03030)); c.line(cx - 3, cy + 4, cx + 1, cy - 5, rgb(0xC03030))
    c.line(cx, cy - 5, cx + 4, cy + 4, rgb(0xC03030)); c.line(cx + 1, cy - 5, cx + 5, cy + 4, rgb(0xC03030))
    c.rect(cx - 2, cy + 1, cx + 3, cy + 1, rgb(0xC03030))

def b_locale(c, cx, cy):                                        # a globe
    badge_disc(c, cx, cy, rgb(0x80C0FF), rgb(0x2060C0))
    c.ellipse(cx - 2, cy - 1, 2.5, 3.5, rgb(0x50C060)); c.ellipse(cx + 3, cy + 2, 2.2, 2.5, rgb(0x50C060))
    c.rect(cx - 6, cy, cx + 6, cy, rgb(0xC0E0FF))

def harddisk(label_colour=rgb(0x5A80E0)):
    c = Canvas(48, 32)
    c.poly([(3, 9), (39, 9), (45, 4), (9, 4)], rgb(0xF0F2F6))
    c.poly([(39, 9), (45, 4), (45, 22), (39, 27)], rgb(0x8E949E))
    c.vgrad(3, 9, 39, 27, rgb(0xE0E4EA), rgb(0x9AA0AA))
    c.frame(3, 9, 39, 27, OUT)
    c.line(9, 4, 45, 4, OUT); c.line(3, 9, 9, 4, OUT); c.line(45, 4, 45, 22, OUT); c.line(39, 27, 45, 22, OUT)
    c.rect(6, 12, 24, 15, label_colour)                                        # label stripe
    c.frame(6, 12, 24, 15, OUT)
    for x in range(7, 24, 3):
        c.rect(x, 20, x + 1, 24, rgb(0x6A707A))                                # vents
    c.ellipse(34, 22, 2, 2, rgb(0x40E060))                                     # activity light
    c.put(33, 21, rgb(0xC0FFC8))
    c.shadow()
    return c

def floppy():
    c = Canvas(40, 38)
    body0, body1 = rgb(0x4A5878), rgb(0x2A3450)
    c.poly([(3, 2), (32, 2), (37, 7), (37, 34), (3, 34)], body0)             # the case, one corner cut
    c.vgrad(4, 3, 36, 33, body0, body1)
    c.line(3, 2, 32, 2, OUT); c.line(32, 2, 37, 7, OUT); c.line(37, 7, 37, 34, OUT)
    c.line(3, 34, 37, 34, OUT); c.line(3, 2, 3, 34, OUT)
    c.rect(4, 3, 31, 3, rgb(0x7888A8))
    c.vgrad(11, 3, 28, 13, rgb(0xE4E8EE), rgb(0x9CA4B0))                     # the metal shutter
    c.frame(11, 3, 28, 13, OUT)
    c.rect(22, 5, 25, 11, rgb(0x2A3040))                                      # its window
    c.vgrad(7, 18, 33, 33, rgb(0xFAFAF6), rgb(0xDCDAD2))                     # the label
    c.frame(7, 18, 33, 33, rgb(0x6A7080))
    for y in (22, 25, 28):
        c.rect(10, y, 30, y, rgb(0x9AA6C8))                                   # lines on it
    c.rect(10, 19, 30, 19, rgb(0xE05A40))
    c.rect(5, 30, 6, 32, rgb(0x161A24))                                       # write-protect hole
    c.shadow()
    return c

def fontprefs():
    c = Canvas(44, 38)
    c.vgrad(5, 3, 36, 32, rgb(0xFBFBF8), rgb(0xDAD8D0))                     # a page
    c.frame(5, 3, 36, 32, OUT)
    a = [(9, 26), (14, 8), (18, 8), (23, 26), (19, 26), (18, 22), (14, 22), (13, 26)]   # a large A
    c.poly(a, rgb(0x2A3C8C))
    c.rect(15, 17, 17, 18, rgb(0xFBFBF8))
    c.ellipse(29, 22, 4, 4, rgb(0xC03A2A))                                    # and a small a
    c.ellipse(29, 22, 2, 2, rgb(0xF2F0EA))
    c.rect(32, 18, 33, 26, rgb(0xC03A2A))
    for y in (29,):
        c.rect(9, y, 32, y, rgb(0x9AA6C8))
    c.shadow()
    return c

def ramdisk():
    c = Canvas(48, 30)
    c.vgrad(2, 4, 44, 22, rgb(0x3CA854), rgb(0x1E6E34))                      # the board
    c.frame(2, 4, 44, 22, OUT)
    for x in range(5, 42, 3):
        c.rect(x, 23, x + 1, 26, rgb(0xE8C040))                                # gold contacts
    c.rect(2, 23, 44, 23, OUT)
    for x0 in (6, 19, 32):
        c.vgrad(x0, 8, x0 + 9, 18, rgb(0x404650), rgb(0x16181E))              # chips
        c.frame(x0, 8, x0 + 9, 18, rgb(0x08090C))
        c.rect(x0 + 1, 9, x0 + 8, 9, rgb(0x6A7280))
    c.rect(3, 5, 43, 5, rgb(0x7CD890))
    c.shadow()
    return c

def macvolume():
    c = Canvas(44, 38)
    c.vgrad(5, 2, 38, 26, rgb(0xF2EEE6), rgb(0xC4BEB2))                      # a computer's screen case
    c.frame(5, 2, 38, 26, OUT)
    c.vgrad(9, 5, 34, 21, rgb(0x6E9CF8), rgb(0x2A4CB8))                      # the screen
    c.frame(9, 5, 34, 21, rgb(0x404040))
    c.rect(10, 6, 33, 6, rgb(0xB0CCFF))
    c.ellipse(21, 13, 5, 4, rgb(0xF4F8FF))                                    # a little cloud
    c.ellipse(26, 12, 4, 3, rgb(0xF4F8FF))
    c.rect(18, 27, 25, 30, rgb(0xA8A296)); c.frame(18, 27, 25, 30, OUT)      # the stand
    c.vgrad(10, 31, 33, 34, rgb(0xE6E0D6), rgb(0xA8A296)); c.frame(10, 31, 33, 34, OUT)
    c.shadow()
    return c

def trashcan():
    c = Canvas(34, 40)
    c.vgrad(6, 9, 27, 35, rgb(0xC8CED8), rgb(0x7C8492))                      # the bin
    c.frame(6, 9, 27, 35, OUT)
    for x in (11, 16, 21):
        c.rect(x, 12, x + 1, 32, rgb(0x5C6470))
        c.rect(x + 2, 12, x + 2, 32, rgb(0xE6EAF0))
    c.vgrad(3, 4, 30, 8, rgb(0xE6EAF0), rgb(0x9AA2AE))                       # the lid
    c.frame(3, 4, 30, 8, OUT)
    c.rect(13, 1, 20, 3, rgb(0x9AA2AE)); c.frame(13, 1, 20, 3, OUT)
    c.shadow()
    return c

def tool():
    c = Canvas(40, 38)
    import math
    for a in range(12):                                                       # a gear
        ang = a * math.pi / 6
        x, y = 18 + 13 * math.cos(ang), 17 + 13 * math.sin(ang)
        c.ellipse(x, y, 3.2, 3.2, rgb(0xF4B040))
    c.ellipse(18, 17, 12, 12, rgb(0xFFCC60), rgb(0xD07818))
    c.ellipse(18, 17, 5, 5, rgb(0x40301A))
    c.ellipse(18, 17, 3, 3, (0xA0, 0x80, 0x50))
    c.poly([(22, 22), (25, 19), (37, 31), (34, 34)], rgb(0xC8CED8))          # a screwdriver across it
    c.poly([(30, 27), (33, 24), (38, 29), (35, 32)], rgb(0xE04838))
    c.line(22, 22, 25, 19, OUT)
    c.shadow()
    return c

def project():
    c = Canvas(34, 40)
    c.poly([(4, 2), (23, 2), (30, 9), (30, 36), (4, 36)], WHITE)              # a page
    c.vgrad(4, 2, 30, 36, rgb(0xFFFFFF), rgb(0xD8E0EC))
    c.poly([(23, 2), (30, 9), (23, 9)], rgb(0xB8C4D8))                        # the folded corner
    c.line(4, 2, 23, 2, OUT); c.line(23, 2, 30, 9, OUT); c.line(30, 9, 30, 36, OUT)
    c.line(4, 36, 30, 36, OUT); c.line(4, 2, 4, 36, OUT); c.line(23, 2, 23, 9, OUT); c.line(23, 9, 30, 9, OUT)
    c.rect(8, 7, 19, 9, rgb(0x5A80E0))
    for y in range(14, 33, 4):
        c.rect(8, y, 26 if y % 8 else 21, y, rgb(0x8C96A8))
    c.shadow()
    return c

def shell():
    c = Canvas(46, 36)
    c.rect(2, 2, 42, 31, rgb(0x6A88E8))                                       # a Shell window
    c.frame(2, 2, 42, 31, OUT)
    c.rect(3, 3, 41, 3, rgb(0xB8CCFF))
    c.rect(4, 4, 7, 7, WHITE); c.frame(4, 4, 7, 7, OUT)
    c.vgrad(5, 9, 39, 29, rgb(0x20242C), rgb(0x0C0E12))
    c.frame(5, 9, 39, 29, OUT)
    for (x, y) in [(8, 12), (8, 13), (8, 14), (8, 15), (8, 16), (7, 13)]:      # "1>"
        c.put(x, y, rgb(0x60F080))
    c.line(11, 12, 14, 14, rgb(0x60F080)); c.line(14, 14, 11, 16, rgb(0x60F080))
    c.rect(17, 15, 20, 16, rgb(0xF0F0F0))                                     # the cursor
    c.shadow()
    return c

def screenmode():
    c = Canvas(44, 38)
    c.vgrad(3, 2, 40, 27, rgb(0x505664), rgb(0x22252C))                      # a monitor
    c.frame(3, 2, 40, 27, OUT)
    c.vgrad(7, 5, 36, 23, rgb(0x78C0FF), rgb(0x2050C0))
    c.rect(7, 18, 36, 23, rgb(0x3CA854))                                      # hills
    c.ellipse(15, 18, 9, 4, rgb(0x52C46C))
    c.line(10, 8, 33, 8, rgb(0xFFE070)); c.line(10, 8, 12, 6, rgb(0xFFE070)); c.line(10, 8, 12, 10, rgb(0xFFE070))
    c.line(33, 8, 31, 6, rgb(0xFFE070)); c.line(33, 8, 31, 10, rgb(0xFFE070))   # <-> : a resolution
    c.rect(18, 28, 25, 31, rgb(0x3A3E48)); c.vgrad(11, 32, 32, 34, rgb(0x6A707C), rgb(0x2E323A))
    c.frame(11, 32, 32, 34, OUT)
    c.shadow()
    return c

# ---- writing .info files --------------------------------------------------------------

WB = [rgb(0xA8B8D0), rgb(0x000000), rgb(0xF4F8FF), rgb(0x6A88E8)]   # the classic pens

def classic_planes(cv):
    words = (cv.w + 15) // 16
    planes = bytearray()
    pens = []
    for y in range(cv.h):
        row = []
        for x in range(cv.w):
            p = cv.px[y][x]
            if p is None:
                row.append(0)
            else:
                row.append(min(range(4), key=lambda i: sum((p[k] - WB[i][k]) ** 2 for k in range(3))))
        pens.append(row)
    for plane in range(2):
        for row in pens:
            bits = [(v >> plane) & 1 for v in row] + [0] * (words * 16 - cv.w)
            for i in range(0, len(bits), 8):
                planes.append(int(''.join(map(str, bits[i:i + 8])), 2))
    return bytes(planes)

class BitWriter:
    def __init__(self):
        self.bits = []
    def put(self, v, n):
        self.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]
    def data(self):
        b = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int(''.join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))

def pack(values, bits):
    """the RLE of colour icons: runs as 257-n, literals as n-1 (n <= 128)"""
    w, i, n = BitWriter(), 0, len(values)
    while i < n:
        run = 1
        while i + run < n and run < 128 and values[i + run] == values[i]:
            run += 1
        if run >= 3:
            w.put(257 - run, 8); w.put(values[i], bits); i += run
            continue
        j = i
        while j < n and j - i < 128 and not (j + 2 < n and values[j] == values[j + 1] == values[j + 2]):
            j += 1
        w.put(j - i - 1, 8)
        for v in values[i:j]:
            w.put(v, bits)
        i = j
    return w.data()

def chunk(name, data):
    return name + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')

def colour_form(cv):
    pal, index = [], {}
    pix = []
    for y in range(cv.h):
        for x in range(cv.w):
            p = cv.px[y][x]
            if p is None:
                pix.append(0)
                continue
            if p not in index:
                index[p] = len(pal) + 1
                pal.append(p)
            pix.append(index[p])
    ncol = len(pal) + 1                                    # pen 0: transparent
    assert ncol <= 256, 'too many colours'
    depth = max(1, (ncol - 1).bit_length())
    img = pack(pix, depth)
    flat = [0, 0, 0] + [c for p in pal for c in p]
    palette = pack(flat, 8)
    face = struct.pack('>BBBBH', cv.w - 1, cv.h - 1, 1, 0x11, ncol * 3 - 1)   # frameless
    imag = struct.pack('>BBBBBBHH', 0, ncol - 1, 3, 1, 1, depth, len(img) - 1, len(palette) - 1) + img + palette
    body = b'ICON' + chunk(b'FACE', face) + chunk(b'IMAG', imag)
    return b'FORM' + struct.pack('>I', len(body)) + body

def write_info(path, cv, kind, drawer=False, tool=None):
    words = (cv.w + 15) // 16
    gadget = struct.pack('>IhhhhHHHIIIIIHI', 0, 0, 0, cv.w, cv.h + 1, 0x0004, 0x0003, 0x0001, 1, 0, 0, 0, 0, 0, 1 if drawer else 0)
    disk = struct.pack('>HH', 0xE310, 1) + gadget + struct.pack('>BBIIiiIII', kind, 0, 1 if tool else 0, 0,
           -0x80000000, -0x80000000, 1 if drawer else 0, 0, 4096)
    out = disk
    if drawer:
        out += struct.pack('>hhhhBBIIIIIIIhhhhH', 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) + struct.pack('>ii', 0, 0)
    out += struct.pack('>hhhhhIBBI', 0, 0, cv.w, cv.h, 2, 1, 3, 0, 0) + classic_planes(cv)
    if tool:
        t = tool.encode('latin-1') + b'\0'
        out += struct.pack('>I', len(t)) + t
    if drawer:
        out += struct.pack('>IH', 0, 0)
    out += colour_form(cv)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(out)
    print('%-44s %2dx%2d' % (os.path.relpath(path, ROOT), cv.w, cv.h))

WBDISK, WBDRAWER, WBTOOL, WBPROJECT, WBGARBAGE = 1, 2, 3, 4, 5
ENVARC = os.path.join(ROOT, 'Prefs', 'Env-Archive', 'Sys')

if __name__ == '__main__':
    write_info(os.path.join(ENVARC, 'def_drawer.info'), drawer(), WBDRAWER, drawer=True)
    write_info(os.path.join(ENVARC, 'def_disk.info'), harddisk(), WBDISK, drawer=True)
    write_info(os.path.join(ENVARC, 'def_RAM.info'), ramdisk(), WBDISK, drawer=True)
    write_info(os.path.join(ENVARC, 'def_floppy.info'), floppy(), WBDISK, drawer=True)
    write_info(os.path.join(ENVARC, 'def_Mac.info'), macvolume(), WBDISK, drawer=True)
    write_info(os.path.join(ENVARC, 'def_trashcan.info'), trashcan(), WBGARBAGE, drawer=True)
    write_info(os.path.join(ENVARC, 'def_tool.info'), tool(), WBTOOL)
    write_info(os.path.join(ENVARC, 'def_project.info'), project(), WBPROJECT)
    write_info(os.path.join(ROOT, 'Disk.info'), harddisk(), WBDISK, drawer=True)
    for name, b in (('Prefs', b_prefs), ('System', b_system), ('Tools', b_tools), ('Utilities', b_utilities),
                    ('WBStartup', b_wbstartup), ('Storage', b_storage), ('Expansion', b_expansion),
                    ('Devs', b_devs), ('Fonts', b_fonts), ('Locale', b_locale)):
        write_info(os.path.join(ROOT, name + '.info'), drawer(b), WBDRAWER, drawer=True)
    write_info(os.path.join(ROOT, 'Trashcan.info'), trashcan(), WBGARBAGE, drawer=True)
    write_info(os.path.join(ROOT, 'System', 'Shell.info'), shell(), WBTOOL)
    write_info(os.path.join(ROOT, 'Prefs', 'ScreenMode.info'), screenmode(), WBTOOL)
    write_info(os.path.join(ROOT, 'Prefs', 'Font.info'), fontprefs(), WBTOOL)
