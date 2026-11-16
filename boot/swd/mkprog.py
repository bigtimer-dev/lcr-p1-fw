#!/usr/bin/env python3
"""Make prog_data.tcl (for prog.tcl) and image64k.bin from the loader, the app and the backed-up data pages.

usage: mkprog.py lcrp1-bl.bin lcrp1-fw.bin flash_F800_FFFF.bin [outdir]
The third file is the 2 KB at 0x0800F800-0x0800FFFF read by backup.tcl (adjustment page + calibration record)."""
import struct, sys, zlib, os

bl, app, data = sys.argv[1:4]
out = sys.argv[4] if len(sys.argv) > 4 else '.'
img = bytearray(b'\xff' * 0x10000)
for addr, path, limit in ((0x08000000, bl, 0x1800), (0x08001800, app, 0xE000), (0x0800F800, data, 0x800)):
    d = open(path, 'rb').read()
    if len(d) > limit:
        sys.exit(f'{path}: {len(d)} bytes, more than {limit}')
    d += b'\xff' * (-len(d) % 4)
    img[addr - 0x08000000:addr - 0x08000000 + len(d)] = d
if img[0x17F0:0x17F4] != b'LBL1':
    sys.exit('loader has no LBL1 info block')
if img[0xFFD0:0xFFD4] != b'dEnK':
    print('warning: no calibration record "dEnK" at 0x0800FFD0')
open(os.path.join(out, 'image64k.bin'), 'wb').write(img)
words = [(0x08000000 + i, struct.unpack_from('<I', img, i)[0]) for i in range(0, 0x10000, 4)]
with open(os.path.join(out, 'prog_data.tcl'), 'w') as f:
    f.write('set words {\n' + '\n'.join(f'0x{a:08x} 0x{v:08x}' for a, v in words if v != 0xffffffff) + '\n}\n')
print(f'image CRC {zlib.crc32(img):08x}, sha256 of image64k.bin must match readback.bin after prog.tcl')
