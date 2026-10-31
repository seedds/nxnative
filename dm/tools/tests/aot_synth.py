#!/usr/bin/env python3
"""A synthetic game module for the x86-64 recompiler's input path, with no game files (docs/design/aot-recompiler.md 1.1, 1.2): an NSO named main of
hand-assembled AArch64 code, its patch file (a registered cut, an observation gate and a frame step), and what `patch_nso.py --aot` writes for it, the behaviour
image and its manifest. dm/tools/tests/test_aot_manifest.py checks the rows against the bytes and, where capstone and lz4 are installed, that patch_nso.py --aot
writes exactly these files; dm/tools/aot/tests/build_packs.sh packs them with nxaot for dm/host's t_aot_module, which runs the module through the host's
game-module loader.

  python3 aot_synth.py <out dir>
    writes <out>/main.nso, <out>/profiles/<PROFILE>/patches.py (a profile folder for DM_GAMES_DIR) and <out>/main.image.bin, <out>/main.aot.tsv

The module has every row kind of the manifest: a data block in text (4 KiB of words that are no instructions, after the last function, and the MOD0 offset word),
the observation gate, a spin wait (ldr; cbz back), the cut and the frame step's branch, the frame step's stub in the auxiliary region, and the rewriter's oracle
rows: an x18 byte jump table (oracle-J), an x18 offset table (oracle-T) and an x18 exclusive window (oracle-W). Its entry, run by t_aot_module, calls a host
function through its GOT (t_aot_mod_combine, an HLE entry of the test), an import nothing implements (t_aot_mod_missing), SetHeapSize, the functions above
(the frame step's function excepted), a function through a relocated pointer, OutputDebugString, and stores what each returned in `results`, then ExitProcess.
Every encoding was checked with llvm-mc-18 (nxnative-aot image). No game bytes, no keys."""
import ast
import hashlib
import os
import struct
import sys
import types
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, B   # the encoders the patch files use

BUILD_ID = 'aa07' * 8                  # the NSO build ID (first 16 bytes): claimed by no profile
PROFILE = 'aa07aa07synthetic'          # the profile folder of its patch file
NAME = 'main'                          # FRAME_STEP applies to main only

# The layout: text, read-only data, data, bss (an NSO's segments)
TEXT_END, RO_OFF, DATA_OFF, DATA_END, BSS = 0x2000, 0x2000, 0x3000, 0x4000, 0x1000
MOD0, DYNAMIC, SYMTAB = 0x2000, 0x2020, 0x2100
RELA, JMPREL, EH_HDR, EH_FRAME = 0x2400, 0x2440, 0x2480, 0x2500
BTAB, TTAB, MSG = 0x2700, 0x2710, 0x2740
GOT, FNPTR, RESULTS = 0x3000, 0x3010, 0x3040
COUNTER, FLAG = RESULTS + 96, RESULTS + 100   # after the 12 results
MESSAGE = b'aot module: done'
DATA_WORD = 0x01000000                 # op0 0, op1 0000 with bits 24:16 set: unallocated, never an instruction
OBS_NUMBER = 23 | 5 << 8               # OBS_REGION_GOT (dm/host/dm.h) with the parameter 5
# The rewriter's layout of this module (patch_nso.py: not FAR; the key page after bss, the frame step's state page after it) and the aux region of --aot
KEY_PAGE_OFF = (DATA_END + BSS + 0x3FFF) & ~0x3FFF
FS_STATE_OFF = KEY_PAGE_OFF + 0x4000
AUX_OFF = FS_STATE_OFF + 0x4000
# What the entry stores in results[i], i = 0..11 (t_aot_module checks them; results[3] is the heap's base, any address)
EXPECTED = [7 * 1000 + 3, 0, 0, None, 0x1234, 30, 400, 2, 9, 42, 2, 2]


