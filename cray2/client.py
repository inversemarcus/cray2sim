"""Python client for cray2sim's stdin/stdout protocol (see docs/PROTOCOL.md)."""
import math
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
CLOCK_NS = 4.1


def float_to_cray(x):
    """Cray-2 floating representation of a Python float (exact for 48-bit values)."""
    if x == 0:
        return 0
    m, e = math.frexp(abs(x))
    return (int(x < 0) << 63) | ((e + 0o40000) << 48) | int(m * (1 << 48))


def cray_to_float(w):
    e = (w >> 48) & 0x7fff
    c = w & 0xffffffffffff
    if not c or e < 0o20000:
        return 0.0
    try:
        v = math.ldexp(c, e - 0o40000 - 48)
    except OverflowError:
        v = math.inf
    return -v if w >> 63 else v


class Sim:
    """Pipe to one cray2sim process."""

    def __init__(self, args=(), exe=None):
        exe = pathlib.Path(exe) if exe else ROOT / 'sim' / 'cray2sim'
        if not exe.exists():
            raise SystemExit('%s not built: run `make` in the cray2sim repository' % exe)
        self.p = subprocess.Popen([str(exe)] + list(args), stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
        assert self.p.stdout.readline().startswith('READY')

    def cmd(self, s):
        self.p.stdin.write(s + '\n')
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def go(self):
        self.p.stdin.write('go\n')
        self.p.stdin.flush()
        lines = []
        while True:
            line = self.p.stdout.readline().strip()
            if not line:
                raise RuntimeError('simulator ended')
            if line == 'END':
                return lines
            lines.append(line)

    def read(self, addr, n):
        return [int(x, 16) for x in self.cmd('r %d %d' % (addr, n)).split()[1:]]

    def write(self, addr, vals):
        for i in range(0, len(vals), 40):
            self.cmd('w %d %s' % (addr + i, ' '.join('%x' % v for v in vals[i:i + 40])))

    def stats(self):
        self.p.stdin.write('stats\n')
        self.p.stdin.flush()
        return [self.p.stdout.readline().strip() for _ in range(4)]

    def close(self):
        try:
            self.p.stdin.write('quit\n')
            self.p.stdin.flush()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def parse_exit(line):
    f = line.split()
    regs = dict(x.split('=', 1) for x in f[4:])
    return int(f[1]), int(f[2]), int(f[3]), [int(v) for v in regs['A'].split(',')]
