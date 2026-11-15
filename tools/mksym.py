#!/usr/bin/env python3
"""Draw the component symbols of the result screen and write them as a 1-bit glyph table.

    python3 tools/mksym.py > src/sym_data.c        (needs Pillow)
    python3 tools/mksym.py --png build/symbols.png   (preview, 4x)

Each symbol is W x H pixels. Terminals (in the order of struct mres.pins): two-terminal parts left, right;
diode anode, cathode; transistors base/gate (left), collector/drain (top), emitter/source (bottom).
LABELS gives where the probe number (8x16 font) goes for each terminal, relative to the symbol's corner.
"""
import sys
from PIL import Image, ImageDraw

W, H = 48, 44
M = 22                                  # middle row: left and right leads


def new():
    im = Image.new('1', (W, H), 0)
    return im, ImageDraw.Draw(im)


def hline(d, x0, x1, y):
    d.rectangle([x0, y - 1, x1, y], fill=1)


def vline(d, x, y0, y1):
    d.rectangle([x - 1, y0, x, y1], fill=1)


def res():
    im, d = new()
    hline(d, 0, 10, M)
    d.rectangle([10, M - 7, 37, M + 6], outline=1, width=2)
    hline(d, 37, 47, M)
    return im


def cap():
    im, d = new()
    hline(d, 0, 20, M)
    d.rectangle([19, 9, 21, 35], fill=1)
    d.rectangle([26, 9, 28, 35], fill=1)
    hline(d, 28, 47, M)
    return im


def ind():
    im, d = new()
    hline(d, 0, 7, M)
    for k in range(4):
        x = 7 + 8 * k
        d.arc([x, M - 7, x + 9, M + 3], 180, 360, fill=1, width=2)
    hline(d, 40, 47, M)
    return im


def diode():
    im, d = new()
    hline(d, 0, 14, M)
    d.polygon([(14, M - 10), (14, M + 9), (31, M)], fill=1)
    d.rectangle([31, M - 10, 33, M + 9], fill=1)
    hline(d, 33, 47, M)
    return im


def zener():
    """cathode on the left, like the K-A-A socket"""
    im, d = new()
    hline(d, 0, 14, M)
    d.rectangle([14, M - 10, 15, M + 9], fill=1)           # bar, ends bent like a Z
    d.rectangle([14, M - 11, 20, M - 10], fill=1)
    d.rectangle([9, M + 9, 15, M + 10], fill=1)
    d.polygon([(34, M - 10), (34, M + 9), (17, M)], fill=1)
    hline(d, 17, 47, M)
    return im


def arrow(d, tip, frm, size=6):
    """filled arrow head at tip, pointing away from frm"""
    (x1, y1), (x0, y0) = tip, frm
    dx, dy = x1 - x0, y1 - y0
    n = (dx * dx + dy * dy) ** 0.5
    ux, uy = dx / n, dy / n
    bx, by = x1 - ux * size, y1 - uy * size
    px, py = -uy * size * 0.55, ux * size * 0.55
    d.polygon([(x1, y1), (bx + px, by + py), (bx - px, by - py)], fill=1)


def bjt(npn):
    im, d = new()
    d.ellipse([7, 3, 45, 41], outline=1, width=1)
    hline(d, 0, 18, M)
    d.rectangle([18, 9, 20, 35], fill=1)
    d.line([(20, 15), (34, 7)], fill=1, width=2)
    vline(d, 34, 0, 7)
    d.line([(20, 29), (34, 37)], fill=1, width=2)
    vline(d, 34, 37, H - 1)
    if npn:
        arrow(d, (34, 37), (20, 29))
    else:
        arrow(d, (22, 30), (34, 37))
    return im


def mos(n, depletion):
    im, d = new()
    hline(d, 0, 14, M)
    d.rectangle([13, 10, 14, 34], fill=1)                   # gate plate
    if depletion:
        d.rectangle([18, 8, 19, 36], fill=1)
    else:
        for y0 in (8, 19, 30):
            d.rectangle([18, y0, 19, y0 + 6], fill=1)       # broken channel: enhancement
    hline(d, 19, 34, 12)
    vline(d, 34, 0, 12)                                     # drain
    hline(d, 19, 34, 33)
    vline(d, 34, 33, H - 1)                                 # source
    hline(d, 19, 34, M)
    vline(d, 34, M, 33)                                     # body to source
    if n:
        arrow(d, (20, M), (34, M))
    else:
        arrow(d, (33, M), (20, M))
    return im