# ---------- Encoders (A64; each checked against llvm-mc) ----------
def b(t): return lambda pc, L: B(pc, L[t])
def bl(t): return lambda pc, L: 0x94000000 | ((L[t] - pc) >> 2 & 0x3FFFFFF)
def bhi(t): return lambda pc, L: 0x54000008 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5
def cbz_w(rt, t): return lambda pc, L: 0x34000000 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5 | rt
def cbnz_w(rt, t): return lambda pc, L: 0x35000000 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5 | rt
def adr(rd, t):
    def enc(pc, L):
        imm = (L[t] if isinstance(t, str) else t) - pc
        assert -(1 << 20) <= imm < (1 << 20)
        return 0x10000000 | (imm & 3) << 29 | (imm >> 2 & 0x7FFFF) << 5 | rd
    return enc
def adrp(rd, target): return lambda pc, L: ADRP(rd, pc, target)
def add_x(rd, rn, imm): return 0x91000000 | imm << 10 | rn << 5 | rd                 # add xd, xn, #imm (mov xd, sp when imm is 0)
def add_w(rd, rn, imm): return 0x11000000 | imm << 10 | rn << 5 | rd                 # add wd, wn, #imm
def add_xr(rd, rn, rm, lsl=0): return 0x8B000000 | rm << 16 | lsl << 10 | rn << 5 | rd   # add xd, xn, xm, lsl #lsl
def movz_x(rd, imm, hw=0): return 0xD2800000 | hw << 21 | imm << 5 | rd
def movz_w(rd, imm): return 0x52800000 | imm << 5 | rd
def mov_x(rd, rm): return 0xAA0003E0 | rm << 16 | rd                                 # orr xd, xzr, xm
def mov_w(rd, rm): return 0x2A0003E0 | rm << 16 | rd                                 # orr wd, wzr, wm
def cmp_w(rn, imm): return 0x7100001F | imm << 10 | rn << 5
def ldr_x(rt, rn, off): return 0xF9400000 | off // 8 << 10 | rn << 5 | rt
def str_x(rt, rn, off): return 0xF9000000 | off // 8 << 10 | rn << 5 | rt
def ldr_w(rt, rn, off): return 0xB9400000 | off // 4 << 10 | rn << 5 | rt
def ldrb_uxtw(rt, rn, rm): return 0x38604800 | rm << 16 | rn << 5 | rt               # ldrb wt, [xn, wm, uxtw]
def ldrsw_lsl2(rt, rn, rm): return 0xB8A07800 | rm << 16 | rn << 5 | rt              # ldrsw xt, [xn, xm, lsl #2]
def ldaxr_w(rt, rn): return 0x885FFC00 | rn << 5 | rt
def stlxr_w(rs, rt, rn): return 0x8800FC00 | rs << 16 | rn << 5 | rt
def blr(rn): return 0xD63F0000 | rn << 5
def br(rn): return 0xD61F0000 | rn << 5
def svc(imm): return 0xD4000001 | imm << 5
def brk(imm): return 0xD4200000 | imm << 5
RET, STP_FP_LR, MOV_FP_SP = 0xD65F03C0, 0xA9BF7BFD, 0x910003FD   # ret; stp x29, x30, [sp, #-16]!; mov x29, sp
MOV_W0_1, MOV_W0_2, MOV_W0_5 = movz_w(0, 1), movz_w(0, 2), movz_w(0, 5)
ADD_X0_1 = add_x(0, 0, 1)
LDR_W17_X16 = ldr_w(17, 16, 0)


