#!/usr/bin/env python3
"""Host checks for the kernel LSE emulation (xnu/patches-12377/1020-sweet-lse-emulation.patch).

  tools/lseemu/check.py [--fuzz N] [--bench]

Needs an Apple-silicon Mac with LSE and RCpc (any M1 or later) and Xcode (xcrun clang, llvm-objdump).

1. sweet_lseemu.h and sweet_lse_asm.h are taken from the patch. forms_gen.S gets one function per
   LSE/LDAPR form (every op, size and ordering, plus XZR operands); lseemu_test.c + lse_host.S (the
   kernel's own LL/SC loops) are built with ASan/UBSan.
2. unit: each form runs natively and through the emulator on random inputs, results must be equal;
   misaligned accesses must be refused without side effects; emulated and native atomics are mixed
   on the same words from 8 threads (LDADDAL X, LDADDALB with a native neighbour byte, CASPAL X,
   a SWPA spinlock released by a plain STLR).
3. decode: every form, its single-bit flips and N random words from the LSE encoding spaces are
   disassembled by llvm-objdump; the decoder must accept exactly the instructions it handles,
   with the same operands.
4. --bench: software cost per emulated atomic, from a build without sanitizers.
--emit DIR writes the extracted headers and forms_gen.{S,c} for build.sh (the on-device test).
Exit status 0 = all checks passed.
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PATCH = os.environ.get('LSEEMU_PATCH') or os.path.join(ROOT, 'xnu', 'patches-12377', '1020-sweet-lse-emulation.patch')
MATTR = '--mattr=+v8.5a,+lse,+rcpc'
FAILS = []


def fail(msg):
    FAILS.append(msg)
    if len(FAILS) <= 40:
        print('FAIL ' + msg)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit(f'{" ".join(cmd)} failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}')
    return r.stdout


def extract_headers(tmp):
    d = os.path.join(tmp, 'x')
    os.makedirs(d)
    run(['git', 'apply', '--include=osfmk/arm64/sweet_lseemu.h', '--include=osfmk/arm64/sweet_lse_asm.h', PATCH],
        cwd=d)
    return os.path.join(d, 'osfmk', 'arm64')


ALUS = ['add', 'clr', 'eor', 'set', 'smax', 'smin', 'umax', 'umin']
SIZES = [('b', 'w'), ('h', 'w'), ('', 'w'), ('', 'x')]


def forms():
    """assembler text of every form, with Rs = x2 (pair x2,x3), Rt = x4 (pair x4,x5), Rn = x0"""
    out = []
    for alu in ALUS:
        for o in ('', 'a', 'l', 'al'):
            for suf, r in SIZES:
                out.append(f'ld{alu}{o}{suf} {r}2, {r}4, [x0]')
        for o in ('', 'l'):
            for suf, r in SIZES:
                out.append(f'st{alu}{o}{suf} {r}2, [x0]')
    for o in ('', 'a', 'l', 'al'):
        for suf, r in SIZES:
            out.append(f'swp{o}{suf} {r}2, {r}4, [x0]')
            out.append(f'cas{o}{suf} {r}2, {r}4, [x0]')
        for r in 'wx':
            out.append(f'casp{o} {r}2, {r}3, {r}4, {r}5, [x0]')
    for suf, r in SIZES:
        out.append(f'ldapr{suf} {r}4, [x0]')
    # zero-register operands
    out += ['swp xzr, x4, [x0]', 'swpal w2, wzr, [x0]', 'cas xzr, x4, [x0]', 'casal w2, wzr, [x0]',
            'ldadda x2, xzr, [x0]', 'ldaddal xzr, x4, [x0]', 'ldsetb wzr, w4, [x0]', 'ldapr xzr, [x0]']
    return out


def gen_forms(tmp, texts):
    s = ['\t.text']
    for i, t in enumerate(texts):
        s += ['\t.align 2', f'\t.globl _form_{i}', f'_form_{i}:',
              '\tldp x2, x3, [x1]', '\tldp x4, x5, [x1, #16]', f'\t{t}',
              '\tstp x2, x3, [x1]', '\tstp x4, x5, [x1, #16]', '\tret']
    open(os.path.join(tmp, 'forms_gen.S'), 'w').write('\n'.join(s) + '\n')
    c = ['#include <stdint.h>', 'typedef void (*form_fn)(void *, uint64_t *);',
         'struct form { form_fn fn; const char *text; };']
    c += [f'extern void form_{i}(void *, uint64_t *);' for i in range(len(texts))]
    c.append('const struct form forms[] = {')
    c += [f'\t{{ form_{i}, "{t}" }},' for i, t in enumerate(texts)]
    c += ['};', f'const unsigned nforms = {len(texts)};']
    open(os.path.join(tmp, 'forms_gen.c'), 'w').write('\n'.join(c) + '\n')


# ---- llvm text -> the decoder's canonical form ------------------------------------------------------------------
def reg(t):
    t = t.strip().strip('[]').strip()
    if t in ('wzr', 'xzr', 'sp', 'wsp'):
        return 31
    return int(t[1:])


MN = re.compile(r'^(ld|st)(add|clr|eor|set|smax|smin|umax|umin)(al|a|l)?(b|h)?$')


def llvm_canonical(text):
    parts = text.replace(',', ' ').split()
    if not parts:
        return 'NONE'
    mn, ops = parts[0], parts[1:]

    def size_of(suf, r):
        return {'b': 0, 'h': 1}.get(suf, 2 if r.startswith('w') else 3)

    m = MN.match(mn)
    if m:
        st = m.group(1) == 'st'
        o = m.group(3) or ''
        rs = reg(ops[0])
        rt = 31 if st else reg(ops[1])
        rn = reg(ops[-1])
        return (f'LDOP alu={ALUS.index(m.group(2))} size={size_of(m.group(4), ops[0])} s={rs} t={rt} n={rn} '
                f'a={int("a" in o)} r={int("l" in o)}')
    m = re.match(r'^(swp|cas)(al|a|l)?(b|h)?$', mn)
    if m:
        o = m.group(2) or ''
        op = 'SWP' if m.group(1) == 'swp' else 'CAS'
        return (f'{op} alu=0 size={size_of(m.group(3), ops[0])} s={reg(ops[0])} t={reg(ops[1])} n={reg(ops[2])} '
                f'a={int("a" in o)} r={int("l" in o)}')
    m = re.match(r'^casp(al|a|l)?$', mn)
    if m:
        o = m.group(1) or ''
        return (f'CASP alu=0 size={2 if ops[0].startswith("w") else 3} s={reg(ops[0])} t={reg(ops[2])} '
                f'n={reg(ops[4])} a={int("a" in o)} r={int("l" in o)}')
    m = re.match(r'^ldapr(b|h)?$', mn)
    if m:
        return f'LDAPR alu=0 size={size_of(m.group(1), ops[0])} s=31 t={reg(ops[0])} n={reg(ops[1])} a=1 r=0'
    return 'NONE'


LSE_SPACES = [(0x3F200C00, 0x38200000), (0x3FA07C00, 0x08A07C00), (0xBFA07C00, 0x08207C00)]
HANDLED_MN = re.compile(r'^((ld|st)(add|clr|eor|set|smax|smin|umax|umin)|swp|cas|casp|ldapr)(al|a|l)?(b|h)?$')


def objdump(o):
    """{word: text} of an object file"""
    out = {}
    for line in run(['xcrun', 'llvm-objdump', '-d', MATTR, o]).splitlines():
        m = re.match(r'\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(.*)$', line)
        if m:
            out[int(m.group(1), 16)] = m.group(2).split('//')[0].split(';')[0].strip()
    return out


def disassemble(tmp, words):
    s = os.path.join(tmp, 'words.s')
    o = os.path.join(tmp, 'words.o')
    open(s, 'w').write('\t.text\n' + ''.join(f'\t.long 0x{w:08x}\n' for w in words))
    run(['xcrun', 'clang', '-arch', 'arm64', '-c', s, '-o', o])
    return objdump(o)


def build(tmp, hdr, exe, flags):
    run(['xcrun', 'clang', '-arch', 'arm64', '-march=armv8.5-a', '-std=c11', '-g', '-Wall', '-Wextra',
         '-Werror', '-Wshadow', '-I', hdr, *flags,
         os.path.join(HERE, 'lseemu_test.c'), os.path.join(HERE, 'lse_host.S'),
         os.path.join(tmp, 'forms_gen.S'), os.path.join(tmp, 'forms_gen.c'), '-o', exe])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fuzz', type=int, default=30000)
    ap.add_argument('--bench', action='store_true')
    ap.add_argument('--emit', metavar='DIR', help='only write the headers and forms_gen.{S,c} to DIR (build.sh)')
    args = ap.parse_args()
    random.seed(1020)

    if args.emit:
        os.makedirs(args.emit, exist_ok=True)
        with tempfile.TemporaryDirectory() as tmp:
            hdr = extract_headers(tmp)
            for h in ('sweet_lseemu.h', 'sweet_lse_asm.h'):
                open(os.path.join(args.emit, h), 'w').write(open(os.path.join(hdr, h)).read())
        gen_forms(args.emit, forms())
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        hdr = extract_headers(tmp)
        texts = forms()
        gen_forms(tmp, texts)
        exe = os.path.join(tmp, 'lseemu_test')
        build(tmp, hdr, exe, ['-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])

        # 1+2. semantics, misalignment, concurrency
        r = subprocess.run([exe, 'unit'], capture_output=True, text=True)
        print(r.stdout, end='')
        if r.returncode:
            fail(f'unit tests: {r.stderr[-1500:]}')

        # 3. decoder vs llvm-objdump
        o = os.path.join(tmp, 'f.o')
        run(['xcrun', 'clang', '-arch', 'arm64', '-march=armv8.5-a', '-c', os.path.join(tmp, 'forms_gen.S'),
             '-o', o])
        form_words = [w for w, t in objdump(o).items() if HANDLED_MN.match(t.split()[0])]
        if len(form_words) != len(texts):
            fail(f'found {len(form_words)} form words in forms_gen.o, expected {len(texts)}')
        cand = set(form_words)
        for w in form_words:
            for b in range(32):
                cand.add(w ^ (1 << b))
        for _ in range(args.fuzz):
            mask, val = random.choice(LSE_SPACES)
            cand.add((random.getrandbits(32) & ~mask) | val)
        cand = sorted(cand)
        dis = disassemble(tmp, cand)
        out = subprocess.run([exe, 'decode'], input=''.join(f'{w:08x}\n' for w in cand),
                             capture_output=True, text=True).stdout
        mine = {int(line[:8], 16): line[9:] for line in out.splitlines()}
        nacc = known = 0
        for w in cand:
            text = dis.get(w, '<unknown>')
            want = llvm_canonical(text)
            got = mine.get(w, '?')
            if got != 'NONE':
                nacc += 1
            if want == got:
                continue
            # CASP with an odd register pair: unallocated for us; llvm may still print it
            if got == 'NONE' and re.match(r'^casp', text):
                ops = text.replace(',', ' ').split()[1:]
                if reg(ops[0]) & 1 or reg(ops[2]) & 1:
                    known += 1
                    continue
            fail(f'{w:08x} "{text}": llvm {want} / decoder {got}')
        print(f'decode: {len(form_words)} forms + bit flips + {args.fuzz} random = {len(cand)} words vs '
              f'llvm-objdump, {nacc} LSE/LDAPR instructions, {known} odd-pair CASP (unallocated, refused)')

        if args.bench:
            bexe = os.path.join(tmp, 'lseemu_bench')
            build(tmp, hdr, bexe, ['-O2'])
            print(run([bexe, 'bench']), end='')

    print('PASS' if not FAILS else f'FAIL: {len(FAILS)} failures')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
