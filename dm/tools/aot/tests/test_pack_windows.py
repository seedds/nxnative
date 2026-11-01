#!/usr/bin/env python3
"""Windows packs (CTest `pack_windows`; README.md "Windows packs", docs/design/aot-recompiler.md 9.4): two corpus NROs (switch-gcc: jump tables and
sw_word split into parts at the default limit; fp-gcc: the floating-point helpers and the shared NaN helpers of comdat groups), the synthetic game module
(gate sites, the aux region) and the host-function module's two (dm/tools/tests/aot_synth.py), each packed twice by `nxaot build`: as the Linux shared
object and with `--target windows` as a DLL. Each DLL is checked with llvm-readobj and llvm-objdump and with the small PE and ELF readers below:

  - a PE32+ DLL for x86-64 (AMD64, IMAGE_FILE_DLL, dynamic base, NX), no entry point, no import descriptor or delay import (it names no module);
  - its exports are exactly nxaot_module and nxaot_imports;
  - its descriptor (read through the export: ABI, flags, name, SHA-256, sizes, entry, FPCR, the standard entries' signature, the counts) is the ELF
    pack's, and so are its tables' module-side columns (function map: offsets and sizes; lookup table: offsets and flags; returns; gate sites);
  - nxaot_imports names exactly the ELF pack's undefined symbols, each a run-time entry the host exports (DM_AOT_EXPORT in dm/host/aot_run.c,
    FPEXPORT in aot_fp.c), each slot 8 zero bytes in a writable section, each entry's thunk in the code a `jmp *` through its slot;
  - every pointer of the descriptor and of nxaot_imports has a base relocation (DIR64), so the DLL loads at any base;
  - `nxaot lines` on the DLL: no bad entry, as many entries as on the ELF pack;
  - the code: every function (bodies, standard entries, parts, the tables' code) disassembled in both packs and compared instruction by instruction,
    the operands that depend on the layout (rip-relative displacements, branch targets outside the function) normalized and the raw bytes compared
    where no such operand is: every function that does not call a run-time entry directly is identical; the ones that differ are listed with
    their first difference;
  - the DLL is deterministic: the same bytes with one worker and with eight;
and a pack that would call something else than the run time fails: fp-gcc for the baseline x86-64 CPU (no SSE4.1) calls libm's ceil and floor, which
the build must refuse, naming them.

  test_pack_windows.py --nxaot <nxaot> --readobj <llvm-readobj> --objdump <llvm-objdump> --work <dir>

Exit 0 when every check holds.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..'))
CORPUS = os.path.join(HERE, '..', 'corpus', 'nro')
TOOLS_TESTS = os.path.join(HERE, '..', '..', 'tests')
DESC = struct.Struct('<II32s32sQQQQIIIIQQQQQQQQQQQIIQQ')   # include/nxaot/aot_module.h nxaot_module_desc (232 bytes)
DESC_FIELDS = ('abi flags name sha256 image_size text_end bss_size entry fpcr lookup_mask rv pad base lookup fns fn_count fn_lines lines enter tool '
               'returns return_count jit_entry std_x std_v gates gate_count').split()
POINTERS = ('base', 'lookup', 'fns', 'fn_lines', 'lines', 'enter', 'tool', 'returns', 'jit_entry', 'gates')
FAILS = []
CHECKS = [0]


def check(ok, what):
    print(f'    {"ok  " if ok else "FAIL"} {what}')
    CHECKS[0] += 1
    if not ok:
        FAILS.append(what)
    return ok


# ---- PE ----
class Pe:
    def __init__(self, path):
        self.b = b = open(path, 'rb').read()
        pe = struct.unpack_from('<I', b, 0x3C)[0]
        assert b[pe:pe + 4] == b'PE\0\0', 'no PE signature'
        self.machine, nsec, _, _, _, opt_size, self.chars = struct.unpack_from('<HHIIIHH', b, pe + 4)
        opt = pe + 24
        self.magic = struct.unpack_from('<H', b, opt)[0]
        self.entry = struct.unpack_from('<I', b, opt + 16)[0]
        self.image_base = struct.unpack_from('<Q', b, opt + 24)[0]
        self.dll_chars = struct.unpack_from('<H', b, opt + 70)[0]
        ndirs = struct.unpack_from('<I', b, opt + 108)[0]
        self.dirs = [struct.unpack_from('<II', b, opt + 112 + 8 * i) for i in range(ndirs)]
        self.sections = []
        for i in range(nsec):
            name, vsize, va, rawsize, rawptr, _, _, _, _, ch = struct.unpack_from('<8sIIIIIIHHI', b, opt + opt_size + 40 * i)
            self.sections.append({'name': name.rstrip(b'\0').decode(), 'vsize': vsize, 'va': va, 'rawsize': rawsize, 'rawptr': rawptr, 'chars': ch})

    def section_of(self, rva):
        return next((s for s in self.sections if s['va'] <= rva < s['va'] + max(s['vsize'], s['rawsize'])), None)

    def at(self, rva, n):   # bytes at an RVA as the loader maps them (zero beyond a section's raw data)
        s = self.section_of(rva)
        assert s, f'RVA {rva:#x} is in no section'
        off = rva - s['va']
        raw = self.b[s['rawptr'] + off:s['rawptr'] + min(s['rawsize'], off + n)] if off < s['rawsize'] else b''
        return raw + bytes(n - len(raw))

    def va(self, va, n):
        return self.at(va - self.image_base, n)

    def cstr(self, rva):
        out = b''
        while True:
            c = self.at(rva + len(out), 1)
            if c == b'\0':
                return out.decode()
            out += c

    def exports(self):
        rva, size = self.dirs[0]
        if not size:
            return {}
        _, _, _, _, _, base, nfun, nnames, afun, anames, aord = struct.unpack_from('<IIHHIIIIIII', self.at(rva, 40))
        out = {}
        for i in range(nnames):
            name = self.cstr(struct.unpack('<I', self.at(anames + 4 * i, 4))[0])
            ordinal = struct.unpack('<H', self.at(aord + 2 * i, 2))[0]
            out[name] = struct.unpack('<I', self.at(afun + 4 * ordinal, 4))[0]
        return out

    def base_relocs(self):   # the RVAs with an IMAGE_REL_BASED_DIR64
        rva, size = self.dirs[5]
        out, pos = set(), 0
        while pos + 8 <= size:
            page, block = struct.unpack('<II', self.at(rva + pos, 8))
            for k in range((block - 8) // 2):
                e = struct.unpack('<H', self.at(rva + pos + 8 + 2 * k, 2))[0]
                if e >> 12 == 10:
                    out.add(page + (e & 0xFFF))
            pos += block
        return out


# ---- ELF ----
class Elf:
    def __init__(self, path):
        self.b = b = open(path, 'rb').read()
        assert b[:4] == b'\x7fELF', 'not ELF'
        shoff = struct.unpack_from('<Q', b, 0x28)[0]
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x3A)
        hdrs = [struct.unpack_from('<IIQQQQIIQQ', b, shoff + i * shentsize) for i in range(shnum)]
        names = hdrs[shstrndx]
        self.sections = []
        for h in hdrs:
            name = b[names[4] + h[0]:b.index(b'\0', names[4] + h[0])].decode()
            self.sections.append({'name': name, 'type': h[1], 'addr': h[3], 'offset': h[4], 'size': h[5], 'link': h[6], 'entsize': h[9]})
        self.by_name = {s['name']: s for s in self.sections}

    def data(self, name):
        s = self.by_name.get(name)
        return self.b[s['offset']:s['offset'] + s['size']] if s else b''

    def symbols(self, table):   # (name, value, section index) of .symtab or .dynsym
        s = self.by_name[table]
        strs = self.sections[s['link']]
        out = []
        for k in range(s['size'] // 24):
            st_name, _, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', self.b, s['offset'] + 24 * k)
            name = self.b[strs['offset'] + st_name:self.b.index(b'\0', strs['offset'] + st_name)].decode()
            out.append((name, value, shndx))
        return out

    def at(self, addr, n):
        s = next(s for s in self.sections if s['addr'] and s['addr'] <= addr < s['addr'] + s['size'])
        return self.b[s['offset'] + addr - s['addr']:s['offset'] + addr - s['addr'] + n]


def desc_of(raw):
    return dict(zip(DESC_FIELDS, DESC.unpack(raw)))


# ---- The code, instruction by instruction ----
def functions(objdump, path):
    """Every function of a pack (the module's bodies, standard entries and parts, the tables' code): its instructions as (raw bytes, normalized text,
    whether an operand depends on the layout)"""
    out = subprocess.run([objdump, '-d', path], capture_output=True, text=True, check=True).stdout
    fns, cur = {}, None
    for line in out.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            cur = m.group(1)
            fns[cur] = []
            continue
        m = re.match(r'^\s*[0-9a-f]+:((?: [0-9a-f]{2})+)\s*(?:\t(.*))?$', line)   # "addr: bytes<spaces>\tmnemonic\toperands"
        if cur is None or not m:
            continue
        raw, text = bytes.fromhex(m.group(1).replace(' ', '')), (m.group(2) or '').strip()
        if not text:   # the rest of a long instruction's bytes
            if fns[cur]:
                b, t, lay = fns[cur][-1]
                fns[cur][-1] = (b + raw, t, lay)
            continue
        text = re.sub(r'\s+#.*$', '', text)
        text = text.replace('@plt', '')
        layout = '(%rip)' in text
        target = re.search(r'0x[0-9a-f]+ <([^>]+)>', text)
        if target and not (target.group(1) == cur or target.group(1).startswith(cur + '+')):
            layout = True   # a branch or call out of the function: its displacement depends on the layout
        text = re.sub(r'0x[0-9a-f]+ <', '<', text)
        text = re.sub(r'-?0x[0-9a-f]+\(%rip\)', 'X(%rip)', text)
        text = re.sub(r'\s+', ' ', text)
        fns[cur].append((raw, text, layout))
    for name in fns:   # the padding after a function
        while fns[name] and (fns[name][-1][1] == 'int3' or fns[name][-1][1].startswith('nop')):
            fns[name].pop()
    return fns


def compare_code(objdump, so, dll, imports, tag):
    """The functions of both packs, compared; returns the names of the identical ones. A function may differ only where the ABI makes it: at the host's
    boundary (a direct call of a run-time entry, Win64 in the DLL; nxaot.enter, which the host calls) and where the ELF code uses SysV's red zone (128
    bytes below rsp, which a Windows function may not: the DLL's frame holds them)"""
    a, b = functions(objdump, so), functions(objdump, dll)
    ours = [n for n in a if '+0x' in n or n.startswith('nxaot.')]
    insns = raw_bytes = 0
    same, kinds, report = set(), {'host': 0, 'red zone': 0}, []
    for name in ours:
        x, y = a[name], b.get(name)
        if y is None:
            check(False, f'{tag}: the DLL has no function {name}')
            continue
        texts_equal = [t for _, t, _ in x] == [t for _, t, _ in y]
        if texts_equal and all(rx == ry for (rx, _, lx), (ry, _, _) in zip(x, y) if not lx):
            same.add(name)
            insns += len(x)
            raw_bytes += sum(len(r) for r, _, lx in x if not lx)
            continue
        if name == 'nxaot.enter' or any(re.match(r'(callq|jmpq|call|jmp) <(' + '|'.join(map(re.escape, imports)) + r')>$', t) for _, t, _ in x):
            kind = 'host'
        elif any(re.search(r'(^|[ ,])-0x[0-9a-f]+\(%rsp\)', t) for _, t, _ in x):
            kind = 'red zone'
        else:
            kind = None
        first = next(((i, p, q) for i, (p, q) in enumerate(zip(x, y)) if p[1] != q[1] or (not p[2] and p[0] != q[0])), None)
        what = f'{name}: ' + (f'instruction {first[0]}: ELF `{first[1][1]}`, DLL `{first[2][1]}`' if first else f'{len(x)} instructions, {len(y)} in the DLL')
        if kind is None:
            check(False, f'{tag}: {what}: it differs, and it neither calls the run time nor uses the red zone')
        else:
            kinds[kind] += 1
        report.append(f'{what} ({kind or "unexplained"})')
    print(f'    code: {len(ours)} functions, {len(same)} identical ({insns} instructions, {raw_bytes} bytes compared raw); {len(ours) - len(same)} differ: '
          f'{kinds["host"]} at the host\'s boundary, {kinds["red zone"]} by the red zone')
    for r in report[:6]:
        print(f'      {r}')
    if len(report) > 6:
        print(f'      ... and {len(report) - 6} more')
    return same, ours


