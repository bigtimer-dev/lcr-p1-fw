"""Stream the meter's serial output (115200 8N1) to stdout line by line: python3 tools/serlog.py [secs] [dev]
Without dev it follows the meter across power cycles (the USB serial adapter comes back as ttyUSB0/1/...)."""
import glob, os, sys, time, termios, select
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 600
fixed = sys.argv[2] if len(sys.argv) > 2 else None
end = time.time() + secs

def log(text):
    print(time.strftime('%H:%M:%S ') + text, flush=True)

def open_port():
    for dev in [fixed] if fixed else sorted(glob.glob('/dev/ttyUSB*')):
        try:
            fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError:
            continue
        a = termios.tcgetattr(fd)
        a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, a)
        log(f'[serlog: {dev}]')
        return fd
    return None

fd, buf = None, b''
while time.time() < end:
    if fd is None:
        fd = open_port()
        if fd is None:
            time.sleep(0.5)
            continue
    try:
        r, _, _ = select.select([fd], [], [], 0.2)
        data = os.read(fd, 4096) if r else b''
        if r and not data:
            raise OSError('closed')
    except OSError:
        os.close(fd)
        fd = None
        log('[serlog: port gone]')
        continue
    buf += data
    *lines, buf = buf.split(b'\n')
    for l in lines:
        log(l.decode('latin-1').rstrip('\r').replace('\x7f', 'Ω'))
