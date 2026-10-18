#!/usr/bin/env python3
"""mkmod.py out.mod: a small original test module in the ProTracker MOD format
(4 channels: a lead, a bass, a pad, a beat), to try music players on AmigaMac."""
import math, struct, sys

def sample(kind, n):
    out = []
    for i in range(n):
        t = i / n
        if kind == 'square': v = 70 if (i % 32) < 16 else -70
        elif kind == 'saw': v = int(((i % 64) / 64 * 2 - 1) * 80)
        elif kind == 'sine': v = int(math.sin(2 * math.pi * i / 32) * 90)
        else:                                                  # a short noisy drum
            import random
            random.seed(i * 7919)
            v = int((random.random() * 2 - 1) * 110 * (1 - t) ** 3)
        out.append(v & 0xFF)
    return bytes(out)

samples = [('lead', sample('square', 32), 64, True), ('bass', sample('saw', 64), 64, True),
           ('pad', sample('sine', 32), 40, True), ('drum', sample('noise', 3000), 64, False)]

# the Amiga period of each note, octave 2 (C-2 = 428)
PER = {'C': 428, 'C#': 404, 'D': 381, 'D#': 360, 'E': 339, 'F': 320, 'F#': 302, 'G': 285, 'G#': 269, 'A': 254, 'A#': 240, 'B': 226}
def note(n, octave=2):
    p = PER[n]
    return p >> (octave - 2) if octave >= 2 else p << (2 - octave)

melody = ['E', 'G', 'A', 'G', 'E', 'D', 'C', 'D', 'E', 'G', 'A', 'B', 'A', 'G', 'E', 'D']
bass = ['C', 'C', 'A', 'A', 'F', 'F', 'G', 'G']

def cell(period, smp, effect=0, param=0):
    return struct.pack('>BBBB', (smp & 0xF0) | (period >> 8), period & 0xFF, ((smp & 0x0F) << 4) | effect, param)

patterns = []
for p in range(2):
    rows = b''
    for r in range(64):
        c1 = cell(note(melody[(r // 4) % 16], 3), 1) if r % 4 == 0 else cell(0, 0)
        c2 = cell(note(bass[(r // 8) % 8], 1), 2) if r % 8 == 0 else cell(0, 0)
        c3 = cell(note(['E', 'C', 'F', 'G'][(r // 16) % 4], 2), 3, 0xC, 0x18) if r % 16 == 0 else cell(0, 0)
        c4 = cell(note('C', 2), 4) if r % 4 == 0 or (p and r % 8 == 6) else cell(0, 0)
        rows += c1 + c2 + c3 + c4
    patterns.append(rows)

head = b'AmigaMac test'.ljust(20, b'\0')
for name, data, vol, loop in samples:
    words = len(data) // 2
    head += name.encode().ljust(22, b'\0') + struct.pack('>HBBHH', words, 0, vol, 0 if loop else 0, words if loop else 1)
for _ in range(31 - len(samples)):
    head += b'\0' * 22 + struct.pack('>HBBHH', 0, 0, 0, 0, 1)
order = [0, 1, 0, 1]
head += bytes([len(order), 127]) + bytes(order + [0] * (128 - len(order))) + b'M.K.'
mod = head + b''.join(patterns) + b''.join(d for _, d, _, _ in samples)
open(sys.argv[1], 'wb').write(mod)
print('%s : %d octets' % (sys.argv[1], len(mod)))
