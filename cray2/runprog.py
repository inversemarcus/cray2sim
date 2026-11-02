"""Assemble a CAL file and run it on CPU 0 of the emulator.

    python3 -m cray2.runprog examples/hello.cal --mem 4096=1ff,3,ffffffffffffffff --show 8192:3
"""
import argparse
import os
import tempfile

from . import cal2
from .client import Sim


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source')
    ap.add_argument('--mem', action='append', default=[], help='ADDR=hex,hex,... initial Common Memory words')
    ap.add_argument('--show', action='append', default=[], help='ADDR:N words to print after the run')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as d:
        binf = os.path.join(d, 'prog.bin')
        n = cal2.assemble_file(a.source, binf)
        s = Sim()
        s.cmd('load %s 0' % binf)
        for m in a.mem:
            addr, vals = m.split('=')
            s.write(int(addr, 0), [int(v, 16) for v in vals.split(',')])
        s.cmd('start 0 0')
        for line in s.go():
            print(line)
        for sh in a.show:
            addr, cnt = sh.split(':')
            print('%s: %s' % (addr, ' '.join('%016x' % w for w in s.read(int(addr, 0), int(cnt)))))
        print('(%d words of code)' % n)
        s.close()


if __name__ == '__main__':
    main()
