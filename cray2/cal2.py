"""cal2: assembler for CRAY-2 Assembly Language (CAL Version 2) syntax.

Instruction forms follow HR-2000 (May 1985) section 3 (see isa/cray2_isa.json).
Source line:   [label:] RESULT OPERAND   [; comment]
Directives:    name = expr        symbol definition
               ALIGN              pad with pass parcels to a word boundary
               CON e1,e2,...      64-bit data words (word aligned)
               BSSZ n             n zero words (word aligned)
Code labels have parcel-address values; use label/4 for the word address of
word-aligned data.  Numbers use Python syntax (0o17 octal, 0x1f hex).
Output: raw little-endian 64-bit words (host loader format) and a listing.
"""
import re, sys, struct

R = {'Ai': r'(?P<i>A[0-7])', 'Aj': r'(?P<j>A[0-7])', 'Ak': r'(?P<k>A[0-7])',
     'Si': r'(?P<i>S[0-7])', 'Sj': r'(?P<j>S[0-7])', 'Sk': r'(?P<k>S[0-7])',
     'Vi': r'(?P<i>V[0-7])', 'Vj': r'(?P<j>V[0-7])', 'Vk': r'(?P<k>V[0-7])',
     'exp': r'(?P<x>[^\s].*?)'}

# (template, opcode, kind).  kind: 'ijk' plain; special handling otherwise.
FORMS = [
    ('EXIT exp', 0o000, 'exitx'), ('EXIT', 0o000, 'none'), ('ERR', 0o000, 'none'), ('PASS', 0o076, 'none'),
    ('DRI', 0o035, 'k0'), ('ERI', 0o035, 'k1'), ('DFI', 0o035, 'k2'), ('EFI', 0o035, 'k3'),
    ('R,Ai Ak', 0o002, 'ijk'), ('J Ak', 0o002, 'jak'), ('J exp', 0o003, 'm2'),
    ('JCS exp', 0o004, 'm2'), ('JSS exp', 0o005, 'm2'), ('SSM', 0o006, 'none'), ('CSM', 0o007, 'none'),
    ('JZ Ak,exp', 0o010, 'm2'), ('JN Ak,exp', 0o011, 'm2'), ('JP Ak,exp', 0o012, 'm2'), ('JM Ak,exp', 0o013, 'm2'),
    ('JZ Sj,exp', 0o014, 'm2'), ('JN Sj,exp', 0o015, 'm2'), ('JP Sj,exp', 0o016, 'm2'), ('JM Sj,exp', 0o017, 'm2'),
    ('Ai Aj+Ak', 0o020, 'ijk'), ('Ai Aj-Ak', 0o021, 'ijk'), ('Ai Aj*Ak', 0o022, 'ijk'),
    ('Ai Sj', 0o024, 'ijk'), ('Ai VL', 0o025, 'ijk'),
    ('VM Vk,Z', 0o030, 'ijk'), ('VM Vk,N', 0o031, 'ijk'), ('VM Vk,P', 0o032, 'ijk'), ('VM Vk,M', 0o033, 'ijk'),
    ('VM Sj', 0o034, 'ijk'), ('VL Ak', 0o036, 'ijk'),
    ('Ai [Ak]', 0o046, 'ijk'), ('[Ak] Aj', 0o047, 'ijk'), ('Ai [exp]', 0o044, 'm1'), ('[exp] Ak', 0o045, 'm1'),
    ('Si [Ak]', 0o056, 'ijk'), ('[Ak] Si', 0o057, 'ijk'), ('Si [exp]', 0o054, 'm1'), ('[exp] Sj', 0o055, 'm1'),
    ('Si (Aj,Ak)', 0o060, 'ijk'), ('(Aj,Ak) Si', 0o061, 'ijk'), ('Si (Ak)', 0o062, 'ijk'), ('(Ak) Si', 0o063, 'ijk'),
    ('Si (Ak,exp)', 0o064, 'm2'), ('(Ak,exp) Si', 0o065, 'm2'),
    ('Vi (Ak,Vj)', 0o072, 'ijk'), ('(Ak,Vj) Vi', 0o073, 'ijk'),
    ('Vi (Aj,Ak)', 0o070, 'ijk'), ('(Aj,Ak) Vi', 0o071, 'ijk'),
    ('Si (exp)', 0o066, 'm2'), ('(exp) Si', 0o067, 'm2'),
    ('Vi [Ak]', 0o074, 'ijk'), ('[Ak] Vi', 0o075, 'ijk'),
    ('Si #Sk&Sj', 0o101, 'ijk'), ('Si Sj&Sk', 0o100, 'ijk'), ('Si Sj\\Sk', 0o102, 'ijk'), ('Si Sj!Sk', 0o103, 'ijk'),
    ('Si Sj+FSk', 0o120, 'ijk'), ('Si Sj-FSk', 0o121, 'ijk'), ('Si Sj*FSk', 0o124, 'ijk'),
    ('Si Sj*ISk', 0o126, 'ijk'), ('Si Sj*QSk', 0o127, 'ijk'),
    ('Si Sj+Sk', 0o104, 'ijk'), ('Si Sj-Sk', 0o105, 'ijk'),
    ('Si PSj', 0o106, 'k0'), ('Si QSj', 0o106, 'k1'), ('Si ZSj', 0o107, 'ijk'),
    ('Si Si,Sj<Ak', 0o112, 'ijk'), ('Si Sj,Si>Ak', 0o113, 'ijk'),
    ('Si Si<exp', 0o110, 'shl'), ('Si Si>exp', 0o111, 'shr'),
    ('Si VM', 0o114, 'ijk'), ('Si RT', 0o115, 'ijk'),
    ('Si FIX,Sk', 0o122, 'ijk'), ('Si FLT,Sk', 0o123, 'ijk'),
    ('Si /HSj', 0o132, 'ijk'), ('Si *QSj', 0o133, 'ijk'),
    ('Si +Ak', 0o131, 'ijk'), ('Si Ak', 0o130, 'ijk'), ('Si Sj', 0o103, 'copy'),
    ('Vi Sj!Vk&VM', 0o146, 'ijk'), ('Vi Vj!Vk&VM', 0o147, 'ijk'),
    ('Vi Sj&Vk', 0o140, 'ijk'), ('Vi Vj&Vk', 0o141, 'ijk'), ('Vi Sj\\Vk', 0o142, 'ijk'), ('Vi Vj\\Vk', 0o143, 'ijk'),
    ('Vi Sj!Vk', 0o144, 'ijk'), ('Vi Vj!Vk', 0o145, 'ijk'),
    ('Vi Vj,Vj<Ak', 0o152, 'ijk'), ('Vi Vj,Vj>Ak', 0o153, 'ijk'), ('Vi Vj<Ak', 0o150, 'ijk'), ('Vi Vj>Ak', 0o151, 'ijk'),
    ('Vi Sj*FVk', 0o154, 'ijk'), ('Vi Vj*FVk', 0o155, 'ijk'), ('Vi Vj*IVk', 0o156, 'ijk'), ('Vi Vj*QVk', 0o157, 'ijk'),
    ('Vi Sj+FVk', 0o170, 'ijk'), ('Vi Vj+FVk', 0o171, 'ijk'), ('Vi Sj-FVk', 0o172, 'ijk'), ('Vi Vj-FVk', 0o173, 'ijk'),
    ('Vi Sj+Vk', 0o160, 'ijk'), ('Vi Vj+Vk', 0o161, 'ijk'), ('Vi Sj-Vk', 0o162, 'ijk'), ('Vi Vj-Vk', 0o163, 'ijk'),
    ('Vi PVj', 0o164, 'k0'), ('Vi QVj', 0o164, 'k1'), ('Vi ZVj', 0o165, 'ijk'),
    ('Vi /HVk', 0o166, 'ijk'), ('Vi *QVk', 0o167, 'ijk'),
    ('Vi FIX,Vk', 0o174, 'ijk'), ('Vi FLT,Vk', 0o175, 'ijk'), ('Vi CI,Sj&Sk', 0o176, 'ijk'),
    ('Vi Vj', 0o145, 'copy'),
    ('Ai exp', None, 'aconst'), ('Si exp', None, 'sconst'),
]


