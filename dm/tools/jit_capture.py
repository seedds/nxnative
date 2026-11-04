#!/usr/bin/env python3
"""Build the image a host that runs no code made at run time (iOS) serves nn::jit from, out of what the Mac host captured (hle_jit.c, DM_JIT_CAPTURE=<dir>).
Usage: jit_capture.py [--profile <title ID>] <capture dir>... <out dir>        writes <out dir>/jit.img, jit.tsv and jit.out
       jit_capture.py [--profile <title ID>] --check <capture dir>...           only reports what the captures hold and whether they agree
--profile (default DM_GAME) gives the profile key jit_key_data: per command, the data bytes that make a request ("4:0-7,24-27"; a command not listed: all its bytes). A game's
request struct may leave padding uninitialised (Super Mario 64's command 4), which differs from run to run; the key leaves such bytes out (jit.tsv keydata rows, which the host
applies when it serves). jit_out_code ("4:entries=4,count=0x20004,size=0x20000") says where a command's output lists the code's entry points: the game takes the code to be where it
asked, and a request the image holds elsewhere is served with those moved (jit.tsv outcode rows, hle_jit.c); a moved request of a command without one fails at serve time.

A capture holds one <key>.jit file per request (DMJ5): the request (its data with host words masked, and as it was), the layout it was made in (every module, the code, data and
transfer memory, with their bases), the code the plugin wrote, its data memory bytes, the output buffer and an input of up to 1 MB. The code depends on that layout: Super Mario 64's plugin builds every host address it uses (main's helpers and N64
state, the precompiled NRO's blocks, other code in the code memory) as movz xN, #0, lsl #48; movk lsl #32; movk lsl #16; movk, and nothing else in it depends on where it runs
(no x18, SVC, system register, adr / adrp or literal load: its census). So the image is position-independent instead:
  · the host maps the code memory and the modules in one canonical layout (jit.tsv mod rows: each module's offset from the code memory's base), and every address sequence that
    points into one of them becomes adrp xN; add xN, xN, #lo12; nop; nop to the same place in that layout (adrp reaches 4 GB);
  · requests keep their captured code offset when every capture agrees on it and nothing else is placed there (the boot commands); the others (blocks compiled in play, whose
    offsets follow the order they were met in) go after the rest, and their PC-relative branches out of the block and addresses into moved code are moved with them;
  · output buffers are served as captured, with the words that are host addresses relocated (jit.tsv reloc lists). Which words those are comes from two captures of the same
    request in different layouts (the words that differ, each by one region's base difference); a request captured once must hold no word that could be one.
jit.tsv: "layout <code size> <data size> <image size>", "mod <name> <offset> <size> <front patch>" and per request "req <key> <command> <code offset> <code size> <data offset>
<data size> <return value> <out offset in jit.out> <out size> <relocations>" (relocations: comma-separated <out offset>=<region>+<offset>, '-' for none)."""
import os, struct, sys
from collections import Counter, defaultdict

PAGE = 0x4000
NOP = 0xd503201f


def align(v, a=PAGE): return (v + a - 1) // a * a


def read(path):
    b = open(path, 'rb').read()
    if b[:4] != b'DMJ5': sys.exit(f'[RED] {path}: not a capture of this format (rewrite it with the current host)')
    cmd, = struct.unpack_from('<I', b, 4)
    c0o, c0s, insz, dsz = struct.unpack_from('<4Q', b, 8)
    o = 40; insha = b[o:o + 32]; o += 32; data = b[o:o + 32]; o += 32; raw = b[o:o + 32]; o += 32
    outsz, = struct.unpack_from('<Q', b, o); o += 8
    ret, = struct.unpack_from('<i', b, o); o += 4
    o0 = struct.unpack_from('<2Q', b, o); o += 16
    o1 = struct.unpack_from('<2Q', b, o); o += 16
    nr, = struct.unpack_from('<I', b, o); o += 4
    regs = {}
    for _ in range(nr):
        name = b[o:o + 16].split(b'\0')[0].decode(); base, size, pre = struct.unpack_from('<3Q', b, o + 16); regs[name] = (base, size, pre); o += 40
    code = b[o:o + o0[1]]; o += o0[1]
    ro = b[o:o + o1[1]]; o += o1[1]
    out = b[o:o + outsz]; o += outsz
    if len(out) != outsz: sys.exit(f'[RED] {path}: truncated')
    inp = b[o:o + insz] if insz <= 1 << 20 else None
    return dict(key=os.path.basename(path)[:-4], cmd=cmd, c0=(c0o, c0s), data=data, dsize=dsz, insha=insha, inp=inp, raw=raw, ret=ret, o0=o0, o1=o1, regs=regs, code=code, ro=ro, out=out, session=regs['jit_rx'][0])


