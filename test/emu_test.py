#!/usr/bin/env python3
"""Boot the firmware image in unicorn with a model of the LCR-P1 peripherals and check phase-1 behaviour.

    test/emu_test.py build/lcrp1-fw.bin build/lcrp1-fw.elf [stock_boot_mmio.log]

Model: RCC ready flags, flash, GPIO (outputs + key inputs), SPI -> ST7735 panel (renders
build/screen*.png), USART1 (captures text, feeds input). SysTick: the harness advances g_ms.
If the stock boot log (~/Work/lcr-p1/hwmap/boot_mmio.log) exists, the LCD init sequence is
compared with what the stock firmware sends.
"""
import os
import math
import struct
import subprocess
import sys
import zlib

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc, UcError
from unicorn.arm_const import UC_ARM_REG_PRIMASK, UC_ARM_REG_PC, UC_ARM_REG_SP

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


def solve3(A, b):
    A = [row[:] for row in A]; b = b[:]
    for c in range(3):
        p = max(range(c, 3), key=lambda r: abs(A[r][c]))
        A[c], A[p] = A[p], A[c]; b[c], b[p] = b[p], b[c]
        for r in range(c + 1, 3):
            f = A[r][c] / A[c][c]
            for k in range(c, 3):
                A[r][k] -= f * A[c][k]
            b[r] -= f * b[c]
    x = [0.0] * 3
    for r in (2, 1, 0):
        x[r] = (b[r] - sum(A[r][k] * x[k] for k in range(r + 1, 3))) / A[r][r]
    return x


R_LEAK = 1e9                                            # probe node leak to ground in the transient models
TC_BASE, COMP_BASE = 0x40000400, 0x40002400      # vendor TIM3 (capture), comparators


def expm3(A):
    """exp(A) of a 3x3 matrix: scaling and squaring with a Taylor series."""
    mul = lambda X, Y: [[sum(X[r][k] * Y[k][c] for k in range(3)) for c in range(3)] for r in range(3)]
    norm = max(sum(abs(x) for x in row) for row in A)
    s = max(0, math.ceil(math.log2(norm / 0.5))) if norm > 0.5 else 0
    X = [[x / 2 ** s for x in row] for row in A]
    E = [[float(r == c) for c in range(3)] for r in range(3)]
    T = [row[:] for row in E]
    for k in range(1, 14):
        T = [[x / k for x in row] for row in mul(T, X)]
        E = [[E[r][c] + T[r][c] for c in range(3)] for r in range(3)]
    for _ in range(s):
        E = mul(E, E)
    return E


class Devices:
    """Semiconductor models (probes 0..2), e.g. ('D', a, k), ('NPN', b, c, e), ('PNP', b, c, e),
    ('NMOS', g, d, s), ('PMOS', g, d, s). Device parameters as keyword dict in a 5th element."""
    VT = 0.02585

    @staticmethod
    def junction(vd, Is, n):
        """Diode current with the exponential continued linearly above 40 n Vt (Newton safety)."""
        x = vd / (n * Devices.VT)
        if x > 40:
            return Is * (math.exp(40) * (1 + x - 40) - 1)
        return Is * (math.exp(x) - 1)

    @staticmethod
    def add(d, v, out):
        kind, p = d[0], (d[4] if len(d) > 4 else {})
        if kind == 'D':                         # 1N4148-like: V_f about 0.70 V at 7 mA
            a, k = d[1], d[2]
            c = Devices.junction(v[a] - v[k], p.get('Is', 2.68e-9), p.get('n', 1.84)) + (v[a] - v[k]) * 1e-10
            out[a] -= c; out[k] += c
        elif kind in ('NPN', 'PNP'):            # Ebers-Moll transport model
            b, cl, e = d[1], d[2], d[3]
            s = 1 if kind == 'NPN' else -1
            Is, bf, br = p.get('Is', 1e-14), p.get('bf', 200), p.get('br', 5)
            fe = Devices.junction(s * (v[b] - v[e]), Is, 1)
            fc = Devices.junction(s * (v[b] - v[cl]), Is, 1)
            ic = fe - fc - fc / br              # into the collector (NPN)
            ib = fe / bf + fc / br
            ie = -(ic + ib)
            out[cl] += s * -ic; out[b] += s * -ib; out[e] += s * -ie   # model currents flow into the device
        elif kind in ('NMOS', 'PMOS'):          # square law + body diode (S->D for N)
            gt, dr, sr = d[1], d[2], d[3]
            s = 1 if kind == 'NMOS' else -1
            vth, K = p.get('vth', 2.0), p.get('K', 0.5)
            vd, vs, vg = s * v[dr], s * v[sr], s * v[gt]
            swap = vd < vs
            if swap:
                vd, vs = vs, vd
            vgs, vds = vg - vs, vd - vs
            if vgs <= vth:
                i = 0.0
            elif vds < vgs - vth:
                i = K * ((vgs - vth) * vds - vds * vds / 2)
            else:
                i = K / 2 * (vgs - vth) ** 2
            if swap:
                i = -i
            i += vds * 1e-9 * (-1 if swap else 1)     # off-state leakage
            out[dr] -= s * i; out[sr] += s * i      # drain -> source (N)
            bd_ = Devices.junction(s * (v[sr] - v[dr]), 2.68e-9, 1.84)
            out[sr] -= s * bd_; out[dr] += s * bd_
        else:
            raise ValueError(kind)