def text_items():
    """The text as (label or None, word or encoder) items from offset 0, and the functions (name, label) in order"""
    lo = lambda a: a & 0xFFF
    t = [
        (None, b('start')), (None, MOD0), (None, 0), (None, 0),
        # the entry: x19 = &results; each result stored as it comes
        ('start', STP_FP_LR), (None, MOV_FP_SP), (None, adrp(19, RESULTS)), (None, add_x(19, 19, lo(RESULTS))),
        (None, adrp(16, GOT)), (None, ldr_x(16, 16, lo(GOT))), (None, movz_x(0, 7)), (None, movz_x(1, 3)), (None, blr(16)), (None, str_x(0, 19, 0)),
        (None, adrp(16, GOT)), (None, ldr_x(16, 16, lo(GOT + 8))), (None, movz_x(0, 0x55)), (None, blr(16)), (None, str_x(0, 19, 8)),
        (None, movz_x(1, 0x20, 1)), (None, svc(0x01)), (None, str_x(0, 19, 16)), (None, str_x(1, 19, 24)),   # SetHeapSize(2 MiB): w0 result, x1 the heap
        (None, movz_x(2, 0x1234)), (None, str_x(2, 1, 0)), (None, ldr_x(2, 1, 0)), (None, str_x(2, 19, 32)),
        (None, movz_w(0, 2)), (None, bl('f_j')), (None, str_x(0, 19, 40)),
        (None, movz_w(0, 1)), (None, bl('f_t')), (None, str_x(0, 19, 48)),
        (None, add_x(0, 19, COUNTER - RESULTS)), (None, bl('f_window')), (None, add_x(0, 19, COUNTER - RESULTS)), (None, bl('f_window')), (None, str_x(0, 19, 56)),
        (None, add_x(0, 19, FLAG - RESULTS)), (None, bl('f_spin')), (None, str_x(0, 19, 64)),
        (None, movz_x(0, 41)), (None, bl('f_obs')), (None, str_x(0, 19, 72)),
        (None, bl('f_cut')), (None, str_x(0, 19, 80)),
        (None, adrp(16, FNPTR)), (None, ldr_x(16, 16, lo(FNPTR))), (None, movz_x(0, 1)), (None, blr(16)), (None, str_x(0, 19, 88)),
        (None, adrp(0, MSG)), (None, add_x(0, 0, lo(MSG))), (None, movz_x(1, len(MESSAGE))), (None, svc(0x27)),   # OutputDebugString
        (None, svc(0x07)), (None, brk(1)),                                                                     # ExitProcess
        # a byte jump table through x18 (the rewriter's J form): target = base + 4 * entry
        ('f_j', cmp_w(0, 3)), (None, bhi('f_j_def')), (None, adr(16, BTAB)), (None, ldrb_uxtw(9, 16, 0)), (None, adr(18, 'f_j_base')),
        (None, add_xr(18, 18, 9, 2)), ('f_j_br', br(18)),
        ('f_j_base', movz_w(0, 10)), (None, RET), (None, movz_w(0, 20)), (None, RET), (None, movz_w(0, 30)), (None, RET), (None, movz_w(0, 40)), (None, RET),
        ('f_j_def', movz_w(0, 0)), (None, RET),
        # an offset table through x18 (its T form): target = table + signed entry
        ('f_t', cmp_w(0, 3)), (None, bhi('f_t_def')), (None, mov_w(0, 0)), (None, adrp(15, TTAB)), (None, add_x(15, 15, lo(TTAB))),
        (None, ldrsw_lsl2(18, 15, 0)), (None, add_xr(18, 18, 15)), ('f_t_br', br(18)),
        ('f_t_c0', movz_w(0, 100)), (None, RET), ('f_t_c1', movz_w(0, 200)), (None, RET), ('f_t_c2', movz_w(0, 300)), (None, RET),
        ('f_t_c3', movz_w(0, 400)), (None, RET), ('f_t_def', movz_w(0, 0)), (None, RET),
        # an exclusive window on x18: an atomic increment
        ('f_window', mov_x(18, 0)), ('f_w_loop', ldaxr_w(8, 18)), (None, add_w(8, 8, 1)), (None, stlxr_w(9, 8, 18)), ('f_w_cbnz', cbnz_w(9, 'f_w_loop')),
        (None, mov_w(0, 8)), (None, RET),
        # a spin wait: poll a word until another thread sets it
        ('f_spin', ldr_w(8, 0, 0)), ('f_spin_be', cbz_w(8, 'f_spin')), (None, mov_w(0, 8)), (None, RET),
        # the observation gate's function, the cut's and the frame step's
        ('f_obs', ADD_X0_1), (None, RET),
        ('f_cut', MOV_W0_1), (None, RET),
        ('f_fs', MOV_W0_5), (None, RET), ('f_end', None),
    ]
    functions = [('synth_start', 'start'), ('synth_jump_byte', 'f_j'), ('synth_jump_offset', 'f_t'), ('synth_window', 'f_window'),
                 ('synth_spin', 'f_spin'), ('synth_observed', 'f_obs'), ('synth_cut', 'f_cut'), ('synth_frame_step', 'f_fs')]
    return t, functions


