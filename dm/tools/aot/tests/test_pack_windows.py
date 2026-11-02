#!/usr/bin/env python3
"""Windows packs (CTest `pack_windows`; README.md "Windows packs", docs/design/aot-recompiler.md 9.4): three corpus NROs (switch-gcc: jump tables and
sw_word split into parts at the default limit; fp-gcc: the floating-point helpers and the shared NaN helpers of comdat groups; sjlj-gcc: tail calls
that pop their own stack arguments), the synthetic game module (gate sites, the aux region) and the host-function module's two
(dm/tools/tests/aot_synth.py), each packed twice by `nxaot build`: as the Linux shared
object and with `--target windows` as a DLL. Each DLL is checked with llvm-readobj and llvm-objdump and with the small PE and ELF readers below:

  - a PE32+ DLL for x86-64 (AMD64, IMAGE_FILE_DLL, dynamic base, NX), no entry point, no import descriptor or delay import (it names no module);
  - its exports are exactly nxaot_module, nxaot_imports and nxaot_positions (the sampler's positions: the ELF pack's tables, read through the export);
  - its descriptor (read through the export: ABI, flags, name, SHA-256, sizes, entry, FPCR, the standard entries' signature, the counts) is the ELF
    pack's, and so are its tables' module-side columns (function map: offsets and sizes; lookup table: offsets and flags; returns; gate sites);
  - nxaot_imports names exactly the ELF pack's undefined symbols, each a run-time entry the host exports (DM_AOT_EXPORT in dm/host/aot_run.c,
    FPEXPORT in aot_fp.c), each slot 8 zero bytes in a writable section, each entry's thunk in the code a `jmp *` through its slot;
  - every pointer of the descriptor, of nxaot_imports and of nxaot_positions has a base relocation (DIR64), so the DLL loads at any base;
  - `nxaot lines` on the DLL: no bad entry, as many entries as on the ELF pack;
  - the code: every function (bodies, standard entries, parts, the tables' code) disassembled in both packs and compared instruction by instruction,
    the operands that depend on the layout (rip-relative displacements, branch targets outside the function) normalized and the raw bytes compared
    where no such operand is: every function that does not call a run-time entry directly is identical; the ones that differ are listed with
    their first difference;
  - the unwind information (.pdata, .xdata; src/build/coff.cpp, "Unwind information"): `llvm-readobj --unwind` reads every RUNTIME_FUNCTION
    without a warning, as many as the exception directory holds and nxaot's log counts; every UNWIND_INFO parses (version 1, no handler, each
    code's slots inside CountOfCodes, CodeOffsets in order inside SizeOfProlog); the entries are sorted and disjoint and tile every function
    nxaot described from its start to its last instruction's end, the function map's bodies among them; and at every call of a function whose
    code is the shared object's, the codes of the entry holding the return address, run as RtlVirtualUnwind runs them, give the return address
    and the saved registers the shared object's CFI gives at the call;
  - the DLL is deterministic: the same bytes with one worker and with eight;
and a pack that would call something else than the run time fails: fp-gcc for the baseline x86-64 CPU (no SSE4.1) calls libm's ceil and floor, which
the build must refuse, naming them.

  test_pack_windows.py --nxaot <nxaot> --readobj <llvm-readobj> --objdump <llvm-objdump> [--dwarfdump <llvm-dwarfdump>] --work <dir>

Exit 0 when every check holds.
"""
import argparse
import bisect
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