class Board:
    def __init__(self):
        self.reg = {}                                   # last written value per address
        self.out = {p: 0 for p in 'ABCF'}
        self.inp = {p: 0 for p in 'ABCF'}
        self.inp['C'] |= 1 << 13                        # TEST released (pull-up)
        self.inp['F'] |= 1 << 7                         # ZENER released
        self.inp['A'] |= 1 << 11                        # IR receiver idle (high)
        self.zener = None                               # Zener diode in the K-A-A socket (V), None = empty
        self.ir = None                                  # (start us, [mark, space, mark ...] us) on PA11
        self.events = []                                # (ms, what)
        self.uart_tx, self.uart_rx = bytearray(), bytearray()   # uart_rx: bytes the PC is sending
        self.rx_reg, self.rx_ore = None, False                  # single-byte receive register, overrun
        self.lcd = Panel()
        self.ms = 0
        self.first_pa8 = None
        self.writes = 0
        self.kr = []
        self.adc = {8: 1700, 0: 2035, 2: 0}     # raw codes; ch8 1700 = 4.10 V battery, ch0 2.5 V ref at 5.02 V
        self.us = 0
        self.v_nodes, self.t_nodes, self.g_nodes = None, 0, [0, 0, 0]
        self.dut = {}                           # {(probe_a, probe_b): ohms}, probes 0..2
        self.devs = []                          # semiconductors, see Devices (probes 0..2)
        self.caps = {}                          # {(probe_a, probe_b): farads}
        self.expm_cache = {}
        self.tc_run, self.tc_sts, self.tc_ccr, self.tc_t0, self.tc_ovf = False, 0, 0, 0, 0
        self.vcc = 5.020                        # ADC full scale (V)
        self.vdrive = self.vcc                  # probe pins drive to the ADC full scale (4.39 V seen on the meter was the stale-sample artifact)
        self.adc_dat, self.adc_pending = 0, None    # data register; (time, value) of the running conversion
        self.esr = {}                           # {(probe_a, probe_b): ohms} series resistance of self.caps
        self.ind = {}                           # {(probe_a, probe_b): henries} in series with self.dut[pair]
        self.tc_arm = 0                         # time the capture got enabled (CCER)

    def zener_raw(self):
        if not self.out['F'] >> 6 & 1:
            return 0
        v = 38.0 if self.zener is None else self.zener          # boost open voltage: a guess
        return min(4095, int(v / 10 * 4096 / 5.032))

    def ir_level(self, now):
        """PA11 while an IR frame plays: low during marks."""
        t0, durs = self.ir
        t = now - t0
        if t < 0:
            return 1
        for i, d in enumerate(durs):
            if t < d:
                return 0 if i % 2 == 0 else 1
            t -= d
        return 1

    def ir_playing(self):
        return self.ir is not None and max(self.us, self.ms * 1000) < self.ir[0] + sum(self.ir[1]) + 60000

    def pin(self, port, n):
        return self.out[port] >> n & 1

    def set_in(self, port, n, v):
        self.inp[port] = (self.inp[port] & ~(1 << n)) | (v << n)

    def adc_start(self):
        ch = self.reg.get(0x40020838, 0) & 31
        if self.adc_pending:                            # the previous conversion has finished by now
            self.adc_dat = self.adc_pending[1]
        code0 = not self.reg.get(0x40020814, 0) >> 4 * ch & 15
        if self.esr and ch in (1, 3, 6) and code0:      # ESR pulses (only they use sample code 0): sampled
                                                        # at the end of a few-us pulse
            v = self.esr_instant({1: 0, 3: 1, 6: 2}[ch])
        else:
            self.us = max(self.us, self.ms * 1000) + 4  # a conversion takes time: nodes settle meanwhile
            if ch == 2:                                 # Zener socket via 10:1: boost on (PF6) -> the
                v = self.zener_raw()                    # diode voltage, or the open boost voltage
            else:
                v = self.probe_adc(ch) if ch in (1, 3, 6) else self.adc.get(ch, 0)
        self.adc_pending = (self.us, v)

    def esr_instant(self, n):
        """Node voltage right after the drive changed: caps keep their voltage (in series with their ESR),
        the 40 pF node capacitances follow at once."""
        self.settle()
        g, i, links = self.drive_state()
        G = [[0.0] * 3 for _ in range(3)]
        I = list(i)
        for k in range(3):
            G[k][k] += g[k]
        for (a, b), r in links.items():
            x = 1 / r
            G[a][a] += x; G[b][b] += x; G[a][b] -= x; G[b][a] -= x
        for (a, b) in self.caps:
            x = 1 / (self.esr.get((a, b), 0) + 0.2)     # + 0.2 Ohm: probe leads (NV R0 default)
            vc = self.v_nodes[a] - self.v_nodes[b]
            G[a][a] += x; G[b][b] += x; G[a][b] -= x; G[b][a] -= x
            I[a] += vc * x; I[b] -= vc * x
        v = solve3(G, I)[n]
        return max(0, min(4095, int(v / self.vcc * 4096)))

    def write(self, a, v):
        self.writes += 1
        if a == 0x40020808 and v & 0x500000 == 0x500000:
            self.adc_start()
        if a == 0x40003000:
            self.kr.append(v)
        base, off = a & ~0x3FF, a & 0x3FF
        if (base in PORTS and off in (0x00, 0x14, 0x18, 0x28)) and PORTS[base] in 'AB':
            self.settle()                               # nodes move under the old drive (mode + level) until now
        if a == TC_BASE + 0x10:                         # vendor TIM3 STS: write 0 to clear
            self.tc_sts &= v
            return
        if a == TC_BASE:                                # vendor TIM3 CR1: start/stop
            self.settle()
            if v & 1 and not self.tc_run:
                self.tc_t0, self.tc_ovf = max(self.us, self.ms * 1000), 0
            self.tc_run = bool(v & 1)
        if a == TC_BASE + 0x20 and v & 1 and not self.reg.get(a, 0) & 1:
            self.tc_arm = max(self.us, self.ms * 1000)
        self.reg[a] = v
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

    # probe pins: (port, pin) for direct, Rl, Rh of probes 1..3
    PROBE_PINS = [(('A', 1), ('A', 4), ('A', 5)), (('A', 3), ('A', 7), ('B', 1)), (('A', 6), ('B', 2), ('B', 10))]
    PORT_BASE = {'A': 0x40010800, 'B': 0x40010C00}

    def driven(self, port, pin):
        mode = self.reg.get(self.PORT_BASE[port], 0xFFFFFFFF) >> 2 * pin & 3
        return mode == 1, self.out[port] >> pin & 1

    def node_volts(self):
        """Nodal analysis of the three probe nodes (V)."""
        G = [[0.0] * 3 for _ in range(3)]
        I = [0.0] * 3
        for n, pins in enumerate(self.PROBE_PINS):
            G[n][n] += 1 / 66e6                 # leak to ground (meter: 470k pull-up settles 35 mV low)
            for (port, pin), r in zip(pins, (0, 680, 470000)):
                on, lvl = self.driven(port, pin)
                if on:
                    g = 1 / (r + (22 if lvl else 20))   # MCU pin resistance high / low
                    G[n][n] += g
                    I[n] += g * self.vdrive * lvl
        self.g_nodes = [G[n][n] for n in range(3)]   # before links: node's own paths
        links = dict(self.dut)
        if not self.pin('B', 3):                # relay off: probes shorted
            for pair in ((0, 1), (0, 2), (1, 2)):
                links[pair] = 0.1
        for (a, b), r in links.items():
            g = 1 / r
            G[a][a] += g; G[b][b] += g; G[a][b] -= g; G[b][a] -= g
            self.g_nodes[a] += g * 0.5; self.g_nodes[b] += g * 0.5   # rough: a link helps both nodes settle
        for c in range(3):                      # Gaussian elimination
            p = max(range(c, 3), key=lambda r: abs(G[r][c]))
            G[c], G[p] = G[p], G[c]; I[c], I[p] = I[p], I[c]
            for r in range(c + 1, 3):
                f = G[r][c] / G[c][c]
                for k in range(c, 3):
                    G[r][k] -= f * G[c][k]
                I[r] -= f * I[c]
        v = [0.0] * 3
        for r in (2, 1, 0):
            v[r] = (I[r] - sum(G[r][k] * v[k] for k in range(r + 1, 3))) / G[r][r]
        return v

    C_NODE = 40e-12                             # probe node capacitance (pin + PCB + socket; the meter showed a gate
                                                # node charging through 470k in under 1 us, so it is only pF)

    def settle(self):
        """Move the probe node voltages to the present time under the present drive."""
        now = max(self.us, self.ms * 1000)
        dt = (now - self.t_nodes) * 1e-6
        self.t_nodes = now
        if self.v_nodes is None:
            if not self.devs and not self.caps:
                self.v_nodes = self.node_volts()
                return
            self.v_nodes, dt = [0.0] * 3, 1.0
        self.v_nodes = self.propagate(self.v_nodes, dt)

    def propagate(self, v0, dt):
        """Node voltages dt seconds after v0 with the present drive (does not change the state)."""
        if self.devs:
            return self.propagate_nonlinear(v0, dt)
        if self.caps:
            return self.propagate_exact(v0, dt)
        return self.propagate_linear(v0, dt)

    def propagate_linear(self, v0, dt):
        """tau = C_NODE * Thevenin resistance of each node (coupling between nodes ignored for tau)."""
        target = self.node_volts()
        v = list(v0)
        for n in range(3):
            g = self.g_nodes[n]
            k = 1 - math.exp(-dt * g / self.C_NODE) if g > 0 else 0
            v[n] += (target[n] - v[n]) * k
        return v

    def propagate_exact(self, v0, dt):
        """Linear network with capacitors between probes: C dv/dt = I - G v, solved with a matrix exponential."""
        g, i, links = self.drive_state()
        key = (tuple(g), tuple(i), tuple(sorted(links.items())), tuple(sorted(self.caps.items())), round(dt, 12))
        hit = self.expm_cache.get(key)
        if hit is None:
            G = [[0.0] * 3 for _ in range(3)]
            for n in range(3):
                G[n][n] += g[n]
            for (a, b), r in links.items():
                x = 1 / r
                G[a][a] += x; G[b][b] += x; G[a][b] -= x; G[b][a] -= x
            C = self.cap_matrix()
            v_inf = solve3([r[:] for r in G], list(i))
            cols = [solve3([r[:] for r in C], [G[r][k] for r in range(3)]) for k in range(3)]   # C^-1 G
            A = [[-cols[k][r] * dt for k in range(3)] for r in range(3)]
            hit = (v_inf, expm3(A))
            if len(self.expm_cache) > 20000:
                self.expm_cache.clear()
            self.expm_cache[key] = hit
        v_inf, E = hit
        d = [v0[n] - v_inf[n] for n in range(3)]
        return [v_inf[r] + sum(E[r][k] * d[k] for k in range(3)) for r in range(3)]

    # ---- semiconductors: node currents + backward-Euler transient (Newton on the 3 probe nodes) ----
    def drive_state(self):
        """(conductance, source current) per node from the MCU pins, leak and relay/resistor links.
        Leak 1 GOhm: a PNP with open base read I_CEO 0.4-0.5 uA on the meter (hFE x a few nA). The linear
        path (node_volts) keeps 66 MOhm, fitted to 470k readings settling 35 mV low (probably ADC loading)."""
        g, i = [1 / R_LEAK] * 3, [0.0] * 3
        for n, pins in enumerate(self.PROBE_PINS):
            for (port, pin), r in zip(pins, (0, 680, 470000)):
                on, lvl = self.driven(port, pin)
                if on:
                    gg = 1 / (r + (22 if lvl else 20))
                    g[n] += gg
                    i[n] += gg * self.vdrive * lvl
        links = dict(self.dut)
        if not self.pin('B', 3):
            for pair in ((0, 1), (0, 2), (1, 2)):
                links[pair] = 0.1
        return g, i, links

    def node_currents(self, v, g, i, links):
        """Net current into each node (A)."""
        out = [i[n] - g[n] * v[n] for n in range(3)]
        for (a, b), r in links.items():
            c = (v[a] - v[b]) / r
            out[a] -= c; out[b] += c
        for d in self.devs:
            Devices.add(d, v, out)
        return out

    def propagate_nonlinear(self, v0, dt):
        g, i, links = self.drive_state()
        v = list(v0)
        h = 1e-6                                # geometric sub-steps: 1, 2, 4 ... us until dt is used up
        while dt > 1e-9:
            step = min(h, dt)
            v = self.be_step(v, step, g, i, links)
            dt -= step
            h *= 2
        return v

    # ---- COMP1 + vendor TIM3 CH1 capture (cap.c SmallCap): the counter runs at 48 MHz ----
    def tc_poll(self):
        """Firmware polls the timer flags: advance time by a step, capture where probe-1 crosses VREF1."""
        self.settle()
        now = max(self.us, self.ms * 1000)
        dt = min(1000.0, max(0.2, 0.05 * (now - self.tc_t0)))
        v0 = list(self.v_nodes)
        self.us = now + dt
        self.settle()
        ctrl, ref = self.reg.get(COMP_BASE + 0x10, 0), self.reg.get(COMP_BASE + 0x40, 0)
        if ctrl & 1 and ctrl >> 1 & 7 == 7 and ctrl >> 7 & 15 == 6 and ref & 1 and not self.tc_sts & 2 \
                and any(n in pair for pair in self.ind for n in [ctrl >> 4 & 7]):
            n = ctrl >> 4 & 7
            vref = self.vcc * (ref >> 1 & 63) / 64
            if self.driven(*self.PROBE_PINS[n][0]) == (True, 0):
                vref *= 0.885                       # sense pin as shunt at ~55 mA: trips early (fitted to the meter)
            tx = self.ind_cross(n, vref)            # us after the timer start, None: never
            if tx is not None and self.tc_t0 + tx <= self.us and self.tc_t0 + tx >= self.tc_arm:
                self.tc_ccr = int((tx + 0.21) * 48) & 0xFFFF    # + pin/comparator/capture delay (meter: 10 ticks)
                self.tc_sts |= 2
        elif ctrl & 1 and ctrl >> 1 & 7 == 7 and ctrl >> 7 & 15 == 6 and ref & 1 and not self.tc_sts & 2:
            n = ctrl >> 4 & 7
            vref = self.vcc * (ref >> 1 & 63) / 64
            if v0[n] < vref <= self.v_nodes[n]:
                lo, hi = 0.0, dt
                for _ in range(40):
                    mid = (lo + hi) / 2
                    if self.propagate(v0, mid * 1e-6)[n] >= vref:
                        hi = mid
                    else:
                        lo = mid
                ticks = int((now + hi + 0.1 - self.tc_t0) * 48)     # + 100 ns comparator delay
                self.tc_ccr = ticks & 0xFFFF
                self.tc_sts |= 2
        ovf = int((self.us - self.tc_t0) * 48) >> 16
        if ovf > self.tc_ovf:
            self.tc_sts |= 1
            self.tc_ovf = ovf

    def ind_cross(self, n, vref):
        """Inductor (in series with its dut resistance) switched on at the timer start, current zero before:
        node n moves from its value with the inductor open to the DC value with the time constant
        L / (R_L + resistance seen by the inductor). First-order model, node capacitances ignored."""
        (a, b), L = next(iter(self.ind.items()))
        g, i, links = self.drive_state()
        def solve(lk, inj=None):
            G = [[0.0] * 3 for _ in range(3)]
            for k in range(3):
                G[k][k] += g[k]
            for (p, q), r in lk.items():
                x = 1 / r
                G[p][p] += x; G[q][q] += x; G[p][q] -= x; G[q][p] -= x
            return solve3(G, inj if inj else list(i))
        v_end = solve(links)[n]
        open_links = {k: r for k, r in links.items() if k != (a, b)}
        v_start = solve(open_links)[n]
        th = solve(open_links, [(1.0 if k == a else -1.0 if k == b else 0.0) for k in range(3)])
        tau = L / (self.dut[(a, b)] + th[a] - th[b]) * 1e6
        if not v_start < vref < v_end:
            return 0.0 if v_start >= vref else None
        return -tau * math.log(1 - (vref - v_start) / (v_end - v_start))

    def cap_matrix(self):
        C = [[0.0] * 3 for _ in range(3)]
        for n in range(3):
            C[n][n] = self.C_NODE
        for (a, b), x in self.caps.items():
            C[a][a] += x; C[b][b] += x; C[a][b] -= x; C[b][a] -= x
        return C

    def be_step(self, v0, h, g, i, links):
        C = self.cap_matrix()
        c = [[x / h for x in row] for row in C]
        v = list(v0)
        for _ in range(60):
            f = self.node_currents(v, g, i, links)
            F = [sum(c[n][k] * (v[k] - v0[k]) for k in range(3)) - f[n] for n in range(3)]
            if max(abs(x) for x in F) < 1e-9:
                break
            J = [[0.0] * 3 for _ in range(3)]
            for k in range(3):
                vk = list(v); vk[k] += 1e-6
                fk = self.node_currents(vk, g, i, links)
                for n in range(3):
                    J[n][k] = c[n][k] - (fk[n] - f[n]) / 1e-6
            dv = solve3(J, [-x for x in F])
            m = max(abs(x) for x in dv)
            if m > 0.2:                         # damping: junction exponentials overshoot otherwise
                dv = [x * 0.2 / m for x in dv]
            v = [v[n] + dv[n] for n in range(3)]
            if m < 1e-7:
                break
        return v

    def probe_adc(self, ch):
        self.settle()
        v = self.v_nodes[{1: 0, 3: 1, 6: 2}[ch]]
        return max(0, min(4095, int(v / self.vcc * 4096)))

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
            # hardware: the end-of-conversion flag stays set, so a read right after the start (adc.c convert())
            # still gets the previous conversion; once time has passed (ESR: 30 us) the new one is there
            if self.adc_pending and self.us > self.adc_pending[0]:
                self.adc_dat, self.adc_pending = self.adc_pending[1], None
            v = self.adc_dat
        elif a == 0x40000824:                           # TIM3 counter: 1 us per read, never behind the ms tick
            # (8 us per read while an IR frame plays: the polling loop keeps pace with the ms tick, so
            #  its time has no 1 ms jumps)
            self.us = max(self.us + (8 if self.ir_playing() else 1), self.ms * 1000)
            v = int(self.us) & 0xFFFF
        elif a == TC_BASE + 0x10:
            if self.tc_run:
                self.tc_poll()
            v = self.tc_sts
        elif a == TC_BASE + 0x34:
            v = self.tc_ccr
        elif a == TC_BASE + 0x24:                       # vendor TIM3 counter (48 MHz): 0.25 us per read
            if self.tc_run:
                self.us = max(self.us, self.ms * 1000) + 0.25
            v = int((max(self.us, self.ms * 1000) - self.tc_t0) * 48) & 0xFFFF if self.tc_run else 0
        elif a == 0x40012008:
            v = 0x02                                    # SPI: TX empty, not busy
        elif a == 0x40013800:                           # USART1 status: TX empty, RXNE, ORE
            v = 0x80 | (0x20 if self.rx_reg is not None else 0) | (0x08 if self.rx_ore else 0)
        elif a == 0x40013804:
            v = self.rx_reg if self.rx_reg is not None else 0
            self.rx_reg, self.rx_ore = None, False
        elif base in PORTS and off == 0x10:
            p = PORTS[base]
            mode = self.reg.get(base, 0)
            outs = sum(1 << n for n in range(16) if mode >> 2 * n & 3 == 1)
            inp = self.inp[p]
            if p == 'A' and self.ir is not None:
                inp = (inp & ~(1 << 11)) | self.ir_level(max(self.us, self.ms * 1000)) << 11
            v = (self.out[p] & outs) | (inp & ~outs)
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


