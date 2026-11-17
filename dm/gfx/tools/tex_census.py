#!/usr/bin/env python3
# Census of the Maxwell texture instructions whose LOD clamp (.LC) or residency predicate the translator drops (Ryujinx.Graphics.Shader InstEmitTexture.cs: Tex() decodes Lc and DestPred and passes
# neither on). A game that clamps the LOD of a streamed sparse texture in its shaders samples levels it has mapped but not yet filled once the clamp is gone: Densha de GO!! Hashiro Yamanote Line's
# 512x512 ASTC 6x6 textures map their 64 KB mip tail (levels 1-9) at once, fill levels 3-9 first, and sampled from level 1 read all-zero blocks, ASTC's error colour (GitHub issue #2350).
# Reads the packages shader_split.py writes (records 'BTCDBTCD' + the key at +0x08, control length +0x28, code length +0x2c, header padded to 0x38, then code area + control section), as dmsh_build.py does:
# a graphics stage's instructions follow its 0x30-byte header and 0x50-byte SPH (+0x80), a compute shader's start at +0x100 behind the 0x12345679 header, else at +0. Every fourth 64-bit word from the
# start is a scheduling control word; a program ends at its branch to itself, or where its constant data c1 starts (control +0x700): read as instructions, Mario Golf: Super Rush's c1 floats
# made 1,444 false residency predicates. NVN2 (Ampere) shaders are counted and skipped: their translator refuses a residency predicate and any LOD mode it does not know.
# Opcode patterns and field bits are the translator's own (Decoders/InstTable.cs, InstDecoders.cs). Not part of the translation chain: it decides no output.
# Usage: tex_census.py <package dir or package .bin>... [--keys N]   (N: how many shader keys to list per finding, default 10)
import argparse, collections, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmsh_ampere

SELF_BRANCH = 0xE2400FFFFF87000F   # BRA to itself, the end of a Maxwell program


def pattern(bits):   # an InstTable.cs pattern, bit 63 first → (mask, value)
    mask = val = 0
    for i, c in enumerate(bits):
        b = 63 - i
        if c in '01': mask |= 1 << b; val |= (c == '1') << b
    return mask, val


def bit(n): return lambda w: (w >> n) & 1


def pred(w): return (w >> 51) & 7   # the destination (residency) predicate, 7 = PT (none), the same field in every form below


# name, pattern, LOD clamp bit (None: the form has none), whether it has a residency predicate (InstProps.TPd)
OPS = [(n, pattern(p), lc, tpd) for n, p, lc, tpd in (
    ('TEX', '11000' + 'x' * 59, bit(58), True),
    ('TEX.B', '1101111010' + 'x' * 54, bit(40), True),
    ('TLD', '11011100' + 'x' * 56, None, True),
    ('TLD.B', '11011101' + 'x' * 56, None, True),
    ('TLD4', '110010' + 'x' * 58, bit(58), True),
    ('TLD4.B', '1101111011' + 'x' * 13 + '0' + 'x' * 40, bit(40), True),
    ('TXD', '110111100' + 'x' * 55, bit(50), True),
    ('TXD.B', '1101111001' + 'x' * 54, bit(50), True),
    ('TEXS', '1101100' + 'x' * 57, None, False),
    ('TLDS', '1101101' + 'x' * 57, None, False),
    ('TLD4S', '1101111100' + 'x' * 54, None, False),
)]
# the longer patterns first, so TEX.B / TLD4.B / TXD.B / TLD4S are not taken for a shorter one that shares their leading bits
OPS.sort(key=lambda o: -bin(o[1][0]).count('1'))


def records(path):   # → (key, code area, control section)
    data = open(path, 'rb').read()
    offs = [m.start() for m in re.finditer(b'BTCDBTCD', data)] + [len(data)]
    for o, e in zip(offs, offs[1:]):
        rec = data[o:e]
        if len(rec) < 0x38: continue
        cl, kl = struct.unpack_from('<2I', rec, 0x28)
        yield rec[8:16].hex(), rec[0x38:0x38 + kl], rec[0x38 + kl:0x38 + kl + cl]


def program(code, stage):   # the instruction area's start
    if stage == 'cs': return 0x100 if code[:4] == bytes.fromhex('79563412') else 0
    return 0x80


def end_of(code, ctl):   # where the instructions end: the constant data c1 the code area carries behind them starts at control +0x700 (an offset into the code area, as dmsh_build.py reads it)
    c1 = struct.unpack_from('<I', ctl, 0x700)[0] if len(ctl) >= 0x704 else 0
    return c1 if 0 < c1 <= len(code) else len(code)


def scan(code, start, end):   # → [(op name, LOD clamp, residency predicate, byte offset)]
    out = []
    for i in range(start, end - 7, 8):
        if ((i - start) // 8) % 4 == 0: continue   # the scheduling control word of each 32-byte group
        w = struct.unpack_from('<Q', code, i)[0]
        if w == SELF_BRANCH: break
        for name, (m, v), lc, tpd in OPS:
            if w & m == v:
                out.append((name, bool(lc and lc(w)), tpd and pred(w) != 7, i - start))
                break
    return out


def packages(args):
    for a in args:
        if os.path.isdir(a):
            for f in sorted(os.listdir(a)):
                if f.endswith('.bin') and re.match(r'[a-z]{2}_', f): yield os.path.join(a, f)
        else: yield a


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('paths', nargs='+'); ap.add_argument('--keys', type=int, default=10)
    a = ap.parse_args()
    shaders = nvn2 = tex_shaders = 0; ops = collections.Counter(); lc = collections.Counter(); rp = collections.Counter()
    lc_keys, rp_keys, stages = [], [], collections.Counter()
    for p in packages(a.paths):
        stage = os.path.basename(p)[:2]
        for key, code, ctl in records(p):
            if dmsh_ampere.is_nvn2(ctl): nvn2 += 1; continue
            shaders += 1; stages[stage] += 1
            found = scan(code, program(code, stage), end_of(code, ctl))
            if found: tex_shaders += 1
            for name, c, r, off in found:
                ops[name] += 1; lc[name] += c; rp[name] += r
            if any(c for _, c, _, _ in found): lc_keys.append((key, stage, sum(c for _, c, _, _ in found)))
            if any(r for _, _, r, _ in found): rp_keys.append((key, stage, sum(r for _, _, r, _ in found)))
    print(f'Maxwell shaders: {shaders} ({", ".join(f"{s} {n}" for s, n in sorted(stages.items()))}); NVN2 skipped: {nvn2}; with a texture instruction: {tex_shaders}')
    print(f'{"op":8} {"count":>9} {"with .LC":>9} {"residency":>9}')
    for name, *_ in sorted(OPS, key=lambda o: o[0]):
        if ops[name]: print(f'{name:8} {ops[name]:9} {lc[name]:9} {rp[name]:9}')
    print(f'shaders with a LOD clamp: {len(lc_keys)}; with a residency predicate: {len(rp_keys)}')
    for what, ks in (('LOD clamp', lc_keys), ('residency predicate', rp_keys)):
        for key, stage, n in ks[:a.keys]: print(f'  {what}: {key} ({stage}, {n} instructions)')


if __name__ == '__main__':
    main()