def sanitize(name):   # src/build/pack.cpp Sanitize: the module's symbol prefix
    if len(name) > 4 and name.endswith('.nro'):
        name = name[:-4]
    return re.sub(r'[^A-Za-z0-9_]', '_', name) or 'module'


def line_runs(fns, fn_lines, lines):
    """Per guest function (module offset): its run of the line table, [(native - body, guest offset)]"""
    out = {}
    for k, (offset, _, _) in enumerate(fns):
        first, count = struct.unpack_from('<II', fn_lines, 8 * k)
        out[offset] = [struct.unpack_from('<II', lines, 8 * (first + j)) for j in range(count)]
    return out


def host_exports():
    names = set()
    for f, pat in (('aot_run.c', r'DM_AOT_EXPORT\s+[\w\s\*]+?\b(\w+)\s*\('), ('aot_fp.c', r'FPEXPORT\s+[\w\s\*]+?FPNAME\((\w+)\)')):
        with open(os.path.join(REPO, 'dm', 'host', f)) as fh:
            names |= set(re.findall(pat, fh.read()))
    return names


def lines_of(nxaot, pack, module):
    r = subprocess.run([nxaot, 'lines', pack, module], capture_output=True, text=True)
    m = re.search(r'(\d+) entries in (\d+) of (\d+) functions; .*?(\d+) bad', r.stdout)
    return (r.returncode, int(m.group(1)), int(m.group(4))) if m else (r.returncode, -1, -1)