buzz = []                                               # buzzer channel register (TIM1 CCER) writes


def on_w(uc, acc, a, sz, v, _):
    if a == 0x40013420:
        buzz.append(v)
    bd.write(a, v)


def on_r(uc, acc, a, sz, v, _):
    uc.mem_write(a, struct.pack('<I', bd.read(a) & 0xFFFFFFFF)[:sz])


NV_PAGE, FL = 0x0800F800, 0x40022000
STOCK_CAL = bytes(range(0x30))                          # stand-in for the stock calibration record (0x0800FFD0)
uc.mem_write(NV_PAGE, b'\xff' * 0x800)
uc.mem_write(0x0800FFD0, STOCK_CAL)
flash = {'locked': True, 'keys': [], 'errors': [], 'erases': [], 'words': 0}


def on_flash_w(uc, acc, a, sz, v, _):
    """flash controller: KEY1/KEY2 unlock, CTRL PG/PER/START/LOCK; programming only erased words"""
    if a == FL + 0x04:
        flash['keys'] = (flash['keys'] + [v])[-2:]
        if flash['keys'] == [0x45670123, 0xCDEF89AB]:
            flash['locked'] = False
    elif a == FL + 0x10:
        if v & 0x80:
            flash['locked'] = True
        elif v & 0x42 == 0x42:
            if flash['locked']:
                flash['errors'].append('erase while locked')
            else:
                pg = bd.reg.get(FL + 0x14, 0) & ~0x1FF
                flash['erases'].append(pg)
                if not NV_PAGE <= pg < NV_PAGE + 0x400:
                    flash['errors'].append(f'erase outside the NV page {pg:#x}')
                else:
                    uc.mem_write(pg, b'\xff' * 0x200)
    if a == FL + 0x0C:
        return                                          # STS: write 1 to clear, reads 0 (never busy)
    bd.write(a, v)


