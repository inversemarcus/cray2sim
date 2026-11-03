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
        # load highest addresses first: each image starts with BSSZ padding so
        # its labels are assembled at the address where it is loaded
        for n, (cpu, src) in reversed(list(enumerate(sorted(programs.items())))):
            base = 0x10000 * (n + 1)                 # word address
            path, out = os.path.join(d, 'p%d.cal' % cpu), os.path.join(d, 'p%d.bin' % cpu)
            open(path, 'w').write('        BSSZ %d\n' % base + src)
            cal2.assemble_file(path, out)
            s.cmd('load %s 0' % out)
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
    # four-processor barrier: each processor adds its number+1 to a shared
    # counter under the semaphore lock, waits until all four have arrived,
    # then reads the total (1+2+3+4 = 10).  Run in exact and fast (mt) modes.
    barrier = '''
lock:   JSS lock
        S1 (0x9000)
        S2 A7
        S3 1
        S2 S2+S3
        S1 S1+S2
        (0x9000) S1
        S4 (0x9001)
        S4 S4+S3
        (0x9001) S4
        CSM
wait:   S4 (0x9001)
        S5 4
        S5 S4-S5
        JM S5,wait
        S6 (0x9000)
        A1 0x9010
        A1 A1+A7
        (A1) S6
        EXIT 1
'''
    for args in ((), ('mt=1', 'qwin=50')):
        s_, ex = run({c: barrier for c in range(4)}, args=args)
        r = s_.read(0x9010, 4); s_.close()
        check('4-processor semaphore barrier %s' % (' '.join(args) or 'exact'), r == [10] * 4 and len(ex) == 4, str(r))
    # malformed protocol write: rejected without changing memory
    s = Sim(())
    s.write(0x100, [5, 6])
    r = s.cmd('w 256 7 nope')
    check('malformed write rejected, memory unchanged', r.startswith('ERR') and s.read(0x100, 2) == [5, 6],
          '%s %s' % (r, s.read(0x100, 2)))
    s.close()

    # bare EXIT is a normal exit (code 1), not an error exit
    s, ex = run({0: """
        EXIT
"""})
    s.close()
    check('bare EXIT exits with code 1', ex.get(0, (None,))[0] == 1, str(ex.get(0)))

    # reciprocal square root of an operand with exponent 0-3: overflow result,
    # but no error (HR-2000 p.2-19), so EFI does not halt
    s, ex = run({0: """
        EFI
        S1 0x0001800000000000   ; exponent 1
        S2 *QS1
        (0x8100) S2
        EXIT 5
"""})
    r = s.read(0x8100, 1); s.close()
    check('rsqrt of exponent 0-3: overflow result, no error', ex.get(0, (None,))[0] == 5 and r[0] >> 48 == 0o60004,
          '%s %s' % (ex.get(0), [oct(x >> 48) for x in r]))

    # a vector register being read by one stream cannot start another stream
    # (HR-2000 2.2.3: the issue logic reserves source registers too).  Two
    # 64-element operations on different units both reading V1 must start at
    # least 64 clock periods apart.
    s, ex = run({0: """
        A1 0
        VL A1               ; VL = 64
        S2 RT
        V3 V1&V4
        V5 V1+FV6
        S3 RT
        (0x8200) S2
        (0x8201) S3
        EXIT 1
"""})
    r = s.read(0x8200, 2); s.close()
    check('second reader of a vector register waits', r[1] - r[0] >= 64, str(r))

    # CSM: the following instruction waits until Common Memory stores drain
    # (HR-2000 p.3-9).  A zero-stride 64-element store keeps one bank busy for
    # about 64 bank cycles; RT after CSM must read a time after that.
    s, ex = run({0: """
        A1 0
        VL A1
        A2 0x9100
        A3 0
        S1 RT
        (A2,A3) V1
        CSM
        S2 RT
        (0x8300) S1
        (0x8301) S2
        EXIT 1
"""})
    r = s.read(0x8300, 2); s.close()
    check('CSM waits for outstanding stores', r[1] - r[0] >= 63 * 57, str(r))
    # more malformed writes: bad address token, overflowing value, overlong line
    s = Sim(())
    s.write(0x100, [5, 6])
    bad = ['w 256a 7', 'w 256 10000000000000000', 'w 256 ' + '1 ' * 2100 + 'nope']
    try:
        rs = [s.cmd(c) for c in bad]
        mem = s.read(0x100, 2)
    except Exception as x:          # an emulator that loses protocol sync fails here
        rs, mem = [repr(x)], None
    check('malformed writes rejected (address, overflow, length)',
          all(r.startswith('ERR') for r in rs) and mem == [5, 6], '%s %s' % (rs, mem))
    s.close()

    # VM is reserved until a vector compare's result is complete: a second
    # compare (on another register) waits for it
    s, ex = run({0: """
        A1 0
        VL A1
        S1 RT
        VM V1,Z
        VM V2,Z
        S2 VM
        S3 RT
        (0x8400) S1
        (0x8401) S3
        EXIT 1
"""})
    r = s.read(0x8400, 2); s.close()
    check('second vector compare waits for VM', r[1] - r[0] >= 2 * (22 + 64), str(r))

    # CSM in fast mode: another processor cannot take the semaphore before the
    # releasing processor's stores have drained (same as exact mode)
    for args in ((), ('mt=1', 'qwin=50'), ('mt=1', 'qwin=2000')):
        s, ex = run({0: """
        SSM
        A1 0
        VL A1
        A2 0x9100
        A3 0
        (A2,A3) V1
        CSM
        S1 RT
        (0x8500) S1
        EXIT 1
""", 1: """
        S2 %d
        S3 S2*FS2           ; delay so that CPU0 sets the semaphore first
        S3 S3*FS2
        S3 S3*FS2
        S3 S3*FS2
        S3 S3*FS2
        S3 S3*FS2
        S4 S3
wait:   JCS got
        J wait
got:    S1 RT
        (0x8501) S1
        CSM
        EXIT 1
""" % float_to_cray(1.0)}, args=args)
        r = s.read(0x8500, 2); s.close()
        check('CSM drain orders the semaphore %s' % (' '.join(args) or 'exact'), r[1] >= r[0] - 10 and r[0] > 3000, str(r))
    sys.exit(1 if FAIL else 0)


if __name__ == '__main__':
    main()
