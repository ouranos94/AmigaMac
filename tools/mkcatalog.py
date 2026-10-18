#!/usr/bin/env python3
"""mkcatalog.py source.txt out.catalog language version
source.txt: one "ID text" per line (UTF-8, written as Latin-1, \\n allowed).
Writes an IFF CTLG catalog for locale.library."""
import struct, sys
src, out, lang, ver = sys.argv[1:5]
name = out.rsplit('/', 1)[-1]
def chunk(id, body):
    body = bytes(body)
    return id + struct.pack('>I', len(body)) + body + (b'\0' if len(body) & 1 else b'')
strs = b''
for line in open(src, encoding='utf-8'):
    line = line.rstrip('\n')
    if not line or line.startswith('#'): continue
    i, text = line.split(' ', 1)
    t = text.replace('\\n', '\n').encode('latin-1')
    strs += struct.pack('>II', int(i), len(t)) + t + b'\0' * (4 - len(t) % 4)
body = b'CTLG' + chunk(b'FVER', ('$VER: %s %s (17.10.2026)\0' % (name, ver)).encode('latin-1'))
body += chunk(b'LANG', lang.encode('latin-1') + b'\0') + chunk(b'CSET', b'\0' * 32) + chunk(b'STRS', strs)
open(out, 'wb').write(b'FORM' + struct.pack('>I', len(body)) + body)
