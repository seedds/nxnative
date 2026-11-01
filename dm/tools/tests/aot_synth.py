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
(the frame step's through its stub in the aux region, which counts its passes at the state page's +0x30 as the games' stubs count vsyncs there), a function
through a relocated pointer, OutputDebugString, and stores what each returned in `results`, then ExitProcess. Two data words are 0 for a plain run and set by
t_aot_module's child for its checks: synth_flag (the spin wait's flag: 9 in the image, so the loop ends at once; 0 there, so the loop spins until the host sets
it, its back-edge passing the spin gate) and synth_faultptr (0: a fault probe in the entry is skipped; an address no page backs: the probe's ldr x9, [x9] faults
there, and the host's fault report must name that instruction).
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
from nso_asm import ADRP, B, LDP_POST, STP_PRE   # the encoders the patch files use

BUILD_ID = 'aa07' * 8                  # the NSO build ID (first 16 bytes): claimed by no profile
PROFILE = 'aa07aa07synthetic'          # the profile folder of its patch file
NAME = 'main'                          # FRAME_STEP applies to main only

# The layout: text, read-only data, data, bss (an NSO's segments)
TEXT_END, RO_OFF, DATA_OFF, DATA_END, BSS = 0x2000, 0x2000, 0x3000, 0x4000, 0x1000
MOD0, DYNAMIC, SYMTAB = 0x2000, 0x2020, 0x2100
RELA, JMPREL, EH_HDR, EH_FRAME = 0x2400, 0x2440, 0x2480, 0x2500
BTAB, TTAB, MSG = 0x2700, 0x2710, 0x2740
GOT, FNPTR, RESULTS = 0x3000, 0x3010, 0x3040
NRESULTS = 16                                  # the results array's words (13 used)
COUNTER, FLAG, FAULTPTR = RESULTS + 8 * NRESULTS, RESULTS + 8 * NRESULTS + 4, RESULTS + 8 * NRESULTS + 8   # after the results
FLAG_VALUE = 9                                 # the spin wait's flag in the image: the loop ends at once (f_spin returns it)
MESSAGE = b'aot module: done'
DATA_WORD = 0x01000000                 # op0 0, op1 0000 with bits 24:16 set: unallocated, never an instruction
OBS_NUMBER = 23 | 5 << 8               # OBS_REGION_GOT (dm/host/dm.h) with the parameter 5
# The rewriter's layout of this module (patch_nso.py: not FAR; the key page after bss, the frame step's state page after it) and the aux region of --aot
KEY_PAGE_OFF = (DATA_END + BSS + 0x3FFF) & ~0x3FFF
FS_STATE_OFF = KEY_PAGE_OFF + 0x4000
AUX_OFF = FS_STATE_OFF + 0x4000
# What the entry stores in results[i], i = 0..12 (t_aot_module checks them; results[3] is the heap's base, any address; results[12] is f_fs's 5, through the frame step's
# stub in the aux region). f_obs runs twice, for results[9] (x0 = 41) and through the relocated pointer for results[11] (x0 = 1): t_aot_module's observer adds 1000 to the x0
# it sees at the gate, so the host test expects 1042 and 1002 there
EXPECTED = [7 * 1000 + 3, 0, 0, None, 0x1234, 30, 400, 2, FLAG_VALUE, 42, 2, 2, 5]
FAULT_WORD = 0xF9400129                # ldr x9, [x9]: the fault probe's load, the only such word of the text


# ---------- Encoders (A64; each checked against llvm-mc) ----------
def b(t): return lambda pc, L: B(pc, L[t])
def bl(t): return lambda pc, L: 0x94000000 | ((L[t] - pc) >> 2 & 0x3FFFFFF)
def bhi(t): return lambda pc, L: 0x54000008 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5
def cbz_w(rt, t): return lambda pc, L: 0x34000000 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5 | rt
def cbnz_w(rt, t): return lambda pc, L: 0x35000000 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5 | rt
def cbz_x(rt, t): return lambda pc, L: 0xB4000000 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5 | rt
def cbnz_x(rt, t): return lambda pc, L: 0xB5000000 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5 | rt
def bne(t): return lambda pc, L: 0x54000001 | ((L[t] - pc) >> 2 & 0x7FFFF) << 5
def cmp_x(rn, imm): return 0xF100001F | imm << 10 | rn << 5                          # cmp xn, #imm (subs xzr, xn, #imm)
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
        (None, bl('f_fs')), (None, str_x(0, 19, 96)),                                                         # the frame step's function, through its stub
        # the fault probe: synth_faultptr is 0 in the image; t_aot_module's fault run sets an address no page backs, and fault_ld faults there
        (None, ldr_x(9, 19, FAULTPTR - RESULTS)), (None, cbz_x(9, 'no_fault')), ('fault_ld', FAULT_WORD),
        ('no_fault', adrp(16, FNPTR)), (None, ldr_x(16, 16, lo(FNPTR))), (None, movz_x(0, 1)), (None, blr(16)), (None, str_x(0, 19, 88)),
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


def eh_frame_at(eh_frame, eh_hdr, ranges):
    """.eh_frame at offset eh_frame (a CIE zR with pc-relative sdata4 pointers, then an FDE per function range, no instructions) and .eh_frame_hdr at eh_hdr (its sorted table)"""
    cie = bytearray(struct.pack('<I', 0)) + bytes([1]) + b'zR\0' + uleb(1) + bytes([0x78, 30]) + uleb(1) + bytes([0x1B, 0x0C, 31, 0])
    while (len(cie) + 4) % 4: cie.append(0)
    frame = bytearray(struct.pack('<I', len(cie)) + cie)
    fdes = []
    for start, end in ranges:
        at = eh_frame + len(frame)
        body = bytearray(struct.pack('<I', at + 4 - eh_frame))      # the CIE pointer: back to the CIE from this field
        body += struct.pack('<i', start - (at + 8)) + struct.pack('<i', end - start) + uleb(0)
        while (len(body) + 4) % 4: body.append(0)
        frame += struct.pack('<I', len(body)) + body
        fdes.append((start, at))
    frame += struct.pack('<I', 0)
    hdr = bytearray([1, 0x1B, 0x03, 0x3B]) + struct.pack('<i', eh_frame - (eh_hdr + 4)) + struct.pack('<I', len(fdes))
    for start, at in sorted(fdes): hdr += struct.pack('<ii', start - eh_hdr, at - eh_hdr)
    return bytes(hdr), bytes(frame)


def eh_frame():
    """The module's .eh_frame_hdr and .eh_frame (eh_frame_at with its layout and functions)"""
    return eh_frame_at(EH_FRAME, EH_HDR, FN_RANGES)


SYMBOLS = [('t_aot_mod_combine', 0, 0x12, 0, 0), ('t_aot_mod_missing', 0, 0x12, 0, 0),
           ('results', 3, 0x11, RESULTS, 8 * NRESULTS), ('synth_got', 3, 0x11, GOT, 16), ('synth_fnptr', 3, 0x11, FNPTR, 8),
           ('synth_flag', 3, 0x11, FLAG, 4), ('synth_faultptr', 3, 0x11, FAULTPTR, 8)] + \
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
    struct.pack_into('<II', img, COUNTER, 0, FLAG_VALUE)   # the window's counter, the spin wait's flag (synth_faultptr after them stays 0)
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
    start = ctx.here(); ctx.emit(ADRP(16, ctx.here(), ctx.state))
    ctx.emit(0xb9403211); ctx.emit(0x11000631); ctx.emit(0xb9003211)                         # ldr w17, [x16, #0x30]; add w17, w17, #1; str w17, [x16, #0x30]: a pass counted
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


# ---------- The host-function module (CTest t_aot_hostfn): every way a function address crosses between the host and recompiled guest code ----------
# A second module named main, in its own folder, with no patch file (no cut, gate or frame step): its image and manifest as patch_nso.py --aot writes them for such a module,
# and an NRO named sub that main loads at run time through nn::ro. main's entry (t_aot_hostfn checks results[i] against HF_EXPECTED):
#   [0]  strlen through its GOT: the C library passthrough (dm/host/libc_pass.c's PASS entry, its host-call thunk)
#   [1]  sqrtf(6.25f) through its GOT (a float in v0 and back), the result's bits
#   [12] the function pointer t_hf_get returns (an HLE entry whose return type is a function pointer: its thunk hands over the guest-visible slot of t_hf_mul)
#   [2]  that pointer called with blr; [3] stored in a vtable-like table (hf_vt[1]) and called from there
#   [4]  t_hf_install(hf_vt2) writes host functions into guest memory (dm_guest_fnptr): hf_vt2[0] t_hf_add, called; [5] hf_vt2[1] a host function with no thunk: its
#        stub answers 0 (an UNIMPL line; the [RED] line when it was handed over)
#   [6]  t_hf_spawn(14) creates a guest thread through hle_guestthread.c (nn::os::CreateThread, StartThread, WaitThread, DestroyThread, malloc and free are this
#        module's: a bump allocator in bss and the thread SVCs); its host body calls synth_triple(14) on the new thread: 42 when it ran on another guest thread
#   [7]  nn::ro::LoadModule of the NRO the host put in guest memory (nro_image): hle_ro.c maps it through nro.tsv, loads sub.aot.so, relocates it (its strlen import
#        at the passthrough's slot) and runs its DT_INIT (sub_flag = 1000) as a guest call: 0; [8] LookupModuleSymbol(sub_fn): 0; [21] LookupModuleSymbol(sub_call): 0;
#        then HF_CROSS_PASSES passes of a loop with three calls between the packs (ABI 3: the standard entries' common signature, an inline cache bound to the other
#        pack's entry): main calls sub_fn(5) = 5 + strlen("sub nro") + sub_flag = 1012, and sub_call(synth_triple, 7), which calls main's synth_triple(7) back and adds 1:
#        22; [9] the sum of sub_fn's, [20] of sub_call's. The host's t_hf_mark(0) after the first pass and t_hf_mark(1) after the last snapshot the dispatcher's
#        counters: the three sites miss in the first pass only (t_aot_hostfn checks 0 misses after that warm-up, and no hand-over through the JIT fallback's path)
#        [10] sub_value through main's own GOT, a stub until the load bound it to the NRO: 77; [11] after UnloadModule, the GOT is the stub again: 0
# then the host calling guest code back (aot.h dm_gcall, DM_GUEST_CALL: the plain-pointer calls of Apple's text), main having registered its callback hf_cb with t_re_register:
#   [13] t_re_fire(1): the host calls hf_cb(data, 1, 2, ..., 9) from inside that host call, on this thread (nested re-entry), ten arguments, the 9th and 10th on the guest stack:
#        *data (1000) + 1 + 2 + ... + 9 = 1045; [14] x22, set to 0x1234 before the call and stored after it (a callee-saved register across the nested call)
#   [15] t_re_deep(5): the host calls hf_deep(5), which calls the host's t_re_leaf(5), which calls synth_triple(5): 15 + 100, + 1 in hf_deep = 116 (two levels of nesting)
#   [16] t_re_thread(10): hf_cb(data, 10, 2..9) from a guest thread the host creates (hle_guestthread.c, as the audio renderer's render thread): 1054
#   [17] t_re_hostthread(20): hf_cb(data, 20, 2..9) from a host thread that is no guest thread (a context of its own, with a [RED] line): 1064
#   [18] t_re_bind(hf_ev): hle_thread.c's hle_bind_system_event, the real path, calls main's nn::os::CreateSystemEvent and GetWritableHandleOfSystemEvent (svcCreateEvent)
#        as nested guest calls, then the host signals the event through its binding (hle_signal_system_event_binding): the binding, non-zero; [19] svcWaitSynchronization on
#        the event's readable handle with timeout 0: 0, signalled
# then OutputDebugString, and t_re_stop: the host, holding a lock, calls hf_exit (svcExitProcess) as a guest call; the process stop leaves that nested call, the host
# function runs on to its end (it releases the lock, notes what it saw) and its host call leaves in turn: the run ends with ExitProcess's status
HF_DIR_NAME = 'hostfn'
HF_BUILD_ID = 'aa08' * 8
HF_TEXT_END, HF_RO_OFF, HF_DATA_OFF, HF_DATA_END, HF_BSS = 0x1000, 0x1000, 0x2000, 0x3000, 0x40000
HF_MOD0, HF_DYNAMIC, HF_SYMTAB, HF_RELA, HF_JMPREL, HF_EH_HDR, HF_EH_FRAME = 0x1000, 0x1020, 0x1100, 0x1880, 0x1AA0, 0x1AC0, 0x1B60   # the string table grew past 0x1800 (the t_ga_* and t_hf_mark imports)
HF_MSG, HF_CONSTF, HF_NAME, HF_NAME2 = 0x1E00, 0x1E20, 0x1E40, 0x1E48
HF_CBDATA, HF_EV = 0x2340, 0x2380   # hf_cb's data word (1000), the SystemEventType t_re_bind binds (write handle, read handle)
HF_CBDATA_VALUE = 1000
HF_GOT, HF_RESULTS, HF_VT, HF_VT2, HF_NROIMG, HF_HEAPPTR, HF_ROMOD, HF_OUT, HF_OUT2 = 0x2000, 0x2100, 0x2200, 0x2240, 0x2280, 0x2288, 0x22C0, 0x2300, 0x2308
HF_CROSS_PASSES = 4                 # the passes of the cross-pack loop (three calls between the packs each)
HF_HEAP = HF_DATA_END   # bss: the bump allocator's memory
HF_MESSAGE = b'hostfn module: done'
HF_SUB_MESSAGE = b'sub nro'
HF_GOT_SYMS = ['strlen', 'sqrtf', 't_hf_get', 't_hf_install', 't_hf_spawn', '_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi',
               '_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc', 'sub_value', '_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE',
               't_re_register', 't_re_fire', 't_re_thread', 't_re_hostthread', 't_re_deep', 't_re_leaf', 't_re_bind', 't_re_stop', 't_hf_mark',
               't_ga_buf', 't_ga_str', 't_ga_local', 't_ga_outside']
HF_JUMP_SLOT = {'sub_value'}   # in DT_JMPREL (rebound at the NRO's load and unload as a game's lazily bound functions); the rest GLOB_DAT
HF_SQRT_IN, HF_SQRT_OUT = 6.25, 2.5
# What main stores in results[i] (None: checked by the host otherwise)
HF_EXPECTED = [len(HF_MESSAGE), struct.unpack('<I', struct.pack('<f', HF_SQRT_OUT))[0], 5 * 6 + 1000, 7 * 8 + 1000, 123, 0, 42, 0, 0,
               HF_CROSS_PASSES * (5 + len(HF_SUB_MESSAGE) + 1000), 77, 0, None,
               HF_CBDATA_VALUE + 1 + 44, 0x1234, 3 * 5 + 100 + 1, HF_CBDATA_VALUE + 10 + 44, HF_CBDATA_VALUE + 20 + 44, None, 0,
               HF_CROSS_PASSES * (3 * 7 + 1), 0]
HF_NRESULTS = 24   # the results array's words
# Host memory the guest sees (dm/host/aot.h; t_aot_hostfn_entries.c's t_ga_* entries): what main stores in ga_results[i] (None: checked by the host otherwise)
#   [0] the buffer t_ga_buf hands it (garena, in the arena below 2^39); [1] the word main reads from it (the host wrote HF_GA_WORD)
#   [2] the string t_ga_str hands it (an arena copy of a literal); [3] strlen of it through the GOT; [4] its first byte, read by main itself
#   [5] t_ga_local(hf_sum2): the host passes hf_sum2 an array of its scoped region (11, 0, 0, 44), which main reads: 55, + 1000 when the array was in the arena
#   [6] t_ga_outside(hf_sum2): the host passes hf_sum2 a host stack array and returns a host string on purpose (the arena guard's lines, DM_AOT_ARENA_CHECK=1); strlen of it: 7
HF_GA_RESULTS = 0x2400
HF_GA_WORD = 0x1122334455667788
HF_GA_STRING = b'guest string'
HF_GA_EXPECTED = [None, HF_GA_WORD, None, len(HF_GA_STRING), HF_GA_STRING[0], 11 + 44 + 1000, len(b'outside')]
# The NRO: text (with the NRO header at +0x10), read-only data (MOD0 and the dynamic tables), data, bss
SUB_CODE, SUB_TEXT_END, SUB_RO_OFF, SUB_DATA_OFF, SUB_DATA_END, SUB_BSS = 0x80, 0x1000, 0x1000, 0x2000, 0x3000, 0x1000
SUB_MOD0, SUB_DYNAMIC, SUB_SYMTAB, SUB_RELA, SUB_EH_HDR, SUB_EH_FRAME, SUB_MSG = 0x1000, 0x1020, 0x1100, 0x1400, 0x1500, 0x1580, 0x1900
SUB_GOT, SUB_FLAG = 0x2000, 0x2008


def ldr_s(rt, rn, off): return 0xBD400000 | off // 4 << 10 | rn << 5 | rt            # ldr st, [xn, #off]
def str_s(rt, rn, off): return 0xBD000000 | off // 4 << 10 | rn << 5 | rt            # str st, [xn, #off]
def str_w(rt, rn, off): return 0xB9000000 | off // 4 << 10 | rn << 5 | rt            # str wt, [xn, #off]
def movn_x(rd, imm): return 0x92800000 | imm << 5 | rd                               # movn xd, #imm (mov xd, #~imm)
def movn_w(rd, imm): return 0x12800000 | imm << 5 | rd
def lsr_x(rd, rn, sh): return 0xD3400000 | sh << 16 | 63 << 10 | rn << 5 | rd         # ubfm xd, xn, #sh, #63
def lsl_x(rd, rn, sh): return 0xD3400000 | (64 - sh) % 64 << 16 | (63 - sh) << 10 | rn << 5 | rd   # ubfm xd, xn, #(64-sh), #(63-sh)
def ldrb(rt, rn, off): return 0x39400000 | off << 10 | rn << 5 | rt                    # ldrb wt, [xn, #off]
LDP_FP_LR = 0xA8C17BFD   # ldp x29, x30, [sp], #16
FMOV_S0_1 = 0x1E2E1000   # fmov s0, #1.0
HF_ENCODINGS = [   # (llvm-mc's text, the word above): test_aot_manifest.py's NxaotReads checks them with llvm-mc-18 where it is installed
    ('ldr s0, [x9, #32]', ldr_s(0, 9, 32)), ('str s0, [x19, #8]', str_s(0, 19, 8)), ('str w1, [x9, #16]', str_w(1, 9, 16)),
    ('mov x3, #-1', movn_x(3, 0)), ('mov w5, #-2', movn_w(5, 1)), ('lsr x11, x11, #4', lsr_x(11, 11, 4)), ('lsl x11, x11, #4', lsl_x(11, 11, 4)),
    ('ldp x29, x30, [sp], #16', LDP_FP_LR), ('stp x29, x30, [sp, #-32]!', STP_PRE(29, 30, 31, -32)), ('ldp x29, x30, [sp], #32', LDP_POST(29, 30, 31, 32)),
    ('fmov s0, #1.0', FMOV_S0_1), ('str x19, [sp, #16]', str_x(19, 31, 16)), ('ldr x19, [sp, #16]', ldr_x(19, 31, 16)), ('mov x19, x0', mov_x(19, 0)), ('add x0, x0, x19', add_xr(0, 0, 19)),
    ('mov w4, w5', mov_w(4, 5)), ('add x1, x0, #16', add_x(1, 0, 16)), ('ldr w0, [x0, #16]', ldr_w(0, 0, 16)), ('svc #0x18', svc(0x18)), ('brk #0x2', brk(2)),
    ('ldr x9, [sp]', ldr_x(9, 31, 0)), ('ldr x9, [sp, #8]', ldr_x(9, 31, 8)), ('ldr x0, [x0]', ldr_x(0, 0, 0)), ('add x0, x0, x7', add_xr(0, 0, 7)),
    ('str w2, [x9, #4]', str_w(2, 9, 4)), ('ldr w0, [x0]', ldr_w(0, 0, 0)), ('mov x9, x0', mov_x(9, 0)), ('svc #0x45', svc(0x45)), ('mov w0, #0', movz_w(0, 0)),
    ('mov x22, #4660', movz_x(22, 0x1234)), ('add x1, x9, #4', add_x(1, 9, 4)), ('mov x3, #0', movz_x(3, 0)), ('add x0, x0, #1', add_x(0, 0, 1)), ('brk #0x3', brk(3)),
    ('ldrb w0, [x20]', ldrb(0, 20, 0)), ('mov x20, x0', mov_x(20, 0)), ('mov x0, x20', mov_x(0, 20)), ('ldr x1, [x0]', ldr_x(1, 0, 0)), ('ldr x2, [x0, #24]', ldr_x(2, 0, 24)),
    ('add x0, x1, x2', add_xr(0, 1, 2)), ('str x0, [x21, #8]', str_x(0, 21, 8)),
    # the cross-pack loop and sub_call; the synthetic module's fault probe and frame-step stub
    ('ldr x23, [x23, #768]', ldr_x(23, 23, 0x300)), ('add x26, x26, x0', add_xr(26, 26, 0)), ('add x27, x27, x0', add_xr(27, 27, 0)), ('mov x1, #7', movz_x(1, 7)),
    ('add x25, x25, #1', add_x(25, 25, 1)), ('cmp x25, #4', cmp_x(25, 4)), ('mov x9, x0', mov_x(9, 0)), ('mov x0, x1', mov_x(0, 1)), ('blr x9', blr(9)),
    ('str x26, [x19, #72]', str_x(26, 19, 72)), ('str x27, [x19, #160]', str_x(27, 19, 160)),
    ('ldr x9, [x9]', FAULT_WORD), ('ldr x9, [x19, #136]', ldr_x(9, 19, 136)), ('ldr w17, [x16, #48]', 0xB9403211), ('add w17, w17, #1', 0x11000631),
    ('str w17, [x16, #48]', 0xB9003211), ('str x0, [x19, #96]', str_x(0, 19, 96)),
]
# The branch encoders with a label 8 bytes on, against llvm-mc-18's disassembly of their words (test_aot_manifest.py test_branch_encoders)
HF_BRANCHES = [('cbz\tx9, #8', cbz_x(9, 't')(0, {'t': 8})), ('cbnz\tx25, #8', cbnz_x(25, 't')(0, {'t': 8})), ('b.ne\t#8', bne('t')(0, {'t': 8})),
               ('cbz\tx9, #-8', cbz_x(9, 't')(8, {'t': 0}))]


def assemble(items, base=0):
    """Text items (label or None, word or encoder of (pc, labels)) laid out from offset base: (labels, [(offset, word)])"""
    labels = {}
    for i, (name, _) in enumerate(items):
        if name: labels[name] = base + 4 * i
    words = [(base + 4 * i, w(base + 4 * i, labels) if callable(w) else w) for i, (_, w) in enumerate(items) if w is not None]
    return labels, words


def dynamic_tables(img, mod0, dynamic, symtab, rela_off, symbols, rela, jmprel_off=None, jmprel=(), extra=(), mod0_fields=None):
    """MOD0, the symbols with their string table right after them (the host's and nxaot's count rule), RELA and JMPREL, and the dynamic section, written into img.
    symbols: (name, section index, st_info, value, size); rela / jmprel: (offset, type, symbol index, addend); extra: more (tag, value) dynamic entries"""
    struct.pack_into('<4s6i', img, mod0, b'MOD0', *[f - mod0 for f in mod0_fields], 0)
    strtab = bytearray(b'\0'); names = []
    for name, *_ in symbols: names.append(len(strtab)); strtab += name.encode() + b'\0'
    o = symtab + 24
    for (name, shndx, info, value, size), n in zip(symbols, names):
        struct.pack_into('<IBBHQQ', img, o, n, info, 0, shndx, value, size); o += 24
    strtab_off = o
    img[strtab_off:strtab_off + len(strtab)] = strtab
    assert strtab_off + len(strtab) <= rela_off, 'the string table runs into the relocations'
    for i, (off, typ, sym, add) in enumerate(rela): struct.pack_into('<QQq', img, rela_off + 24 * i, off, sym << 32 | typ, add)
    for i, (off, typ, sym, add) in enumerate(jmprel): struct.pack_into('<QQq', img, jmprel_off + 24 * i, off, sym << 32 | typ, add)
    dyn = [(6, symtab), (5, strtab_off), (10, len(strtab)), (11, 24), (7, rela_off), (8, 24 * len(rela)), (9, 24)]
    if jmprel: dyn += [(23, jmprel_off), (2, 24 * len(jmprel)), (20, 7)]
    dyn += list(extra) + [(0, 0)]
    for i, (tag, v) in enumerate(dyn): struct.pack_into('<qQ', img, dynamic + 16 * i, tag, v)
    return strtab_off + len(strtab)


def got_load(i): return [(None, adrp(16, HF_GOT)), (None, ldr_x(16, 16, (HF_GOT + 8 * i) & 0xFFF))]   # x16 = main's GOT slot i
def lo12(a): return a & 0xFFF


def hf_text():
    """main's text: the entry, then the SDK functions hle_guestthread.c calls by name (malloc, free, nn::os's thread functions), the thread trampoline, synth_triple"""
    r = lambda i: 8 * i   # results[i]
    t = [(None, b('hf_start')), (None, HF_MOD0), (None, 0), (None, 0),
         ('hf_start', STP_FP_LR), (None, MOV_FP_SP), (None, adrp(19, HF_RESULTS)), (None, add_x(19, 19, lo12(HF_RESULTS)))]
    t += got_load(0) + [(None, adrp(0, HF_MSG)), (None, add_x(0, 0, lo12(HF_MSG))), (None, blr(16)), (None, str_x(0, 19, r(0)))]
    t += got_load(1) + [(None, adrp(9, HF_CONSTF)), (None, ldr_s(0, 9, lo12(HF_CONSTF))), (None, blr(16)), (None, str_s(0, 19, r(1)))]
    t += got_load(2) + [(None, blr(16)), (None, mov_x(20, 0)), (None, str_x(0, 19, r(12))),
                        (None, movz_x(0, 5)), (None, movz_x(1, 6)), (None, blr(20)), (None, str_x(0, 19, r(2))),
                        (None, adrp(21, HF_VT)), (None, add_x(21, 21, lo12(HF_VT))), (None, str_x(20, 21, 8)), (None, ldr_x(16, 21, 8)),
                        (None, movz_x(0, 7)), (None, movz_x(1, 8)), (None, blr(16)), (None, str_x(0, 19, r(3)))]
    t += got_load(3) + [(None, adrp(0, HF_VT2)), (None, add_x(0, 0, lo12(HF_VT2))), (None, blr(16)),
                        (None, adrp(21, HF_VT2)), (None, add_x(21, 21, lo12(HF_VT2))), (None, ldr_x(16, 21, 0)), (None, movz_x(0, 100)), (None, movz_x(1, 23)),
                        (None, blr(16)), (None, str_x(0, 19, r(4))), (None, ldr_x(16, 21, 8)), (None, movz_x(0, 9)), (None, blr(16)), (None, str_x(0, 19, r(5)))]
    t += got_load(4) + [(None, movz_x(0, 14)), (None, blr(16)), (None, str_x(0, 19, r(6)))]
    t += got_load(5) + [(None, adrp(0, HF_ROMOD)), (None, add_x(0, 0, lo12(HF_ROMOD))), (None, adrp(1, HF_NROIMG)), (None, ldr_x(1, 1, lo12(HF_NROIMG))),
                        (None, movz_x(2, 0)), (None, movz_x(3, 0)), (None, movz_x(4, 0)), (None, blr(16)), (None, str_x(0, 19, r(7)))]
    t += got_load(6) + [(None, adrp(0, HF_OUT)), (None, add_x(0, 0, lo12(HF_OUT))), (None, adrp(1, HF_ROMOD)), (None, add_x(1, 1, lo12(HF_ROMOD))),
                        (None, adrp(2, HF_NAME)), (None, add_x(2, 2, lo12(HF_NAME))), (None, blr(16)), (None, str_x(0, 19, r(8)))]
    t += got_load(6) + [(None, adrp(0, HF_OUT2)), (None, add_x(0, 0, lo12(HF_OUT2))), (None, adrp(1, HF_ROMOD)), (None, add_x(1, 1, lo12(HF_ROMOD))),
                        (None, adrp(2, HF_NAME2)), (None, add_x(2, 2, lo12(HF_NAME2))), (None, blr(16)), (None, str_x(0, 19, r(21)))]
    # The cross-pack loop: x23 = sub_fn, x24 = sub_call, x25 the pass, x26 and x27 the sums; t_hf_mark(0) after the first pass, t_hf_mark(1) after the last
    t += [(None, adrp(23, HF_OUT)), (None, ldr_x(23, 23, lo12(HF_OUT))), (None, adrp(24, HF_OUT2)), (None, ldr_x(24, 24, lo12(HF_OUT2))),
          (None, movz_x(25, 0)), (None, movz_x(26, 0)), (None, movz_x(27, 0)),
          ('hf_cross', movz_x(0, 5)), (None, blr(23)), (None, add_xr(26, 26, 0)),
          (None, adr(0, 'hf_triple')), (None, movz_x(1, 7)), (None, blr(24)), (None, add_xr(27, 27, 0)),
          (None, cbnz_x(25, 'hf_cross_next'))] + got_load(17) + [(None, movz_x(0, 0)), (None, blr(16)),
          ('hf_cross_next', add_x(25, 25, 1)), (None, cmp_x(25, HF_CROSS_PASSES)), (None, bne('hf_cross'))]
    t += got_load(17) + [(None, movz_x(0, 1)), (None, blr(16)), (None, str_x(26, 19, r(9))), (None, str_x(27, 19, r(20)))]
    t += got_load(7) + [(None, blr(16)), (None, str_x(0, 19, r(10)))]
    t += got_load(8) + [(None, adrp(0, HF_ROMOD)), (None, add_x(0, 0, lo12(HF_ROMOD))), (None, blr(16))]
    t += got_load(7) + [(None, blr(16)), (None, str_x(0, 19, r(11)))]
    # The host calling guest code back: the callback registered, then called from the host in each context
    t += got_load(9) + [(None, adr(0, 'hf_cb')), (None, adrp(1, HF_CBDATA)), (None, add_x(1, 1, lo12(HF_CBDATA))), (None, blr(16))]
    t += [(None, movz_x(22, 0x1234))] + got_load(10) + [(None, movz_x(0, 1)), (None, blr(16)), (None, str_x(0, 19, r(13))), (None, str_x(22, 19, r(14)))]
    t += got_load(13) + [(None, movz_x(0, 5)), (None, blr(16)), (None, str_x(0, 19, r(15)))]
    t += got_load(11) + [(None, movz_x(0, 10)), (None, blr(16)), (None, str_x(0, 19, r(16)))]
    t += got_load(12) + [(None, movz_x(0, 20)), (None, blr(16)), (None, str_x(0, 19, r(17)))]
    t += got_load(15) + [(None, adrp(0, HF_EV)), (None, add_x(0, 0, lo12(HF_EV))), (None, blr(16)), (None, str_x(0, 19, r(18))),
                         (None, adrp(9, HF_EV)), (None, add_x(9, 9, lo12(HF_EV))), (None, add_x(1, 9, 4)), (None, movz_w(2, 1)), (None, movz_x(3, 0)), (None, svc(0x18)),
                         (None, str_x(0, 19, r(19)))]
    # Host memory the guest sees (dm/host/aot.h): a buffer, a string and a scoped array the host hands main, all in the arena; then host memory on purpose (the arena guard)
    g = lambda i: 8 * i   # ga_results[i], x21 its base
    t += [(None, adrp(21, HF_GA_RESULTS)), (None, add_x(21, 21, lo12(HF_GA_RESULTS)))]
    ga = lambda name: got_load(HF_GOT_SYMS.index(name))   # the slot by name: the list grows at its end
    t += ga('t_ga_buf') + [(None, blr(16)), (None, str_x(0, 21, g(0))), (None, ldr_x(0, 0, 0)), (None, str_x(0, 21, g(1)))]
    t += ga('t_ga_str') + [(None, blr(16)), (None, mov_x(20, 0)), (None, str_x(0, 21, g(2)))] + got_load(0) + \
         [(None, mov_x(0, 20)), (None, blr(16)), (None, str_x(0, 21, g(3))), (None, ldrb(0, 20, 0)), (None, str_x(0, 21, g(4)))]
    t += ga('t_ga_local') + [(None, adr(0, 'hf_sum2')), (None, blr(16)), (None, str_x(0, 21, g(5)))]
    t += ga('t_ga_outside') + [(None, adr(0, 'hf_sum2')), (None, blr(16)), (None, mov_x(20, 0))] + got_load(0) + [(None, mov_x(0, 20)), (None, blr(16)), (None, str_x(0, 21, g(6)))]
    t += [(None, adrp(0, HF_MSG)), (None, add_x(0, 0, lo12(HF_MSG))), (None, movz_x(1, len(HF_MESSAGE))), (None, svc(0x27))]
    t += got_load(16) + [(None, blr(16)), (None, svc(0x07)), (None, brk(1))]
    # malloc(n): a bump allocator over bss, 16-byte steps (hf_heapptr starts at the heap: a relative relocation); free: nothing
    t += [('hf_malloc', adrp(9, HF_HEAPPTR)), (None, add_x(9, 9, lo12(HF_HEAPPTR))), (None, ldr_x(10, 9, 0)), (None, add_x(11, 0, 15)), (None, lsr_x(11, 11, 4)),
          (None, lsl_x(11, 11, 4)), (None, mov_x(0, 10)), (None, add_xr(10, 10, 11)), (None, str_x(10, 9, 0)), (None, RET)]
    t += [('hf_free', RET)]
    # nn::os::CreateThread(tt, fn, arg, stack, size, priority): fn and arg into tt, svcCreateThread(hf_tramp, tt, stack + size, priority, core -2), the handle at tt+16
    t += [('hf_create', str_x(1, 0, 0)), (None, str_x(2, 0, 8)), (None, mov_x(9, 0)), (None, add_xr(3, 3, 4)), (None, mov_w(4, 5)), (None, movn_w(5, 1)),
          (None, adr(1, 'hf_tramp')), (None, mov_x(2, 9)), (None, svc(0x08)), (None, str_w(1, 9, 16)), (None, RET)]
    t += [('hf_start_thread', ldr_w(0, 0, 16)), (None, svc(0x09)), (None, RET)]                                                # svcStartThread(handle)
    t += [('hf_wait', add_x(1, 0, 16)), (None, movz_w(2, 1)), (None, movn_x(3, 0)), (None, svc(0x18)), (None, RET)]            # svcWaitSynchronization(&handle, 1, -1)
    t += [('hf_destroy', ldr_w(0, 0, 16)), (None, svc(0x16)), (None, RET)]                                                     # svcCloseHandle(handle)
    t += [('hf_tramp', ldr_x(16, 0, 0)), (None, ldr_x(0, 0, 8)), (None, blr(16)), (None, svc(0x0A)), (None, brk(2))]           # fn(arg), then svcExitThread
    t += [('hf_triple', add_xr(1, 0, 0)), (None, add_xr(0, 1, 0)), (None, RET)]
    # hf_cb(data, a1, ..., a9): *data + a1 + ... + a9, a8 and a9 the stacked arguments at [sp] and [sp, #8]
    t += [('hf_cb', ldr_x(0, 0, 0))] + [(None, add_xr(0, 0, k)) for k in range(1, 8)] + \
         [(None, ldr_x(9, 31, 0)), (None, add_xr(0, 0, 9)), (None, ldr_x(9, 31, 8)), (None, add_xr(0, 0, 9)), (None, RET)]
    # hf_deep(x): t_re_leaf(x) + 1, t_re_leaf a host function that calls synth_triple back
    t += [('hf_deep', STP_FP_LR), (None, MOV_FP_SP)] + got_load(14) + [(None, blr(16)), (None, add_x(0, 0, 1)), (None, LDP_FP_LR), (None, RET)]
    # nn::os::CreateSystemEvent(ev, mode, interprocess): svcCreateEvent, the write handle at ev, the read handle at ev + 4, 0; GetWritableHandleOfSystemEvent(ev): the write handle
    t += [('hf_cev', mov_x(9, 0)), (None, svc(0x45)), (None, str_w(1, 9, 0)), (None, str_w(2, 9, 4)), (None, movz_w(0, 0)), (None, RET)]
    t += [('hf_gwh', ldr_w(0, 0, 0)), (None, RET)]
    # hf_exit: svcExitProcess, from a guest call the host makes inside t_re_stop
    t += [('hf_exit', svc(0x07)), (None, brk(3))]
    # A function of the module that writes v0, which nothing calls. nxaot's indirect call sites reload after the call only what the module's own functions may write (its
    # plan.cpp, indirect_writes: the union over the standard entries), while the dispatcher may hand the call to a host function, which returns a float in v0: without
    # one, sqrtf's result would be dropped for the stale v0 (found with this module; a lifter issue, reported, not the host's). Every real module has such functions
    # hf_sum2(a): a[0] + a[3], the array the host passes (t_ga_local, t_ga_outside)
    t += [('hf_sum2', ldr_x(1, 0, 0)), (None, ldr_x(2, 0, 24)), (None, add_xr(0, 1, 2)), (None, RET)]
    t += [('hf_fone', FMOV_S0_1), (None, RET), ('hf_end', None)]
    functions = [('hf_start', 'hf_start'), ('malloc', 'hf_malloc'), ('free', 'hf_free'), ('_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi', 'hf_create'),
                 ('_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE', 'hf_start_thread'), ('_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE', 'hf_wait'),
                 ('_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE', 'hf_destroy'), ('hf_thread_tramp', 'hf_tramp'), ('synth_triple', 'hf_triple'),
                 ('hf_cb', 'hf_cb'), ('hf_deep', 'hf_deep'), ('_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb', 'hf_cev'),
                 ('_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE', 'hf_gwh'), ('hf_exit', 'hf_exit'), ('hf_sum2', 'hf_sum2'), ('hf_one', 'hf_fone')]
    return t, functions


def hf_image():
    """main's image (text, read-only data, data: HF_DATA_END bytes; bss follows) and the end of its last function"""
    img = bytearray(HF_DATA_END)
    items, functions = hf_text()
    L, words = assemble(items)
    assert L['hf_end'] <= HF_TEXT_END, 'main of the host-function module outgrew its text'
    for off, w in words: struct.pack_into('<I', img, off, w)
    order = [lab for _, lab in functions] + ['hf_end']
    ranges = [(L[order[i]], L[order[i + 1]]) for i in range(len(functions))]
    undef = [(s, 0, 0x12, 0, 0) for s in HF_GOT_SYMS]
    objects = [('results', 3, 0x11, HF_RESULTS, 8 * HF_NRESULTS), ('hf_got', 3, 0x11, HF_GOT, 8 * len(HF_GOT_SYMS)), ('hf_vt', 3, 0x11, HF_VT, 32),
               ('hf_vt2', 3, 0x11, HF_VT2, 32), ('nro_image', 3, 0x11, HF_NROIMG, 8), ('ga_results', 3, 0x11, HF_GA_RESULTS, 8 * len(HF_GA_EXPECTED))]
    funcs = [(name, 1, 0x12, s, e - s) for (name, _), (s, e) in zip(functions, ranges)]
    symbols = undef + objects + funcs
    rela = [(HF_GOT + 8 * i, 1025, 1 + i, 0) for i, s in enumerate(HF_GOT_SYMS) if s not in HF_JUMP_SLOT] + [(HF_HEAPPTR, 1027, 0, HF_HEAP)]
    jmprel = [(HF_GOT + 8 * i, 1026, 1 + i, 0) for i, s in enumerate(HF_GOT_SYMS) if s in HF_JUMP_SLOT]
    hdr, frame = eh_frame_at(HF_EH_FRAME, HF_EH_HDR, ranges)
    end = dynamic_tables(img, HF_MOD0, HF_DYNAMIC, HF_SYMTAB, HF_RELA, symbols, rela, HF_JMPREL, jmprel,
                         mod0_fields=(HF_DYNAMIC, HF_DATA_END, HF_DATA_END + HF_BSS, HF_EH_HDR, HF_EH_FRAME))
    assert end <= HF_RELA and HF_RELA + 24 * len(rela) <= HF_JMPREL and HF_JMPREL + 24 * len(jmprel) <= HF_EH_HDR and HF_EH_HDR + len(hdr) <= HF_EH_FRAME and HF_EH_FRAME + len(frame) <= HF_MSG
    img[HF_EH_HDR:HF_EH_HDR + len(hdr)] = hdr
    img[HF_EH_FRAME:HF_EH_FRAME + len(frame)] = frame
    img[HF_MSG:HF_MSG + len(HF_MESSAGE)] = HF_MESSAGE
    struct.pack_into('<f', img, HF_CONSTF, HF_SQRT_IN)
    struct.pack_into('<Q', img, HF_CBDATA, HF_CBDATA_VALUE)
    img[HF_NAME:HF_NAME + 7] = b'sub_fn\0'
    img[HF_NAME2:HF_NAME2 + 9] = b'sub_call\0'
    return img, L['hf_end']


def hf_manifest(img, f_end):
    """main's manifest, through patch_nso.py's own writer: its layout, the MOD0 offset word and the gap after the last function as data, no other row"""
    data_rows, manifest = patch_nso_functions('aot_data_rows', 'aot_manifest')
    data = data_rows(img, HF_TEXT_END, [(f_end, HF_TEXT_END)])
    return manifest(NAME, HF_BUILD_ID, hashlib.sha256(img).hexdigest(), len(img), (HF_TEXT_END, HF_RO_OFF, HF_DATA_OFF, HF_BSS), data, [], [], [], [], {}, {})


def sub_nro():
    """The NRO main loads: sub_init (its DT_INIT: sub_flag = 1000), sub_fn(x) = x + strlen("sub nro") (through its own GOT) + sub_flag, sub_value() = 77,
    sub_call(fn, x) = fn(x) + 1 (a call back into the module that loaded it, through a register)"""
    items = [('sub_init', adrp(9, SUB_FLAG)), (None, movz_x(10, 1000)), (None, str_x(10, 9, lo12(SUB_FLAG))), (None, RET),
             ('sub_fn', STP_PRE(29, 30, 31, -32)), (None, str_x(19, 31, 16)), (None, MOV_FP_SP), (None, mov_x(19, 0)),
             (None, adrp(0, SUB_MSG)), (None, add_x(0, 0, lo12(SUB_MSG))), (None, adrp(16, SUB_GOT)), (None, ldr_x(16, 16, lo12(SUB_GOT))), (None, blr(16)),
             (None, add_xr(0, 0, 19)), (None, adrp(9, SUB_FLAG)), (None, ldr_x(9, 9, lo12(SUB_FLAG))), (None, add_xr(0, 0, 9)),
             (None, ldr_x(19, 31, 16)), (None, LDP_POST(29, 30, 31, 32)), (None, RET),
             ('sub_value', movz_x(0, 77)), (None, RET),
             ('sub_call', STP_FP_LR), (None, MOV_FP_SP), (None, mov_x(9, 0)), (None, mov_x(0, 1)), (None, blr(9)), (None, add_x(0, 0, 1)), (None, LDP_FP_LR), (None, RET),
             ('sub_end', None)]
    L, words = assemble(items, SUB_CODE)
    img = bytearray(SUB_DATA_END)
    for off, w in words: struct.pack_into('<I', img, off, w)
    struct.pack_into('<II', img, 0, B(0, L['sub_value']), SUB_MOD0)   # the entry branch (to a function that returns), the MOD0 offset word
    order = ['sub_init', 'sub_fn', 'sub_value', 'sub_call', 'sub_end']
    ranges = [(L[order[i]], L[order[i + 1]]) for i in range(len(order) - 1)]
    symbols = [('strlen', 0, 0x12, 0, 0), ('sub_flag', 3, 0x11, SUB_FLAG, 8)] + [(n, 1, 0x12, s, e - s) for n, (s, e) in zip(order, ranges)]
    hdr, frame = eh_frame_at(SUB_EH_FRAME, SUB_EH_HDR, ranges)
    end = dynamic_tables(img, SUB_MOD0, SUB_DYNAMIC, SUB_SYMTAB, SUB_RELA, symbols, [(SUB_GOT, 1025, 1, 0)], extra=[(12, L['sub_init'])],
                         mod0_fields=(SUB_DYNAMIC, SUB_DATA_END, SUB_DATA_END + SUB_BSS, SUB_EH_HDR, SUB_EH_FRAME))
    assert end <= SUB_RELA and SUB_EH_HDR + len(hdr) <= SUB_EH_FRAME and SUB_EH_FRAME + len(frame) <= SUB_MSG
    img[SUB_EH_HDR:SUB_EH_HDR + len(hdr)] = hdr
    img[SUB_EH_FRAME:SUB_EH_FRAME + len(frame)] = frame
    img[SUB_MSG:SUB_MSG + len(HF_SUB_MESSAGE)] = HF_SUB_MESSAGE
    # The NRO header (+0x10): magic, version, size, flags, the segments (offset, size), bss, the module ID
    struct.pack_into('<4sIII6II', img, 0x10, b'NRO0', 0, SUB_DATA_END, 0, 0, SUB_TEXT_END, SUB_RO_OFF, SUB_DATA_OFF - SUB_RO_OFF, SUB_DATA_OFF,
                     SUB_DATA_END - SUB_DATA_OFF, SUB_BSS)
    img[0x40:0x60] = bytes.fromhex('ab08' * 16)
    return bytes(img)


def write_hostfn(out):
    """<out>/main.image.bin and main.aot.tsv (the host-function module), <out>/sub.nro and <out>/nro.tsv (its row: sub, SHA-256, size)"""
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    img, f_end = hf_image()
    (out / f'{NAME}.image.bin').write_bytes(bytes(img))
    (out / f'{NAME}.aot.tsv').write_text(hf_manifest(img, f_end))
    sub = sub_nro()
    (out / 'sub.nro').write_bytes(sub)
    (out / 'nro.tsv').write_text(f'# the NROs main may load (loader.c): name, SHA-256 of the NRO, size\nsub\t{hashlib.sha256(sub).hexdigest()}\t{len(sub)}\n')
    return out


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
    if len(sys.argv) == 3 and sys.argv[1] == '--hostfn':
        o = write_hostfn(sys.argv[2])
        print(f'aot_synth: {o}/{NAME}.image.bin, {NAME}.aot.tsv (the host-function module), sub.nro ({len(sub_nro())} bytes), nro.tsv')
        sys.exit(0)
    if len(sys.argv) != 2: sys.exit('usage: aot_synth.py <out dir>\n       aot_synth.py --hostfn <out dir>')
    o = write(sys.argv[1])
    print(f'aot_synth: {o}/{NAME}.nso, {NAME}.image.bin ({len(aot_tables()["image"])} bytes), {NAME}.aot.tsv, profiles/{PROFILE}/patches.py')