def region_of(regs, v):
    for name, (base, size, pre) in regs.items():
        if base - pre <= v < base + size: return name, v - base
    return None, None


def seqs(code):
    """(position, register, value) of each movz #, lsl #48; movk lsl #32; movk lsl #16; movk on one register"""
    n = len(code) // 4; w = struct.unpack_from(f'<{n}I', code); out = []
    i = 0
    while i + 3 < n:
        a = w[i]
        if a & 0xffe00000 == 0xd2e00000:   # movz x, #imm, lsl #48
            rd = a & 31
            if w[i + 1] & 0xffe0001f == 0xf2c00000 | rd and w[i + 2] & 0xffe0001f == 0xf2a00000 | rd and w[i + 3] & 0xffe0001f == 0xf2800000 | rd:
                v = ((a >> 5) & 0xffff) << 48 | ((w[i + 1] >> 5) & 0xffff) << 32 | ((w[i + 2] >> 5) & 0xffff) << 16 | ((w[i + 3] >> 5) & 0xffff)
                out.append((i * 4, rd, v)); i += 4; continue
        i += 1
    return out


def branch_target(word, pos):
    """(kind, target) of a PC-relative instruction at pos, None for others; kind 'lit' for the forms the image does not move (adr / adrp / literal loads)"""
    if word & 0x7c000000 == 0x14000000: return 'b26', pos + (((word & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4
    if word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000: return 'b19', pos + ((((word >> 5) & 0x7ffff) ^ 0x40000) - 0x40000) * 4
    if word & 0x7e000000 == 0x36000000: return 'b14', pos + ((((word >> 5) & 0x3fff) ^ 0x2000) - 0x2000) * 4
    if word & 0x1f000000 == 0x10000000 or word & 0x3b000000 == 0x18000000: return 'lit', None
    return None


def set_branch(word, kind, disp):
    d = disp // 4
    if kind == 'b26':
        assert -(1 << 25) <= d < 1 << 25, 'branch out of reach'; return word & 0xfc000000 | d & 0x3ffffff
    if kind == 'b19':
        assert -(1 << 18) <= d < 1 << 18, 'branch out of reach'; return word & 0xff00001f | (d & 0x7ffff) << 5
    assert -(1 << 13) <= d < 1 << 13, 'branch out of reach'; return word & 0xfff8001f | (d & 0x3fff) << 5


def adrp_add(rd, pos, target):
    pages = (target >> 12) - (pos >> 12); assert -(1 << 20) <= pages < 1 << 20, 'adrp out of reach'
    adrp = 0x90000000 | (pages & 3) << 29 | ((pages >> 2) & 0x7ffff) << 5 | rd
    add = 0x91000000 | (target & 0xfff) << 10 | rd << 5 | rd
    return [adrp, add, NOP, NOP]


def key_masks(profile):
    """command → 32-byte mask from the profile key jit_key_data"""
    if not profile: return {}
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'games')); import game
    out = {}
    for item in game.load(profile).words('jit_key_data', ''):
        cmd, spans = item.split(':'); m = bytearray(32)
        for sp in spans.split(','):
            a, _, b = sp.partition('-'); a = int(a, 0); b = int(b or a, 0)
            for i in range(a, b + 1): m[i] = 0xff
        out[int(cmd)] = bytes(m)
    return out


def out_codes(profile):
    """command → (entries, count, size) offsets from the profile key jit_out_code"""
    if not profile: return {}
    import game
    out = {}
    for item in game.load(profile).words('jit_out_code', ''):
        cmd, kv = item.split(':'); d = dict(x.split('=') for x in kv.split(','))
        out[int(cmd)] = (int(d['entries'], 0), int(d['count'], 0), int(d['size'], 0))
    return out


def rekey(r, masks):   # as hle_jit.c jit_key: SHA-256 of the command, the data size, the masked data and the input's hash
    import hashlib
    d = r['data']   # host words masked by value, unless the profile names the request bytes: then those bytes as they are
    if r['cmd'] in masks: d = bytes((x if i < r['dsize'] else 0) & y for i, (x, y) in enumerate(zip(r['raw'], masks[r['cmd']])))
    return hashlib.sha256(struct.pack('<QQ', r['cmd'], r['dsize']) + d + r['insha']).hexdigest()[:32]


def load(dirs, masks):
    recs = defaultdict(list)
    for d in dirs:
        for f in sorted(os.listdir(d)):
            if f.endswith('.jit'): r = read(os.path.join(d, f)); r['key'] = rekey(r, masks); recs[r['key']].append(r)
    for k in recs:   # one record per session (a capture folder used twice adds nothing)
        seen = {}; recs[k] = [seen.setdefault(r['session'], r) for r in recs[k] if r['session'] not in seen]
    return recs


class Unplaced(AssertionError):   # a session's code refers to code memory that none of its own records holds
    def __init__(self, session, off): super().__init__(f'code offset {off:#x} of session {session:#x} lies in no captured request'); self.session = session


def build(dirs, out_dir=None, profile=None):
    masks = key_masks(profile); ocs = out_codes(profile)
    recs = load(dirs, masks)
    # The host skips a request already captured in the folder, so a session whose code memory is somewhere no earlier one had it (one that started after an in-process program
    # switch, DM_PROGRAM_SWITCH=inproc) may hold only its later requests, whose code calls into boot code it did not capture itself: it is left out, and the rest builds
    while True:
        if not recs: sys.exit('[RED] no captures')
        try: return build_from(recs, masks, ocs, out_dir)
        except Unplaced as e:
            n = sum(r['session'] == e.session for rl in recs.values() for r in rl)
            only = sum(all(r['session'] == e.session for r in rl) for rl in recs.values())
            print(f'[RED] capture session {e.session:#x} left out ({e}): its {n} requests, {only} of them captured in no other session', file=sys.stderr)
            recs = {k: v for k, v in ((k, [r for r in rl if r['session'] != e.session]) for k, rl in recs.items()) if v}


def build_from(recs, masks, ocs, out_dir):
    any_r = next(iter(recs.values()))[0]
    rx_size = any_r['regs']['jit_rx'][1]; ro_size = any_r['regs']['jit_ro'][1]
    # Canonical layout: the code memory at 0, then every module in load order (the order of the capture's regions), 16 KB aligned, a front patch area in front of its base
    mods, cur = {}, align(rx_size)
    for name, (base, size, pre) in any_r['regs'].items():
        if name.startswith('jit_'): continue
        off = align(cur + pre); mods[name] = (off, size, pre); cur = off + size
    for rl in recs.values():
        for r in rl:
            for name, (base, size, pre) in r['regs'].items():
                if not name.startswith('jit_') and (name not in mods or mods[name][1:] != (size, pre)): sys.exit(f"[RED] {r['key']}: module {name} is {size:#x} + {pre:#x} here, {mods.get(name)} elsewhere")
    # Placement: requests that every capture made at the same offset keep it, first come first served; the rest go after the end
    placed = []   # (image offset, size)
    where = {}    # key → image offset
    def free(o, n): return all(o + n <= p or p + m <= o for p, m in placed)
    order = sorted(recs, key=lambda k: (-len(recs[k]), recs[k][0]['o0'][0], k))
    for k in order:
        offs = {r['o0'][0] for r in recs[k]}; n = recs[k][0]['o0'][1]
        if len(offs) == 1 and free(next(iter(offs)), n) and len(recs[k]) == max(len(v) for v in recs.values()): where[k] = next(iter(offs)); placed.append((where[k], n))
    end = align(max((o + n for o, n in placed), default=0), 64)
    for k in order:
        if k in where: continue
        n = recs[k][0]['o0'][1]; where[k] = end; placed.append((end, n)); end = align(end + n, 64)
    if end > rx_size: sys.exit(f'[RED] the image ({end:#x} bytes) does not fit the code memory ({rx_size:#x})')
    # Captured code offset → image offset, per session
    spans = defaultdict(list)
    for k, rl in recs.items():
        for r in rl: spans[r['session']].append((r['o0'][0], r['o0'][1], where[k]))
    def map_rx(session, off):
        for c, n, i in spans[session]:
            if c <= off < c + n or (n == 0 and off == c): return i + off - c
        raise Unplaced(session, off)
    def canon(r, v):   # canonical offset of a host address of record r, or None for a constant
        name, off = region_of(r['regs'], v)
        if name is None: return None
        if name == 'jit_rx': return map_rx(r['session'], off)
        if name in mods: return mods[name][0] + off
        raise AssertionError(f"{r['key']}: an address into {name} (+{off:#x}), which the canonical layout does not hold")
    img = bytearray(end)
    stats = Counter(); disagree = []
    for k, rl in recs.items():
        r = rl[0]; pos0 = where[k]; cap0 = r['o0'][0]; code = r['code']
        w = list(struct.unpack_from(f'<{len(code) // 4}I', code))
        inseq = set()
        for p, rd, v in seqs(code):
            t = canon(r, v)
            for j in range(4): inseq.add(p // 4 + j)
            if t is None: stats['constants'] += 1; continue
            w[p // 4:p // 4 + 4] = adrp_add(rd, pos0 + p, t); stats['addresses'] += 1
        for i, word in enumerate(w):
            if i in inseq: continue
            bt = branch_target(word, cap0 + 4 * i)
            if not bt: continue
            kind, tgt = bt
            if kind == 'lit': raise AssertionError(f'{k}: a literal / adr form at +{4 * i:#x}, which the image does not move')
            if cap0 <= tgt < cap0 + len(code): continue   # inside the request: moves with it
            w[i] = set_branch(word, kind, map_rx(r['session'], tgt) - (pos0 + 4 * i)); stats['branches out'] += 1
        img[pos0:pos0 + len(code)] = struct.pack(f'<{len(w)}I', *w)
        for r2 in rl[1:]:   # the same request from another session must come out the same
            w2 = bytearray(struct.pack(f'<{len(w)}I', *w)); c2 = r2['code']
            if len(c2) != len(code): disagree.append(k); continue
            x = list(struct.unpack_from(f'<{len(c2) // 4}I', c2))
            for p, rd, v in seqs(c2):
                t = canon(r2, v)
                if t is not None: x[p // 4:p // 4 + 4] = adrp_add(rd, pos0 + p, t)
            for i, word in enumerate(x):
                bt = branch_target(word, r2['o0'][0] + 4 * i)
                if bt and bt[0] != 'lit' and not (r2['o0'][0] <= bt[1] < r2['o0'][0] + len(c2)): x[i] = set_branch(word, bt[0], map_rx(r2['session'], bt[1]) - (pos0 + 4 * i))
            if struct.pack(f'<{len(x)}I', *x) != bytes(w2): disagree.append(k)
    # Output buffers and their relocations
    rows, blob = [], bytearray()
    for k, rl in recs.items():
        r = rl[0]; out = r['out']; rel = []
        if len(rl) > 1:
            r2 = rl[1]; o2 = r2['out']
            for i in range(0, len(out) - 7, 8):
                a, b = struct.unpack_from('<Q', out, i)[0], struct.unpack_from('<Q', o2, i)[0]
                if a == b: continue
                name, off = region_of(r['regs'], a); name2, off2 = region_of(r2['regs'], b)
                if name is None or name != name2 or (name != 'jit_rx' and off != off2): raise AssertionError(f'{k}: output word +{i:#x} differs between captures ({a:#x}, {b:#x}) and is no host address in both')
                rel.append((i, 'jit_rx', map_rx(r['session'], off)) if name == 'jit_rx' else (i, name, off))
            for name in r['regs']:
                if r['regs'][name][0] == r2['regs'][name][0] and not name.startswith('jit_t'): print(f'[RED] {k}: {name} was at the same base in both captures; its addresses in the output cannot be told apart', file=sys.stderr)
        else:
            for i in range(0, len(out) - 7, 8):
                v = struct.unpack_from('<Q', out, i)[0]
                if v >> 32 and region_of(r['regs'], v)[0]: raise AssertionError(f'{k}: output word +{i:#x} ({v:#x}) may be a host address and the request was captured once: capture it again in another run')
        stats['relocations'] += len(rel)
        o = len(blob); blob += r['ro'] + out
        rl_s = ','.join(f'{i:x}={n}+{v:x}' for i, n, v in rel) or '-'
        rows.append(f"req\t{k}\t{r['cmd']}\t{where[k]:#x}\t{r['o0'][1]:#x}\t{r['o1'][0]:#x}\t{r['o1'][1]:#x}\t{r['ret']}\t{o:#x}\t{len(out):#x}\t{rl_s}")
    sessions = {r['session'] for rl in recs.values() for r in rl}
    print(f"{len(recs)} requests from {len(sessions)} captures: {dict(Counter(rl[0]['cmd'] for rl in recs.values()))} by command; image {end / 1048576:.1f} MB; "
          f"{stats['addresses']} address sequences made PC-relative, {stats['constants']} constants left, {stats['branches out']} branches moved, {stats['relocations']} output relocations; "
          f"{sum(len(rl) > 1 for rl in recs.values())} requests seen in more than one capture, {len(disagree)} of them different after relocation")
    if disagree: print('[RED] requests that differ between captures: ' + ' '.join(disagree), file=sys.stderr)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        open(os.path.join(out_dir, 'jit.img'), 'wb').write(bytes(img) + b'\0' * (align(end) - end))
        open(os.path.join(out_dir, 'jit.out'), 'wb').write(blob)
        with open(os.path.join(out_dir, 'jit.tsv'), 'w') as f:
            f.write(f'# nn::jit image (dm/tools/jit_capture.py) from {len(sessions)} captures\nlayout\t{rx_size:#x}\t{ro_size:#x}\t{align(end):#x}\n')
            for c, m in sorted(masks.items()): f.write(f'keydata\t{c}\t{m.hex()}\n')
            for c, (e, n, z) in sorted(ocs.items()): f.write(f'outcode\t{c}\t{e:#x}\t{n:#x}\t{z:#x}\n')
            for name, (off, size, pre) in mods.items(): f.write(f'mod\t{name}\t{off:#x}\t{size:#x}\t{pre:#x}\n')
            f.write('\n'.join(rows) + '\n')
    return 1 if disagree else 0


if __name__ == '__main__':
    a = sys.argv[1:]; prof = os.environ.get('DM_GAME')
    if a[:1] == ['--profile']: prof = a[1]; a = a[2:]
    if not a or a[0] in ('-h', '--help'): sys.exit(__doc__)
    if a[0] == '--check': sys.exit(build(a[1:], None, prof))
    if len(a) < 2: sys.exit(__doc__)
    sys.exit(build(a[:-1], a[-1], prof))
