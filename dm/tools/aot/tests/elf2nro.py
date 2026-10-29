#!/usr/bin/env python3
"""ELF to NRO for the corpus (dm/tools/aot/corpus), as devkitPro's elf2nro (switch-tools) writes one, and the symbol table the discovery test
compares with (tests/test_discover.py).

  elf2nro.py <in.elf> <out.nro> [--symbols <out.syms.tsv>]

The NRO: the first three PT_LOAD segments (text, read-only data, data) at their virtual addresses, each padded to 0x1000; bss from the data
segment's memory size past its padded file size, rounded up to 0x1000; the header (NRO0, version 0, total size, flags 0, the three segments, bss,
the GNU build ID's first 0x20 bytes) written at +0x10 over the text, where the program's crt0 leaves 0x70 bytes free; the header's last 0x20 bytes
zero, as elf2nro leaves them. build_corpus.sh compares this with devkitPro's elf2nro when it has one (--check-elf2nro).

The symbol table: every symbol of .symtab defined in an executable section, one row each: offset (hex), size (hex), type (func, notype, object),
binding (local, global, weak), name. Mapping symbols ($x code, $d data) are rows of type `map`, so the test can tell code from data in the text.
"""
import argparse
import struct
import sys

PT_LOAD = 1
SHT_SYMTAB, SHT_NOTE = 2, 7
SHF_EXECINSTR = 4
NT_GNU_BUILD_ID = 3
PAGE = 0x1000


def align(v, a=PAGE):
    return (v + a - 1) & ~(a - 1)


class Elf:
    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
            raise SystemExit('not a little-endian ELF64 file')
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, _, _, self.phentsize, self.phnum, self.shentsize, self.shnum,
         self.shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
        if self.machine != 183:
            raise SystemExit('not an AArch64 ELF file')
        self.data = data
        self.phdrs = [struct.unpack_from('<IIQQQQQQ', data, self.phoff + i * self.phentsize) for i in range(self.phnum)]
        self.shdrs = [struct.unpack_from('<IIQQQQIIQQ', data, self.shoff + i * self.shentsize) for i in range(self.shnum)]

    def section_name(self, sh):
        strtab = self.shdrs[self.shstrndx]
        o = strtab[4] + sh[0]
        return self.data[o:self.data.index(b'\0', o)].decode()


def nro_bytes(elf):
    loads = [p for p in elf.phdrs if p[0] == PT_LOAD]
    if len(loads) < 3:
        raise SystemExit('expected 3 loadable segments (text, rodata, data) and a bss')
    segs, file_off = [], 0
    for p in loads[:3]:
        _, _, offset, vaddr, _, filesz, memsz, _ = p
        size = align(filesz)
        segs.append((vaddr, size, elf.data[offset:offset + filesz]))
        file_off = align(file_off + size)
    data_filesz, data_memsz = loads[2][5], loads[2][6]
    bss = align(data_memsz - align(data_filesz)) if data_memsz > align(data_filesz) else 0
    build_id = b''
    for sh in elf.shdrs:
        if sh[1] == SHT_NOTE:
            namesz, descsz, ntype = struct.unpack_from('<III', elf.data, sh[4])
            name = elf.data[sh[4] + 12:sh[4] + 12 + namesz]
            if ntype == NT_GNU_BUILD_ID and namesz == 4 and name == b'GNU\0':
                build_id = elf.data[sh[4] + 16:sh[4] + 16 + descsz][:0x20]
    end = max(v + s for v, s, _ in segs)
    out = bytearray(end)  # elf2nro seeks to each segment's address and writes it: the file ends with the last one, gaps read as zeros
    for vaddr, size, body in segs:
        out[vaddr:vaddr + len(body)] = body
        out[vaddr + len(body):vaddr + size] = bytes(size - len(body))
    header = b'NRO0' + struct.pack('<III', 0, file_off, 0)  # the size field is the sum of the padded segments, as elf2nro counts it
    header += b''.join(struct.pack('<II', v, s) for v, s, _ in segs)
    header += struct.pack('<II', bss, 0) + build_id.ljust(0x20, b'\0') + bytes(0x20)
    assert len(header) == 0x70
    out[0x10:0x80] = header
    return bytes(out)


def symbols(elf):
    rows = []
    for sh in elf.shdrs:
        if sh[1] != SHT_SYMTAB:
            continue
        strtab = elf.shdrs[sh[6]]
        for i in range(sh[5] // 24):
            name_off, info, _, shndx, value, size = struct.unpack_from('<IBBHQQ', elf.data, sh[4] + 24 * i)
            if shndx == 0 or shndx >= 0xff00 or not elf.shdrs[shndx][2] & SHF_EXECINSTR:
                continue
            o = strtab[4] + name_off
            name = elf.data[o:elf.data.index(b'\0', o)].decode()
            stype, bind = info & 15, info >> 4
            if name.startswith('$x') or name.startswith('$d'):
                kind = 'map'
            else:
                kind = {0: 'notype', 1: 'object', 2: 'func'}.get(stype, f'type{stype}')
            rows.append((value, size, kind, {0: 'local', 1: 'global', 2: 'weak'}.get(bind, f'bind{bind}'), name))
    rows.sort(key=lambda r: (r[0], r[4]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('elf')
    ap.add_argument('nro')
    ap.add_argument('--symbols', help='write the symbol table of the executable sections as TSV')
    a = ap.parse_args()
    elf = Elf(open(a.elf, 'rb').read())
    with open(a.nro, 'wb') as f:
        f.write(nro_bytes(elf))
    if a.symbols:
        with open(a.symbols, 'w', newline='\n') as f:
            f.write('# offset\tsize\ttype\tbinding\tname\n')
            for value, size, kind, bind, name in symbols(elf):
                f.write(f'{value:x}\t{size:x}\t{kind}\t{bind}\t{name}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