def labels():
    items, _ = text_items()
    L = {}
    for i, (name, _) in enumerate(items):
        if name: L[name] = 4 * i
    return L


L = labels()
FUNCS = text_items()[1]
FN_RANGES = [(L[lab], (L[FUNCS[i + 1][1]] if i + 1 < len(FUNCS) else L['f_end'])) for i, (_, lab) in enumerate(FUNCS)]
TTAB_ORDER = ['f_t_c1', 'f_t_c3', 'f_t_c0', 'f_t_c2']   # index 1 -> c3 (400)
assert L['f_end'] <= 0x1000 - 0x40


def uleb(v):
    out = bytearray()
    while True:
        c = v & 0x7F; v >>= 7
        out.append(c | (0x80 if v else 0))
        if not v: return bytes(out)


def eh_frame():
    """.eh_frame (a CIE zR with pc-relative sdata4 pointers, then an FDE per function, no instructions) and .eh_frame_hdr (its sorted table)"""
    cie = bytearray(struct.pack('<I', 0)) + bytes([1]) + b'zR\0' + uleb(1) + bytes([0x78, 30]) + uleb(1) + bytes([0x1B, 0x0C, 31, 0])
    while (len(cie) + 4) % 4: cie.append(0)
    frame = bytearray(struct.pack('<I', len(cie)) + cie)
    fdes = []
    for start, end in FN_RANGES:
        at = EH_FRAME + len(frame)
        body = bytearray(struct.pack('<I', at + 4 - EH_FRAME))      # the CIE pointer: back to the CIE from this field
        body += struct.pack('<i', start - (at + 8)) + struct.pack('<i', end - start) + uleb(0)
        while (len(body) + 4) % 4: body.append(0)
        frame += struct.pack('<I', len(body)) + body
        fdes.append((start, at))
    frame += struct.pack('<I', 0)
    hdr = bytearray([1, 0x1B, 0x03, 0x3B]) + struct.pack('<i', EH_FRAME - (EH_HDR + 4)) + struct.pack('<I', len(fdes))
    for start, at in sorted(fdes): hdr += struct.pack('<ii', start - EH_HDR, at - EH_HDR)
    return bytes(hdr), bytes(frame)


SYMBOLS = [('t_aot_mod_combine', 0, 0x12, 0, 0), ('t_aot_mod_missing', 0, 0x12, 0, 0),
           ('results', 3, 0x11, RESULTS, 104), ('synth_got', 3, 0x11, GOT, 16), ('synth_fnptr', 3, 0x11, FNPTR, 8)] + \
          [(name, 1, 0x12, s, e - s) for (name, _), (s, e) in zip(FUNCS, FN_RANGES)]


