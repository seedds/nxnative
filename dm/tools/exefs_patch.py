#!/usr/bin/env python3
"""ExeFS patches of mods, applied when a code module is rewritten (patch_nso.py), before the rewrite (docs/reference/rewriter.md, Code patches of mods
rewritten with the module). The formats and the folder rules are dm/host/hle_exefs.c's, which applies the patches that were not baked in at load:

  DM_EXEFS_PATCHES=<dir>:<dir>:...   the exefs folders of the mods that are on, first mod first (Cartridge's mods.txt order)
  - IPS ("PATCH" ... "EOF") and IPS32 ("IPS32" ... "EEOF"): offsets count from the NSO header (image offset = offset - 0x100); the file is named after the module's
    build ID in hex (at least the first 8 bytes)
  - pchtxt: "@nsobid-<build ID>" names the module, "@flag offset_shift <n>" is added to every offset, "@little-endian" / "@big-endian", "@enabled" /
    "@disabled" groups, "@stop"; "<hex offset> <hex bytes>" or "<hex offset> \"<string>\"", "//" comments
  - a folder that also holds a module (main, subsdk0-9, sdk, rtld) or main.npdm is skipped whole: the mod adds code that is not loaded
  - a file that would write past the image, into a registered cut, or over a byte an earlier file wrote differently is skipped whole

Why before the rewrite: the rewriter moves every use of x18 to a thread slot, because the Apple kernel may change x18 at any instruction, and rewrites
system calls, timer reads, exclusive accesses and the rest (patch_nso.py's header). Code a mod adds after the rewrite keeps them as they are: Ocarina of
Time's Fast Travel kept a glyph's height in w18 in its drawing loop, and letters lost all but their top rows. The bytes a patch writes are only code
where the code reaches them (code_words): a mod may keep data in the padding between functions too, and those words must stay as they are."""
import os, re, struct

HEADER = 0x100   # an NSO header: IPS offsets count from it
MODULE_FILES = {'main', 'main.npdm', 'sdk', 'rtld'} | {f'subsdk{i}' for i in range(10)}


def parse_ips(d):
    """The records (image offset, bytes) of an IPS or IPS32 file; None when it is not one or is cut short"""
    if d[:5] == b'IPS32': w, i = 4, 5
    elif d[:5] == b'PATCH': w, i = 3, 5
    else: return None
    out = []
    while True:
        if i + w > len(d): return None
        if (w == 3 and d[i:i + 3] == b'EOF') or (w == 4 and d[i:i + 4] == b'EEOF'): return out
        off = int.from_bytes(d[i:i + w], 'big'); i += w
        if i + 2 > len(d): return None
        n = int.from_bytes(d[i:i + 2], 'big'); i += 2
        if n:
            if i + n > len(d): return None
            b = d[i:i + n]; i += n
        else:   # a run
            if i + 3 > len(d): return None
            b = bytes([d[i + 2]]) * int.from_bytes(d[i:i + 2], 'big'); i += 3
        if off >= HEADER and b: out.append((off - HEADER, b))


def parse_pchtxt(text):
    """(build ID named, records of the enabled groups); records None when a patch line cannot be read"""
    shift, le, on, bid, out = 0, False, False, '', []
    for raw in text.split('\n'):
        l = raw.strip(' \t\r')
        if not l or l.startswith(('//', '#')): continue
        if l.startswith('@'):
            low = l.lower()
            if low.startswith('@stop'): break
            if low.startswith('@nsobid-'): bid = re.findall(r'[0-9A-Fa-f]*', l[8:])[0]
            elif low.startswith('@flag'):
                p = l[5:].split()
                if len(p) >= 2 and p[0].lower() == 'offset_shift':
                    try: shift = int(p[1], 0)
                    except ValueError: pass
            elif low.startswith('@little-endian'): le = True
            elif low.startswith('@big-endian'): le = False
            elif low.startswith('@enabled'): on = True
            elif low.startswith('@disabled'): on = False
            continue
        if not on: continue
        m = re.match(r'([0-9A-Fa-f]+)[ \t]+(.*)$', l)
        if not m: return bid, None
        off, rest = int(m.group(1), 16), m.group(2)
        if rest.startswith('"'):
            b, k = bytearray(), 1
            while k < len(rest) and rest[k] != '"':
                c = rest[k]
                if c == '\\' and k + 1 < len(rest):
                    k += 1; c = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0'}.get(rest[k], rest[k])
                b += c.encode('latin1'); k += 1
            if k >= len(rest): return bid, None
            b = bytes(b)
        else:
            h = re.match(r'([0-9A-Fa-f]+)[ \t]*(//.*)?$', rest)
            if not h or len(h.group(1)) % 2: return bid, None
            b = bytes.fromhex(h.group(1))
            if le: b = b[::-1]
        at = off + shift - HEADER
        if at >= 0 and b: out.append((at, b))
    return bid, out


