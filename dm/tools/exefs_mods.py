"""ExeFS code patches of mods, in the two formats emulators and Atmosphère load from a mod's exefs folder, read into (build ID, [(module offset, bytes)]).

- IPSwitch text (.pchtxt): @nsobid-<build ID>, @flag offset_shift <n>, @enabled / @disabled blocks, @stop, // comments and "<offset> <hex bytes>" entries. An entry
  plus the shift is an IPS offset, which counts the NSO's 0x100-byte header: the module offset is entry + shift - 0x100 (with the usual shift of 0x100, the entry).
  @flag print_values and debug_info change only how IPSwitch shows the file and are ignored. Anything else (string entries, other flags) is an error, so a mod is
  never applied half-understood.
- IPS (.ips, named <build ID>.ips): "PATCH" records of a 3-byte offset and a 2-byte size (size 0: a run, 2-byte count and 1 byte) up to "EOF"; IPS32 ("IPS32",
  4-byte offsets, up to "EEOF"). Offsets count the 0x100-byte NSO header, as above.

patch_nso.py applies the files whose build ID is the module's (the first 16 bytes compared, as a shorter ID in a file name is the start of the full one) before it
rewrites, so the rewriter handles a mod's code as the game's own (DM_EXEFS_PATCHES). scripts/build_ipa_from_nsp.py --mods collects the files from mod folders.
"""
import os
import re
import sys

HEADER = 0x100   # the NSO header, which IPS offsets count


class ModError(Exception):
    pass


def _bid(s):
    s = s.strip().lower()
    if not s or not re.fullmatch(r'[0-9a-f]+', s): raise ModError(f'not a build ID: {s!r}')
    return s[:32]


def parse_pchtxt(path):
    """(build ID, [(module offset, bytes)], [(line, entry)]) of an IPSwitch text file; entries in @disabled blocks are left out"""
    bid, shift, on, out, lines = None, 0, False, [], []
    with open(path, encoding='utf-8-sig') as f:
        for ln, raw in enumerate(f, 1):
            line = raw.split('//', 1)[0].strip()
            if not line: continue
            where = f'{path}:{ln}'
            if line.startswith('@nsobid-'): bid = _bid(line[8:]); continue
            if bid is None: raise ModError(f'{where}: entries before the @nsobid line')
            if line.startswith('@flag'):
                f_ = line.split()
                if len(f_) >= 3 and f_[1] == 'offset_shift': shift = int(f_[2], 0)
                elif f_[1:2] not in (['print_values'], ['debug_info']): raise ModError(f'{where}: unsupported flag: {line}')
                continue
            if line == '@enabled': on = True; continue
            if line == '@disabled': on = False; continue
            if line == '@stop': break
            if line.startswith('@'): raise ModError(f'{where}: unsupported directive: {line}')
            f_ = line.split()
            if len(f_) != 2 or not re.fullmatch(r'[0-9a-fA-F]+', f_[0]) or not re.fullmatch(r'(?:[0-9a-fA-F]{2})+', f_[1]):
                raise ModError(f'{where}: not an "<offset> <hex bytes>" entry: {line}')
            if not on: continue
            off = int(f_[0], 16) + shift - HEADER
            if off < 0: raise ModError(f'{where}: offset {f_[0]} lies in the NSO header')
            out.append((off, bytes.fromhex(f_[1]))); lines.append((ln, line))
    if bid is None: raise ModError(f'{path}: no @nsobid line')
    return bid, out, lines


def parse_ips(path):
    """(build ID, [(module offset, bytes)]) of an IPS or IPS32 file named <build ID>.ips"""
    bid = _bid(os.path.splitext(os.path.basename(path))[0])
    data = open(path, 'rb').read()
    if data.startswith(b'IPS32'): pos, width, end = 5, 4, b'EEOF'
    elif data.startswith(b'PATCH'): pos, width, end = 5, 3, b'EOF'
    else: raise ModError(f'{path}: neither IPS ("PATCH") nor IPS32')
    out = []
    while True:
        if data[pos:pos + len(end)] == end and pos + len(end) == len(data): break
        if pos + width + 2 > len(data): raise ModError(f'{path}: cut off at byte {pos}')
        off = int.from_bytes(data[pos:pos + width], 'big'); size = int.from_bytes(data[pos + width:pos + width + 2], 'big'); pos += width + 2
        if size: chunk = data[pos:pos + size]; pos += size
        else: n = int.from_bytes(data[pos:pos + 2], 'big'); chunk = data[pos + 2:pos + 3] * n; pos += 3
        if len(chunk) != (size or len(chunk)) or not chunk: raise ModError(f'{path}: a record at byte {pos} is cut off')
        if off < HEADER: raise ModError(f'{path}: a record at offset {off:#x} lies in the NSO header')
        out.append((off - HEADER, chunk))
    return bid, out


def parse(path):
    """(build ID, [(module offset, bytes)]) of a .pchtxt or .ips file"""
    if path.lower().endswith('.pchtxt'): bid, out, _ = parse_pchtxt(path); return bid, out
    if path.lower().endswith('.ips'): return parse_ips(path)
    raise ModError(f'{path}: neither .pchtxt nor .ips')


def matches(file_bid, module_bid):
    """a file's build ID names the module: the file may give fewer digits than the module's 32 (it is then their start)"""
    return module_bid.lower()[:len(file_bid)] == file_bid


if __name__ == '__main__':   # exefs_mods.py <file>…: what each file patches
    for p in sys.argv[1:]:
        try: b, e = parse(p); print(f'{p}: build {b}, {len(e)} entries, {sum(len(x) for _, x in e)} bytes')
        except ModError as x: sys.exit(f'[RED] {x}')
