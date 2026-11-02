"""Instruction-semantics and multiprocessor-ordering tests with hand-derived
expected results (HR-2000 May 1985).  Each case assembles a small program,
runs it, and checks memory or exit state."""
import os
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from cray2 import cal2  # noqa: E402
from cray2.client import Sim, float_to_cray, parse_exit  # noqa: E402

FAIL = []


def run(programs, mem=(), args=()):
    """programs: {cpu: CAL source} each assembled at its own address."""
    s = Sim(args)
    with tempfile.TemporaryDirectory() as d:
        starts = {}
        for n, (cpu, src) in enumerate(sorted(programs.items())):
            base = 0x10000 * (n + 1)                 # word address
            path, out = os.path.join(d, 'p%d.cal' % cpu), os.path.join(d, 'p%d.bin' % cpu)
            open(path, 'w').write(src)
            cal2.assemble_file(path, out)
            s.cmd('load %s %d' % (out, base))
            starts[cpu] = base * 4                    # parcel address
        for addr, vals in mem:
            s.write(addr, vals)
        for cpu, p in starts.items():
            assert s.cmd('start %d %d' % (cpu, p)) == 'OK'
        exits = {}
        while len(exits) < len(programs):
            lines = s.go()
            for l in lines:
                if l.startswith('EXIT'):
                    c, code, t, A = parse_exit(l)
                    exits[c] = (code, t, l)
            if not any(l.startswith('EXIT') for l in lines):
                break
        return s, exits


def check(name, cond, detail=''):
    print('%-44s %s %s' % (name, 'PASS' if cond else 'FAIL', detail))
    if not cond:
        FAIL.append(name)


def main():
    # scatter where the data and index registers are the same register:
    # both are sources, so V1 = [1, 2] stores 1 at base+1 and 2 at base+2
    s, ex = run({0: """
        A1 2
        VL A1
        A2 0x3000
        A3 1
        V1 (A2,A3)          ; V1 = [1, 2]
        A4 0x4000
        (A4,V1) V1
        EXIT 1
"""}, mem=[(0x3000, [1, 2])])
    r = s.read(0x4000, 3); s.close()
    check('scatter with data register = index register', r == [0, 1, 2], str(r))

    # gather whose destination is also the index register: the index reads as
    # zeros (destination-equals-source rule), so every element loads base+0
    s, ex = run({0: """
        A1 3
        VL A1
        A2 0x3000
        A3 1
        V1 (A2,A3)          ; V1 = [1, 2, 0]
        A4 0x5000
        V1 (A4,V1)
        A5 0x6000
        (A5,A3) V1
        EXIT 1
"""}, mem=[(0x3000, [1, 2, 0]), (0x5000, [77, 88, 99])])
    r = s.read(0x6000, 3); s.close()
    check('gather with destination = index register', r == [77, 77, 77], str(r))

    # cross-processor chronology: CPU0's store depends on a 5-deep chain of
    # 19-clock floating multiplies, so it cannot issue before about clock 95.
    # CPU1 loads the word at about clock 2 and must see the old value.
    s, ex = run({0: """
        S1 %d
        S2 S1*FS1
        S3 S2*FS1
        S4 S3*FS1
        S5 S4*FS1
        S6 S5*FS1
        (0x7000) S6
        EXIT 1
""" % float_to_cray(1.0), 1: """
        S1 (0x7000)
        (0x7001) S1
        EXIT 2
"""})
    r = s.read(0x7000, 2); s.close()
    check('no cross-processor read of a future store', r[1] == 0 and r[0] == float_to_cray(1.0), str(r))

    # EFI: halt on floating-point range error.  Reciprocal approximation of 0
    # is a range error, so the processor stops (exit code -3) instead of
    # reaching EXIT 5.
    s, ex = run({0: """
        EFI
        S1 0
        S2 /HS1
        EXIT 5
"""})
    s.close()
    check('EFI halts on a floating-point range error', ex.get(0, (None,))[0] == -3, str(ex.get(0)))

    # without EFI the same program runs to its exit
    s, ex = run({0: """
        S1 0
        S2 /HS1
        EXIT 5
"""})
    s.close()
    check('range error without EFI continues', ex.get(0, (None,))[0] == 5, str(ex.get(0)))

    # floating-point values from the format definition (HR-2000 2.5.2)
    s, ex = run({0: """
        S1 %d
        S2 %d
        S3 S1+FS2
        S4 S1*FS2
        S5 S1-FS2
        (0x8000) S3
        (0x8001) S4
        (0x8002) S5
        EXIT 1
""" % (float_to_cray(3.0), float_to_cray(0.5))})
    r = s.read(0x8000, 3); s.close()
    want = [float_to_cray(3.5), float_to_cray(1.5), float_to_cray(2.5)]
    check('float add/multiply/subtract exact values', r == want, str([hex(x) for x in r]))
    sys.exit(1 if FAIL else 0)


if __name__ == '__main__':
    main()
