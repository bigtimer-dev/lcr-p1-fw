# minimal SWD bit-bang via /dev/gpiomem: DIO=GPIO24, CLK=GPIO25. Prints raw response bits of a DPIDR read.
import mmap, os, struct, sys, time
DIO, CLK = 24, 25
f = os.open("/dev/gpiomem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(f, 4096)
def rd(o): return struct.unpack_from("<I", m, o)[0]
def wr(o, v): struct.pack_into("<I", m, o, v)
def fsel(pin, out):
    o = (pin // 10) * 4; s = (pin % 10) * 3
    wr(o, (rd(o) & ~(7 << s)) | ((1 if out else 0) << s))
def setp(pin, v): wr(0x1C if v else 0x28, 1 << pin)
def lev(pin): return rd(0x34) >> pin & 1
T = float(sys.argv[1]) if len(sys.argv) > 1 else 50e-6
def clk():
    setp(CLK, 0); time.sleep(T); setp(CLK, 1); time.sleep(T)
def out_bits(bits):
    fsel(DIO, 1)
    for b in bits:
        setp(DIO, b); time.sleep(T / 2); clk()
def in_bits(n):
    fsel(DIO, 0)
    r = []
    for _ in range(n):
        setp(CLK, 0); time.sleep(T); r.append(lev(DIO)); setp(CLK, 1); time.sleep(T)
    return r
setp(CLK, 1); fsel(CLK, 1)
def word(v, n): return [(v >> i) & 1 for i in range(n)]
seq = [1] * 60 + word(0xE79E, 16) + [1] * 60 + [0] * 4
out_bits(seq)
out_bits(word(0xA5, 8))            # read DPIDR
r = in_bits(1 + 3 + 33)            # trn, ack, data, parity
print("raw:", "".join(map(str, r)))
ack = r[1] | r[2] << 1 | r[3] << 2
data = sum(b << i for i, b in enumerate(r[4:36]))
print(f"ack={ack:03b} (1=OK) data={data:08x}")
out_bits([0] * 8)
fsel(DIO, 0); fsel(CLK, 0)