def on_flash_prog(uc, acc, a, sz, v, _):
    old = struct.unpack('<I', uc.mem_read(a & ~3, 4))[0]
    ok = (not flash['locked'] and bd.reg.get(FL + 0x10, 0) & 1 and sz == 4 and not a & 3 and old == 0xFFFFFFFF
          and NV_PAGE <= a < NV_PAGE + 0x400)
    flash['words'] += 1
    if not ok:
        flash['errors'].append(f'bad write {a:#x}={v:#x} size {sz} old {old:#x} locked {flash["locked"]}')


uc.hook_add(UC_HOOK_MEM_WRITE, on_flash_w, begin=FL, end=FL + 0x3FF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_flash_prog, begin=0x08000000, end=0x0800FFFF)
uc.hook_add(UC_HOOK_MEM_WRITE, on_w, begin=0x40000000, end=FL - 1)
uc.hook_add(UC_HOOK_MEM_WRITE, on_w, begin=FL + 0x400, end=0x4002FFFF)
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


held = [0]

def run_ms(n):
    """Advance n ms. Each chunk of CHUNK instructions normally counts as 1 ms (idle code waiting for the tick).
    When the firmware is timing itself (ADC conversions / TIM3 reads advanced the us clock in the chunk), the chunk
    only counts its real duration at 48 MHz and the ms tick follows the us clock: busy loops such as polling
    a node while a gate charges then see the right time between polls."""
    for _ in range(n):
        while True:
            us0 = bd.us
            try:
                uc.emu_start(pc[0] | 1, 0, count=CHUNK)
            except UcError as e:
                raise SystemExit(f'emulation error {e} at {uc.reg_read(UC_ARM_REG_PC):#x}')
            pc[0] = uc.reg_read(UC_ARM_REG_PC)
            if bd.us == us0:
                break                                   # idle: 1 ms
            if uc.reg_read(UC_ARM_REG_PRIMASK) & 1:    # interrupts off (ESR pulses: a few us, the chunk
                held[0] += CHUNK // 48                  # ended inside one): count the time after the pulse
                continue
            bd.us += CHUNK // 48 + held[0]
            held[0] = 0
            if bd.us >= (bd.ms + 1) * 1000:
                break
        # the ms tick catches up when one chunk advanced the us clock by more than 1 ms (cap.c's timer polls)
        for _ in range(max(1, int(bd.us // 1000) - bd.ms)):
            bd.ms += 1
            for _ in range(11):                         # ~11 bytes per ms at 115200 baud
                if not bd.uart_rx:
                    break
                b = bd.uart_rx.pop(0)
                if bd.rx_reg is None:
                    bd.rx_reg = b
                else:
                    bd.rx_ore = True                    # overrun: byte lost (N32: RXNE stays as it was)
            t = struct.unpack('<I', uc.mem_read(G_MS, 4))[0]
            uc.mem_write(G_MS, struct.pack('<I', t + 1))


def send(data, gap_ms=2):
    """PC sends bytes paced gap_ms apart (the USART holds one byte)."""
    for b in data:
        bd.uart_rx.append(b)
        run_ms(gap_ms)


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
boot_log = s
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
      '#A ' in s and 'bat=4100mV' in s and 'BAT=1700' in s and 'REF=2035' in s, s[-200:])
check('ADC: sample time code 14 on ch8 (settles from 470k), rank 1 = selected channel', (bd.reg.get(0x40020810, 0) & 15) == 14)
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

# --- measurements: TEST runs a ComponentTester probing cycle on the modelled probe network
def measure(dut, max_ms=8000, devs=(), caps=None, esr=None, ind=None):
    bd.dut = dict(dut)
    bd.ind = dict(ind or {})
    bd.devs = list(devs)
    bd.caps = dict(caps or {})
    bd.esr = dict(esr or {})
    bd.v_nodes = None
    uart_text()
    ev0 = len(bd.events)
    bd.set_in('C', 13, 0); run_ms(80); bd.set_in('C', 13, 1)
    out = ''
    for _ in range(max_ms // 100):
        run_ms(100)
        out += uart_text()
        if '#R found' in out:
            run_ms(600); out += uart_text()     # result screen + result beep
            break
    relay = [e for _, e in bd.events[ev0:] if e.startswith('PB3=')]
    if os.environ.get('DUMP'):
        print(f'--- {dut} {list(devs)}'); print('\n'.join(l for l in out.splitlines() if l.startswith('#')))
    return out, relay

def resistors(out):
    return [(l.split()[2], int(l.split()[3]) * 10 ** int(l.split()[4][1:])) for l in out.splitlines()
            if l.startswith('#R R ')]

out, relay = measure({})
check('no part: "no component", ADC full scale from 2.5 V ref, drive level measured', 'found=0' in out
      and 'ref=1' in out and 'fs=502' in out and 'vcc=50' in out, out[-160:])
check('relay frees the probes for the test and shorts them again after', relay[:1] == ['PB3=1'] and relay[-1:] == ['PB3=0']
      and bd.pin('B', 3) == 0, relay)
for dut, pins, ohms, tol in (({(0, 1): 1000}, '1-2', 1000, 0.01), ({(1, 2): 360}, '2-3', 360, 0.01),
                             ({(0, 2): 1e6}, '1-3', 1e6, 0.02), ({(0, 1): 4.7 + 0.2}, '1-2', 4.7, 0.03)):   # 0.2 ohm probe leads (R_ZERO)
    out, _ = measure(dut)
    r = resistors(out)
    ok = len(r) == 1 and r[0][0] == pins and abs(r[0][1] - ohms) <= tol * ohms
    check(f'resistor {ohms:g} ohm on {pins}: {r}', ok, out[-200:])
run_ms(2500)                                        # result must survive the once-a-second refresh
bd.lcd.png(os.path.join(OUTDIR, 'screen_resistor.png'))
# edge case seen on the meter: open pairs read just under CheckResistor's 20 mV "discharged" limit
# and get further into the check; they must still not become (phantom) resistors
c_node, bd.C_NODE = bd.C_NODE, 1.85e-9
out, _ = measure({(0, 2): 360})
r = resistors(out)
check('no phantom resistors when open pairs read just under 20 mV', len(r) == 1 and r[0][0] == '1-3', r)
bd.C_NODE = c_node
check('result screen stays (no ADC rows drawn over it)', bd.lcd.count(0xB47F, 72, 104) > 150
      and bd.lcd.count(0xFFFF, 64, 128) < 50, (bd.lcd.count(0xB47F, 72, 104), bd.lcd.count(0xFFFF, 64, 128)))
bd.dut = {}
check('beep: buzzer channel enabled then disabled again', bd.reg.get(0x40013420) == 0x12, hex(bd.reg.get(0x40013420, 0)))
n0 = len(buzz)
measure({(0, 1): 1000})
check('a test ends with a beep', len(buzz) > n0, buzz[n0:])

# --- semiconductors (upstream semi.c on the device models)
def result(out):
    """title and lines of the last '#R' result block"""
    lines = [l for l in out.splitlines() if l.startswith('#R ') and not l.startswith('#R found')]
    return [l[3:].strip() for l in lines]

def num(text):
    """'0.702V' -> 0.702"""
    return float(''.join(c for c in text if c in '0123456789.-'))

# --- capacitors (upstream cap.c: SmallCap with COMP1 + TIM3 capture below 4.7 uF, LargeCap above)
def cap_value(r):
    c = [l for l in r if l.startswith('C ')]
    if not c:
        return None
    t = c[0].split()[1]
    mult = {'p': 1e-12, 'n': 1e-9, 'u': 1e-6, 'µ': 1e-6, 'm': 1e-3}
    return num(t) * mult.get(t.rstrip('F')[-1], 1)

def esr_tests():
    """cap.c MeasureESR (the ESR model adds 0.2 Ohm of probe leads, which R0 takes off again). The noiseless
    ADC gives about 0.18 Ohm per code of U_2 - U_1 (U_1 = 113 codes): the meter's noise dithers that."""
    for pair, farads, ohms in (((0, 2), 22e-6, 1.0), ((1, 0), 470e-6, 0.3), ((0, 2), 1e-6, 4.7), ((1, 2), 100e-6, 100)):
        out, _ = measure({}, max_ms=20000, caps={pair: farads}, esr={pair: ohms})
        r = result(out)
        e = [l for l in r if l.startswith('ESR')]
        v = num(e[0].split()[1]) * (1e-3 if e[0].rstrip('\x7f').endswith('m') else 1) if e else None
        want = ohms
        check(f'ESR {ohms:g} Ohm of {farads:g} F reads {want:g}: {r}', v is not None and abs(v - want) <= 0.05 * want + 0.1,
              [l for l in out.splitlines() if 'ESR' in l][-3:])
    bd.caps, bd.esr = {}, {}

def ind_tests():
    """inductor.c: L from the current rise time (first-order model in Board.ind_cross)"""
    for pair, henries, ohms in (((0, 1), 10e-3, 30.0), ((0, 2), 1e-3, 3.0), ((1, 2), 100e-6, 0.5),
                                ((0, 1), 47e-3, 120.0)):
        out, _ = measure({pair: ohms}, ind={pair: henries})
        r = [l for l in out.splitlines() if l.startswith('#R L ')]
        v = int(r[0].split()[2]) * 10 ** int(r[0].split()[3][1:]) if r else None
        if henries == 10e-3:
            bd.lcd.png(os.path.join(OUTDIR, 'screen_inductor.png'))
        check(f'inductor {henries:g} H + {ohms:g} Ohm reads {v}', v is not None and abs(v - henries) <= 0.1 * henries,
              [l for l in out.splitlines() if l.startswith(('#C L', '#R'))][-6:])
    for ohms in (100, 10):
        out, _ = measure({(0, 1): ohms})
        check(f'plain {ohms} Ohm resistor: no inductance', '#R L ' not in out and '#R R 1-2' in out, out[-200:])

    bd.ind = {}

# ---- IR remotes (generators as in lcr-p1-linux firmware/test/ir_test.py; +100 us marks, -100 us spaces)
def _skew(ms):
    return [d + (100 if i % 2 == 0 else -100) for i, d in enumerate(ms)]

def _pd(lead, b4):
    out = [lead, 4500]
    for k in range(32):
        out += [560, 1690 if b4[k >> 3] >> (k & 7) & 1 else 560]
    return _skew(out + [560])

def _sony(cmd, addr, nb):
    v, out = cmd | addr << 7, [2400]
    for k in range(nb):
        out += [600, 1200 if v >> k & 1 else 600]
    return _skew(out)

def _rc5(toggle, addr, cmd):
    bits = [1, 1 if cmd < 64 else 0, toggle] + [addr >> i & 1 for i in range(4, -1, -1)] + [cmd >> i & 1 for i in range(5, -1, -1)]
    halves = []
    for b in bits:
        halves += [0, 1] if b else [1, 0]
    halves = halves[1:]
    while halves[-1] == 0:
        halves.pop()
    durs, cur, n = [], halves[0], 0
    for h in halves:
        if h == cur:
            n += 1
        else:
            durs.append((cur, n)); cur, n = h, 1
    durs.append((cur, n))
    return [n * 889 + (100 if lvl else -100) for lvl, n in durs]

def _gree(b8):
    b8 = list(b8)
    b8[7] = (b8[7] & 15) | ((10 + sum(x & 15 for x in b8[:4]) + sum(x >> 4 for x in b8[4:7])) & 15) << 4
    bits = lambda bs: [x >> k & 1 for x in bs for k in range(8)]
    out = [9000, 4500]
    for b in bits(b8[:4]) + [0, 1, 0]:
        out += [560, 1690 if b else 560]
    out += [560, 20000]
    for b in bits(b8[4:]):
        out += [560, 1690 if b else 560]
    return _skew(out + [560]), b8

def ir_play(durs, ms=None):
    uart_text()
    bd.ir = (max(bd.us, bd.ms * 1000) + 2000, list(durs))
    run_ms(ms or sum(durs) // 1000 + 400)
    bd.ir = None
    return uart_text()

def ir_tests():
    bd.set_in('C', 14, 1)
    run_ms(300)
    s = uart_text()
    check('mode switch IR shown + logged', 'mode IR' in s, s)
    g, g8 = _gree([0x09, 0x08, 0x60, 0x50, 0, 0xC0, 0, 0])
    cases = [('NEC 04/08', _pd(9000, [0x04, 0xFB, 0x08, 0xF7]), '#I NEC 04 FB 08 F7'),
             ('Samsung 07/02', _pd(4500, [0x07, 0x07, 0x02, 0xFD]), '#I Samsung 07 07 02 FD'),
             ('Sony12 a1 c21', _sony(21, 1, 12), '#I Sony12 00 01 00 15'),
             ('Sony15 a0x97 c0x12', _sony(0x12, 0x97, 15), '#I Sony15 00 97 00 12'),
             ('Sony20 a0x1a5b c0x3c', _sony(0x3C, 0x1A5B, 20), '#I Sony20 1A 5B 00 3C'),
             ('RC5 a5 c12 toggle 1', _rc5(1, 5, 12), '#I RC5 00 05 01 0C'),
             ('RC5 a0 c70 (field bit) toggle 0', _rc5(0, 0, 70), '#I RC5 00 00 00 46'),
             ('Gree AC', g, '#I AC Gree ' + ' '.join('%02X' % x for x in g8))]
    for name, durs, want in cases:
        out = ir_play(durs)
        check(f'IR {name}: {want}', want + '\r\n' in out, out[-200:])
        if name == 'NEC 04/08':
            bd.lcd.png(os.path.join(OUTDIR, 'screen_ir.png'))
            check('IR result drawn (address/command rows)', bd.lcd.count(0xB47F, 64, 120) > 200)
    out = ir_play(_pd(9000, [0x04, 0xFB, 0x08, 0xF7]) + [40000] + _skew([9000, 2250, 560]))
    check('IR NEC + repeat frame: decoded once', out.count('#I NEC') == 1, out[-200:])
    out = ir_play([300, 7000, 120])
    check('IR noise: unknown, timings over serial', '#I raw 3:' in out and '#I NEC' not in out, out[-200:])
    import json
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ir_ac.jsonl')
    real = [[int(x) for x in json.loads(l)['fields']['timings_us'].split()] for l in open(path)]
    good = 0
    for t in real:
        out = ir_play(t)
        good += '#I AC Gree ' in out and f'#I raw {len(t)}:' in out
    check(f'IR: {good}/{len(real)} real Gree AC captures decoded (+ raw dump)', good == len(real))
    bd.set_in('C', 14, 0)
    run_ms(400)
    s = uart_text()
    check('back to normal mode from IR', 'mode normal' in s, s)

if os.environ.get('IR_ONLY'):
    ir_tests()
    sys.exit(0)

if os.environ.get('IND_ONLY'):
    ind_tests()
    sys.exit(0)

if os.environ.get('ESR_ONLY'):
    esr_tests()
    sys.exit(0)

for pair, farads, tol in (((0, 2), 100e-12, 0.05), ((1, 2), 10e-9, 0.03), ((0, 1), 1e-6, 0.03),
                          ((0, 2), 22e-6, 0.06), ((1, 0), 470e-6, 0.08)):   # LargeCap: CAP_FACTOR_LARGE not tuned yet
    out, _ = measure({}, max_ms=20000, caps={pair: farads})
    r = result(out)
    c = cap_value(r)
    names = '%u-%u' % (min(pair) + 1, max(pair) + 1)
    check(f'capacitor {farads:g} F on {names}: {r}', r[:1] and r[0].startswith('Capacitor') and c is not None
          and abs(c - farads) <= tol * farads + 15e-12, out[-300:])
    if farads == 10e-9:
        bd.lcd.png(os.path.join(OUTDIR, 'screen_cap.png'))
bd.caps = {}

esr_tests()
ind_tests()

out, _ = measure({}, devs=[("D", 0, 2)])
r = result(out)
check('diode A1 K3: "Diode", V_f about 0.70 V', len(r) >= 2 and r[0] == 'Diode' and r[1].startswith('A1 K3')
      and 0.6 < num(r[1].split()[2]) < 0.8, r)
bd.lcd.png(os.path.join(OUTDIR, 'screen_diode.png'))
out, _ = measure({}, devs=[('D', 1, 0), ('D', 0, 1)])
r = result(out)
check('anti-parallel diodes: two found', r[:1] == ['Diodes x2'], r)
out, _ = measure({}, devs=[('NPN', 1, 0, 2)])
r = result(out)
hfe = [l for l in r if l.startswith('hFE')]
check('NPN B2 C1 E3: pinout and hFE about 200, no I_CEO (model has none: base discharged first)', r[:2] == ['NPN', '1C 2B 3E']
      and hfe and 150 < num(hfe[0]) < 260 and not any(l.startswith('Iceo') for l in r), r)
bd.lcd.png(os.path.join(OUTDIR, 'screen_npn.png'))
out, _ = measure({}, devs=[('PNP', 2, 1, 0, {'bf': 120})])
r = result(out)
hfe = [l for l in r if l.startswith('hFE')]
iceo = [l for l in r if l.startswith('Iceo')]
check('PNP B3 C2 E1: pinout and hFE about 120, I_CEO below 1 uA (open base: leak x hFE, 0.4-0.5 uA on the meter)',
      r[:2] == ['PNP', '1E 2C 3B'] and hfe and 90 < num(hfe[0]) < 160
      and (not iceo or (iceo[0].endswith('nA') and num(iceo[0]) < 1000)), r)
out, _ = measure({}, devs=[('NMOS', 0, 1, 2)], caps={(0, 2): 1e-9, (0, 1): 200e-12})   # Cgs, Cgd
r = result(out)
vth = [l for l in r if l.startswith('Vth')]
check('N-MOSFET G1 D2 S3: pinout, V_th about 2.1 V, body diode', r[:2] == ['N-MOSFET enh', '1G 2D 3S'] and vth
      and 1.9 < num(vth[0]) < 2.4 and any(l.startswith('D 3>2') for l in r), r)
bd.lcd.png(os.path.join(OUTDIR, 'screen_nmos.png'))
out, _ = measure({}, devs=[('PMOS', 2, 0, 1, {'vth': 1.5})], caps={(2, 1): 1e-9, (2, 0): 200e-12})
r = result(out)
vth = [l for l in r if l.startswith('Vth')]
check('P-MOSFET G3 D1 S2: pinout, V_th about -1.6 V, body diode', r[:2] == ['P-MOSFET enh', '1D 2S 3G'] and vth
      and -1.9 < num(vth[0]) < -1.4 and any(l.startswith('D 1>2') for l in r), r)
bd.devs = []

# --- adjustment values: flash store + self-adjustment (#J)
check('boot: no stored adjustment -> defaults', '#N defaults' in boot_log, boot_log[-300:])
NV_ADDR = syms['NV']


def nv_ram():
    b = uc.mem_read(NV_ADDR, 12)
    return dict(zip(('RiL', 'RiH', 'RZero', 'CapZero', 'RefOffset', 'CompOffset', 'Contrast', 'CheckSum'),
                    struct.unpack('<HHHBbbBBx', bytes(b))))


def nv_records():
    recs = []
    for i in range(32):
        r = bytes(uc.mem_read(NV_PAGE + 16 * i, 16))
        if r[:4] == b'P1NV':
            recs.append(r[4:16])
    return recs


nv0 = nv_ram()
recs0 = nv_records()
check('after the cap tests: a record exactly when CompOffset moved by >= 3 mV',
      all(abs((b[8] ^ 0x80) - (a[8] ^ 0x80)) >= 3 for a, b in zip([bytes(10)] + recs0, recs0)) and   # int8 CompOffset (not recs0 or recs0[-1][:10] == bytes(uc.mem_read(NV_ADDR, 10))),
      (recs0, nv0))
uart_text()
def press_test():
    bd.set_in('C', 13, 0); run_ms(80); bd.set_in('C', 13, 1); run_ms(50)


def wait_for(text, out, ms=10000):
    for _ in range(ms // 100):
        if text in out:
            break
        run_ms(100); out += uart_text()
    return out


bd.dut, bd.devs, bd.caps, bd.v_nodes = {}, [], {}, None
n_buzz = len(buzz)
send(b'#J\n')
out = wait_for('Short all probes', '')
press_test()                                            # TEST before the wire is clamped
out = wait_for('Not shorted', out)
check('self-adjust: TEST without the short -> "Not shorted", no measurement', 'Not shorted' in out
      and '#J R0' not in out, out[-300:])
# probes shorted at one point, 0.15 ohm per lead: star -> triangle of 0.45 ohm = 0.3 ohm per pair
bd.dut, bd.v_nodes = {(0, 1): 0.45, (0, 2): 0.45, (1, 2): 0.45}, None
run_ms(3000); out += uart_text()
check('self-adjust: shorted but no TEST yet -> waits', '#J R0' not in out, out[-200:])
press_test()
out = wait_for('Remove the short', out)
r0 = [int(l.split()[3]) for l in out.splitlines() if l.startswith('#J R0 ')]
bd.dut, bd.v_nodes = {}, None                            # short removed
run_ms(500); out += uart_text()
check('self-adjust: open but no TEST yet -> waits', 'Adjusting Ri' not in out, out[-200:])
press_test()
for _ in range(200):
    run_ms(100); out += uart_text()
    if '#J done' in out:
        run_ms(500); out += uart_text()
        break
if os.environ.get('DUMP'):
    print('\n'.join(l for l in out.splitlines() if l.startswith('#')))
nv1 = nv_ram()
check('self-adjust: silent (no buzzer register writes)', len(buzz) == n_buzz, buzz[n_buzz:])
check(f'self-adjust: 15 R0 readings of the shorted probes ~ 0.3 ohm (noiseless ADC: 0.17 ohm steps): {r0[:3]}', len(r0) == 15
      and all(15 <= v <= 45 for v in r0), out[-300:])
check(f'self-adjust: all 4 values adjusted and saved: {nv1}', '#J done 4' in out and f'#N save slot {len(recs0)} ok' in out
      and 30 <= nv1['CapZero'] <= 50 and 15 <= nv1['RZero'] <= 45 and nv1['RiL'] and nv1['RiH'], [l for l in out.splitlines() if l.startswith(('#J done', '#J C0', '#N save'))])
bd.lcd.png(os.path.join(OUTDIR, 'screen_adjust.png'))
recs = nv_records()
check('flash: one record = NV in RAM, nothing else touched, page locked again',
      recs == recs0 + [bytes(uc.mem_read(NV_ADDR, 12))] and not flash['errors'] and not flash['erases']
      and flash['locked'] and bytes(uc.mem_read(0x0800FFD0, 0x30)) == STOCK_CAL, (recs, flash))
out, _ = measure({})
check('a test with unchanged values writes nothing', len(nv_records()) == len(recs0) + 1 and '#N save' not in out, out[-200:])

# --- serial probe diagnostic with 1k on 1-2: probe 1 via Rl -> divider 1k / (680 + 1k)
bd.dut = {(0, 1): 1000}
uart_text(); send(b'#D\n'); run_ms(400)
s = uart_text()
d = [l for l in s.splitlines() if l.startswith('#D P1 Rl')]
v = int(d[0].split()[3]) if d else -1
check('diag: P1 via Rl with 1k to P2 (low) reads ~59 % of full scale', 0.55 < v / 4096 < 0.63
      and '#D end' in s, d)
uart_text(); send(b'#X\n'); run_ms(300)
s = uart_text()
check('drive experiment runs and restores', '#X end' in s and bd.pin('B', 3) == 0, s[-200:])
check('diag: probes shorted again afterwards', bd.pin('B', 3) == 0)
bd.dut = {}

# --- ZENER: Zener screen (boost off); TEST measures with the boost on, then off again; ZENER leaves.
#     The ZENER+TEST chord never starts it.
def zener_press():
    bd.set_in('F', 7, 0); run_ms(300); bd.set_in('F', 7, 1); run_ms(600)

def zener_measure():
    hv0 = sum(e == 'PF6=1' for _, e in bd.events)
    bd.set_in('C', 13, 0); run_ms(200); bd.set_in('C', 13, 1)
    s = ''
    for _ in range(60):
        run_ms(100); s += uart_text()
        if '#R Zener' in s:
            break
    run_ms(300); s += uart_text()
    return s, sum(e == 'PF6=1' for _, e in bd.events) - hv0

uart_text()
bd.zener = 12.0
zener_press()
s = uart_text()
check('ZENER: Zener screen, boost and relay still off', 'zener mode' in s and bd.pin('F', 6) == 0
      and bd.pin('B', 3) == 0 and '#Z ' not in s, s[-200:])
run_ms(1500)
check('Zener screen: no reading without TEST', '#Z ' not in uart_text() and bd.pin('F', 6) == 0)
s, hv = zener_measure()
m = [l for l in s.splitlines() if l.startswith('#R Zener ')]
check('TEST: one measurement, 12 V diode read as 12 V, boost off afterwards', hv == 1 and bd.pin('F', 6) == 0
      and bd.pin('B', 3) == 0 and m and abs(int(m[-1].split()[2]) - 12000) < 360, s[-300:])  # emulated ref ~2 % off
bd.lcd.png(os.path.join(OUTDIR, 'screen_zener.png'))
check('Zener symbol and voltage drawn', bd.lcd.count(0xFFFF, 19, 63) > 150 and bd.lcd.count(0xB47F, 72, 96) > 100)
bd.zener = None
s, hv = zener_measure()
check('empty socket: open (> 33 V)', '#R Zener open' in s and bd.lcd.count(0xFFFF, 72, 96) > 50, s[-200:])
bd.zener = 5.1
s, hv = zener_measure()
check('5.1 V diode', any(l.startswith(('#R Zener 49', '#R Zener 50', '#R Zener 51')) for l in s.splitlines()), s[-300:])
zener_press()
s = uart_text()
check('ZENER leaves Zener mode, boost off', bd.pin('F', 6) == 0 and 'zener off' in s, s[-200:])
bd.zener = None
hv_on = sum(e == 'PF6=1' for _, e in bd.events)

# chord ZENER held + TEST = self-adjust: never Zener mode; ZENER cancels the adjust
bd.set_in('F', 7, 0); run_ms(300)
bd.set_in('C', 13, 0); run_ms(300); bd.set_in('C', 13, 1); run_ms(300)
bd.set_in('F', 7, 1)
out = wait_for('Short all probes', '')
check('ZENER+TEST chord: self-adjust, HV stays off', 'Short all probes' in out and 'zener on' not in out
      and bd.pin('F', 6) == 0, out[-200:])
zener_press(); run_ms(1500)
out = uart_text()
check('ZENER cancels the self-adjust, no Zener mode', 'zener on' not in out and bd.pin('F', 6) == 0, out[-200:])

# --- mode switch: IR remote decoding
ir_tests()
uart_text()

# --- ADC page (serial #A)
uart_text(); send(b'#A\n'); run_ms(1500)
bd.lcd.png(os.path.join(OUTDIR, 'screen_adc.png'))
check('ADC page drawn (#A switches page)', bd.lcd.count(0xB47F, 0, 16) > 30 and bd.lcd.count(0x8C33, 0, 128) > 100)
send(b'#A\n'); run_ms(1500)
uart_text()

# --- implausible battery reading (0 V): never switches off
bd.adc[8] = 0
run_ms(7000)
check('implausible battery reading does not switch off', bd.pin('A', 8) == 1)
bd.adc[8] = 1700
run_ms(1100)
uart_text()

# --- stray bytes (e.g. firmware data sent to a running meter) do not trigger commands
uart_text(); send(b'T?DX\x00\xffT\n'); run_ms(300)
s = uart_text()
check('stray bytes without "#" are ignored', '#R' not in s and '#?' not in s and '#D' not in s, s[-120:])
bd.uart_rx += b'#?\n' * 30; run_ms(200)                  # unpaced burst: overruns
uart_text(); send(b'#?\n'); run_ms(300)              # replies wait for the 1 s ADC/screen update (slow here)
check('reception recovers after an overrun', '#? ' in uart_text())
uart_text(); send(b'#S15\n'); run_ms(300)
_t = uart_text(); check('#S sets the ADC sample time', '#S 15' in _t and (bd.reg.get(0x40020810, 0) & 15) in (14, 15), repr(_t[-300:]))

# --- sweep over sample times with 1M on 1-3
bd.dut = {(0, 2): 1e6}
uart_text(); send(b'#W\n'); run_ms(5000)
s = uart_text()
check('#W sweep measures with codes 12..15', '#W end' in s and s.count('found=10') == 4, s[-300:])
bd.dut = {}

# --- serial status
uart_text(); send(b'#?\n')
run_ms(300)
s = ''.join(l for l in uart_text().splitlines(True) if l.startswith('#? '))
check('serial "?" status reply', s.startswith('#? ') and 'relay=0' in s, s)

# --- long TEST: power off
bd.set_in('C', 13, 0)
run_ms(2500)
s = uart_text()
check('long TEST: PA8 cleared, relay off, logged', bd.pin('A', 8) == 0 and bd.pin('B', 3) == 0
      and 'power off (key)' in s, s)
bd.lcd.png(os.path.join(OUTDIR, 'screen_off.png'))
check('HV boost set only in Zener mode (not by self-adjust, power off)', bd.pin('F', 6) == 0 and sum(e == 'PF6=1' for _, e in bd.events) == hv_on)

# --- tick ISR: clears the update flag and counts
RET = 0x08000100
uc.mem_write(RET, b'\xfe\xe7')
t0 = struct.unpack('<I', uc.mem_read(G_MS, 4))[0]
bd.reg[0x40001010] = 1
uc.reg_write(UC_ARM_REG_SP, 0x20001F00)
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0
uc.reg_write(UC_ARM_REG_LR, RET | 1)
uc.emu_start(syms['TIM6_IRQHandler'] | 1, RET, count=1000)
t1 = struct.unpack('<I', uc.mem_read(G_MS, 4))[0]
check('TIM6 ISR: g_ms + 1 and update flag cleared', t1 == t0 + 1 and bd.reg.get(0x40001010) == 0, (t0, t1))

# --- flash store: 40 saves wrap the 32-slot page once; nv_load returns the last one
def call(fn, count=2000000):
    uc.reg_write(UC_ARM_REG_SP, 0x20001F00)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(syms[fn] | 1, RET, count=count)
    return uc.reg_read(UC_ARM_REG_R0)


nv_base = bytearray(uc.mem_read(NV_ADDR, 12))
oks = []
for i in range(40):
    b = bytearray(nv_base); b[7] = i                    # RefOffset = i
    uc.mem_write(NV_ADDR, bytes(b))
    oks.append(call('nv_save'))
recs = nv_records()
n_wrap = (len(recs0) + 1 + 39) - 32                     # records before + adjust + 39 changed saves, 32 slots
check(f'40 saves (#0 = unchanged: no write): all ok, one erase of the NV page, {n_wrap} records after the wrap, no flash errors',
      all(oks) and flash['erases'] == [NV_PAGE] and len(recs) == n_wrap and not flash['errors']
      and bytes(uc.mem_read(0x0800FFD0, 0x30)) == STOCK_CAL, (oks, flash, len(recs)))
uc.mem_write(NV_ADDR, b'\0' * 12)
loaded = call('nv_load')
check('nv_load: the newest record (RefOffset 39)', loaded == 1 and nv_ram()['RefOffset'] == 39, nv_ram())
uc.mem_write(NV_PAGE + 16 * (n_wrap - 1) + 14, b'\x00')              # corrupt the newest record's checksum
call('nv_load')
check('nv_load: skips a record with a bad checksum', nv_ram()['RefOffset'] == 38, nv_ram())

print(f'{len(fails)} failed' if fails else 'all passed', '- screens in', OUTDIR)
sys.exit(1 if fails else 0)
