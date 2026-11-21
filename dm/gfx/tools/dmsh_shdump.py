#!/usr/bin/env python3
# The run-time half of a shader package: a DM_NVN_SHDUMP directory → the map rows no scan can write, and the records dmsh_build.py reads. Keys and provenance only; the bytes stay on the machine that dumped them.
# What is dumped (both writers are in this tree, both end the name with the key they computed):
#   · hle_nvn.c, in nvnProgramSetShaders where the key is computed: SetShaders-<key>.ctl (the first 0x880 bytes of the control section) and SetShaders-<key>.code (the guest address
#     the key hashes, code area length from control +0x704). This is the copy whose bytes are the ones the key was made of. LEGO Harry Potter's launcher shaders are built in RAM by its
#     engine, so their control sections are only valid during the call; a copy read at draw time names no map row (dm/games/010052A00B5D2000).
#   · nvn_mtl.m, prog_get, for a program missing from the map: <key>.ctl and <key>.code, the same two bytes read at first draw.
# The key is the host's own rule (keys_hash in hle_nvn.c, skey in shader_split.py, the map's column 4): sha256(control[0:nctl] + code)[:16], nctl = control +0x20 when that says
# 0x760..0x87f, else 0x880 — this tool recomputes it from the pair on disk and rejects a pair whose key is not the one in the file name, so a dump of freed memory, a short write or a
# file renamed by hand cannot put a row in the map. The profile's shader_key_skip is applied as the split and the host apply it (shader_split.key_ctl).
# Rows: the columns of the split's shader_map.tsv (package, index, stage, dm_ctl_sha16, source). Stage is glslc_stage's: the code area's own header (SPH word 0, bits 10..13, a type 0
# read as a vertex shader), compute when the code has no header and control +0x748 has a thread group. Package is '<stage>_<group>.bin' with the group the profile's shader_groups gives
# the source label (shader_split.group), so a dump whose container is unknown is named after no container: 'other', as Need for Speed: Hot Pursuit Remastered's SHADERS.BNDL rows are.
# A profile that wants its own name for the run-time half adds a group for the label ('/shdump/=shdump' → ps_shdump.bin).
# Source (column 5) is /shdump/<key>.ctl#SHDUMP@0x0: the shape every other row has, <label>#<reader>@<offset> (Portal's '/glslc_capture/<key>.glslc#GLSLC@0x0#var'), with the dump's own
# file name as the label — it is the one place those bytes existed — and the offset the file holds them at (0, the whole file). The dump's shaders are single-stage programs, and the
# #SHDUMP@ marker keeps them out of the sources shader_split.separable_pairs pairs by their container (SINGLE_STAGE); a split that reads a dump itself adds the marker there.
# Merge: rows are appended to the map it is given and keys already in it are left alone, so running it twice adds nothing and rewrites nothing. Each new row's index is the next free
# record of its package, taken from the package's own file (the records the split wrote: 'BTCDBTCD', control length at +0x28, code length at +0x2c, 0x38 bytes of header, then the code
# area and the control section, one copy per key); the .bin is appended to, never rewritten. The map must be one the split wrote (its three header lines), since its column order and
# its sorting are the split's.
# Usage: dmsh_shdump.py <DM_NVN_SHDUMP dir> <package dir> <shader map>   Environment: DM_GAME (required), as for shader_split.py
#   The package dir is the split's (it holds vs_nro.bin / ps_nro.bin …) and must be outside the repository: game bytes are not committed (CONTRIBUTING.md, "Never game files or keys").
#   The map is written as it was read plus the new rows; it is the file the split writes and then this tool merges into, so a split run after it rewrites the map and must be followed
#   by this tool again. A compute shader from a dump also needs its answer-table row in shader_facts.tsv (thread group, shared memory): the line to add is printed.
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_split as split   # ctl_len, key_ctl, skey, glslc_stage, group, STAGE_NAME, CTL_MAGIC: one definition of the host's key and of what a map row is