def positions_bad(runs, marks, data, stride, fns, rows):
    """The sampler's positions (include/nxaot/aot_module.h, "The sampler's positions") decoded independently of the host's reader: runs [(at, rows,
    first)], parallel to fns [(entry, size)]. What is wrong, or '' when every run's rows decode from its bytes up to the next run's, its marks are its own,
    its native addresses rise, every offset is none or an instruction of its function, and the rows add up to rows"""
    NONE, total, ends = 0xFFFFFFFF, 0, [at for at, _, _ in runs[1:]] + [len(data)]

    def leb(i, signed):
        v = s = 0
        while True:
            c = data[i]
            i += 1
            v |= (c & 0x7F) << s
            s += 7
            if not c & 0x80:
                return (v - (1 << s) if signed and c & 0x40 else v), i
    for k, ((at, n, first), (entry, size)) in enumerate(zip(runs, fns)):
        total += n
        i, pos, last, prev = at, 0, entry, -1
        try:
            for r in range(n):
                if r and r % stride == 0:
                    native, off, mat = struct.unpack_from('<III', marks, 12 * (first + r // stride - 1))
                    if mat != i:
                        return f'function {entry:#x}: mark {r // stride} at byte {mat}, its rows end at {i}'
                    pos, last = native + 1, off if off != NONE else entry
                else:
                    c = data[i]
                    i += 1
                    if c < 0x80:
                        d, off = (c & 15) + 1, last + 4 * ((c >> 4) - 4)
                    elif c >= 0xC0:
                        d, off = (c & 0x3F) + 1, NONE
                    elif c >= 0x82:
                        d, off = data[i] + 1, last + 4 * (c - 0x82 - 24)
                        i += 1
                    elif c == 0x80:
                        d, i = leb(i, False)
                        e, i = leb(i, True)
                        off = last + 4 * e
                    else:
                        d, i = leb(i, False)
                        off = NONE
                    pos += d
                native = pos - 1
                if native <= prev or (off != NONE and not entry <= off < entry + size):
                    return f'function {entry:#x}: row {r} at +{native:#x}, offset {off:#x}'
                prev = native
                if off != NONE:
                    last = off
        except (IndexError, struct.error):
            return f'function {entry:#x}: its rows run past the table'
        if i != ends[k]:
            return f'function {entry:#x}: its rows end at byte {i}, the next run starts at {ends[k]}'
    return '' if total == rows else f'{total} rows in the runs, {rows} in the header'


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


# ---- Unwind information (.pdata, .xdata; Microsoft's "x64 exception handling") ----
UWOP = {0: 'PUSH_NONVOL', 1: 'ALLOC_LARGE', 2: 'ALLOC_SMALL', 3: 'SET_FPREG', 4: 'SAVE_NONVOL', 5: 'SAVE_NONVOL_FAR', 8: 'SAVE_XMM128', 9: 'SAVE_XMM128_FAR'}
GPR = ['RAX', 'RCX', 'RDX', 'RBX', 'RSP', 'RBP', 'RSI', 'RDI'] + [f'R{i}' for i in range(8, 16)]


def parse_unwind_info(pe, rva):
    """An UNWIND_INFO: (SizeOfProlog, FrameRegister, [(CodeOffset, operation, register, operand)] in array order); ValueError when it does not
    parse as version 1 without handler or chained information, with every code's slots inside CountOfCodes and the offsets in order"""
    head = pe.at(rva, 4)
    version, flags, prolog, count, frame = head[0] & 7, head[0] >> 3, head[1], head[2], head[3]
    if version != 1 or flags:
        raise ValueError(f'version {version}, flags {flags:#x}')
    raw = pe.at(rva + 4, 2 * count)
    slot = lambda k: struct.unpack_from('<H', raw, 2 * k)[0]
    codes, k, last = [], 0, 255
    while k < count:
        off, op, info = raw[2 * k], raw[2 * k + 1] & 15, raw[2 * k + 1] >> 4
        if op not in UWOP or (op == 1 and info > 1):
            raise ValueError(f'operation {op} info {info}')
        size = {1: 2 + info, 4: 2, 5: 3, 8: 2, 9: 3}.get(op, 1)
        if k + size > count:
            raise ValueError(f'{UWOP[op]} past CountOfCodes')
        if off > prolog or off > last:
            raise ValueError(f'CodeOffset {off:#x} after SizeOfProlog {prolog:#x} or out of order')
        last = off
        if op in (0, 3):
            operand = None
        elif op == 2:
            operand = info * 8 + 8
        elif size == 3:   # ALLOC_LARGE with info 1, the _FAR saves: 32 bits, unscaled
            operand = slot(k + 1) | slot(k + 2) << 16
        else:             # ALLOC_LARGE with info 0, SAVE_NONVOL: scaled by 8; SAVE_XMM128: by 16
            operand = slot(k + 1) * (16 if op == 8 else 8)
        codes.append((off, op, f'XMM{info}' if op in (8, 9) else GPR[info], operand))
        k += size
    if (frame & 15) != 0 and not any(op == 3 for _, op, _, _ in codes):
        raise ValueError('a frame register without UWOP_SET_FPREG')
    return prolog, frame & 15, codes


def win_unwind(codes, pc_off):
    """What RtlVirtualUnwind does with an entry's codes at pc_off from its start (outside an epilogue, no frame register): the return address's
    distance above rsp, and each restored register's slot as a distance above rsp"""
    delta, saves = 0, {}
    for off, op, reg, operand in codes:
        if off > pc_off:   # inside the prologue: not done yet
            continue
        if op == 0:
            saves[reg] = delta
            delta += 8
        elif op in (1, 2):
            delta += operand
        elif op in (4, 5, 8, 9):
            saves[reg] = operand   # from the frame's base: rsp where the unwinding starts
        else:
            return None, None
    return delta, saves


def code_map(objdump, path):
    """Every function's start and instructions [(address, length, text)], padding after it dropped"""
    out = subprocess.run([objdump, '-d', path], capture_output=True, text=True, check=True).stdout
    fns, cur = {}, None
    for line in out.splitlines():
        m = re.match(r'^([0-9a-f]+) <(.+)>:$', line)
        if m:
            cur = m.group(2)
            fns[cur] = (int(m.group(1), 16), [])
            continue
        m = re.match(r'^\s*([0-9a-f]+):((?: [0-9a-f]{2})+)\s*(?:\t(.*))?$', line)
        if cur is None or not m:
            continue
        addr, n, text = int(m.group(1), 16), len(m.group(2).split()), (m.group(3) or '').strip()
        insns = fns[cur][1]
        if not text and insns:   # the rest of a long instruction's bytes
            a0, n0, t0 = insns[-1]
            insns[-1] = (a0, n0 + n, t0)
        else:
            insns.append((addr, n, text))
    for name, (_, insns) in fns.items():
        while insns and (insns[-1][2] == 'int3' or insns[-1][2].startswith('nop')):
            insns.pop()
    return fns


def unwind_checks(a, tag, pe, dll, so, d, fn_d, same, log):
    """The DLL's .pdata and .xdata: llvm-readobj --unwind reads every entry; each parses; the entries are sorted, disjoint and tile each function
    with unwind information from its start to its end (the bodies of the function map among them); nxaot's log counts them; and at every call of
    a function whose code is the shared object's, what RtlVirtualUnwind does with the codes (return address and saved registers) is what the
    shared object's CFI says"""
    m = re.search(r'aot: windows: unwind info for (\d+) of (\d+) functions, (\d+) not describable: ([^;]*)(?:; (\d+) without CFI[^;]*)?; (\d+) RUNTIME_FUNCTION entries', log)
    if not check(m is not None, 'nxaot build logs the unwind information (aot: windows: unwind info for N of M functions, ...)'):
        return
    described, total, missing, entries_logged = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(6))
    print('    ' + next(line for line in log.splitlines() if line.startswith('aot: windows: unwind info')))
    rva, size = pe.dirs[3]
    pdata = [struct.unpack('<III', pe.at(rva + 12 * k, 12)) for k in range(size // 12)]
    ro = subprocess.run([a.readobj, '--unwind', dll], capture_output=True, text=True)
    n_ro = ro.stdout.count('RuntimeFunction {')
    check(ro.returncode == 0 and 'warning' not in (ro.stdout + ro.stderr).lower() and 'error' not in (ro.stdout + ro.stderr).lower()
          and n_ro == len(pdata) == entries_logged and size == 12 * len(pdata),
          f'llvm-readobj --unwind reads {n_ro} RUNTIME_FUNCTIONs, without a warning: the exception directory\'s {len(pdata)}, the log\'s {entries_logged}')
    infos, bad = {}, []
    for b, e, u in pdata:
        try:
            infos[u] = infos.get(u) or parse_unwind_info(pe, u)
        except ValueError as ex:
            bad.append(f'{b:#x}: {ex}')
    check(not bad, f'every UNWIND_INFO parses ({len(infos)} distinct, version 1, codes inside their count and in order)' + (f': {bad[:3]}' if bad else ''))
    ordered = all(b < e for b, e, _ in pdata) and all(pdata[k][1] <= pdata[k + 1][0] for k in range(len(pdata) - 1))
    check(ordered, 'the entries are sorted and disjoint')
    # Each function's entries: contiguous from its start to its last instruction's end
    fns = code_map(a.objdump, dll)
    by_begin = {b: (e, u) for b, e, u in pdata}
    covered, partial = 0, []
    starts = {}
    for name, (start, insns) in fns.items():
        if not insns:
            continue
        rs, end = start - pe.image_base, insns[-1][0] + insns[-1][1] - pe.image_base
        starts[rs] = (name, end)
        if rs not in by_begin:
            continue
        at = rs
        while at in by_begin and at < end:
            at = by_begin[at][0]
        if at == end:
            covered += 1
        else:
            partial.append(f'{name} [{rs:#x}, {end:#x}): entries end at {at:#x}')
    check(not partial and covered == described, f'{covered} functions covered by their entries from their start to their last instruction\'s end, '
                                               f'as many as nxaot described ({described} of {total})' + (f': {partial[:3]}' if partial else ''))
    bodies = [d['fns'] + 16 * k + 8 + body - pe.image_base for k, (_, body, _) in enumerate(fn_d)]
    without = [hex(b) for b in bodies if b not in by_begin]
    check(len(without) <= missing and all(b in starts for b in bodies),
          f'the function map\'s {len(bodies)} bodies: each a function, each with its entries but for at most the {missing} nxaot could not describe'
          + (f' (without: {without[:5]})' if without else ''))
    # The calls of the functions whose code is the shared object's: the codes against the CFI
    rows = []   # (address, CFA offset, {register: slot})
    dump = subprocess.run([a.dwarfdump, '--eh-frame', so], capture_output=True, text=True, check=True).stdout
    for line in dump.splitlines():
        r = re.match(r'^\s+0x([0-9a-f]+): CFA=RSP\+(\d+): ?(.*)$', line)
        if r:
            saves = {reg: -int(o) for reg, o in re.findall(r'(\w+)=\[CFA-(\d+)\]', r.group(3))}
            rows.append((int(r.group(1), 16), int(r.group(2)), saves))
    rows.sort(key=lambda x: x[0])
    keys = [x[0] for x in rows]
    elf_fns = code_map(a.objdump, so)
    ordered_b = sorted(by_begin)
    calls = agree = last_calls = 0
    wrong = []
    for name in sorted(same):
        if name not in elf_fns or name not in fns:
            continue
        e_start, e_insns = elf_fns[name]
        w_start, w_insns = fns[name]
        for (ea, en, et), (wa, wn, wt) in zip(e_insns, w_insns):
            if not et.startswith('call'):
                continue
            # The CFI's row at the call (the state while the callee runs: a callee that pops its arguments has not yet), and the entry that holds
            # the return address, where RtlVirtualUnwind continues from the callee
            ret_w = wa + wn - pe.image_base
            k = bisect.bisect_right(keys, ea) - 1
            if k < 0:
                continue
            cfa, saves = rows[k][1], rows[k][2]
            j = bisect.bisect_right(ordered_b, ret_w) - 1
            if j < 0 or not (ordered_b[j] <= ret_w < by_begin[ordered_b[j]][0]):
                if ret_w == w_insns[-1][0] + w_insns[-1][1] - pe.image_base:
                    last_calls += 1   # a call that does not return, the function's last instruction: its return address is the next function's
                else:
                    wrong.append(f'{name}+{ret_w - (w_start - pe.image_base):#x}: no entry')
                continue
            _, _, codes = infos[by_begin[ordered_b[j]][1]]
            delta, wsaves = win_unwind(codes, ret_w - ordered_b[j])
            calls += 1
            want = {reg: s for reg, s in saves.items() if reg != 'RIP' and s >= -cfa and not reg.startswith('XMM')}
            got = {reg: pos - (delta + 8) for reg, pos in (wsaves or {}).items() if not reg.startswith('XMM')}
            if delta is not None and delta + 8 == cfa and want == got:
                agree += 1
            else:
                wrong.append(f'{name}+{ret_w - (w_start - pe.image_base):#x}: CFI CFA rsp+{cfa} {want}, codes rsp+{None if delta is None else delta + 8} {got}')
    check(calls > 0 and agree == calls, f'at each of the {calls} calls of the {len(same)} functions the shared object has the same code, the codes at the '
                                        f'return address give the CFI\'s return address and saved registers at the call'
                                        + (f' ({last_calls} more end their function: no return)' if last_calls else '') + (f': {wrong[:3]}' if wrong else ''))


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
    check(sorted(ex) == ['nxaot_imports', 'nxaot_module', 'nxaot_positions'] and re.findall(r'Name: (\S+)', ro) == sorted(ex),
          f'exports exactly nxaot_imports, nxaot_module and nxaot_positions (llvm-readobj: {re.findall(r"Name: (\S+)", ro)})')
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
    # The sampler's positions (include/nxaot/aot_module.h nxaot_positions), read through their export: whole and in range (the DLL's code is not the ELF pack's
    # everywhere, below, so neither are its positions)
    pv, pst, pfn, p_runs, p_marks, p_bytes, p_rows, p_nm, p_nb = struct.unpack('<IIQQQQQQQ', pe.at(ex['nxaot_positions'], 64))
    runs = pe.va(p_runs, 12 * pfn)
    bad = positions_bad([struct.unpack_from('<III', runs, 12 * k) for k in range(pfn)], pe.va(p_marks, 12 * p_nm) if p_nm else b'',
                        pe.va(p_bytes, p_nb) if p_nb else b'', pst, [(o, s) for o, _, s in fn_d], p_rows) if pv == 1 and pst and pfn == d['fn_count'] else 'its header'
    check(not bad, f'nxaot_positions (version {pv}, {p_rows} rows for {pfn} functions, {p_nm} marks, {p_nb} bytes): every run decodes, its rows in order, every '
                   f'offset in its function' + (f': {bad}' if bad else ''))
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
    want += [ex['nxaot_positions'] + o for o in (16, 24, 32)]   # runs, marks, bytes
    check(all(w in relocs for w in want), f'a base relocation (DIR64) at each of the {len(want)} pointers of the descriptor, of nxaot_imports and of '
                                           f'nxaot_positions ({len(relocs)} in all)')
    # The line table: nxaot's own reader on both, then per function: the same run wherever the code is the same
    rd, nd, bd = lines_of(a.nxaot, dll, module)
    re_, ne, be = lines_of(a.nxaot, so, module)
    check(rd == 0 and bd == 0 and nd > 0 and re_ == 0 and be == 0, f'nxaot lines on the DLL: {nd} entries, {bd} bad (the ELF pack: {ne}, {be} bad)')
    same, all_fns = compare_code(a.objdump, so, dll, names, tag)
    unwind_checks(a, tag, pe, dll, so, d, fn_d, same, log)
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
    ap.add_argument('--dwarfdump', help='llvm-dwarfdump (default: the one beside --readobj)')
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    a.dwarfdump = a.dwarfdump or os.path.join(os.path.dirname(a.readobj), os.path.basename(a.readobj).replace('readobj', 'dwarfdump'))
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
              ('hostfn-sub', os.path.join(a.work, 'hostfn', 'sub.nro')), ('sjlj-gcc', os.path.join(CORPUS, 'sjlj-gcc.nro'))]
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
