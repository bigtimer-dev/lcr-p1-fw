#!/usr/bin/env python3
"""Bootloader emulator tests (unicorn): boot path vs. the stock FNIRSI loader, both update protocols, safety.

    test/bl_test.py build/lcrp1-bl.bin build/lcrp1-fw.bin [stock_bootloader.bin] [stock_app.bin]

Time is the instruction count (48 per us). Flash erase stalls the CPU for 20 ms, word programming 40 us;
meanwhile the UART keeps receiving (115200 baud, by DMA when the firmware set it up, otherwise into the
1-byte data register with overrun). Anything erased or programmed below 0x08001800 is a failure.
Without the stock files (not in the repo: FNIRSI's) the comparisons with the stock loader are skipped.
"""
import os
import struct
import sys
import zlib

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc, UcError
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_PRIMASK, UC_ARM_REG_SP, UC_ARM_REG_MSP
import unicorn.arm_const as A
CORE = {n: getattr(A, f'UC_ARM_REG_{n}') for n in ('R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R12', 'LR')}

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'lcr-p1-linux', 'app'))
BL = open(sys.argv[1], 'rb').read()
APP = open(sys.argv[2], 'rb').read()
STOCK_BL = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser('~/Work/lcr-p1/bootloader/bootloader.bin')
STOCK_APP = sys.argv[4] if len(sys.argv) > 4 else os.path.expanduser(
    '~/Work/lcr-p1-linux/fw/APP_LCR-P1-EN_V1.1.0_260324.BIN')
STOCK_BL = open(STOCK_BL, 'rb').read() if os.path.exists(STOCK_BL) else None
STOCK_APP = open(STOCK_APP, 'rb').read() if os.path.exists(STOCK_APP) else None
OUT = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), 'boot')
APP_BASE, FL, CPU = 0x08001800, 0x40022000, 48_000_000
PORTS = {0x40010800: 'A', 0x40010C00: 'B', 0x40011000: 'C', 0x40011C00: 'F'}
fails = []
METERS = []


def check(name, good, info=''):
    print(('PASS ' if good else 'FAIL ') + name + ('' if good else f'   {info}'))
    if not good:
        fails.append(name)


class Panel:
    """ST7735 command decoder (CASET/RASET/RAMWR, offsets 1/2) -> 128x128 RGB565 frame."""

    def __init__(self):
        self.fb = [[0] * 128 for _ in range(128)]
        self.cmd, self.args, self.win, self.pos, self.hi = None, [], [0, 0, 127, 127], None, None
        self.cmds = []

    def byte(self, b, dc):
        if not dc:
            self.cmd, self.args, self.hi = b, [], None
            self.cmds.append(b)
            if b == 0x2C:
                self.pos = [self.win[0], self.win[1]]
            return
        if self.cmd in (0x2A, 0x2B):
            self.args.append(b)
            if len(self.args) == 4:
                lo, hi = (self.args[0] << 8 | self.args[1]), (self.args[2] << 8 | self.args[3])
                o = 1 if self.cmd == 0x2A else 2
                if self.cmd == 0x2A:
                    self.win[0], self.win[2] = lo - o, hi - o
                else:
                    self.win[1], self.win[3] = lo - o, hi - o
        elif self.cmd == 0x2C and self.pos:
            if self.hi is None:
                self.hi = b
                return
            c, self.hi = self.hi << 8 | b, None
            x, y = self.pos
            if 0 <= x < 128 and 0 <= y < 128:
                self.fb[y][x] = c
            x += 1
            if x > self.win[2]:
                x, y = self.win[0], y + 1
            self.pos = [x, y]

    def png(self, path, scale=3):
        raw = b''
        for row in self.fb:
            line = b''
            for c in row:
                r, g, b = (c >> 11) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31
                line += bytes((r, g, b)) * scale
            raw += (b'\0' + line) * scale

        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
        open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 128 * scale, 128 * scale,
                                                                                8, 2, 0, 0, 0))
                               + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

    def count(self, color):
        return sum(row.count(color) for row in self.fb)


