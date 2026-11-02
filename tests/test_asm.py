"""Assemble a small CAL program, run it on the emulator, check the results.
Exercises scalar/vector loads and stores, vector logical/popcount/integer ops,
a counted loop and floating-point arithmetic."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from cray2 import cal2  # noqa: E402
from cray2.client import Sim, cray_to_float  # noqa: E402

PROGRAM = (ROOT / 'examples' / 'hello.cal').read_text()


def main():
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, 't.cal'), os.path.join(d, 't.bin')
        open(src, 'w').write(PROGRAM)
        cal2.assemble_file(src, out)
        s = Sim()
        assert s.cmd('load %s 0' % out).startswith('OK')
        s.write(4096, [0x1ff, 3, 0xffffffffffffffff])
        assert s.cmd('start 0 0') == 'OK'
        lines = s.go()
        ex = [l for l in lines if l.startswith('EXIT')][0].split()
        assert ex[1] == '0' and ex[2] == '7', lines
        r = s.read(8192, 3)
        s.close()
    ok = (cray_to_float(r[0]) == 3.75 and r[1] == 3 + 2 and r[2] == 0xffffffffffffffff + 8 - (1 << 64)
          and 'S=0,ff,a,' in ' '.join(lines))
    print('asm/emulator test', 'PASS' if ok else 'FAIL', [hex(x) for x in r])
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
