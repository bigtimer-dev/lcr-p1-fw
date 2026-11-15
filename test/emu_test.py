#!/usr/bin/env python3
"""Boot the firmware image in unicorn with a model of the LCR-P1 peripherals and check phase-1 behaviour.

    test/emu_test.py build/lcrp1-fw.bin build/lcrp1-fw.elf [stock_boot_mmio.log]

Model: RCC ready flags, flash, GPIO (outputs + key inputs), SPI -> ST7735 panel (renders
build/screen*.png), USART1 (captures text, feeds input). SysTick: the harness advances g_ms.
If the stock boot log (~/Work/lcr-p1/hwmap/boot_mmio.log) exists, the LCD init sequence is
compared with what the stock firmware sends.
"""
import os
import struct
import subprocess
import sys
import zlib

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc, UcError
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP

LOWBAT = '--lowbat' in sys.argv
args = [a for a in sys.argv[1:] if a != '--lowbat']
BIN, ELF = args[0], args[1]
STOCK_LOG = args[2] if len(args) > 2 else os.path.expanduser('~/Work/lcr-p1/hwmap/boot_mmio.log')
OUTDIR = os.path.dirname(os.path.abspath(BIN))
BASE = 0x08001800
img = open(BIN, 'rb').read()
syms = {l.split()[-1]: int(l.split()[0], 16) for l in
        subprocess.check_output(['arm-none-eabi-nm', ELF], text=True).splitlines() if len(l.split()) == 3}
G_MS = syms['g_ms']
fails = []


def check(name, good, info=''):
    print(('PASS ' if good else 'FAIL ') + name + ('' if good else f'   {info}'))
    if not good:
        fails.append(name)


PORTS = {0x40010800: 'A', 0x40010C00: 'B', 0x40011000: 'C', 0x40011C00: 'F'}


class Board:
    def __init__(self):
        self.reg = {}                                   # last written value per address
        self.out = {p: 0 for p in 'ABCF'}
        self.inp = {p: 0 for p in 'ABCF'}
        self.inp['C'] |= 1 << 13                        # TEST released (pull-up)
        self.inp['F'] |= 1 << 7                         # ZENER released
        self.events = []                                # (ms, what)
        self.uart_tx, self.uart_rx = bytearray(), bytearray()
        self.lcd = Panel()
        self.ms = 0
        self.first_pa8 = None
        self.writes = 0
        self.kr = []
        self.adc = {8: 1700, 0: 2050, 1: 3, 3: 5, 6: 4, 2: 0}   # raw codes; ch8 1700 = 4.10 V battery

    def pin(self, port, n):
        return self.out[port] >> n & 1

    def set_in(self, port, n, v):
        self.inp[port] = (self.inp[port] & ~(1 << n)) | (v << n)

    def write(self, a, v):
        self.writes += 1
        if a == 0x40003000:
            self.kr.append(v)
        self.reg[a] = v
        base, off = a & ~0x3FF, a & 0x3FF
        if base in PORTS and off in (0x14, 0x18, 0x28):
            p = PORTS[base]
            old = self.out[p]
            if off == 0x14:
                self.out[p] = v & 0xFFFF
            elif off == 0x18:
                self.out[p] = (self.out[p] | (v & 0xFFFF)) & ~(v >> 16)
            else:
                self.out[p] &= ~v
            if p == 'A' and self.out[p] >> 8 & 1 and self.first_pa8 is None:
                self.first_pa8 = self.writes
            for n in range(16):
                if (old ^ self.out[p]) >> n & 1:
                    self.events.append((self.ms, f'P{p}{n}={self.out[p] >> n & 1}'))
        elif a == 0x4001200C:
            self.lcd.byte(v & 0xFF, dc=self.pin('B', 5), cs=self.pin('B', 6))
        elif a == 0x40013804:
            self.uart_tx.append(v & 0xFF)

    def read(self, a):
        v = self.reg.get(a, 0)
        base, off = a & ~0x3FF, a & 0x3FF
        if a == 0x40021000:                             # RCC_CR: ready flags follow enables
            v |= (v & 1) << 1 | (v >> 24 & 1) << 25
        elif a == 0x40021004:                           # RCC_CFGR: switch status after SW=PLL
            if v & 3 == 2:
                v = (v & ~0xC008) | 0x4000
        elif a == 0x40020854:
            v |= 0x20                                   # ADC ready, not calibrating
            v &= ~0x40
        elif a == 0x40020800:
            v = 0x02                                    # ADC: conversion done
        elif a == 0x40020850:
            v = self.adc.get(self.reg.get(0x40020838, 0) & 31, 0)
        elif a == 0x40012008:
            v = 0x02                                    # SPI: TX empty, not busy
        elif a == 0x40013800:
            v = 0x80 | (0x20 if self.uart_rx else 0)
        elif a == 0x40013804:
            v = self.uart_rx.pop(0) if self.uart_rx else 0
        elif base in PORTS and off == 0x10:
            p = PORTS[base]
            mode = self.reg.get(base, 0)
            outs = sum(1 << n for n in range(16) if mode >> 2 * n & 3 == 1)
            v = (self.out[p] & outs) | (self.inp[p] & ~outs)
        elif base in PORTS and off == 0x14:
            v = self.out[PORTS[base]]
        return v