def one(a, tag, module, exports):
    so, dll = os.path.join(a.work, tag + '.so'), os.path.join(a.work, tag + '.dll')
    print(f'{tag}:')
    for args in (['--shared', so], ['--target', 'windows', '--shared', dll]):
        r = subprocess.run([a.nxaot, 'build', module, *args], capture_output=True, text=True)
        if not check(r.returncode == 0, f'nxaot build {" ".join(args[:-1])} exits 0'):
            print((r.stdout + r.stderr)[-3000:])
            return
    log = r.stdout
    pe, elf = Pe(dll), Elf(so)
    # The file: a DLL for x86-64 that names no module
    check(pe.magic == 0x20B and pe.machine == 0x8664 and pe.chars & 0x2000 and pe.dll_chars & 0x40 and pe.dll_chars & 0x100,
          f'a PE32+ DLL for AMD64, dynamic base and NX (magic {pe.magic:#x}, machine {pe.machine:#x}, characteristics {pe.chars:#x}, DLL {pe.dll_chars:#x})')
    check(pe.entry == 0, 'no entry point')
    ro = subprocess.run([a.readobj, '--coff-imports', '--coff-exports', dll], capture_output=True, text=True).stdout
    check(pe.dirs[1] == (0, 0) and pe.dirs[13] == (0, 0) and 'Import {' not in ro and 'DelayImport' not in ro,
          'no import descriptor and no delay import (llvm-readobj --coff-imports lists none)')
    ex = pe.exports()
    check(sorted(ex) == ['nxaot_imports', 'nxaot_module'] and re.findall(r'Name: (\S+)', ro) == sorted(ex),
          f'exports exactly nxaot_imports and nxaot_module (llvm-readobj: {re.findall(r"Name: (\S+)", ro)})')
    # The descriptor and its tables against the ELF pack's
    d = desc_of(pe.at(ex['nxaot_module'], DESC.size))
    sym = {n: (v, i) for n, v, i in elf.symbols('.dynsym')}
    e = desc_of(elf.at(sym['nxaot_module'][0], DESC.size))
    same_fields = [f for f in DESC_FIELDS if f not in POINTERS]
    check(all(d[f] == e[f] for f in same_fields),
          f'the descriptor (ABI {d["abi"]}, {d["name"].rstrip(bytes(1)).decode()}, flags {d["flags"]:#x}, std {d["std_x"]:#x}/{d["std_v"]:#x}, '
          f'{d["fn_count"]} functions, {d["return_count"]} returns, {d["gate_count"]} gate sites, {d["lookup_mask"] + 1} lookup slots) is the ELF pack\'s'
          + ('' if all(d[f] == e[f] for f in same_fields) else f': {[(f, d[f], e[f]) for f in same_fields if d[f] != e[f]]}'))
    fn_d = [struct.unpack_from('<QiI', pe.va(d['fns'], 16 * d['fn_count']), 16 * k) for k in range(d['fn_count'])]
    fn_e = [struct.unpack_from('<QiI', elf.data('.nxaot.fnmap'), 16 * k) for k in range(len(elf.data('.nxaot.fnmap')) // 16)]
    check([(o, s) for o, _, s in fn_d] == [(o, s) for o, _, s in fn_e], f'the function map: {len(fn_d)} rows, the same offsets and sizes')
    slots = d['lookup_mask'] + 1
    lk_d = [struct.unpack_from('<QiI', pe.va(d['lookup'], 16 * slots), 16 * k) for k in range(slots)]
    lk_e = [struct.unpack_from('<QiI', elf.data('.nxaot.lookup'), 16 * k) for k in range(len(elf.data('.nxaot.lookup')) // 16)]
    check([(o, f) for o, _, f in lk_d] == [(o, f) for o, _, f in lk_e], f'the lookup table: {slots} slots, the same offsets and flags')
    check(pe.va(d['returns'], 4 * d['return_count']) == elf.data('.nxaot.returns'), f'the returns: {d["return_count"]}, the same')
    gates = pe.va(d['gates'], 16 * d['gate_count']) if d['gate_count'] else b''
    check(gates == elf.data('.nxaot.gates') and (d['gates'] == 0) == (d['gate_count'] == 0), f'the gate sites: {d["gate_count"]}, the same rows')
    check(pe.va(d['tool'], 6) == b'nxaot ', 'the tool string')
    # The imports: the ELF pack's undefined symbols, each exported by the host, slots writable and zero, thunks through them
    undefined = sorted(n for n, v, i in elf.symbols('.dynsym') if n and i == 0)
    imports, k = [], 0
    while True:
        name_va, slot_va = struct.unpack('<QQ', pe.at(ex['nxaot_imports'] + 16 * k, 16))
        if not name_va:
            break
        imports.append((pe.cstr(name_va - pe.image_base), slot_va))
        k += 1
    names = [n for n, _ in imports]
    check(names == undefined, f'nxaot_imports names the ELF pack\'s undefined symbols: {" ".join(names)}' + ('' if names == undefined else f' (ELF: {undefined})'))
    check(set(names) <= exports, 'each one exported by the host (DM_AOT_EXPORT, FPEXPORT)' + ('' if set(names) <= exports else f': not {sorted(set(names) - exports)}'))
    writable = all(pe.section_of(s - pe.image_base) and pe.section_of(s - pe.image_base)['chars'] & 0x80000000 and pe.va(s, 8) == bytes(8) for _, s in imports)
    check(writable, 'each slot 8 zero bytes in a writable section')
    m = re.search(r'aot: windows: (\d+) imports bound by the host \(nxaot_imports\):([^;]*);', log)
    check(m is not None and m.group(2).split() == names, 'nxaot build logs the same imports')
    dis = subprocess.run([a.objdump, '-d', '--no-show-raw-insn', dll], capture_output=True, text=True).stdout
    thunks = all(re.search(r'<' + re.escape(n) + r'>:\n\s*[0-9a-f]+:\s+jmpq\s+\*0x[0-9a-f]+\(%rip\)\s+# 0x' + format(s, 'x') + r' <nxaot\.imp\.' + re.escape(n) + '>', dis)
                 for n, s in imports)
    check(thunks, 'each entry\'s thunk is a jmp through its slot')
    # Base relocations: every pointer of the descriptor and of the import table
    relocs = pe.base_relocs()
    offsets = {'base': 120, 'lookup': 128, 'fns': 136, 'fn_lines': 152, 'lines': 160, 'enter': 168, 'tool': 176, 'returns': 184, 'jit_entry': 200, 'gates': 216}
    want = [ex['nxaot_module'] + o for f, o in offsets.items() if d[f]] + [ex['nxaot_imports'] + 16 * i + j for i in range(len(imports)) for j in (0, 8)]
    check(all(w in relocs for w in want), f'a base relocation (DIR64) at each of the {len(want)} pointers of the descriptor and of nxaot_imports '
                                           f'({len(relocs)} in all)')
    # The line table: nxaot's own reader on both, then per function: the same run wherever the code is the same
    rd, nd, bd = lines_of(a.nxaot, dll, module)
    re_, ne, be = lines_of(a.nxaot, so, module)
    check(rd == 0 and bd == 0 and nd > 0 and re_ == 0 and be == 0, f'nxaot lines on the DLL: {nd} entries, {bd} bad (the ELF pack: {ne}, {be} bad)')
    same, all_fns = compare_code(a.objdump, so, dll, names, tag)
    fl_d = pe.va(d['fn_lines'], 8 * d['fn_count'])
    total = max([first + count for first, count in (struct.unpack_from('<II', fl_d, 8 * k) for k in range(d['fn_count']))] + [0])
    runs_d = line_runs(fn_d, fl_d, pe.va(d['lines'], 8 * total))
    runs_e = line_runs(fn_e, elf.data('.nxaot.fnlines'), elf.data('.nxaot.lines'))
    prefix = sanitize(d['name'].rstrip(b'\0').decode())
    whole = [o for o, _, _ in fn_e if all(n in same for n in all_fns if n == f'{prefix}+0x{o:X}' or n.startswith(f'{prefix}+0x{o:X}.part'))
             and f'{prefix}+0x{o:X}' in all_fns]   # its body and its parts, each the same in both
    equal = [o for o in whole if runs_d[o] == runs_e[o]]
    check(len(equal) == len(whole), f'the line table: the same run in each of the {len(whole)} functions whose code is the same '
                                    f'({sum(len(runs_e[o]) for o in whole)} entries); {nd} entries in all, {ne} in the ELF pack')
    return dll


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--readobj', required=True)
    ap.add_argument('--objdump', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    sys.path.insert(0, TOOLS_TESTS)
    import aot_synth  # noqa: E402  (dm/tools/tests: the synthetic modules, as build_packs.sh writes them)
    aot_synth.write(os.path.join(a.work, 'module'))
    aot_synth.write_hostfn(os.path.join(a.work, 'hostfn'))
    exports = host_exports()
    print(f'the host exports {len(exports)} run-time entries to packs: {" ".join(sorted(exports))}')
    inputs = [('switch-gcc', os.path.join(CORPUS, 'switch-gcc.nro')), ('fp-gcc', os.path.join(CORPUS, 'fp-gcc.nro')),
              ('module', os.path.join(a.work, 'module', 'main.aot.tsv')), ('hostfn-main', os.path.join(a.work, 'hostfn', 'main.aot.tsv')),
              ('hostfn-sub', os.path.join(a.work, 'hostfn', 'sub.nro'))]
    for tag, module in inputs:
        dll = one(a, tag, module, exports)
        if dll and tag in ('switch-gcc', 'fp-gcc'):
            again = os.path.join(a.work, 'again', os.path.basename(dll))
            os.makedirs(os.path.dirname(again), exist_ok=True)
            r = subprocess.run([a.nxaot, 'build', module, '--target', 'windows', '--shared', again, '--workers', '1'], capture_output=True, text=True)
            check(r.returncode == 0 and open(again, 'rb').read() == open(dll, 'rb').read(), 'deterministic: the same DLL with one worker, in another folder')
    # A call of something else than the run time fails the pack, naming it
    print('fp-gcc --cpu x86-64 (no SSE4.1: libm\'s rounding functions):')
    bad = os.path.join(a.work, 'bad.dll')
    r = subprocess.run([a.nxaot, 'build', os.path.join(CORPUS, 'fp-gcc.nro'), '--target', 'windows', '--cpu', 'x86-64', '--shared', bad], capture_output=True, text=True)
    check(r.returncode != 0 and not os.path.exists(bad) and re.search(r'\[RED\] nxaot build: .*the pack calls ceil, ceilf, floor, floorf', r.stdout) is not None,
          'refused, naming the calls: ' + (re.search(r'the pack calls [^:]*', r.stdout).group(0) if 'the pack calls' in r.stdout else r.stdout[-300:]))
    r = subprocess.run([a.nxaot, 'build', os.path.join(CORPUS, 'fp-gcc.nro'), '--shared', os.path.join(a.work, 'x.dll')], capture_output=True, text=True)
    check(r.returncode == 2 and '--target windows' in r.stderr, 'a .dll without --target windows is a usage error')
    print(f'windows packs: {len(inputs)} modules packed as DLLs and checked against their ELF packs, {CHECKS[0]} checks, {len(FAILS)} failed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