COLUMNS = 'package\tindex\tstage\tdm_ctl_sha16\tsource'
KIND, WHERE = '# kind: evidence', '# column 5 records where each key first appears'
NAME_KEY = re.compile(r'([0-9a-f]{16})\.ctl$')
REC = b'BTCDBTCD'

def red(msg): sys.exit('[RED] ' + msg)

def map_head(path):
    """The map's header lines, verbatim, after checking they are the split's: a file that is not a shader map is not appended to (its column order and its sorting would be anyone's)"""
    if not os.path.exists(path): red(f'{path}: no shader map there (shader_split.py writes it first)')
    with open(path, encoding='utf-8', newline='') as f: lines = f.read().split('\n')
    if len(lines) < 3 or not lines[0].startswith(KIND) or not lines[1].startswith(WHERE) or lines[2] != COLUMNS:
        red(f'{path}: not a shader map the split writes (header lines "{KIND} …", "{WHERE} …" and "{COLUMNS}", this file has {lines[:3]})')
    return lines

def records(d):
    """The length of each record of a package file, walked: 'BTCDBTCD', control length at +0x28, code length at +0x2c, 0x38 bytes of header, then the code area and the control section"""
    out, o = [], 0
    while o < len(d):
        if d[o:o + 8] != REC: red(f'the package file at {o:#x} is not a record of the split\'s ({d[o:o + 8]!r}), so its indices cannot be continued')
        nctl, ncode = struct.unpack_from('<2I', d, o + 0x28)
        if o + 0x38 + ncode + nctl > len(d): red(f'the package file\'s record at {o:#x} runs past its end')
        out.append(0x38 + ncode + nctl); o += out[-1]
    return out

def rec_of(key, ctl, code):
    """The record shader_split.py writes for one key: the two lengths, the code area, the control section (and the two things that would make dmsh_build.py split it wrongly)"""
    rec = bytearray(REC + bytes.fromhex(key) + bytes(0x28)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); rec += code + ctl
    if rec.find(split.CTL_MAGIC) != 0x38 + len(code): return None, 'the control magic appears in the code area'
    if rec.count(REC) != 1: return None, 'the record separator appears inside the record'
    return bytes(rec), None

def pairs(shd):
    """[(key, file name, control, code, why)] of the dump directory; why = the reason that pair is not a shader. The key is the 16 hex digits both writers end the name with, and is
    checked against the pair's own sha256, never trusted. The SetShaders copy of a key comes before the draw-time one: it is the copy the key was computed from (a draw-time copy of a
    control section the game freed no longer hashes to the name it was written under)"""
    out = []
    for nm in sorted((f for f in os.listdir(shd) if f.endswith('.ctl')), key=lambda f: (not f.startswith('SetShaders-'), f)):
        m = NAME_KEY.search(nm)
        if not m: out.append((None, nm, None, None, f'{nm}: no 16-hex-digit key in the name')); continue
        ctl = open(os.path.join(shd, nm), 'rb').read()
        cp = os.path.join(shd, nm[:-len('.ctl')] + '.code')   # both writers name the code area as the control section (SetShaders-<key>.code, <key>.code)
        if not os.path.exists(cp): out.append((m.group(1), nm, None, None, f'{nm}: no .code beside it')); continue
        code = open(cp, 'rb').read()
        if len(ctl) < 0x24 or ctl[:4] != split.CTL_MAGIC: out.append((m.group(1), nm, None, None, f'{nm}: not a control section (magic {ctl[:4].hex()})')); continue
        nctl = split.ctl_len(ctl)
        if len(ctl) < nctl: out.append((m.group(1), nm, None, None, f'{nm}: {len(ctl):#x} control bytes, its own length says {nctl:#x}')); continue
        ncode = struct.unpack_from('<I', ctl, 0x704)[0]
        if ncode != len(code): out.append((m.group(1), nm, None, None, f'{nm}: control +0x704 says a {ncode:#x}-byte code area, the file holds {len(code):#x}')); continue
        if split.skey(ctl, code) != m.group(1): out.append((m.group(1), nm, None, None, f'{nm}: its sha256 is {split.skey(ctl, code)}, not {m.group(1)}')); continue
        out.append((m.group(1), nm, ctl, code, None))
    return out