class Panel:
    """ST7735-style: CASET 2A / RASET 2B / RAMWR 2C, 16-bit pixels, offsets as used by the firmware."""
    def __init__(self):
        self.fb = [[0] * 128 for _ in range(128)]
        self.cmds = []                                  # [cmd, data...] in order
        self.cur, self.args, self.px, self.hi = None, [], None, None
        self.x0 = self.x1 = self.y0 = self.y1 = self.x = self.y = 0

    def byte(self, b, dc, cs):
        if cs:
            return
        if not dc:
            self.cur, self.args, self.hi = b, [], None
            self.cmds.append([b])
            if b == 0x2C:
                self.x, self.y = self.x0, self.y0
            return
        if self.cur == 0x2C:
            if self.hi is None:
                self.hi = b
                return
            c = self.hi << 8 | b
            self.hi = None
            X, Y = self.x - 1, self.y - 2
            if 0 <= X < 128 and 0 <= Y < 128:
                self.fb[Y][X] = c
            self.x += 1
            if self.x > self.x1:
                self.x, self.y = self.x0, self.y + 1
            return
        self.cmds[-1].append(b)
        self.args.append(b)
        if self.cur == 0x2A and len(self.args) == 4:
            self.x0, self.x1 = self.args[1], self.args[3]
        if self.cur == 0x2B and len(self.args) == 4:
            self.y0, self.y1 = self.args[1], self.args[3]

    def png(self, path, scale=3):
        rows = []
        for r in self.fb:
            line = bytearray()
            for c in r:
                r5, g6, b5 = c >> 11, c >> 5 & 63, c & 31     # RGB565 (0xB47F is purple on the meter)
                line += bytes((r5 * 255 // 31, g6 * 255 // 63, b5 * 255 // 31)) * scale
            rows += [b'\0' + bytes(line)] * scale
        raw = b''.join(rows)
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
        w = h = 128 * scale
        open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                               + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

    def count(self, color, y0=0, y1=128):
        return sum(row.count(color) for row in self.fb[y0:y1])


bd = Board()
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x08000000, 0x10000)
uc.mem_write(BASE, img)
uc.mem_map(0x20000000, 0x4000)
uc.mem_map(0x40000000, 0x30000)
uc.mem_map(0xE0000000, 0x100000)


def on_w(uc, acc, a, sz, v, _):
    bd.write(a, v)


def on_r(uc, acc, a, sz, v, _):
    uc.mem_write(a, struct.pack('<I', bd.read(a) & 0xFFFFFFFF)[:sz])


uc.hook_add(UC_HOOK_MEM_WRITE, on_w, begin=0x40000000, end=0x4002FFFF)
uc.hook_add(UC_HOOK_MEM_READ, on_r, begin=0x40000000, end=0x4002FFFF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_w, begin=0xE000E000, end=0xE000EFFF)

sp, rv = struct.unpack_from('<II', img, 0)
# FNIRSI bootloader (dumped 2026-11-14, jump routine 0x08000190): starts the app only if
# (SP & 0x0FFFFFFF) < 0x2000, otherwise it stays in update mode
check('bootloader would start the image: initial SP < 0x20002000', (sp & 0x0FFFFFFF) < 0x2000, hex(sp))
check('reset vector inside the image (thumb)', rv & 1 and BASE <= rv < BASE + len(img), hex(rv))
vec = struct.unpack_from('<48I', img, 0)
check('IRQ20 vector -> TIM6_IRQHandler', vec[36] == syms['TIM6_IRQHandler'] | 1, hex(vec[36]))
uc.reg_write(UC_ARM_REG_SP, sp)
pc = [rv]
CHUNK = 3000                                            # instructions per emulated millisecond


def run_ms(n):
    """Advance n ms: run CHUNK instructions, then tick g_ms (SysTick interrupt)."""
    for _ in range(n):
        try:
            uc.emu_start(pc[0] | 1, 0, count=CHUNK)
        except UcError as e:
            raise SystemExit(f'emulation error {e} at {uc.reg_read(UC_ARM_REG_PC):#x}')
        pc[0] = uc.reg_read(UC_ARM_REG_PC)
        bd.ms += 1
        t = struct.unpack('<I', uc.mem_read(G_MS, 4))[0]
        uc.mem_write(G_MS, struct.pack('<I', t + 1))


def uart_text():
    s = bd.uart_tx.decode('latin-1')
    bd.uart_tx.clear()
    return s


# --- power-on with TEST still held (the user pressed TEST to switch on)
bd.set_in('C', 13, 0)
run_ms(1)
check('power latch PA8 set within the first GPIO writes', bd.first_pa8 is not None and bd.first_pa8 <= 6,
      bd.first_pa8)
run_ms(399)
check('clock: PLL on, system clock = PLL, flash 2 wait states',
      bd.reg.get(0x40021000, 0) >> 24 & 1 and bd.reg.get(0x40021004, 0) & 3 == 2
      and bd.reg.get(0x40022000, 0) & 3 == 2, [hex(bd.reg.get(a, 0)) for a in (0x40021000, 0x40021004, 0x40022000)])
r = bd.reg.get
check('tick timer TIM6: 48 MHz / 48 / 1000, update IRQ on, running',
      (r(0x40001028), r(0x4000102C), r(0x4000100C), r(0x40001000, 0) & 1) == (47, 999, 1, 1),
      [r(a) for a in (0x40001028, 0x4000102C, 0x4000100C, 0x40001000)])
check('NVIC: IRQ20 enabled; SysTick interrupt not used', r(0xE000E100, 0) >> 20 & 1 and not r(0xE000E010, 0) & 2)
check('watchdog started (~2 s) and fed', r(0x40003004) == 4 and r(0x40003008) == 1250
      and 0xCCCC in bd.kr and bd.kr[-1] == 0xAAAA, bd.kr[-3:])
check('relay off and HV boost off after init', bd.pin('B', 3) == 0 and bd.pin('F', 6) == 0)
s = uart_text()
check('UART banner + ADC ready', 'lcrp1-fw' in s and 'adc init 0' in s, s)

# LCD init vs stock
init = [c for c in bd.lcd.cmds if c[0] not in (0x2A, 0x2B, 0x2C)]
mine = [x for c in init[:init.index([0x36, 0x68]) + 1] for x in c] if [0x36, 0x68] in init else []
if os.path.exists(STOCK_LOG):
    stock, outb = [], 0
    for line in open(STOCK_LOG):
        t, _, a, _, v = line.split()
        if t != 'W':
            continue
        a, v = int(a, 16), int(v, 16)
        if a == 0x40010C14:
            outb = v
        elif a == 0x40010C18:
            outb = (outb | (v & 0xFFFF)) & ~(v >> 16)
        elif a == 0x40010C28:
            outb &= ~v
        elif a == 0x4001200C:
            stock.append(v & 0xFF)
            if len(stock) >= 2 and stock[-2:] == [0x36, 0x68]:
                break
    check('LCD init bytes identical to stock (0x11 .. 0x36 0x68)', mine == stock,
          f'\n  mine  {bytes(mine).hex()}\n  stock {bytes(stock).hex()}')
else:
    print('SKIP LCD init vs stock (no stock boot log)')
check('panel: sleep out, 16-bit colour, display on', [0x11] in init and [0x3A, 0x05] in init and [0x29] in init)

run_ms(200)
check('no key event while TEST is still held from power-on', bd.pin('B', 3) == 0 and 'key' not in uart_text())
bd.set_in('C', 13, 1)
run_ms(200)
bd.lcd.png(os.path.join(OUTDIR, 'screen_boot.png'))
check('title drawn in accent colour', bd.lcd.count(0xB47F, 0, 26) > 100, bd.lcd.count(0xB47F, 0, 26))
check('status rows drawn', bd.lcd.count(0xFFFF, 48, 112) > 100 and bd.lcd.count(0x8C33, 48, 64) > 20)

s = uart_text()
check('ADC readout over serial: battery 4.10 V, raw codes per channel',
      '#A ' in s and 'bat=4100mV' in s and 'BAT=1700' in s and 'REF=2050' in s, s[-200:])
check('ADC: sample time code 12 on ch8, rank 1 = selected channel', (bd.reg.get(0x40020810, 0) & 15) == 12)
bd.lcd.png(os.path.join(OUTDIR, 'screen_bat.png'))
check('battery shown on page 0', bd.lcd.count(0x8C33, 112, 128) > 30)

if LOWBAT:                                              # second pass: battery drops to 3.13 V
    bd.adc[8] = 1300
    run_ms(4000)
    check('3.13 V for < 5 s: still on', bd.pin('A', 8) == 1)
    run_ms(3000)
    s = uart_text()
    check('low battery: relay released, latch released, logged', bd.pin('A', 8) == 0 and bd.pin('B', 3) == 0
          and 'power off (low battery)' in s, s[-120:])
    bd.lcd.png(os.path.join(OUTDIR, 'screen_lowbat.png'))
    print(f'{len(fails)} failed' if fails else 'all passed (low battery)')
    sys.exit(1 if fails else 0)

# --- short TEST: relay toggles, beep
bd.set_in('C', 13, 0)
run_ms(150)
bd.set_in('C', 13, 1)
run_ms(150)
s = uart_text()
check('short TEST: relay on + log line', bd.pin('B', 3) == 1 and 'key TEST: relay on' in s, s)
check('beep: buzzer channel enabled then disabled again', bd.reg.get(0x40013420) == 0x12)
bd.lcd.png(os.path.join(OUTDIR, 'screen_relay.png'))

# --- ZENER: logged, HV stays off
bd.set_in('F', 7, 0)
run_ms(100)
bd.set_in('F', 7, 1)
run_ms(400)
bd.set_in('F', 7, 0)
run_ms(100)
bd.set_in('F', 7, 1)
run_ms(400)
s = uart_text()
check('ZENER switches page 1 and back, HV boost still off',
      'key ZENER: page 1' in s and 'key ZENER: page 0' in s and bd.pin('F', 6) == 0, s)

# --- mode switch
bd.set_in('C', 14, 1)
run_ms(400)
s = uart_text()
check('mode switch IR shown + logged', 'mode IR' in s, s)
bd.set_in('C', 14, 0)
run_ms(400)
uart_text()

# --- ADC page
bd.set_in('F', 7, 0); run_ms(100); bd.set_in('F', 7, 1); run_ms(1200)
bd.lcd.png(os.path.join(OUTDIR, 'screen_adc.png'))
check('ADC page drawn (ZENER switches page)', bd.lcd.count(0xB47F, 0, 16) > 30 and bd.lcd.count(0x8C33, 0, 128) > 100)
bd.set_in('F', 7, 0); run_ms(100); bd.set_in('F', 7, 1); run_ms(1200)
uart_text()

# --- implausible battery reading (0 V): never switches off
bd.adc[8] = 0
run_ms(7000)
check('implausible battery reading does not switch off', bd.pin('A', 8) == 1)
bd.adc[8] = 1700
run_ms(1100)
uart_text()

# --- serial status
bd.uart_rx += b'?'
run_ms(50)
s = uart_text()
check('serial "?" status reply', s.startswith('#? ') and 'relay=1' in s, s)

# --- long TEST: power off
bd.set_in('C', 13, 0)
run_ms(2500)
s = uart_text()
check('long TEST: relay released, PA8 cleared, logged', bd.pin('A', 8) == 0 and bd.pin('B', 3) == 0
      and 'power off (key)' in s, s)
bd.lcd.png(os.path.join(OUTDIR, 'screen_off.png'))
check('HV boost pin never set', not any(e == 'PF6=1' for _, e in bd.events))

# --- tick ISR: clears the update flag and counts
RET = 0x08000100
uc.mem_write(RET, b'\xfe\xe7')
t0 = struct.unpack('<I', uc.mem_read(G_MS, 4))[0]
bd.reg[0x40001010] = 1
uc.reg_write(UC_ARM_REG_SP, 0x20001F00)
from unicorn.arm_const import UC_ARM_REG_LR
uc.reg_write(UC_ARM_REG_LR, RET | 1)
uc.emu_start(syms['TIM6_IRQHandler'] | 1, RET, count=1000)
t1 = struct.unpack('<I', uc.mem_read(G_MS, 4))[0]
check('TIM6 ISR: g_ms + 1 and update flag cleared', t1 == t0 + 1 and bd.reg.get(0x40001010) == 0, (t0, t1))

print(f'{len(fails)} failed' if fails else 'all passed', '- screens in', OUTDIR)
sys.exit(1 if fails else 0)