def image():
    """The module's image as the NSO holds it: text, read-only data, data (DATA_END bytes; bss follows)"""
    img = bytearray(DATA_END)
    items, _ = text_items()
    for i, (_, w) in enumerate(items):
        if w is None: continue
        struct.pack_into('<I', img, 4 * i, w(4 * i, L) if callable(w) else w)
    for a in range(0x1000, TEXT_END, 4): struct.pack_into('<I', img, a, DATA_WORD | (a >> 2 & 0xFFFF))   # the data block in text
    # MOD0: the dynamic section, bss, eh_frame_hdr, relative to MOD0
    struct.pack_into('<4s6i', img, MOD0, b'MOD0', DYNAMIC - MOD0, DATA_END - MOD0, DATA_END + BSS - MOD0, EH_HDR - MOD0, EH_FRAME - MOD0, 0)
    strtab = bytearray(b'\0'); names = []
    for name, *_ in SYMBOLS: names.append(len(strtab)); strtab += name.encode() + b'\0'
    o = SYMTAB + 24
    for (name, shndx, info, value, size), n in zip(SYMBOLS, names):
        struct.pack_into('<IBBHQQ', img, o, n, info, 0, shndx, value, size); o += 24
    strtab_off = o
    img[strtab_off:strtab_off + len(strtab)] = strtab
    assert strtab_off + len(strtab) <= RELA
    rela = [(GOT, 1 << 32 | 1025, 0), (FNPTR, 1027, L['f_obs'])]   # GLOB_DAT t_aot_mod_combine; RELATIVE &f_obs
    for i, r in enumerate(rela): struct.pack_into('<QQq', img, RELA + 24 * i, *r)
    struct.pack_into('<QQq', img, JMPREL, GOT + 8, 2 << 32 | 1026, 0)   # JUMP_SLOT t_aot_mod_missing
    dyn = [(6, SYMTAB), (5, strtab_off), (10, len(strtab)), (11, 24), (7, RELA), (8, 24 * len(rela)), (9, 24), (23, JMPREL), (2, 24), (20, 7), (0, 0)]
    for i, (tag, v) in enumerate(dyn): struct.pack_into('<qQ', img, DYNAMIC + 16 * i, tag, v)
    hdr, frame = eh_frame()
    img[EH_HDR:EH_HDR + len(hdr)] = hdr
    img[EH_FRAME:EH_FRAME + len(frame)] = frame
    assert EH_HDR + len(hdr) <= EH_FRAME and EH_FRAME + len(frame) <= BTAB
    img[BTAB:BTAB + 4] = bytes([0, 2, 4, 6])
    for i, c in enumerate(TTAB_ORDER): struct.pack_into('<i', img, TTAB + 4 * i, L[c] - TTAB)
    img[MSG:MSG + len(MESSAGE)] = MESSAGE
    struct.pack_into('<II', img, COUNTER, 0, 9)   # the window's counter, the spin wait's flag
    return img


def nso(img):
    """An uncompressed NSO (flags 0) of the image: header, then the three segments"""
    h = bytearray(0x100)
    h[0:4] = b'NSO0'
    segs = [(0, TEXT_END), (RO_OFF, DATA_OFF - RO_OFF), (DATA_OFF, DATA_END - DATA_OFF)]
    fo = 0x100
    for i, (mo, sz) in enumerate(segs):
        struct.pack_into('<III', h, 0x10 + 0x10 * i, fo, mo, sz); struct.pack_into('<I', h, 0x60 + 4 * i, sz); fo += sz
    struct.pack_into('<I', h, 0x3C, BSS)
    h[0x40:0x50] = bytes.fromhex(BUILD_ID)
    return bytes(h) + b''.join(bytes(img[mo:mo + sz]) for mo, sz in segs)


PATCHES = f'''# The synthetic module of dm/tools/tests/aot_synth.py (no game): a registered cut, an observation gate and a frame step, as a profile's patches.py has them
from nso_asm import ADRP, B
CUTS = {{'{BUILD_ID}': [({L['f_cut']:#x}, '20008052', '40008052', 'synthetic: f_cut returns 2 instead of 1')]}}
OBS = {{'{BUILD_ID}': [({L['f_obs']:#x}, {OBS_NUMBER}, '91000400', 'synthetic: an observation gate at f_obs (number 23, parameter 5)')]}}
def frame_step(ctx):
    ctx.site({L['f_fs']:#x}, 'a0008052')
    start = ctx.here(); ctx.emit(ADRP(16, ctx.here(), ctx.state)); ctx.emit(0xb9400211)   # ldr w17, [x16]: the target fps
    ctx.emit(0x528000a0); ctx.emit(B(ctx.here(), {L['f_fs'] + 4:#x}))                      # the original mov w0, #5, back
    ctx.put({L['f_fs']:#x}, [B({L['f_fs']:#x}, start)], 'synthetic frame step: f_fs through its stub')
FRAME_STEP = {{'{BUILD_ID}': frame_step}}
'''


