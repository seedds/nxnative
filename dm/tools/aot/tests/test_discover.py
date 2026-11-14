#!/usr/bin/env python3
"""The discovery test (CTest `discover`): `nxaot discover` over every corpus NRO (dm/tools/aot/corpus, corpus.tsv), compared with the full symbol
table of the NRO's ELF (corpus/nro/<nro>.syms.tsv: every STT_FUNC symbol, local ones included, and the assembler's mapping symbols), twice:
with the NRO's own DT_SYMTAB as a source (`symbols`; its exported functions) and with no symbol as a source (`--symbols-as-oracle`, `oracle`).

  test_discover.py --nxaot <path to nxaot> --work <scratch folder>

Checks, per NRO and mode:
- every STT_FUNC symbol of .symtab is the start of a discovered function (0 missed);
- no discovered entry lies inside another symbol's range (a function split) and every discovered entry has a symbol (an extra would have to be
  explained here: there is none in this corpus);
- sizes: the discovered size is the walk's extent, the symbol's size is the assembler's; they are compared as equal, or shorter by trailing
  words that are padding or traps (nop, zero, brk, udf: after a call that does not return, or alignment), and must never be longer;
- the coverage reports 0 uncovered words outside data blocks and 0 undefined words inside functions;
- every `$d` mapping symbol in the text lies in a data block the loader found (the NRO header, MOD0);
- the switch program has no br left for the dispatcher: every one is a jump table; fnptr's cleanup landing pads are found; excl's exclusive
  windows include an ABA-sensitive one; except-gcc's CFA interpreter (rt/unwind.c), a switch in a loop whose table base is set before the
  loop and whose cases call memcpy, is a halfword table of 47 entries.
It prints one row per NRO and mode: functions by primary provenance, jump tables by entry width, the brs left, windows, summary classes.

Then the jump tables of a module written word by word (hand_tables below; README.md, "Module loader and discovery"): each form compilers emit
for a switch whose index is computed from more than one value (a bit field of a product, the case offset of a sum, a mask of an exclusive or,
a shifted sum compared on its low half), with byte, halfword and word entries, signed and unsigned, an `adr` base and a table-relative one,
must be a jump table with exactly its cases, index by index; and each unsound look-alike (the compare on another register, a compare that a
path to the load goes around, the index changed by another value after its compare or its mask, a compare of the low half of a 64-bit index,
a signed compare) must stay a br for the dispatcher.
Exit 0 when every check passes.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus')
NOP = 0xD503201F


def is_pad_or_trap(w):
    return w in (NOP, 0) or (w & 0xFFE0001F) in (0xD4200000, 0xD4400000) or (w >> 16) == 0


def symbols(path):
    funcs, data_maps = {}, []
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                continue
            value, size, kind, bind, name = line.rstrip('\n').split('\t')
            value, size = int(value, 16), int(size, 16)
            if kind == 'func':
                if value not in funcs or bind != 'local':
                    funcs[value] = (size, name)
            elif kind == 'map' and name.startswith('$d'):
                data_maps.append(value)
    return funcs, data_maps


def discover(nxaot, nro, tsv, oracle):
    cmd = [nxaot, 'discover', nro, '--tsv', tsv] + (['--symbols-as-oracle'] if oracle else [])
    run = subprocess.run(cmd, capture_output=True, text=True)
    if run.returncode:
        raise SystemExit(f'{" ".join(cmd)} failed:\n{run.stdout}{run.stderr}')
    fns, tables, brs, summary, windows = {}, [], [], {}, []
    with open(tsv) as f:
        for line in f:
            r = line.rstrip('\n').split('\t')
            if r[0] == 'fn':
                fns[int(r[1], 16)] = {'size': int(r[2], 16), 'prov': r[4], 'class': r[14], 'name': r[17]}
            elif r[0] == 'jt':
                tables.append({'br': int(r[1], 16), 'width': int(r[4]), 'count': int(r[9])})
            elif r[0] == 'br':
                brs.append((int(r[1], 16), r[4]))
            elif r[0] == 'win':
                windows.append(r[4])
            elif r[0] == 'summary':
                summary[r[1]] = dict(zip(r[2::2], r[3::2]))
    return fns, tables, brs, summary, windows


def data_blocks(nxaot, nro):
    run = subprocess.run([nxaot, 'module', nro], capture_output=True, text=True, check=True)
    return [(int(r.split('\t')[1], 16), int(r.split('\t')[2], 16)) for r in run.stdout.splitlines() if r.startswith('data\t')]


# ---------- Jump tables of a module written word by word ----------
# The A64 encodings, each checked with llvm-mc-18 (nxnative-aot image), as gen_bench.py's are

def _rel(off, bits):
    return (off >> 2) & ((1 << bits) - 1)


HI, HS, GT, RET = 8, 2, 12, 0xD65F03C0
def e_b(off): return 0x14000000 | _rel(off, 26)
def e_bl(off): return 0x94000000 | _rel(off, 26)
def e_bcond(cond, off): return 0x54000000 | _rel(off, 19) << 5 | cond                    # b.<cond>: 2 hs, 8 hi, 12 gt
def e_cbz(rt, off): return 0xB4000000 | _rel(off, 19) << 5 | rt                         # cbz xt
def e_br(rn): return 0xD61F0000 | rn << 5
def e_movz(rd, imm): return 0xD2800000 | imm << 5 | rd                                  # movz xd, #imm
def e_adr(rd, off): return 0x10000000 | (off & 3) << 29 | ((off >> 2) & 0x7FFFF) << 5 | rd
def e_adrp(rd, pages): return 0x90000000 | (pages & 3) << 29 | ((pages >> 2) & 0x7FFFF) << 5 | rd
def e_addi(rd, rn, imm): return 0x91000000 | imm << 10 | rn << 5 | rd                  # add xd, xn, #imm
def e_subi_w(rd, rn, imm): return 0x51000000 | imm << 10 | rn << 5 | rd               # sub wd, wn, #imm
def e_add(rd, rn, rm, lsl=0): return 0x8B000000 | rm << 16 | lsl << 10 | rn << 5 | rd  # add xd, xn, xm, lsl #lsl
def e_add_w(rd, rn, rm): return 0x0B000000 | rm << 16 | rn << 5 | rd                   # add wd, wn, wm
def e_add_ext(rd, rn, rm, option, sh): return 0x8B200000 | rm << 16 | option << 13 | sh << 10 | rn << 5 | rd  # add xd, xn, wm, uxtb (0) / sxth (5) #sh
def e_eor(rd, rn, rm): return 0xCA000000 | rm << 16 | rn << 5 | rd
def e_mul(rd, rn, rm): return 0x9B007C00 | rm << 16 | rn << 5 | rd                     # mul xd, xn, xm
def e_ubfx(rd, rn, lsb, width): return 0xD3400000 | lsb << 16 | (lsb + width - 1) << 10 | rn << 5 | rd
def e_and_ones(rd, rn, ones): return 0x92400000 | (ones - 1) << 10 | rn << 5 | rd      # and xd, xn, #(2^ones - 1)
def e_cmpi(rn, imm): return 0xF100001F | imm << 10 | rn << 5                           # cmp xn, #imm
def e_cmpi_w(rn, imm): return 0x7100001F | imm << 10 | rn << 5                         # cmp wn, #imm
def e_ldrb(rt, rn, rm): return 0x38606800 | rm << 16 | rn << 5 | rt                    # ldrb wt, [xn, xm]
def e_ldrb_uxtw(rt, rn, rm): return 0x38604800 | rm << 16 | rn << 5 | rt               # ldrb wt, [xn, wm, uxtw]
def e_ldrh_uxtw(rt, rn, rm): return 0x78605800 | rm << 16 | rn << 5 | rt               # ldrh wt, [xn, wm, uxtw #1]
def e_ldrsw(rt, rn, rm): return 0xB8A07800 | rm << 16 | rn << 5 | rt                   # ldrsw xt, [xn, xm, lsl #2]


class HandModule:
    """A behaviour image of functions written word by word, with no symbol and no FDE: word 0 branches over word +4 (a data word, as an NRO's
    MOD0 offset) to main, which calls every function; the jump tables in read-only data; its manifest beside it (src/module/manifest.h). A word
    is a number or a function of (its address, the address of a label)"""
    PAGE = 0x1000

    def __init__(self):
        self.words, self.labels, self.tables = [e_b(8), 0], {}, []

    def label(self, name):
        self.labels[name] = 4 * len(self.words)

    def emit(self, *words):
        self.words.extend(words)

    def table(self, name, width, count, entries):
        """A table of count entries of width bytes at label name: entries(address of a label, the table's address) -> the entries"""
        self.tables.append((name, width, count, entries))

    def write(self, folder, name):
        text = 4 * len(self.words)
        ro_off = -(-text // self.PAGE) * self.PAGE
        ro, at = bytearray(), []
        for tname, width, count, _ in self.tables:
            ro += bytes(-len(ro) % 8)
            self.labels[tname] = ro_off + len(ro)
            at.append(len(ro))
            ro += bytes(width * count)
        addr = self.labels.__getitem__
        for (tname, width, count, entries), off in zip(self.tables, at):
            values = entries(addr, addr(tname))
            assert len(values) == count
            for i, v in enumerate(values):
                ro[off + i * width:off + (i + 1) * width] = (v & ((1 << 8 * width) - 1)).to_bytes(width, 'little')
        code = b''.join(struct.pack('<I', w(4 * i, addr) if callable(w) else w) for i, w in enumerate(self.words))
        data_off = -(-(ro_off + len(ro)) // self.PAGE) * self.PAGE
        image = code + bytes(ro_off - text) + ro + bytes(data_off - ro_off - len(ro)) + bytes(8)
        with open(os.path.join(folder, name + '.image.bin'), 'wb') as f:
            f.write(image)
        manifest = os.path.join(folder, name + '.aot.tsv')
        with open(manifest, 'w') as f:
            f.write(f'# nxaot-manifest 1\tmain\t{hashlib.sha1(image).hexdigest()}\t{hashlib.sha256(image).hexdigest()}\n'
                    f'layout\ttext_end\t{ro_off:#x}\tro_off\t{ro_off:#x}\tdata_off\t{data_off:#x}\tbss\t0x0\timage_size\t{len(image):#x}\n'
                    'data\t0x4\t0x8\n')
        return manifest


# Each case: (name, what the switch is, how many cases, the body's builder). A builder emits the function at label name and returns the label
# of its br and, for a form that must be bounded, the entries' width, whether they are signed, and the label each index reaches; for a
# look-alike, None. The functions read x0, x1 and x2; case k returns k + 1; default returns 0xffff
def _cases(m, name, n):
    for k in range(n):
        m.label(f'{name}.case{k}')
        m.emit(e_movz(0, k + 1), RET)
    m.label(f'{name}.default')
    m.emit(e_movz(0, 0xffff), RET)


def _adrp_add(m, rd, table):
    m.emit(lambda pc, A: e_adrp(rd, (A(table) >> 12) - (pc >> 12)), lambda pc, A: e_addi(rd, rd, A(table) & 0xFFF))


def _to(cond, label):
    return lambda pc, A: e_bcond(cond, A(label) - pc)


def _byte_dispatch(m, name, index, n):
    """adrp x11 / add x11 (the table); adr x12 (the base: case 0); ldrb w13, [x11, <index>]; add x12, x12, x13, lsl #2; br x12, then the cases"""
    _adrp_add(m, 11, f'{name}.table')
    m.emit(lambda pc, A: e_adr(12, A(f'{name}.case0') - pc), e_ldrb(13, 11, index), e_add(12, 12, 13, 2))
    m.label(f'{name}.br')
    m.emit(e_br(12))
    _cases(m, name, n)
    order = [(5 * i + 3) % n for i in range(n)]  # index i -> case order[i]: not in order, so that the targets' order is checked
    m.table(f'{name}.table', 1, n, lambda A, t: [(A(f'{name}.case{k}') - A(f'{name}.case0')) // 4 for k in order])
    return [f'{name}.case{k}' for k in order]


def form_byte_product(m, name):
    # clang's and GCC's dense switch over a bit field of a product (gen_bench.py's `switch ((a * b) >> 37 & 31)`): ubfx; cmp; b.hi
    n = 12
    m.emit(e_mul(8, 0, 1), e_ubfx(10, 8, 37, 5), e_cmpi(10, n - 1), _to(HI, f'{name}.default'))
    return f'{name}.br', 1, False, _byte_dispatch(m, name, 10, n)


def form_half_offset(m, name):
    # GCC's signed halfword table over a case offset of a sum (sub, cmp, b.hi on w8, the entry at uxtw #1, added sxth #2), with a case placed
    # before the base: its entry is negative
    n = 9
    m.emit(lambda pc, A: e_b(A(f'{name}.dispatch') - pc))
    m.label(f'{name}.neg')
    m.emit(e_movz(0, 100), RET)
    m.label(f'{name}.dispatch')
    m.emit(e_add_w(8, 0, 1), e_subi_w(8, 8, 3), e_cmpi_w(8, n - 1), _to(HI, f'{name}.default'))
    _adrp_add(m, 9, f'{name}.table')
    m.emit(e_ldrh_uxtw(10, 9, 8), lambda pc, A: e_adr(11, A(f'{name}.case0') - pc), e_add_ext(10, 11, 10, 5, 2))
    m.label(f'{name}.br')
    m.emit(e_br(10))
    _cases(m, name, n)
    labels = [f'{name}.neg'] + [f'{name}.case{k}' for k in range(1, n)]
    m.table(f'{name}.table', 2, n, lambda A, t: [(A(lb) - A(f'{name}.case0')) // 4 for lb in labels])
    return f'{name}.br', 2, True, labels


def form_word_relative(m, name):
    # clang's table-relative word entries (ldrsw, added to the table's own address) over a mask of an exclusive or: no compare, the mask bounds it
    n = 8
    m.emit(e_eor(8, 0, 1), e_and_ones(8, 8, 3))
    _adrp_add(m, 9, f'{name}.table')
    m.emit(e_ldrsw(10, 9, 8), e_add(10, 9, 10))
    m.label(f'{name}.br')
    m.emit(e_br(10))
    _cases(m, name, n)
    order = [(3 * i + 1) % n for i in range(n)]
    m.table(f'{name}.table', 4, n, lambda A, t: [A(f'{name}.case{k}') - t for k in order])
    return f'{name}.br', 4, True, [f'{name}.case{k}' for k in order]


def form_byte_low_half(m, name):
    # An unsigned byte table over the zero-extended low half of a shifted sum (cmp w8; b.hs; the entry at uxtw, added uxtb #2)
    n = 6
    m.emit(e_add(8, 0, 1, 4), e_cmpi_w(8, n), _to(HS, f'{name}.default'))
    _adrp_add(m, 10, f'{name}.table')
    m.emit(e_ldrb_uxtw(9, 10, 8), lambda pc, A: e_adr(11, A(f'{name}.case0') - pc), e_add_ext(9, 11, 9, 0, 2))
    m.label(f'{name}.br')
    m.emit(e_br(9))
    _cases(m, name, n)
    order = [n - 1 - i for i in range(n)]
    m.table(f'{name}.table', 1, n, lambda A, t: [(A(f'{name}.case{k}') - A(f'{name}.case0')) // 4 for k in order])
    return f'{name}.br', 1, False, [f'{name}.case{k}' for k in order]


def unsound_other_register(m, name):
    # The compare bounds x2, the table is indexed by x8
    m.emit(e_eor(8, 0, 1), e_cmpi(2, 11), _to(HI, f'{name}.default'))
    _byte_dispatch(m, name, 8, 12)
    return f'{name}.br', None, None, None


def unsound_around(m, name):
    # cbz x2 goes to the table's load around the compare: the bound does not hold on every path to the br
    m.emit(e_eor(8, 0, 1), lambda pc, A: e_cbz(2, A(f'{name}.load') - pc), e_cmpi(8, 11), _to(HI, f'{name}.default'))
    m.label(f'{name}.load')
    _byte_dispatch(m, name, 8, 12)
    return f'{name}.br', None, None, None


def unsound_changed(m, name):
    # The index bounded by its compare, then another value added to it before the load
    m.emit(e_eor(8, 0, 1), e_cmpi(8, 11), _to(HI, f'{name}.default'), e_add(8, 8, 2))
    _byte_dispatch(m, name, 8, 12)
    return f'{name}.br', None, None, None


def unsound_mask_changed(m, name):
    # The relative word table's form, another value added to the index after its mask
    m.emit(e_eor(8, 0, 1), e_and_ones(8, 8, 3), e_add(8, 8, 2))
    _adrp_add(m, 9, f'{name}.table')
    m.emit(e_ldrsw(10, 9, 8), e_add(10, 9, 10))
    m.label(f'{name}.br')
    m.emit(e_br(10))
    _cases(m, name, 8)
    m.table(f'{name}.table', 4, 8, lambda A, t: [A(f'{name}.case{k}') - t for k in range(8)])
    return f'{name}.br', None, None, None


def unsound_low_half(m, name):
    # The low half compared (cmp w8), the whole 64-bit register used as the index
    m.emit(e_add(8, 0, 1, 4), e_cmpi_w(8, 11), _to(HI, f'{name}.default'))
    _byte_dispatch(m, name, 8, 12)
    return f'{name}.br', None, None, None


def unsound_signed(m, name):
    # A signed compare (b.gt): a negative index passes it
    m.emit(e_eor(8, 0, 1), e_cmpi(8, 11), _to(GT, f'{name}.default'))
    _byte_dispatch(m, name, 8, 12)
    return f'{name}.br', None, None, None


HAND_FORMS = [('byte_product', form_byte_product), ('half_offset', form_half_offset), ('word_relative', form_word_relative),
              ('byte_low_half', form_byte_low_half), ('other_register', unsound_other_register), ('around', unsound_around),
              ('changed', unsound_changed), ('mask_changed', unsound_mask_changed), ('low_half', unsound_low_half), ('signed', unsound_signed)]


def hand_tables(nxaot, work, failures):
    """The jump tables of the module of HAND_FORMS: each form bounded with its cases index by index, each look-alike a br for the dispatcher"""
    m = HandModule()
    m.label('main')
    for name, _ in HAND_FORMS:
        m.emit(lambda pc, A, name=name: e_bl(A(name) - pc))
    m.emit(RET)
    expect = {}
    for name, build in HAND_FORMS:
        while len(m.words) % 4:
            m.emit(NOP)
        m.label(name)
        expect[name] = build(m, name)
    folder = os.path.join(work, 'hand')
    os.makedirs(folder, exist_ok=True)
    manifest = m.write(folder, 'hand')
    tsv = os.path.join(folder, 'hand.tsv')
    run = subprocess.run([nxaot, 'discover', manifest, '--tsv', tsv], capture_output=True, text=True)
    if run.returncode:
        failures.append(f'hand-written tables: nxaot discover failed:\n{run.stdout}{run.stderr}')
        return
    tables, brs = {}, {}
    with open(tsv) as f:
        for line in f:
            r = line.rstrip('\n').split('\t')
            if r[0] == 'jt':
                tables[int(r[1], 16)] = {'width': int(r[4]), 'signed': r[5] == '1', 'targets': [int(t, 16) for t in r[12].split(',')]}
            elif r[0] == 'br':
                brs[int(r[1], 16)] = r[4]
    for name, (br_label, width, signed, labels) in expect.items():
        br = m.labels[br_label]
        if width is None:
            ok = br in brs and br not in tables
            got = f'br: {brs[br]}' if br in brs else f'a table of {len(tables[br]["targets"])} targets' if br in tables else 'neither'
        else:
            want = [m.labels[lb] for lb in labels]
            t = tables.get(br)
            ok = t is not None and t['targets'] == want and t['width'] == width and t['signed'] == signed
            got = (f'a table of {len(t["targets"])} entries, width {t["width"]}, signed {int(t["signed"])}' if t else
                   f'br: {brs.get(br, "none")}')
            if t and t['targets'] != want:
                got += f', targets {[hex(x) for x in t["targets"]]} for {[hex(x) for x in want]}'
        print(f'hand-written {"table" if width else "look-alike"} {name:<15} at {br:#x}: {got}')
        if not ok:
            failures.append(f'hand-written {name} (br at {br:#x}): {"a table with its cases" if width else "a br for the dispatcher"} '
                            f'expected, got {got}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    rows = []
    with open(os.path.join(CORPUS, 'corpus.tsv')) as f:
        for line in f:
            if not line.startswith('#') and line.strip():
                nro, program, toolchain = line.split('\t')[:3]
                rows.append((nro, program, toolchain))
    failures = []
    header = (f'{"nro":<18} {"mode":<7} {"fns":>4} {"provenance (primary)":<44} {"tables b/h/w":>12} {"br":>3} {"windows c/a/l":>13} '
              f'{"pres/open/nor":>13} {"pads":>4} {"miss":>4} {"extra":>5} {"split":>5} {"sizes =/<":>9} {"uncov":>5}')
    print(header)
    for nro, program, toolchain in rows:
        path = os.path.join(CORPUS, 'nro', nro)
        image = open(path, 'rb').read()
        funcs, data_maps = symbols(os.path.join(CORPUS, 'nro', nro[:-4] + '.syms.tsv'))
        blocks = data_blocks(a.nxaot, path)
        for addr in data_maps:
            if not any(lo <= addr < hi for lo, hi in blocks):
                failures.append(f'{nro}: the $d mapping symbol at {addr:#x} lies in no data block of the loader ({blocks})')
        ranges = sorted((v, v + s) for v, (s, _) in funcs.items())
        for mode in ('symbols', 'oracle'):
            fns, tables, brs, summary, windows = discover(a.nxaot, path, os.path.join(a.work, f'{nro}.{mode}.tsv'), mode == 'oracle')
            tag = f'{nro} ({mode})'
            missed = [(v, n) for v, (s, n) in funcs.items() if v not in fns]
            for v, n in missed:
                failures.append(f'{tag}: the symbol {n} at {v:#x} is no discovered function')
            extra = [e for e in fns if e not in funcs]
            splits = [e for e in fns if any(lo < e < hi for lo, hi in ranges)]
            for e in extra:
                failures.append(f'{tag}: the discovered function at {e:#x} ({fns[e]["prov"]}) has no symbol')
            for e in splits:
                failures.append(f'{tag}: the discovered function at {e:#x} ({fns[e]["prov"]}) lies inside a symbol\'s range')
            same = shorter = 0
            for v, (s, n) in funcs.items():
                if v not in fns:
                    continue
                d = fns[v]['size']
                if d == s:
                    same += 1
                elif d < s and all(is_pad_or_trap(struct.unpack_from('<I', image, p)[0]) for p in range(v + d, v + s, 4)):
                    shorter += 1
                else:
                    failures.append(f'{tag}: {n} at {v:#x}: discovered size {d:#x}, symbol size {s:#x}, and the difference is not padding or traps')
            cov = summary.get('coverage', {})
            if cov.get('uncovered') != '0' or cov.get('undefined') != '0':
                failures.append(f'{tag}: coverage {cov}')
            if program == 'switch' and brs:
                failures.append(f'{tag}: brs left for the dispatcher in the switch program: {[f"{b:#x} ({w})" for b, w in brs]}')
            if nro == 'except-gcc.nro' and not any(t['width'] == 2 and t['count'] == 47 for t in tables):
                failures.append(f'{tag}: the CFA interpreter\'s switch (a halfword table of 47 entries) is not a jump table: brs {brs}')
            pads = int(summary.get('discovery', {}).get('landing_pads', '0'))
            if program == 'fnptr' and pads == 0:
                failures.append(f'{tag}: no landing pad found in a program built with -fexceptions')
            if program == 'excl' and 'aba' not in windows:
                failures.append(f'{tag}: the ABA-sensitive window of the lock-free pop was not found')
            primary = {}
            for fn in fns.values():
                p = fn['prov'].split(',')[0]
                primary[p] = primary.get(p, 0) + 1
            prov = ','.join(f'{k}:{primary[k]}' for k in sorted(primary, key=lambda k: -primary[k]))
            widths = [sum(1 for t in tables if t['width'] == w) for w in (1, 2, 4)]
            classes = [sum(1 for f in fns.values() if f['class'] == c) for c in ('preserving', 'open', 'noreturn')]
            wins = [windows.count(k) for k in ('cas', 'aba', 'load')]
            print(f'{nro:<18} {mode:<7} {len(fns):>4} {prov:<44} {"/".join(map(str, widths)):>12} {len(brs):>3} {"/".join(map(str, wins)):>13} '
                  f'{"/".join(map(str, classes)):>13} {pads:>4} {len(missed):>4} {len(extra):>5} {len(splits):>5} {f"{same}/{shorter}":>9} '
                  f'{cov.get("uncovered", "?"):>5}')
    hand_tables(a.nxaot, a.work, failures)
    for line in failures:
        print('FAIL ' + line)
    print(f'discover test: {len(rows)} NROs, 2 modes each, {len(HAND_FORMS)} hand-written switches, {len(failures)} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