def main():
    shd, pkg, mapout = sys.argv[1:4]
    if not os.path.isdir(shd): red(f'{shd}: no such directory')
    root = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../..'))
    if os.path.realpath(pkg) == root or os.path.realpath(pkg).startswith(root + os.sep):
        red(f'{pkg}: inside the repository (a package holds game bytes, keep it outside: CONTRIBUTING.md, "Never game files or keys")')
    lines = map_head(mapout)
    known, nextix = set(), {}
    rows = [ln for ln in lines[3:] if ln]
    for ln in rows:
        c = ln.split('\t')
        if len(c) != 5 or not c[0].endswith('.bin'): red(f'{mapout}: a row that is not a map row ({ln!r})')
        known.add(c[3]); nextix[c[0]] = max(nextix.get(c[0], 0), int(c[1]) + 1)
    add, rejected, stages, taken, already, twice = [], [], {}, set(), 0, 0
    for key, nm, ctl, code, why in pairs(shd):
        if why: rejected.append(why); continue
        if key in known: already += 1; continue   # the map already names this shader: a key is a content hash, so the bytes there are these bytes
        if key in taken: twice += 1; continue   # the dump holds one key twice (SetShaders-<key> and <key>): one row and one record, from the copy the key was made of
        st = split.glslc_stage(ctl, code)
        if not st: rejected.append(f'{nm}: the code area names no stage ({code[:4].hex()})'); continue
        rec, why = rec_of(key, ctl, code)
        if why: rejected.append(f'{nm}: {why}'); continue
        src = f'/shdump/{key}.ctl#SHDUMP@0x0'
        add.append((f'{st}_{split.group(src)}.bin', src, key, st, ctl, rec)); stages[st] = stages.get(st, 0) + 1
        taken.add(key)
    for why in rejected: print('[RED] ' + why + ' (rejected)', flush=True)
    counts = ' '.join(f'{split.STAGE_NAME[s]} {n}' for s, n in sorted(stages.items()))
    head = (f'shdump: {len(rejected) + len(add) + already + twice} pair(s), {len(rejected)} rejected, {len(add)} row(s) added' + (f' ({counts})' if counts else '')
            + f', {already} already in the map, {twice} of a key taken twice')
    if not add:
        print(head, flush=True)
        return
    packs = {}
    for name, src, key, st, ctl, rec in sorted(add, key=lambda r: (split.group(r[1]), r[1])):
        if name not in packs:
            path = os.path.join(pkg, name)
            have = open(path, 'rb').read() if os.path.exists(path) else b''
            lens = records(have)
            if nextix.get(name, 0) != len(lens):
                red(f'{mapout} names index {nextix.get(name, 0)} of {name}, and the package holds {len(lens)} record(s): the map and the package were not made by one split')
            packs[name] = [bytearray(have), len(lens)]
        # the row's index continues the package's own records, so dmsh_build.py reads the shader where the map says it is
        rows.append('\t'.join((name, str(packs[name][1]), split.STAGE_NAME[st], key, src)))
        packs[name][0] += rec; packs[name][1] += 1
    os.makedirs(pkg, exist_ok=True)
    for name, (buf, _) in packs.items():
        with open(os.path.join(pkg, name), 'wb') as f: f.write(bytes(buf))
    with open(mapout, 'w', encoding='utf-8', newline='') as f: f.write('\n'.join(lines[:3] + rows) + '\n')
    print(head + '; packages ' + ', '.join(f'{nm} → {n}' for nm, (_, n) in sorted(packs.items())))
    for name, src, key, st, ctl, rec in add:   # what shader_split.py writes into shader_facts.tsv for a compute shader, which only the control section can say
        if st != 'cs': continue
        x, y, z = struct.unpack_from('<3I', ctl, 0x748); sh = struct.unpack_from('<I', ctl, 0x754)[0]
        print(f'facts: {key}\t{{"local":[{x},{y},{z}],"shared":{(sh + 15) // 16 * 16}}}  (add to shader_facts.tsv: a dump writes no answer table)', flush=True)

if __name__ == '__main__': main()
