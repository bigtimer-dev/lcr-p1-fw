#!/usr/bin/env python3
"""Pad the bootloader to 6 KB and stamp struct bl_info (last 16 bytes): size and CRC32 of 0..0x17EF.

    boot/mkinfo.py build/boot/bl.elf.bin build/lcrp1-bl.bin
"""
import struct
import sys
import zlib

SIZE, INFO = 0x1800, 0x17F0
img = bytearray(open(sys.argv[1], 'rb').read())
assert len(img) == SIZE, f'objcopy output is {len(img)} bytes, expected {SIZE}'
magic, ver, _, _, nmagic = struct.unpack_from('<IHHII', img, INFO)
assert magic == 0x314C424C and nmagic == magic ^ 0xFFFFFFFF, 'bl_info missing'
used = len(img[:INFO].rstrip(b'\xff'))
crc = zlib.crc32(bytes(img[:INFO]))
struct.pack_into('<IHHII', img, INFO, magic, ver, used, crc, nmagic)
open(sys.argv[2], 'wb').write(img)
print(f'{sys.argv[2]}: v{ver >> 8}.{ver & 255}, {used} of {INFO} bytes used, crc {crc:08x}')
