import mmap, os, struct, sys
base = 0x3F000000 + 0x100000            # BCM2837 PM/pads block
f = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(f, 4096, offset=base)
old = struct.unpack_from("<I", m, 0x2C)[0]
new = 0x5A000000 | (int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x08)   # hysteresis on, slew limited, 2 mA
struct.pack_into("<I", m, 0x2C, new)
print(f"pads bank0 {old & 0x1f:#x} -> {struct.unpack_from(chr(60)+chr(73), m, 0x2C)[0] & 0x1f:#x}")