def patch_nso_functions(*names):
    """Functions of patch_nso.py by name, compiled alone (its top level is a program that rewrites an image), as test_spin_waits.py takes the spin wait search"""
    tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
    body = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    assert {n.name for n in body} == set(names), f'patch_nso.py lacks {set(names) - {n.name for n in body}}'
    scope = {'hashlib': hashlib, 'struct': struct}
    exec(compile(ast.Module(body=body, type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)
    return [scope[n] for n in names]


def aot_tables():
    """What patch_nso.py --aot finds in this module, from how it was assembled: the behaviour image (the cut and the frame step's branch in it, the stub in the
    aux region after the state page, bss folded in up to it) and every table of the manifest"""
    img = image()
    cut_old, cut_new = struct.pack('<I', MOV_W0_1).hex(), struct.pack('<I', MOV_W0_2).hex()
    assert img[L['f_cut']:L['f_cut'] + 4].hex() == cut_old
    img[L['f_cut']:L['f_cut'] + 4] = bytes.fromhex(cut_new)
    cuts = [(L['f_cut'], cut_old, cut_new, 'synthetic: f_cut returns 2 instead of 1')]
    # the frame step through the patch file's own function, with the --aot context
    aux, scope = bytearray(), {}
    exec(compile(PATCHES, 'patches.py', 'exec'), scope)
    def put(off, words, why):
        old = bytes(img[off:off + 4 * len(words)])
        for i, w in enumerate(words): struct.pack_into('<I', img, off + 4 * i, w)
        cuts.append((off, old.hex(), bytes(img[off:off + 4 * len(words)]).hex(), why))
    def site(off, orig): assert img[off:off + len(bytes.fromhex(orig))] == bytes.fromhex(orig)
    ctx = types.SimpleNamespace(here=lambda: AUX_OFF + len(aux), emit=lambda w: aux.extend(struct.pack('<I', w & 0xFFFFFFFF)), site=site, put=put,
                                state=FS_STATE_OFF, render_scale=0)
    scope['FRAME_STEP'][BUILD_ID](ctx)
    behaviour = bytes(img) + bytes(AUX_OFF - len(img)) + bytes(aux)
    t = {
        'image': behaviour, 'layout': (TEXT_END, RO_OFF, DATA_OFF, 0), 'aux': (AUX_OFF, len(aux), FS_STATE_OFF),
        # nso_code.data_blocks: the gap after the last FDE (more than a quarter of its words decode as nothing); the MOD0 offset word
        'data': [(4, 8), (FN_RANGES[-1][1], TEXT_END)],
        'obs': [(L['f_obs'], OBS_NUMBER, ADD_X0_1)],
        'spins': [(L['f_spin'], [L['f_spin_be']])],
        'cuts': cuts,
        'windows': [(L['f_w_loop'], L['f_w_cbnz'] + 4)],                 # sites.tsv W: from the exclusive load to past the cbnz after the store
        'jumps': {L['f_j_br']: (L['f_j_base'], 1)},                       # J: the adr base, byte entries
        'tabjumps': {L['f_t_br']: [L[c] for c in TTAB_ORDER]},            # T: every target, by index
    }
    return t


def manifest_text(t):
    """The manifest through patch_nso.py's own writer (aot_data_rows, aot_manifest), from the tables above"""
    data_rows, manifest = patch_nso_functions('aot_data_rows', 'aot_manifest')
    data = data_rows(t['image'], TEXT_END, [d for d in t['data'] if d != (4, 8)])
    assert data == t['data'], data
    return manifest(NAME, BUILD_ID, hashlib.sha256(t['image']).hexdigest(), len(t['image']), t['layout'], data, t['obs'], t['spins'], t['cuts'], t['windows'],
                    t['jumps'], t['tabjumps'], t['aux'])


def write(out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    img = image()
    (out / f'{NAME}.nso').write_bytes(nso(img))
    prof = out / 'profiles' / PROFILE; prof.mkdir(parents=True, exist_ok=True)
    (prof / 'patches.py').write_text(PATCHES)
    t = aot_tables()
    (out / f'{NAME}.image.bin').write_bytes(t['image'])
    (out / f'{NAME}.aot.tsv').write_text(manifest_text(t))
    return out


if __name__ == '__main__':
    if len(sys.argv) != 2: sys.exit('usage: aot_synth.py <out dir>')
    o = write(sys.argv[1])
    print(f'aot_synth: {o}/{NAME}.nso, {NAME}.image.bin ({len(aot_tables()["image"])} bytes), {NAME}.aot.tsv, profiles/{PROFILE}/patches.py')