def jfet(n):
    im, d = new()
    d.rectangle([18, 8, 20, 36], fill=1)                    # channel
    hline(d, 0, 18, 30)
    vline(d, 1, M, 30)
    hline(d, 0, 1, M)
    hline(d, 20, 34, 12)
    vline(d, 34, 0, 12)
    hline(d, 20, 34, 33)
    vline(d, 34, 33, H - 1)
    if n:
        arrow(d, (18, 30), (2, 30))
    else:
        arrow(d, (3, 30), (18, 30))
    return im


# name, image, label positions (x, y of the digit's top-left, relative to the symbol)
TWO = [(-10, M - 8), (W + 2, M - 8)]
TRI = [(-10, M - 18), (38, -2), (38, H - 14)]
SYMS = [
    ('RES', res(), TWO), ('CAP', cap(), TWO), ('IND', ind(), TWO), ('DIODE', diode(), TWO),
    ('NPN', bjt(True), TRI), ('PNP', bjt(False), TRI),
    ('NMOS', mos(True, False), TRI), ('PMOS', mos(False, False), TRI),
    ('NMOS_DEP', mos(True, True), TRI), ('PMOS_DEP', mos(False, True), TRI),
    ('NJFET', jfet(True), TRI), ('PJFET', jfet(False), TRI),
    ('ZENER', zener(), TWO),
]


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--png':
        sheet = Image.new('RGB', ((W + 24) * len(SYMS), H + 24), (0, 0, 0))
        d = ImageDraw.Draw(sheet)
        for i, (name, im, lab) in enumerate(SYMS):
            ox, oy = i * (W + 24) + 12, 12
            sheet.paste(Image.new('RGB', (W, H), (255, 255, 255)), (ox, oy), im)
            for k, (x, y) in enumerate(lab):
                d.text((ox + x + 1, oy + y + 2), str(k + 1), fill=(180, 127, 255))
        sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(sys.argv[2])
        return
    bpr = (W + 7) // 8
    out = ['/* SPDX-License-Identifier: EUPL-1.2 */',
           '/* generated by tools/mksym.py: component symbols, one glyph each (codes 1..), MSB = left */',
           '#include "sym.h"', '', f'static const uint8_t bits[{len(SYMS)} * {H} * {bpr}] = {{']
    for name, im, _ in SYMS:
        out.append(f'    /* {name} */')
        px = im.load()
        for y in range(H):
            row = []
            for b in range(bpr):
                v = 0
                for k in range(8):
                    x = b * 8 + k
                    if x < W and px[x, y]:
                        v |= 0x80 >> k
                row.append(f'0x{v:02x}')
            out.append('    ' + ', '.join(row) + ',')
    out.append('};')
    out.append(f'const struct font sym_font = {{{W}, {H}, {bpr}, 1, {len(SYMS)}, bits}};')
    out.append('const int8_t sym_label[][3][2] = {')
    for name, _, lab in SYMS:
        lab = lab + [(0, 0)] * (3 - len(lab))
        out.append('    {' + ', '.join(f'{{{x}, {y}}}' for x, y in lab) + f'}},    /* {name} */')
    out.append('};')
    print('\n'.join(out))
    hdr = ['/* SPDX-License-Identifier: EUPL-1.2 */', '/* generated by tools/mksym.py */',
           '#ifndef SYM_DATA_H', '#define SYM_DATA_H', '#include <stdint.h>', '#include "font.h"', '',
           f'#define SYM_W {W}', f'#define SYM_HT {H}', 'enum { SYM_NONE,']
    hdr += [f'    SYM_{name},' for name, _, _ in SYMS]
    hdr += ['};', 'extern const struct font sym_font;          /* glyph code = SYM_x */',
            'extern const int8_t sym_label[][3][2];       /* [SYM_x - 1][terminal] = x, y of the probe number */',
            '', '#endif']
    import os
    open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'sym.h'), 'w').write(
        '\n'.join(hdr) + '\n')


main()
