"""Run the PC flasher (lcr-p1-linux app/lcrp1/flash.py) against the emulated meter of test/bl_test.py: its
port I/O, select() and clock are patched onto emulated time, so the real host code talks to the bootloaders.

    python -u test/host_test.py              (after `make boot`; CASES=framed,baud,... picks cases)

Cases: framed (ZENER entry), baud (460800), fnirsi (FNIRSI loader: legacy fallback), stock (FNIRSI app image
over the framed protocol), backup (--backup), hashU (running lcrp1-fw -> #U -> update).
"""
import os as _os, sys, types
FW = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
sys.argv = ['x', f'{FW}/build/lcrp1-bl.bin', f'{FW}/build/lcrp1-fw.bin']
src = open(f'{FW}/test/bl_test.py').read().split("INFO_FMT =")[0]   # the emulator part only
g = {'__name__': 'bl', '__file__': f'{FW}/test/bl_test.py'}
exec(compile(src, 'bl_test', 'exec'), g)
sys.path.insert(0, _os.path.join(FW, '..', 'lcr-p1-linux', 'app'))
import lcrp1.flash as F

class Host:
    def __init__(self, m):
        self.m, self.baud = m, 115200
        h = self
        F.os = types.SimpleNamespace(write=lambda fd, d: (h.m.send(d, baud=h.baud), len(d))[1],
                                     read=lambda fd, n: h.m.take_tx(), close=lambda fd: None)
        F.select = types.SimpleNamespace(select=h.select)
        F.time = types.SimpleNamespace(time=lambda: h.m.cyc / g['CPU'], sleep=lambda s: h.m.run(ms=s * 1000))
        F.termios = types.SimpleNamespace(tcflush=lambda *a: None, tcdrain=h.drain, TCIFLUSH=0)
        F.open_port = lambda dev: 3
        F.set_baud = lambda fd, b: (h.drain(fd), setattr(h, 'baud', b))
    def drain(self, fd):
        if self.m.rx_queue:
            self.m.run(ms=(self.m.rx_queue[-1][0] - self.m.cyc) / g['CPU'] * 1000 + 0.01)
    def select(self, r, w, x, t):
        if not self.m.tx:
            self.m.run(ms=t * 1000, until=lambda mm: len(mm.tx) > 0, chunk=200)
        return (r if self.m.tx else []), [], []

fails = []


def result(name, ok, detail=''):
    print('PASS' if ok else 'FAIL', name, detail, flush=True)
    if not ok:
        fails.append(name)

BL, APP, STOCK_BL, STOCK_APP, Meter = g['BL'], g['APP'], g['STOCK_BL'], g['STOCK_APP'], g['Meter']
img = APP
import tempfile
want = (_os.environ.get('CASES') or 'framed,baud,fnirsi,stock,backup,hashU').split(',')

if 'framed' in want:
    m = Meter(BL, STOCK_APP, zener=True); m.run(ms=300)
    Host(m)
    t0 = m.cyc
    ok, msg = F.flash(img, 'emu', listen=0.5)
    m.run(ms=50, until=lambda mm: mm.in_app())
    fl = bytes(m.uc.mem_read(0x08001800, len(img)))
    result('flash.py framed (new loader, ZENER entry)', ok and fl == img and m.in_app(),
           f'{msg} | {(m.cyc - t0) / g["CPU"]:.2f} s')

if 'baud' in want:
    m = Meter(BL, STOCK_APP, zener=True); m.run(ms=300)
    Host(m)
    t0 = m.cyc
    ok, msg = F.flash(img, 'emu', listen=0.5, baud=460800)
    m.run(ms=50, until=lambda mm: mm.in_app())
    fl = bytes(m.uc.mem_read(0x08001800, len(img)))
    result('flash.py framed at 460800', ok and fl == img and m.in_app(), f'{msg} | {(m.cyc - t0) / g["CPU"]:.2f} s')

if 'fnirsi' in want and STOCK_BL:
    m = Meter(STOCK_BL, STOCK_APP, zener=True, allow_bl=True); m.run(ms=300)
    Host(m)
    ok, msg = F.flash(img, 'emu', listen=0.5)
    m.run(ms=100, until=lambda mm: mm.in_app())
    fl = bytes(m.uc.mem_read(0x08001800, len(img)))
    result('flash.py on FNIRSI loader: falls back to the legacy stream', ok and fl == img, msg)

if 'stock' in want and STOCK_APP:
    m = Meter(BL, APP, zener=True); m.run(ms=300)
    Host(m)
    ok, msg = F.flash(STOCK_APP, 'emu', listen=0.5)
    m.run(ms=50, until=lambda mm: mm.in_app())
    fl = bytes(m.uc.mem_read(0x08001800, len(STOCK_APP)))
    result('flash.py framed: stock FNIRSI app image', ok and fl == STOCK_APP and m.in_app(), msg)

if 'backup' in want:
    m = Meter(BL, APP, zener=True); m.run(ms=300)
    Host(m)
    out = _os.path.join(tempfile.mkdtemp(), 'backup.bin')
    ok, msg = F.backup('emu', out)
    data = open(out, 'rb').read() if ok else b''
    result('flash.py --backup: 64 KB equal to emulated flash', ok and data == bytes(m.uc.mem_read(0x08000000, 0x10000)), msg)

if 'hashU' in want:
    import subprocess
    gms = next(int(l.split()[0], 16) for l in subprocess.check_output(['arm-none-eabi-nm', f'{FW}/build/lcrp1-fw.elf'],
               text=True).splitlines() if l.endswith(' g_ms'))
    m = Meter(BL, APP, g_ms=gms)
    m.run(ms=2500)
    print('     app says:', m.tx[:120], flush=True)
    Host(m)
    t0 = m.cyc
    ok, msg = F.flash(STOCK_APP, 'emu', listen=1.2)
    m.run(ms=50, until=lambda mm: mm.in_app())
    fl = bytes(m.uc.mem_read(0x08001800, len(STOCK_APP)))
    result('flash.py: running lcrp1-fw -> #U -> framed update', ok and fl == STOCK_APP and m.in_app(),
           f'{msg} | {(m.cyc - t0) / g["CPU"]:.2f} s')

print(f'\n{len(fails)} FAILED: ' + ', '.join(fails) if fails else '\nALL PASS')
sys.exit(1 if fails else 0)
