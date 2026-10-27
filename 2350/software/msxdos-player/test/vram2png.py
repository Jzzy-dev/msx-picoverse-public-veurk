#!/usr/bin/env python3
# vram2png.py vram.bin out.png - draw a SCREEN 4 (pattern mode, name table
# 1800h, patterns 0000h, colours 2000h) with PICOAMP's palette
import sys
from PIL import Image

def p3(r, g, b):
    return (r * 255 // 7, g * 255 // 7, b * 255 // 7)

PAL = [p3(0, 0, 0), p3(0, 0, 0), p3(1, 1, 3), p3(2, 2, 6), p3(3, 6, 7), p3(7, 7, 7), p3(4, 4, 5), p3(0, 4, 1),
       p3(2, 7, 2), p3(7, 7, 1), p3(7, 4, 0), p3(7, 1, 1), p3(6, 2, 6), p3(2, 2, 3), p3(0, 0, 2), p3(5, 5, 6)]
v = open(sys.argv[1], 'rb').read()
im = Image.new('RGB', (256, 192))
px = im.load()
for ty in range(24):
    for tx in range(32):
        name = v[0x1800 + ty * 32 + tx]
        bank = (ty // 8) * 0x800
        for r in range(8):
            pat = v[bank + name * 8 + r]
            col = v[0x2000 + bank + name * 8 + r]
            for c in range(8):
                px[tx * 8 + c, ty * 8 + r] = PAL[(col >> 4) if (pat >> (7 - c)) & 1 else (col & 15)]
im.resize((768, 576), Image.NEAREST).save(sys.argv[2])