def compile_form(t):
    out = ''
    toks = re.split(r'(Ai|Aj|Ak|Si|Sj|Sk|Vi|Vj|Vk|exp)', t)
    seen = set()
    for tok in toks:
        if tok in R:
            out += ('(?P=%s)' % tok[1]) if tok in seen else R[tok]
            seen.add(tok)
        else:
            out += re.escape(tok).replace(r'\ ', r'\s+')
    return re.compile('^' + out + '$', re.I)


COMPILED = [(compile_form(t), op, kind, t) for t, op, kind in FORMS]


class AsmError(Exception):
    pass


def reg(s):
    return int(s[1])


class Assembler:
    def __init__(self):
        self.sym = {}

    def ev(self, x, required):
        x = x.strip()
        try:
            return int(eval(x, {'__builtins__': {}}, self.sym))
        except NameError:
            if required:
                raise
            return None

    def encode(self, line, final):
        """Return list of 16-bit parcels for one instruction."""
        for rx, op, kind, t in COMPILED:
            mt = rx.match(line)
            if not mt:
                continue
            g = mt.groupdict()
            i = reg(g['i']) if g.get('i') else 0
            j = reg(g['j']) if g.get('j') else 0
            k = reg(g['k']) if g.get('k') else 0
            x = g.get('x')
            val = self.ev(x, final) if x is not None else None
            if kind == 'none':
                return [op << 9]
            if kind == 'exitx':
                v = val if val is not None else 1
                assert 0 <= v < 64, 'exit code'
                return [(op << 9) | v]
            if kind == 'ijk':
                return [(op << 9) | (i << 6) | (j << 3) | k]
            if kind == 'jak':
                return [(op << 9) | (k << 6) | k]
            if kind == 'copy':
                return [(op << 9) | (i << 6) | (j << 3) | j]
            if kind in ('k0', 'k1', 'k2', 'k3'):
                return [(op << 9) | (i << 6) | (j << 3) | int(kind[1])]
            if kind == 'shl':
                v = val if val is not None else 1
                assert 1 <= v <= 64
                jk = (64 - v) & 63
                return [(op << 9) | (i << 6) | jk]
            if kind == 'shr':
                v = val if val is not None else 0
                assert 0 <= v <= 63
                return [(op << 9) | (i << 6) | v]
            if kind == 'm1':
                v = (val or 0)
                assert 0 <= v < 65536, 'm1 range'
                return [(op << 9) | (i << 6) | (j << 3) | k, v]
            if kind == 'm2':
                v = (val or 0) & 0xffffffff
                return [(op << 9) | (i << 6) | (j << 3) | k, v >> 16, v & 0xffff]
            if kind == 'aconst':
                if val is None:
                    return [0o042 << 9 | i << 6, 0, 0]
                v = val
                if 0 <= v < 64:
                    return [0o026 << 9 | i << 6 | v]
                if -64 <= v < 0:
                    return [0o027 << 9 | i << 6 | (v + 64)]
                if 0 <= v < 65536:
                    return [0o040 << 9 | i << 6, v]
                if -65536 <= v < 0:
                    return [0o041 << 9 | i << 6, v & 0xffff]
                v &= 0xffffffff
                return [0o042 << 9 | i << 6, v >> 16, v & 0xffff]
            if kind == 'sconst':
                if val is None:
                    return [0o053 << 9 | i << 6, 0, 0, 0, 0]
                v = val & 0xffffffffffffffff
                if v < 64:
                    return [0o116 << 9 | i << 6 | v]
                if v >= 0xffffffffffffffc0:
                    return [0o117 << 9 | i << 6 | (v & 63)]
                if v < 1 << 32:
                    return [0o050 << 9 | i << 6, v >> 16, v & 0xffff]
                if v >> 32 == 0xffffffff:
                    return [0o051 << 9 | i << 6, (v >> 16) & 0xffff, v & 0xffff]
                if v & 0xffffffff == 0:
                    return [0o052 << 9 | i << 6, v >> 48, (v >> 32) & 0xffff]
                return [0o053 << 9 | i << 6, v >> 48, (v >> 32) & 0xffff, (v >> 16) & 0xffff, v & 0xffff]
            raise AsmError('kind ' + kind)
        raise AsmError('no form matches: ' + line)

    def assemble(self, text):
        lines = []
        for n, raw in enumerate(text.splitlines(), 1):
            s = raw.split(';', 1)[0].strip()
            if not s:
                continue
            m = re.match(r'^([A-Za-z_.$][\w.$]*):\s*(.*)$', s)
            label = None
            if m:
                label, s = m.group(1), m.group(2).strip()
            lines.append((n, raw, label, re.sub(r'\s+', ' ', s)))
        sizes = {}
        for final in (False, True):
            pc = 0
            out = []
            listing = []
            for idx, (n, raw, label, s) in enumerate(lines):
                try:
                    up = s.upper()
                    if up.startswith('ALIGN') or up.startswith('CON ') or up.startswith('BSSZ'):
                        while pc % 4:
                            out.append(0o076 << 9); pc += 1
                    if label:
                        if not final and label in self.sym and self.sym[label] != pc:
                            raise AsmError('duplicate label ' + label)
                        self.sym[label] = pc
                    if not s or up == 'ALIGN':
                        continue
                    m = re.match(r'^([A-Za-z_][\w]*)\s*=\s*(.+)$', s)
                    if m:
                        v = self.ev(m.group(2), final)
                        if v is not None:
                            self.sym[m.group(1)] = v
                        continue
                    if up.startswith('CON '):
                        for e in s[4:].split(','):
                            v = (self.ev(e, final) or 0) & 0xffffffffffffffff
                            out += [v >> 48, (v >> 32) & 0xffff, (v >> 16) & 0xffff, v & 0xffff]
                            pc += 4
                        continue
                    if up.startswith('BSSZ'):
                        cnt = self.ev(s[4:], True)
                        out += [0] * (4 * cnt); pc += 4 * cnt
                        continue
                    ps = self.encode(s, final)
                    if not final:
                        sizes[idx] = len(ps)
                    else:
                        if len(ps) < sizes[idx]:  # keep pass-1 size; pad short forms
                            ps = ps + [0o076 << 9] * (sizes[idx] - len(ps))
                        assert len(ps) == sizes[idx], (s, ps, sizes[idx])
                        listing.append('%08o  %-24s %s' % (pc, ' '.join('%06o' % p for p in ps), raw.strip()))
                    out += ps
                    pc += len(ps)
                except Exception as e:
                    if final or not isinstance(e, NameError):
                        raise AsmError('line %d: %s: %s' % (n, raw.strip(), e))
                    ps = [0, 0, 0, 0, 0]; out += ps; pc += 5; sizes[idx] = 5
        while len(out) % 4:
            out.append(0o076 << 9)
        words = [(out[q] << 48) | (out[q + 1] << 32) | (out[q + 2] << 16) | out[q + 3] for q in range(0, len(out), 4)]
        return words, listing


def assemble_file(src, dst):
    """Assemble CAL source file src into dst (raw words), dst.lst and dst.sym."""
    a = Assembler()
    words, listing = a.assemble(open(src).read())
    with open(dst, 'wb') as f:
        f.write(struct.pack('<%dQ' % len(words), *words))
    with open(str(dst) + '.lst', 'w') as f:
        f.write('\n'.join(listing) + '\n')
    with open(str(dst) + '.sym', 'w') as f:
        for k, v in sorted(a.sym.items()):
            f.write('%s %d\n' % (k, v))
    return len(words)


def main():
    print('%d words' % assemble_file(sys.argv[1], sys.argv[2]))


if __name__ == '__main__':
    main()