class Meter:
    """One power-on: flash contents survive between Meter instances (pass flash=), RAM does not."""

    def __init__(self, bl, app=None, flash=None, zener=False, test=False, ram=None, g_ms=None, allow_bl=False,
                 adc=1637, rdp=False):
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x08000000, 0x10000)
        uc.mem_map(0x20000000, 0x4000)
        uc.mem_map(0x40000000, 0x30000)
        uc.mem_map(0xE000E000, 0x1000)
        if flash is None:
            flash = bytearray(b'\xff' * 0x10000)
            flash[:len(bl)] = bl
            if app:
                flash[APP_BASE - 0x08000000:APP_BASE - 0x08000000 + len(app)] = app
        uc.mem_write(0x08000000, bytes(flash))
        uc.mem_write(0x20000000, ram if ram is not None else os.urandom(0x4000))
        self.reg, self.out = {}, {p: 0 for p in 'ABCF'}
        # option bytes as read on a real meter (2026-11-16): OBR 0x03FFFFFE = read protection on (bit 1), which
        # write-protects the first 4 KB; WRPR 0x0000FFFF = no sector write protection
        self.reg[0x4002201C] = 0x03FFFFFE if rdp else 0x03FFFFFC
        self.reg[0x40022020] = 0x0000FFFF
        self.inp = {p: 0xFFFF for p in 'ABCF'}
        self.inp['C'] &= ~(1 << 14)                      # mode switch: LCR
        if zener:
            self.inp['F'] &= ~(1 << 7)
        if test:
            self.inp['C'] &= ~(1 << 13)
        self.cyc = 0
        self.locked, self.keys = True, []
        self.flash_errors, self.erases, self.programs = [], [], 0
        self.rx_queue, self.rx_reg, self.rx_ore, self.tx = [], None, False, bytearray()
        self.dma_pos = 0
        self.dma_flags, self.in_irq = 0, None           # DMA INTSTS ch2 bits (GIF 0x10, TC 0x20, HT 0x40); IRQ frame
        self.panel, self.spi_bytes = Panel(), 0
        self.writes = []                                 # (addr, value) MMIO writes, for trace comparisons
        self.first_pa8 = None
        self.n_gpio_writes = 0
        self.reset_req = False
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_w, begin=0x40000000, end=0x4002FFFF)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_w, begin=0xE000E000, end=0xE000EFFF)
        uc.hook_add(UC_HOOK_MEM_READ, self.on_r, begin=0x40000000, end=0x4002FFFF)
        uc.hook_add(UC_HOOK_MEM_READ, self.on_r, begin=0xE000E000, end=0xE000EFFF)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_flash_prog, begin=0x08000000, end=0x0800FFFF)
        sp, pc = struct.unpack('<II', bytes(flash[:8]))
        uc.reg_write(UC_ARM_REG_SP, sp)
        self.pc = pc
        self.fault = None
        self.g_ms, self.allow_bl, self.adc, self.tick_ms = g_ms, allow_bl, adc, 0
        METERS.append(self)

    # ---- time
    @property
    def ms(self):
        return self.cyc / (CPU / 1000)

    def stall(self, us):
        self.cyc += int(us * CPU / 1e6)

    # ---- peripherals
    def flash_dump(self):
        return bytearray(self.uc.mem_read(0x08000000, 0x10000))

    def on_flash_prog(self, uc, acc, a, sz, v, _):
        old = struct.unpack('<I', uc.mem_read(a & ~3, 4))[0]
        self.programs += 1
        ok = (not self.locked and self.reg.get(FL + 0x10, 0) & 1 and sz == 4 and not a & 3 and old == 0xFFFFFFFF)
        if a < APP_BASE and not self.allow_bl:
            self.flash_errors.append(f'PROGRAM IN BOOTLOADER AREA {a:#x}')
        elif not ok:
            self.flash_errors.append(f'bad program {a:#x}={v:#x} size {sz} old {old:#x} locked {self.locked}')
        self.stall(40)

    def on_w(self, uc, acc, a, sz, v, _):
        self.writes.append((a, v))
        base, off = a & ~0x3FF, a & 0x3FF
        if a == FL + 0x04:
            self.keys = (self.keys + [v])[-2:]
            if self.keys == [0x45670123, 0xCDEF89AB]:
                self.locked = False
        elif a == FL + 0x10:
            if v & 0x80:
                self.locked = True
            elif v & 0x42 == 0x42:
                pg = self.reg.get(FL + 0x14, 0) & ~0x1FF
                self.erases.append(pg)
                if self.locked:
                    self.flash_errors.append('erase while locked')
                elif pg < APP_BASE and not self.allow_bl:
                    self.flash_errors.append(f'ERASE IN BOOTLOADER AREA {pg:#x}')
                else:
                    uc.mem_write(pg, b'\xff' * 0x200)
                self.stall(20000)
        elif a == FL + 0x0C:
            return
        elif base in PORTS and off in (0x18, 0x28):     # BSC / BC
            p = PORTS[base]
            if off == 0x18:
                self.out[p] = (self.out[p] | (v & 0xFFFF)) & ~(v >> 16)
            else:
                self.out[p] &= ~(v & 0xFFFF)
            self.gpio_write(p)
        elif base in PORTS and off == 0x14:
            self.out[PORTS[base]] = v & 0xFFFF
            self.gpio_write(PORTS[base])
        elif base in PORTS:
            self.gpio_write(None)
        elif a == 0x4001200C:                           # SPI data
            if not self.out['B'] >> 6 & 1:              # CS low
                self.panel.byte(v & 0xFF, self.out['B'] >> 5 & 1)
                self.spi_bytes += 1
        elif a == 0x40013804:
            self.tx.append(v & 0xFF)
        elif a == 0x4002001C + 4:                       # DMA ch2 count (re)load
            self.dma_pos = 0
        elif a == 0x40020004:                           # DMA INTFCR: clear flags
            self.dma_flags &= ~v
            if not self.dma_flags & 0x60:
                self.dma_flags &= ~0x10
        elif a == 0x40001024:                           # TIM6 counter write
            self.t6_t0 = self.cyc
        elif a == 0x40001000 and v & 1 and not self.reg.get(a, 0) & 1:
            self.t6_t0 = self.cyc
        elif a == 0xE000ED0C and v == 0x05FA0004:
            self.reset_req = True
        self.reg[a] = v

    def gpio_write(self, p):
        self.n_gpio_writes += 1
        if p == 'A' and self.out['A'] >> 8 & 1 and self.first_pa8 is None:
            self.first_pa8 = self.n_gpio_writes

    def pa8_on(self):
        return self.out['A'] >> 8 & 1 and self.reg.get(0x40010800, 0) >> 16 & 3 == 1

    def read(self, a):
        v = self.reg.get(a, 0)
        base, off = a & ~0x3FF, a & 0x3FF
        if a == 0x40021000:
            v |= (v & 1) << 1 | (v >> 24 & 1) << 25
        elif a == 0x40021004:
            if v & 3 == 2:
                v = (v & ~0xC008) | 0x4000
        elif a == 0x40012008:
            v = 0x02
        elif a == 0x40020854:                           # ADC ready, not calibrating
            v = (v | 0x20) & ~0x40
        elif a == 0x40020800:
            v = 0x02
        elif a == 0x40020850:
            v = self.adc
        elif a == 0x40013800:
            v = 0xC0 | (0x20 if self.rx_reg is not None else 0) | (0x08 if self.rx_ore else 0)
        elif a == 0x40013804:
            v = self.rx_reg if self.rx_reg is not None else 0
            self.rx_reg, self.rx_ore = None, False
        elif a == 0x40020000:
            v = self.dma_flags
        elif a == 0x4002001C + 4:
            n = self.reg.get(a, 0)
            v = n - self.dma_pos if n else 0
        elif a == 0x40001024:
            psc = self.reg.get(0x40001028, 0) + 1
            v = int((self.cyc - getattr(self, 't6_t0', 0)) // psc) & 0xFFFF if self.reg.get(0x40001000, 0) & 1 else 0
        elif a == 0xE000E010:                           # SysTick: COUNTFLAG after LOAD cycles
            v = v | (0x10000 if v & 1 else 0)
        elif base in PORTS and off == 0x10:
            p = PORTS[base]
            mode = self.reg.get(base, 0)
            outs = sum(1 << n for n in range(16) if mode >> 2 * n & 3 == 1)
            v = (self.out[p] & outs) | (self.inp[p] & ~outs)
        elif base in PORTS and off == 0x14:
            v = self.out[PORTS[base]]
        return v

    def on_r(self, uc, acc, a, sz, v, _):
        uc.mem_write(a, struct.pack('<I', self.read(a) & 0xFFFFFFFF)[:sz])

    def deliver(self):
        while self.rx_queue and self.rx_queue[0][0] <= self.cyc:
            _, b = self.rx_queue.pop(0)
            cfg, n = self.reg.get(0x4002001C, 0), self.reg.get(0x4002001C + 4, 0)
            uart_on = self.reg.get(0x4001380C, 0) & 0x2004 == 0x2004
            if not uart_on:
                continue
            if cfg & 1 and self.reg.get(0x40013814, 0) & 0x40 and n:
                maddr = self.reg.get(0x4002001C + 0xC, 0)
                self.uc.mem_write(maddr + self.dma_pos, bytes([b]))
                self.dma_pos += 1
                if self.dma_pos == n // 2:
                    self.dma_flags |= 0x50
                if self.dma_pos >= n:
                    self.dma_flags |= 0x30
                    self.dma_pos = 0 if cfg & 0x20 else n
            elif self.rx_reg is None:
                self.rx_reg = b
            else:
                self.rx_ore = True

    def send(self, data, gap_us=0.0, baud=115200):
        """Bytes on the wire back to back (10 bits each) after the ones already queued."""
        t = max(self.cyc, self.rx_queue[-1][0] if self.rx_queue else 0) + int(gap_us * 48)
        for b in data:
            t += int(10 / baud * CPU)
            self.rx_queue.append((t, b))

    # ---- execution
    def run(self, ms=None, until=None, chunk=1000):
        end = self.cyc + int(ms * CPU / 1000) if ms is not None else None
        while end is None or self.cyc < end:
            try:
                self.uc.emu_start(self.pc | 1, 0, count=chunk)
            except UcError as e:
                if self.in_irq is not None and self.uc.reg_read(UC_ARM_REG_PC) & ~1 == 0xFFFFFFF8:
                    self.irq_return()
                    continue
                self.fault = f'{e} at {self.uc.reg_read(UC_ARM_REG_PC):#x}'
                return False
            self.pc = self.uc.reg_read(UC_ARM_REG_PC)
            self.cyc += chunk
            self.deliver()
            self.irq()
            if self.g_ms is not None and self.in_app():    # app tick IRQ stand-in: advance its g_ms
                while self.tick_ms < self.ms - 1:
                    self.tick_ms += 1
                    t = struct.unpack('<I', self.uc.mem_read(self.g_ms, 4))[0]
                    self.uc.mem_write(self.g_ms, struct.pack('<I', t + 1))
            if until and until(self):
                return True
            if self.reset_req:
                return False
        return False

    # ---- the one interrupt the stock loader uses: DMA ch2 HT/TC (IRQ 11), delivered while in the loader only
    def irq(self):
        cfg = self.reg.get(0x4002001C, 0)
        want = self.dma_flags & ((0x20 if cfg & 2 else 0) | (0x40 if cfg & 4 else 0))
        if (self.in_irq is not None or not want or not self.reg.get(0xE000E100, 0) & 0x800 or self.in_app()
                or self.uc.reg_read(UC_ARM_REG_PRIMASK)):
            return
        u = self.uc
        sp = u.reg_read(UC_ARM_REG_SP)
        align = sp & 4
        sp = (sp & ~7) - 32
        frame = [u.reg_read(r) for r in (A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3,
                                         A.UC_ARM_REG_R12, A.UC_ARM_REG_LR)]
        frame += [self.pc & ~1, (u.reg_read(A.UC_ARM_REG_XPSR) & 0xF8000000) | 1 << 24 | align << 7]
        u.mem_write(sp, struct.pack('<8I', *frame))
        u.reg_write(UC_ARM_REG_SP, sp)
        u.reg_write(A.UC_ARM_REG_LR, 0xFFFFFFF9)
        self.in_irq = align
        self.pc = struct.unpack('<I', u.mem_read(0x08000000 + (16 + 11) * 4, 4))[0] & ~1

    def irq_return(self):
        u = self.uc
        sp = u.reg_read(UC_ARM_REG_SP)
        f = struct.unpack('<8I', u.mem_read(sp, 32))
        for r, v in zip((A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3, A.UC_ARM_REG_R12,
                         A.UC_ARM_REG_LR), f):
            u.reg_write(r, v)
        u.reg_write(A.UC_ARM_REG_XPSR, (u.reg_read(A.UC_ARM_REG_XPSR) & 0x07FFFFFF) | (f[7] & 0xF8000000))
        u.reg_write(UC_ARM_REG_SP, sp + 32 + (4 if f[7] & 0x200 else 0))
        self.pc = f[6]
        self.in_irq = None

    def in_app(self):
        return APP_BASE <= self.pc < 0x08010000

    def take_tx(self):
        t, self.tx = bytes(self.tx), bytearray()
        return t

    def state(self):
        """peripheral registers the loader leaves behind for the app"""
        r = self.reg.get
        s = {
            'RCC_CR': r(0x40021000, 0), 'RCC_CFGR': r(0x40021004, 0), 'RCC_CIR': r(0x40021008, 0),
            'RCC_AHBEN': r(0x40021014, 0), 'RCC_APB2EN': r(0x40021018, 0), 'RCC_APB1EN': r(0x4002101C, 0),
            'RCC_CFGR2': r(0x4002102C, 0), 'FLASH_ACR': r(FL, 0), 'REMAP': r(0x40024C30, 0),
            'SysTick': r(0xE000E010, 0) & 1,
            'PRIMASK': self.uc.reg_read(UC_ARM_REG_PRIMASK), 'MSP': self.uc.reg_read(UC_ARM_REG_MSP),
        }
        s.update({k: self.uc.reg_read(v) for k, v in CORE.items()})
        for base, p in PORTS.items():
            for off, n in ((0, 'MODE'), (4, 'OTYPE'), (8, 'SPEED'), (0xC, 'PULL'), (0x20, 'AFL'), (0x24, 'AFH')):
                s[f'GPIO{p}_{n}'] = r(base + off, 0)
            s[f'GPIO{p}_OUT'] = self.out[p]
        for a, n in ((0x4001380C, 'USART_CR1'), (0x40013814, 'USART_CR3'), (0x4002001C, 'DMA2_CFG'),
                     (0x40012000, 'SPI_CR1'), (0x40001000, 'TIM6_CR1'), (0x40001028, 'TIM6_PSC')):
            s[n] = r(a, 0)
        return s


# ---------------------------------------------------------------- frame protocol (host side)
def frame(cmd, payload=b''):
    h = bytes([cmd]) + struct.pack('<H', len(payload))
    return b'\xA5' + h + payload + struct.pack('<I', zlib.crc32(h + payload))


def parse_reply(b):
    if len(b) < 9 or b[0] != 0x5A:
        return None
    cmd, st, n = b[1], b[2], b[3] | b[4] << 8
    if len(b) < 9 + n:
        return None
    body, crc = b[5:5 + n], struct.unpack_from('<I', b, 5 + n)[0]
    return cmd, st, body, crc == zlib.crc32(b[1:5] + body), b[9 + n:]


def call(m, cmd, payload=b'', ms=2000):
    m.send(frame(cmd, payload))
    buf = bytearray()

    def got(mm):
        buf.extend(mm.take_tx())
        return parse_reply(bytes(buf)) is not None
    if not m.run(ms=ms, until=got):
        return None
    r = parse_reply(bytes(buf))
    if not r[3]:
        check(f'reply to {cmd:#x} has a valid CRC', False)
    return r[:3]


def legacy_blocks(img):
    from lcrp1.flash import blocks
    return blocks(img)


INFO_FMT = '<6sBBIIHHIBBB'
OK = 0
bl_sp, bl_pc = struct.unpack_from('<II', BL)
check('bootloader is exactly 6 KB', len(BL) == 0x1800, len(BL))
magic, ver, used, crc, nmagic = struct.unpack_from('<IHHII', BL, 0x17F0)
check('bl_info stamped: magic, CRC of 0..0x17EF', magic == 0x314C424C and crc == zlib.crc32(BL[:0x17F0]))
check('bootloader stack below the app request words', bl_sp <= 0x20001FF8, hex(bl_sp))

ONLY = {int(x) for x in os.environ.get('BL_ONLY', '1,2,3,4,5,6').split(',')}

# ---------------------------------------------------------------- 1. direct boot
if 1 in ONLY:
    m = Meter(BL, APP)
    m.run(ms=50, until=lambda mm: mm.in_app(), chunk=1)      # stop on the app's first instruction
    check('valid app, no key: starts the app', m.in_app(), f'pc {m.pc:#x} fault {m.fault}')
    check('power latch PA8 set by the first GPIO writes', m.first_pa8 is not None and m.first_pa8 <= 4, m.first_pa8)
    check('direct boot takes < 5 ms', m.ms < 5, f'{m.ms:.2f} ms')
    check('direct boot: no LCD, no flash writes', m.spi_bytes == 0 and not m.erases and not m.programs)
    new_state = m.state()
    app_sp = struct.unpack_from('<I', APP)[0]
    check('app entered with its own SP', app_sp - 64 <= new_state['MSP'] <= app_sp, hex(new_state['MSP']))
    check('vector remap -> app table', new_state['REMAP'] == 0x80000000 | APP_BASE, hex(new_state['REMAP']))
    check('PRIMASK clear at app entry', new_state['PRIMASK'] == 0)

    if STOCK_BL:
        s = Meter(STOCK_BL, APP)
        s.run(ms=50, until=lambda mm: mm.in_app(), chunk=1)
        check('stock loader starts the app in the emulator too', s.in_app(), f'pc {s.pc:#x} {s.fault}')
        st = s.state()
        diff = {k: (hex(st[k]), hex(new_state[k])) for k in st if st[k] != new_state[k]}
        allowed = set()
        check('hand-off identical to the stock loader: peripherals and core registers', set(diff) <= allowed,
              {k: v for k, v in diff.items() if k not in allowed})
        print('     stock vs new differences:', diff or 'none')

        # the app's own behaviour must not depend on which loader started it: compare its MMIO write trace
        def app_trace(bl, app, ms=300):
            mm = Meter(bl, app, ram=b'\0' * 0x4000)
            mm.run(ms=50, until=lambda x: x.in_app())
            n0 = len(mm.writes)
            mm.run(ms=ms)
            return mm, mm.writes[n0:]
        for name, app in (('open firmware', APP), ('stock firmware', STOCK_APP)):
            if app is None:
                continue
            a, ta = app_trace(STOCK_BL, app)
            b, tb = app_trace(BL, app)
            k = next((i for i, (x, y) in enumerate(zip(ta, tb)) if x != y), None)
            check(f'{name}: same first {min(len(ta), len(tb))} MMIO writes under both loaders',
                  k is None and len(ta) >= 50,   # the stock app waits for its tick IRQ after ~60 (not emulated)
                  f'first difference at write {k}: stock {ta[k][0]:#x}={ta[k][1]:#x} new {tb[k][0]:#x}={tb[k][1]:#x}'
                  if k is not None else f'only {len(ta)} writes')

# ---------------------------------------------------------------- 2. update mode by ZENER, screen
if 2 in ONLY:
    m = Meter(BL, APP, zener=True, test=True)
    m.run(ms=600)
    check('ZENER at power-on: stays in the loader', not m.in_app() and m.fault is None, f'{m.pc:#x} {m.fault}')
    check('update mode: latch on', m.pa8_on())
    check('update screen drawn (title bar accent colour)', m.panel.count(0xB47F) > 1000, m.panel.count(0xB47F))
    os.makedirs(OUT, exist_ok=True)
    m.panel.png(os.path.join(OUT, 'screen_update.png'))
    m.run(ms=1500)
    check('TEST still held from power-on does not switch off', m.pa8_on())
    m.inp['C'] |= 1 << 13
    m.run(ms=50)
    m.inp['C'] &= ~(1 << 13)
    m.run(ms=1100)
    check('TEST held 1 s in update mode: power off', not m.pa8_on())

# ---------------------------------------------------------------- 3. legacy (FNIRSI) protocol
if 3 in ONLY:
    def legacy_flash(img, base_flash=None, cut=None, app=APP, zener=True):
        m = Meter(BL, app, flash=base_flash, zener=zener)
        m.run(ms=400)
        m.send(bytes([0xAC, 0x55, 0x02, 0x03]))
        bl = legacy_blocks(img)
        if cut is not None:
            bl = bl[:cut]
        m.send(b''.join(bl), gap_us=12000)                 # close, 10 ms, reopen
        rx = bytearray()

        def done(mm):
            rx.extend(mm.take_tx())
            return mm.in_app() or len(rx) >= 4
        m.run(ms=len(bl) * 50 + 4000, until=done)
        if len(rx) >= 4 and not m.in_app():
            m.run(ms=50, until=lambda mm: mm.in_app())
        return m, bytes(rx)


    m, rx = legacy_flash(APP, app=STOCK_APP or APP)
    img_flash = m.flash_dump()
    pad = b''.join(legacy_blocks(APP))
    check('legacy: reply AC 55 03 04', rx[:4] == bytes([0xAC, 0x55, 0x03, 0x04]), rx.hex())
    check('legacy: flash = image (+ padding, marker)', img_flash[0x1800:0x1800 + len(pad)] == pad)
    check('legacy: bootloader untouched', img_flash[:0x1800] == BL)
    check('legacy: no flash errors', not m.flash_errors, m.flash_errors[:3])
    check('legacy: new app started', m.in_app(), f'{m.pc:#x} {m.fault}')
    # every block erased once, the vector page twice (invalidated first, written last)
    check('legacy: no byte lost (DMA ring during 20 ms erases)', len(m.erases) == len(legacy_blocks(APP)) + 1,
          len(m.erases))
    print(f'     legacy {len(APP)} bytes: {m.ms / 1000:.2f} s emulated')

    if STOCK_APP:
        m, rx = legacy_flash(STOCK_APP)
        f = m.flash_dump()
        pad = b''.join(legacy_blocks(STOCK_APP))
        check('legacy stock image (59276 B): written + replied', rx[:4] == bytes([0xAC, 0x55, 0x03, 0x04])
              and f[0x1800:0x1800 + len(pad)] == pad and f[:0x1800] == BL, rx.hex())

    # interrupted transfer: app page erased, next power-on enters update mode by itself
    m, rx = legacy_flash(STOCK_APP or APP, cut=30)
    check('cut transfer: no OK reply', rx[:3] != bytes([0xAC, 0x55, 0x03]), rx.hex())
    f = m.flash_dump()
    check('cut transfer: app vector page left erased', f[0x1800:0x1A00] == b'\xff' * 512)
    m2 = Meter(BL, flash=f)
    m2.run(ms=600)
    check('after a cut transfer: next power-on stays in update mode (no ZENER)', not m2.in_app() and m2.pa8_on())
    m2.panel.png(os.path.join(OUT, 'screen_noapp.png'))
    m, rx = legacy_flash(APP, base_flash=f, zener=False)
    check('...and can be flashed from there', rx[:4] == bytes([0xAC, 0x55, 0x03, 0x04]) and m.in_app(), rx.hex())

# ---------------------------------------------------------------- 4. framed protocol
if 4 in ONLY:
    m = Meter(BL, STOCK_APP or APP, zener=True)
    m.run(ms=400)
    r = call(m, 1)
    info = struct.unpack(INFO_FMT, r[2]) if r and len(r[2]) == struct.calcsize(INFO_FMT) else None
    check('INFO: id, version, app base, page size, bl crc', info and info[0] == b'LCRPBL' and info[1:3] == (BL[0x17F5], BL[0x17F4])
          and info[3] == APP_BASE and info[5] == 512 and info[7] == crc, info)
    # new host: FNIRSI header, then quiet -> hello (INFO as cmd 0) -> frames
    m.take_tx()
    m.send(bytes([0xAC, 0x55, 0x02, 0x03]))
    hb = bytearray()
    m.run(ms=450, until=lambda mm: (hb.extend(mm.take_tx()), len(hb) > 0)[1])
    check('header + quiet host: no hello within 450 ms (FNIRSI hosts send blocks by then)', not hb, hb.hex())
    m.run(ms=300, until=lambda mm: (hb.extend(mm.take_tx()), parse_reply(bytes(hb)) is not None)[1])
    h = parse_reply(bytes(hb))
    check('...hello after 500 ms: INFO reply as cmd 0', h and h[0] == 0 and h[2][:6] == b'LCRPBL', hb.hex())
    r = call(m, 1)
    check('...then frames work', r and r[1] == OK and r[2][:6] == b'LCRPBL', r)
    r = call(m, 2, struct.pack('<IH2x', 0x08000000, 512))
    check('READ 0x08000000: bootloader bytes (backup)', r and r[1] == OK and r[2] == BL[:512])
    r = call(m, 3, struct.pack('<II', 0x08000000, 0x1800))
    check('CRC of the bootloader area', r and struct.unpack('<I', r[2])[0] == zlib.crc32(BL))
    bad = bytearray(frame(1))
    bad[-1] ^= 1
    m.send(bytes(bad))
    m.run(ms=50)
    r = parse_reply(m.take_tx())
    check('frame with a bad CRC: ST_CRC reply', r and r[1] == 1, r)
    r = call(m, 5, struct.pack('<I', 0x08000000) + b'\0' * 512)
    check('WRITE to the bootloader area refused', r and r[1] == 3 and m.flash_dump()[:0x1800] == BL, r)
    r = call(m, 5, struct.pack('<I', APP_BASE - 512) + b'\0' * 512)
    check('WRITE to 0x08001600 refused', r and r[1] == 3, r)


    def v2_flash(m, img, bad_crc=False):
        r = call(m, 10, struct.pack('<I', len(img)))
        if not r or r[1]:
            return r
        for off in range(0, len(img), 512):
            chunk = img[off:off + 512].ljust(512, b'\xff')
            r = call(m, 5, struct.pack('<I', APP_BASE + off) + chunk)
            if not r or r[1]:
                return r
        return call(m, 6, struct.pack('<II', len(img), zlib.crc32(img) ^ bad_crc), ms=3000)


    r = v2_flash(m, APP, bad_crc=True)
    check('COMMIT with a wrong CRC: ST_VERIFY, app stays invalid', r and r[1] == 5
          and m.flash_dump()[0x1800:0x1A00] == b'\xff' * 512, r)
    r = call(m, 7)
    check('BOOT refused without a valid app', r and r[1] == 6, r)
    t0 = m.ms
    r = v2_flash(m, APP)
    check('framed update: COMMIT OK', r and r[1] == OK, r)
    print(f'     framed {len(APP)} bytes at 115200: {(m.ms - t0) / 1000:.2f} s emulated')
    f = m.flash_dump()
    check('framed update: flash = image', f[0x1800:0x1800 + len(APP)] == APP and f[:0x1800] == BL)
    check('framed update: no flash errors', not m.flash_errors, m.flash_errors[:3])
    m.panel.png(os.path.join(OUT, 'screen_done.png'))
    r = call(m, 9, struct.pack('<I', 921600))
    check('BAUD 921600 acknowledged at 115200', r and r[1] == OK, r)
    m.send(frame(1), baud=921600)
    buf = bytearray()
    m.run(ms=100, until=lambda mm: (buf.extend(mm.take_tx()), parse_reply(bytes(buf)))[1] is not None)
    r = parse_reply(bytes(buf))
    check('INFO at 921600', r and r[1] == OK and m.reg.get(0x40013808) == 52, (r, m.reg.get(0x40013808)))
    r = call(m, 9, struct.pack('<I', 460800))
    m.run(ms=1200)
    check('BAUD switch with no frame afterwards: back to 115200', m.reg.get(0x40013808) == 417, m.reg.get(0x40013808))
    r = call(m, 7)
    m.run(ms=20, until=lambda mm: mm.in_app())
    check('BOOT starts the new app', m.in_app() and r and r[1] == OK, f'{m.pc:#x} {r}')
    st = m.state()
    check('after update mode: USART, DMA, SPI, TIM6 off for the app', not st['USART_CR1'] and not st['DMA2_CFG'] & 1
          and not st['SPI_CR1'] and not st['TIM6_CR1'] and not st['TIM6_PSC'], st)

# ---------------------------------------------------------------- 5. app request, idle power-off
if 5 in ONLY:
    ram = bytearray(os.urandom(0x4000))
    struct.pack_into('<II', ram, 0x1FF8, 0xB007104D, 0xB007104D ^ 0xFFFFFFFF)
    m = Meter(BL, APP, ram=bytes(ram))
    m.run(ms=400)
    check('app request words: update mode without keys', not m.in_app() and m.pa8_on())
    check('request words cleared', m.uc.mem_read(0x20001FF8, 8) == b'\0' * 8)
    for _ in range(294 // 6):                              # fast-forward 4.9 min in 6 s steps (16-bit ms timer)
        m.cyc += 6 * CPU
        m.run(ms=1)
    check('idle 4.9 min: still on', m.pa8_on())
    for _ in range(3):
        m.cyc += 6 * CPU
        m.run(ms=1)
    check('idle 5 min: power off', not m.pa8_on())

# ---------------------------------------------------------------- 6. installer
INST = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), 'lcrp1-bl-install.bin')
INST_ELF = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), 'inst', 'inst.elf')
if 6 in ONLY and STOCK_BL and os.path.exists(INST):
    import subprocess
    inst = open(INST, 'rb').read()
    gms = next(int(l.split()[0], 16) for l in subprocess.check_output(['arm-none-eabi-nm', INST_ELF], text=True)
               .splitlines() if l.endswith(' g_ms'))

    def inst_meter(**kw):
        mm = Meter(STOCK_BL, inst, g_ms=gms, allow_bl=True, test=True, **kw)
        mm.run(ms=50, until=lambda x: x.in_app())
        mm.run(ms=400)                                   # TEST still held from switching on
        mm.inp['C'] |= 1 << 13
        mm.run(ms=300)
        return mm

    m = inst_meter()
    check('installer runs under the stock loader', m.in_app() and m.fault is None, f'{m.pc:#x} {m.fault}')
    t = m.take_tx().decode('latin-1')
    check('installer recognises the stock loader', 'now FNIRSI stock' in t, t)
    m.panel.png(os.path.join(OUT, 'screen_installer.png'))
    check('TEST held from power-on did not install', m.flash_dump()[:0x1800] == STOCK_BL)
    m.inp['C'] &= ~(1 << 13)
    m.run(ms=2000)
    check('TEST held 2 s: nothing written yet', m.flash_dump()[:0x1800] == STOCK_BL)
    m.run(ms=1500)
    m.inp['C'] |= 1 << 13
    m.run(ms=300)
    t = m.take_tx().decode('latin-1')
    f = m.flash_dump()
    check('TEST held 3 s: bootloader installed and verified', f[:0x1800] == BL and f'installed LCR-P1 BL {BL[0x17F5]}.{BL[0x17F4]}' in t, t)
    check('installer: no flash errors', not [e for e in m.flash_errors if 'BOOTLOADER' not in e], m.flash_errors[:3])
    check('installer: app area untouched', f[0x1800:0x1800 + len(inst)] == inst)
    m.panel.png(os.path.join(OUT, 'screen_installed.png'))
    m.inp['C'] &= ~(1 << 13)
    m.run(ms=100)
    m.inp['C'] |= 1 << 13
    m.run(ms=600)
    check('TEST after install: new bootloader update mode ("Asked by app")', m.pc < APP_BASE and m.pa8_on()
          and m.fault is None, f'{m.pc:#x} {m.fault}')
    m.panel.png(os.path.join(OUT, 'screen_asked.png'))
    m.take_tx()
    m.send(bytes([0xAC, 0x55, 0x02, 0x03]))
    m.send(b''.join(legacy_blocks(APP)), gap_us=12000)
    rx = bytearray()
    m.run(ms=8000, until=lambda mm: (rx.extend(mm.take_tx()), len(rx) >= 4)[1])
    m.run(ms=50, until=lambda mm: mm.in_app())
    f = m.flash_dump()
    check('...then the firmware flashes through the new loader and starts', rx[:4] == bytes([0xAC, 0x55, 3, 4])
          and m.in_app() and f[0x1800:0x1800 + len(APP)] == APP, rx.hex())
    # power-on after that: new loader starts the firmware directly
    m2 = Meter(BL, flash=f)
    m2.run(ms=50, until=lambda x: x.in_app())
    check('next power-on: firmware starts directly', m2.in_app())

    m = inst_meter()
    m.inp['F'] &= ~(1 << 7)
    m.run(ms=100)
    check('installer: ZENER = power off, nothing written', not m.pa8_on() and m.flash_dump()[:0x1800] == STOCK_BL)
    m = inst_meter(adc=1368)                             # ~3.3 V
    m.inp['C'] &= ~(1 << 13)
    m.run(ms=3500)
    check('installer: battery 3.3 V refuses to write', m.flash_dump()[:0x1800] == STOCK_BL)
    m.panel.png(os.path.join(OUT, 'screen_lowbat.png'))
    m = inst_meter(rdp=True)                             # FNIRSI ships the chip read-protected
    t = m.take_tx().decode('latin-1')
    m.inp['C'] &= ~(1 << 13)
    m.run(ms=4000)
    check('installer: read-protected chip refuses, TEST held 4 s writes nothing',
          'PROTECTED' in t and m.erases == [] and m.flash_dump()[:0x1800] == STOCK_BL, t)
    m.panel.png(os.path.join(OUT, 'screen_protected.png'))
    m = Meter(BL, inst, g_ms=gms, allow_bl=True)       # run again on the installed loader
    m.run(ms=50, until=lambda x: x.in_app())
    m.run(ms=500)
    t = m.take_tx().decode('latin-1')
    check('installer on the new loader: recognises it, nothing to do', f'now LCR-P1 BL {BL[0x17F5]}.{BL[0x17F4]}' in t and m.erases == [], t)

bad = [e for mm in METERS for e in mm.flash_errors if 'BOOTLOADER AREA' in e]
check('no erase/program below 0x08001800 in any test', not bad, bad[:3])
print(f'\n{"ALL PASS" if not fails else f"{len(fails)} FAILED: " + ", ".join(fails)}')
sys.exit(1 if fails else 0)