def bid_match(file_hex, module_hex):
    """A build ID written in a file (a prefix of at least 16 hex digits, any case) names the module's"""
    return len(file_hex) >= 16 and len(module_hex) >= 16 and file_hex[:len(module_hex)].lower() == module_hex[:len(file_hex)].lower()


def fnv64(d):
    h = 0xcbf29ce484222325
    for c in d: h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f'{h:016x}'


def files_for(dirs, module_bid):
    """[(path, fnv64 of the file, records)] of the patch files in the folders that name this module's build, in the host's order
    (folders as given, files by name); plus [(path, why)] for those skipped"""
    found, skipped = [], []
    for d in dirs:
        try: names = sorted(n for n in os.listdir(d) if not n.startswith('.'))
        except OSError: skipped.append((d, 'no such folder')); continue
        mod = next((n for n in names if n.lower() in MODULE_FILES), None)
        if mod: skipped.append((d, f'holds a module ({mod}): the mod adds code this host does not load')); continue
        for n in names:
            p = os.path.join(d, n); low = n.lower()
            if not low.endswith(('.pchtxt', '.ips')): continue
            with open(p, 'rb') as f: data = f.read()
            if low.endswith('.pchtxt'):
                bid, recs = parse_pchtxt(data.decode('utf-8', 'replace'))
            else:
                bid, recs = re.findall(r'[0-9A-Fa-f]*', n[:-4])[0], parse_ips(data)
            if not bid_match(bid, module_bid): continue
            if recs is None: skipped.append((p, 'not a valid patch file')); continue
            found.append((p, fnv64(data), recs))
    return found, skipped


def apply(img, text_end, image_len, files, keep_out):
    """Writes the files' records into img (a bytearray of the module image), each file whole or not at all. keep_out: [(offset, length)] of bytes no
    patch may change (the registered cuts). Returns (applied [(path, fnv64, records)], skipped [(path, why)], written: set of byte offsets)"""
    applied, skipped, written = [], [], {}
    for path, h, recs in files:
        why = None
        for off, b in recs:
            if off + len(b) > image_len: why = f'writes +{off:#x}, past the module\'s image'; break
            hit = next((c for c, n in keep_out if off < c + n and c < off + len(b)), None)
            if hit is not None: why = f'writes +{off:#x}, which a registered cut of the rewriter changes (+{hit:#x})'; break
            bad = next((off + k for k, v in enumerate(b) if written.get(off + k, v) != v), None)
            if bad is not None: why = f'writes +{bad:#x}, which a mod higher in the list already patched differently'; break
        if why: skipped.append((path, why)); continue
        for off, b in recs:
            img[off:off + len(b)] = b
            for k, v in enumerate(b): written[off + k] = v
        applied.append((path, h, recs))
    return applied, skipped, set(written)


TERMINAL = {'b', 'br', 'braa', 'brab', 'braaz', 'brabz', 'ret', 'retaa', 'retab', 'brk', 'udf', 'eret', 'hlt'}


def code_words(img, orig, written, text_end, md):
    """The patched words in the text that are code: reached from a word the patch wrote over an instruction of the module (a hook site), or from a
    function entry it wrote (pacibsp, stp x29, x30, [sp, #-n]!: functions it calls through pointers), by fall-through and direct branches, without
    leaving the patched words. The rest of the patched words are data the mod keeps between functions, which the rewrite must leave as they are.
    Fast Travel 1.4.0: 13,621 code words and 41,931 data words (its packed state chain), no data word taken for code, every x18 use reached"""
    pw = {a & ~3 for a in written if a < text_end}
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    seeds = [a for a in pw if struct.unpack_from('<I', orig, a)[0] not in (0, 0xD503201F)]
    seeds += [a for a in pw if word(a) == 0xD503237F or (word(a) & 0xFFC07FFF) == 0xA9807BFD]
    code, todo = set(), seeds
    while todo:
        a = todo.pop()
        if a in code or a not in pw: continue
        i = next(md.disasm(bytes(img[a:a + 4]), a), None)
        if i is None: continue
        code.add(a)
        m = i.mnemonic
        if m in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            t = i.op_str.rsplit('#', 1)
            if len(t) == 2:
                try: todo.append(int(t[1], 16))
                except ValueError: pass
        if m not in TERMINAL: todo.append(a + 4)
    return code, pw - code


def data_ranges(words):
    """Sorted, merged [(start, end)] byte ranges of a set of word offsets"""
    out = []
    for a in sorted(words):
        if out and out[-1][1] == a: out[-1][1] = a + 4
        else: out.append([a, a + 4])
    return [tuple(r) for r in out]
