#!/usr/bin/env python3
"""Ampere instructions through the whole chain against a real Ampere GPU: each op of ampere_orin/ (recorded on a Jetson Orin, sm_87, by
dm/gfx/tools/ampere_orin/capture.py) is the GPU compiler's machine code for one PTX operation, its 1,024 input rows and the outputs the GPU computed. Here the
program goes through the translator (dmsh_xlate, arch sm86, the build's debug and relaxed-precision options), spirv-cross with the chain's arguments and the
MSL rewrites dmsh_build.py's finish() makes of every shader, is compiled with the chain's Metal flags and run on this Mac's GPU on the same inputs, and every
output word is compared with the GPU's bit for bit.

An op is expected to match except for what Metal's float arithmetic itself gives (compare.PLATFORM: NaN bits, denormals). KNOWN lists the ops that still
differ, each with its reason and where it is tracked; APPROX the ones that run Metal's own approximation of a function (bit-exact only by accident). The test
fails when an op outside those lists differs, and when a KNOWN op matches, so that its entry is taken out with the fix.

Needs DMSH_TEST_XLATE (the translator) like the other translator tests, and an Apple-silicon Mac. AMPERE_ORIN_ONLY=<ops or families> runs some;
AMPERE_ORIN_REPORT=<file> writes every op's causes and differing NaN patterns as JSON. Derived ordinary-HFMA2 programs retain the recorded oracle.
Small synthetic contracts are labelled separately from hardware recordings."""
import ast
import collections
import concurrent.futures
import gzip
import importlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / 'tools'
FIXTURE = HERE / 'ampere_orin'
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / 'ampere_orin'))
import compare  # noqa: E402
import dmsh_chain  # noqa: E402

# Ops that still differ from the GPU beyond PLATFORM: name -> why. A fix takes its ops out of here.
KNOWN = {
    # results that differ
    'i2f_s32_rz': 'I2FP.F32.S32.RZ rounds to nearest (PR #1800 has the fix)',
    'fmax_nan': 'FMNMX.NAN: a NaN operand gives the other operand, not NaN (PR #1800 has the fix)', 'fmin_nan': '(as fmax_nan)',
    'hmma_16816_f32': 'HMMA.16816.F32: the f32 accumulator does not add the products as the tensor core does (about 1 ULP)',
    # the translator refuses these
    'fdiv_rn': 'FCHK (0x302) is not implemented; its slow path calls the CUDA driver',
    'f2i_s64': 'F2I.S64 (0x311) is not implemented',
    'dadd': 'DADD (0x229) is not implemented', 'dmul': 'DMUL (0x228) is not implemented',
    'hmin2_nan': 'HMNMX2.NAN is not implemented', 'bmin2': 'HMNMX2.BF16_V2 is not implemented', 'bmax2_nan': '(as bmin2)',
    'bfma2': 'HFMA2.BF16_V2 is not implemented', 'hmma_16816_bf16': 'HMMA .BF16 is not implemented', 'hmma_1688_tf32': 'HMMA .TF32 is not implemented',
    'imma_16832_s8': 'IMMA is not implemented', 'i2i_s8_sat': 'I2I .SAT is not implemented', 'i2i_u16_sat': '(as i2i_s8_sat)',
    'iadd_sat': 'PLOP3 with .SIGN register sources (0x21f) is not implemented', 'isub_sat': '(as iadd_sat)',
    'prmt': 'PRMT modes other than the index form (.F4E .B4E .RC8 .ECL .ECR .RC16) are not implemented',
    'mufu_tanh': 'MUFU.TANH is not implemented', 'mufu_tanh_f16x2': '(as mufu_tanh)',
    # the program calls a routine of the CUDA driver's own (CALL.ABS to a relocation the JIT resolves at load), which a game's shader never does
    'frcp_rn': 'not testable: CALL.ABS into the CUDA driver', 'fsqrt_rn': '(as frcp_rn)',
}
# Ops that run Metal's own approximation (MUFU, and the divisions built on it): compared and reported, never failed
APPROX = {'mufu_ex2', 'mufu_ex2_ftz', 'mufu_lg2', 'mufu_lg2_ftz', 'mufu_rcp', 'mufu_rcp_ftz', 'mufu_rsqrt', 'mufu_rsqrt_ftz', 'mufu_sqrt', 'mufu_sqrt_ftz', 'mufu_sin',
          'mufu_cos', 'mufu_ex2_f16x2', 'fdiv_full', 'fdiv_approx',
          # HMMA with an f16 accumulator: one block of 8 rounded once (PR #1897) matches 99.97 %; the rest are 1-2 ULP where the sum nearly cancels
          'hmma_16816_f16', 'hmma_1688_f16'}

MAC_METAL = dmsh_chain.MAC_METAL


def load_fixture():
    doc = json.loads((FIXTURE / 'manifest.json').read_text())
    blob = gzip.decompress((FIXTURE / 'ampere_orin.u32.gz').read_bytes())
    import array
    for o in doc['ops']:
        o['program'] = blob[o['prog'][0]:o['prog'][0] + o['prog'][1]]
        words = array.array('I')
        words.frombytes(blob[o['rows'][0]:o['rows'][0] + o['rows'][1]])
        if sys.byteorder != 'little':
            words.byteswap()
        w = len(o['ins']) + len(o['outs'])
        o['rows_in'] = [list(words[i:i + len(o['ins'])]) for i in range(0, len(words), w)]
        o['rows_gpu'] = [list(words[i + len(o['ins']):i + w]) for i in range(0, len(words), w)]
        if o['name'] in ('f2fp_pack_bf16x2', 'f2fp_pack_f16x2_relu'):
            o['exact_bits'] = True
    # The recorded FMA uses the tensor-pipe opcode. Exercise the ordinary ALU
    # emitter too, independently of HFMA2.MMA support, retaining the Orin oracle.
    for o in list(doc['ops']):
        if o['name'] not in ('hfma2', 'hfma2_ftz', 'hfma2_relu'):
            continue
        program = half_variant(o, 0x035, clear_opcode=4)
        name = o['name'].replace('hfma2', 'hfma2_alu')
        doc['ops'].append(dict(o, name=name, program=program, derived_from=o['name']))
    doc['ops'].extend(half_ftz_boundaries(doc['ops']))
    doc['ops'].append(half_relu_boundary(doc['ops']))
    doc['ops'].extend(hfma_rounding_cases(doc['ops']))
    doc['ops'].extend(minmax_boundaries(doc['ops']))
    doc['ops'].extend(directed_rounding_boundaries(doc['ops']))
    doc['ops'].extend(i2f64_boundaries(doc['ops']))
    doc['ops'].extend(f2fp_boundaries(doc['ops']))
    doc['ops'].extend(idp_constant_buffer_cases(doc['ops']))
    doc['ops'].extend(vabsdiff_cases(doc['ops']))
    return doc


def half_variant(op, opcode, *, clear_opcode=0, modifiers=0, after=b''):
    """Change one instruction or insert its consumer, retaining the template's registers and guards."""
    program = bytearray(op['program'])
    offsets = []
    for i in range(0, len(program), 16):
        lo, hi = struct.unpack_from('<QQ', program, i)
        if lo & 0x1ff == opcode:
            struct.pack_into('<QQ', program, i, lo & ~clear_opcode, hi | modifiers)
            offsets.append(i + 16)
    if len(offsets) != 1:
        raise ValueError(f"{op['name']}: expected one opcode {opcode:#x}, got {len(offsets)}")
    end = offsets[0]
    program[end:end] = after
    return bytes(program)


def half_ftz_boundaries(ops):
    """Small semantic checks, not additional Orin recordings: min/max flush
    either input to signed zero, keep the smallest normal, and .F32 results
    keep values below the half-normal bound. No NaN or zero-tie min/max cases."""
    cases = {
        'hmin2': [([0x83ff03ff, 0x04000400], 0x80000000),
                  ([0x04000400, 0x83ff03ff], 0x80000000),
                  ([0x84000400, 0x3c003c00], 0x84000400)],
        'hmax2': [([0x83ff03ff, 0x84008400], 0x80000000),
                  ([0x84008400, 0x83ff03ff], 0x80000000),
                  ([0x84000400, 0xbc00bc00], 0x84000400)],
        'hmul2_ftz': [([0x04000400, 0x38003800], 0x38000000),
                      ([0x84008400, 0x38003800], 0xb8000000)],
    }
    out = []
    for o in ops:
        if o['name'] not in cases:
            continue
        rows = cases[o['name']]
        opcode = 0x032 if o['name'] == 'hmul2_ftz' else 0x040
        program = half_variant(o, opcode, modifiers=1 << (80 - 64) | (1 << (78 - 64) if opcode == 0x032 else 0))
        rows = (rows * 32)[:32]
        name = o['name'] + ('_f32_result' if opcode == 0x032 else '_ftz_boundary')
        out.append(dict(o, name=name, program=bytes(program), outs='f' if opcode == 0x032 else 'h', cases=32,
                        rows_in=[r for r, _ in rows], rows_gpu=[[v] for _, v in rows],
                        oracle='synthetic FTZ boundary contract', derived_from=o['name']))
    return out


def half_relu_boundary(ops):
    """HMUL2.RELU semantic contract, not an additional NVIDIA recording.
    Keep NaNs (including 0*inf), clamp negatives, retain positive subnormals."""
    op = next(o for o in ops if o['name'] == 'hmul2')
    cases = [([0x7e007e00, 0x3c003c00], 0x7fff7fff),
             ([0x00000000, 0x7c007c00], 0x7fff7fff),
             ([0x80008000, 0xfc00fc00], 0x7fff7fff),
             ([0xbc00bc00, 0x3c003c00], 0x00000000),
             ([0x3c003c00, 0x3c003c00], 0x3c003c00),
             ([0x7c00fc00, 0x3c003c00], 0x7c000000),
             ([0x03ff83ff, 0x3c003c00], 0x03ff0000),
             ([0x3c003c00, 0x00018001], 0x00010000)] * 4
    return dict(op, name='hmul2_relu_boundary', program=half_variant(op, 0x032, modifiers=1 << (79 - 64)), cases=32,
                rows_in=[r for r, _ in cases], rows_gpu=[[v] for _, v in cases],
                oracle='synthetic RELU boundary contract', derived_from=op['name'])


def hfma_rounding_cases(ops):
    by_name = {o['name']: o for o in ops}
    base = by_name['hfma2']
    # PR #1902's Orin PTX fma.rn.f16x2 recording: u32 count followed by the
    # three input columns and one result column, each containing packed half pairs.
    capture = (FIXTURE / 'hfma_orin_fixture.u32').read_bytes()
    count, = struct.unpack_from('<I', capture)
    columns = struct.unpack(f'<{4 * count}I', capture[4:])
    a, b, c, result = (columns[i:i + count] for i in range(0, len(columns), count))
    yield dict(base, name='hfma2_single_round', cases=count, rows_in=list(zip(a, b, c)),
               rows_gpu=[(word,) for word in result], derived_from='PR #1902 Orin recording')

    def case(template, suffix, rows, *, bits=0, after=b'', outs=None):
        op = by_name[template]
        opcode = 0x023 if template == 'ffma_rn' else 0x031 if '_alu' in template else 0x035
        return dict(op, name=template + suffix, cases=len(rows), outs=outs or op['outs'],
                    program=half_variant(op, opcode, modifiers=bits, after=after),
                    rows_in=[inputs for inputs, _ in rows], rows_gpu=[(word,) for _, word in rows],
                    oracle='synthetic rounding contract', derived_from=template)

    midpoint = [0xea20ea20, 0xc810c810, 0x08160816]
    relu_rows = [(midpoint, 0x76397639), ([0, 0x7c007c00, 0], 0x7fff7fff),
                 ([0x80018001, 0x38003800, 0], 0)]
    ftz_rows = [(midpoint, 0x76397639),
                ([0x84000400, 0x3bff3bff, 0], 0x84000400),   # the midpoint below the smallest normal rounds up
                ([0x84000400, 0x38003800, 0], 0x80000000),   # a rounded subnormal flushes with sign
                ([0x80010001, 0x7c007c00, 0], 0x7fff7fff)]  # input flush gives 0 * inf
    for stem in ('hfma2', 'hfma2_alu'):
        yield case(stem + '_ftz', '_round', ftz_rows)
        yield case(stem + '_relu', '_round', relu_rows)
        yield case(stem + '_relu', '_ftz_round', [relu_rows[0], relu_rows[1], ([0x84000400, 0x38003800, 0], 0)],
                   bits=1 << (80 - 64))

    # All templates write R7 to shared memory. The inserted consumers also write R7.
    pack = struct.pack('<QQ', 0x000000070707723e, 0x000fe20000000000)    # F2FP.PACK_AB R7, R7, R7
    unpack = struct.pack('<QQ', 0x0000000700077304, 0x003fe20000101000)  # F2F.F32.F16 R7, R7
    yield case('hfma2_alu', '_unpack', [(midpoint, 0x46c72000)], after=unpack, outs='f')
    yield case('hfma2_alu', '_f32', [(midpoint, 0x46c71000)], bits=1 << (78 - 64), outs='f')
    yield case('hfma2_alu', '_f32_pack', [(midpoint, 0x76387638)], bits=1 << (78 - 64), after=pack)
    # .F32 source A: 1 + 2^-12 multiplied by 1, plus 2^-11. Narrowing A first would give 0x3c00.
    yield case('hfma2_alu', '_f32_source', [([0x3f800800, 0x3c003c00, 0x10001000], 0x3c013c01)], bits=1 << (74 - 64))
    yield case('ffma_rn', '_pack', [([0xc5440000, 0xc1020000, 0x3902c000], 0x76387638)], after=pack, outs='h')


def minmax_boundaries(ops):
    """Synthetic contracts for signed zeros, numeric/NaN selection, half FTZ,
    and Maxwell's constant and register predicates. No additional GPU recordings."""
    by_name = {o['name']: o for o in ops}
    for template in ('fmin', 'fmax', 'hmin2', 'hmax2'):
        op = by_name[template]
        half = template.startswith('h')
        sign, one, nan = (0x8000, 0x3c00, 0x7e00) if half else (0x80000000, 0x3f800000, 0x7fc00000)
        rows = [(0, 0, 0, 0), (0, sign, sign, 0), (sign, 0, sign, 0), (sign, sign, sign, sign),
                (one, one | sign, one | sign, one), (one | sign, one, one | sign, one),
                (nan, one, one, one), (one, nan, one, one), (nan, nan, nan, nan)]
        pack = (lambda v: v * 0x10001) if half else (lambda v: v)
        is_min = template in ('fmin', 'hmin2')
        for ftz in ((False, True) if half else (False,)):
            cases = rows + ([(1, 0x8001, sign, 0), (0x8001, 1, sign, 0),
                             (0x8001, 0x8001, sign, sign), (1, 1, 0, 0)] if ftz else [])
            yield dict(op, name=template + ('_ftz' if ftz else '') + '_zero_boundary', cases=len(cases),
                       program=half_variant(op, 0x040, modifiers=1 << 16) if ftz else op['program'],
                       rows_in=[[pack(a), pack(b)] for a, b, _, _ in cases],
                       rows_gpu=[[pack(lo if is_min else hi)] for _, _, lo, hi in cases],
                       oracle='synthetic min/max contract', derived_from=template)
        if half:
            continue
        for predicate in (('constant', 'register') if is_min else ('constant',)):
            cases = [(a, b, take_min, lo if take_min else hi)
                     for a, b, lo, hi in rows for take_min in ((is_min,) if predicate == 'constant' else (False, True))]
            yield dict(op, name='maxwell_' + template + '_' + predicate, arch='sm50', ins='fff',
                       program=maxwell_minmax(is_min, predicate == 'register'), cases=len(cases),
                       rows_in=[[a, b, 0x3f800000 if take_min else 0] for a, b, take_min, _ in cases],
                       rows_gpu=[[expected] for _, _, _, expected in cases],
                       oracle='synthetic Maxwell min/max contract', derived_from=template)


def maxwell_minmax(is_min, register_predicate):
    """FMNMX R3, R1, R2, P0 or [!]PT; R1/R2/P0 come from shared input.
    R8 is the lane's four-word row, with R3 stored in its last word."""
    fmnmx = 0x5c60000000070000 | 3 | 1 << 8 | 2 << 20
    if not register_predicate:
        fmnmx |= 7 << 39 | (0 if is_min else 1 << 42)
    body = [0xf0c8000002170009,                                     # S2R R9, SR_TID.X
            0x3848000000070000 | 8 | 9 << 8 | 4 << 20,             # SHL R8, R9, 4
            0xef4c000000070001 | 8 << 8,                            # LDS R1, [R8]
            0xef4c000000070002 | 8 << 8 | 4 << 20,                 # LDS R2, [R8 + 4]
            0xef4c000000070004 | 8 << 8 | 8 << 20,                 # LDS R4, [R8 + 8]
            0x5bb0000000070007 | 4 << 8 | 255 << 20 | 0xd << 48 | 7 << 39,  # FSETP.NEU.AND P0, PT, R4, RZ, PT
            fmnmx,
            0xef5c000000070003 | 8 << 8 | 12 << 20,                # STS [R8 + 12], R3
            0xe30000000007000f]                                    # EXIT
    return b''.join(struct.pack('<4Q', 0x001f8000fc0007e0, *(tuple(body[i:i + 3]) + (0x50b0000000070f00,) * 2)[:3])
                    for i in range(0, len(body), 3))


def directed_rounding_boundaries(ops):
    """Exact synthetic binary32 boundaries, separate from the Orin recording."""
    by_name = {o['name']: o for o in ops}

    def case(template, suffix, rows, *, instruction=None):
        op = by_name[template]
        if instruction is not None:
            offset, lo, hi = instruction
            program = bytearray(op['program'])
            struct.pack_into('<QQ', program, offset, lo, hi)
            op = dict(op, program=bytes(program))
        return dict(op, name=template + '_boundary_' + suffix, cases=len(rows), exact_bits=True,
                    rows_in=[a for a, _ in rows], rows_gpu=[[r] for _, r in rows],
                    oracle='synthetic directed rounding contract', derived_from=template)

    def instruction(template, opcode):
        matches = [(i * 16, lo, hi) for i, (lo, hi) in enumerate(struct.iter_unpack('<QQ', by_name[template]['program']))
                   if lo & 0x1ff == opcode]
        ins, = matches
        return ins

    for mode, code in (('rm', 1), ('rp', 2), ('rz', 3)):
        add, mul, fma = (name + '_' + mode for name in ('fadd', 'fmul', 'ffma'))
        # Near FLT_MAX, rounded sum minus the smaller addend overflows in unscaled TwoSum.
        pos = 0x7f7ffffe if mode == 'rp' else 0x7f7ffffd
        neg = 0xff7ffffe if mode == 'rm' else 0xff7ffffd
        yield case(add, 'finite_sum', [([0xf3c00000, 0x7f7fffff], pos), ([0x7f7fffff, 0xf3c00000], pos),
                                      ([0x73c00000, 0xff7fffff], neg), ([0xff7fffff, 0x73c00000], neg)])

        offset, lo, hi = instruction(mul, 0x020)
        # D2 rescues an overflowing unscaled product and M8 rescues an underflowing one.
        d2_pos = 0x3f000003 if mode == 'rp' else 0x3f000002
        d2_neg = 0xbf000003 if mode == 'rm' else 0xbf000002
        yield case(mul, 'd2', [([0x7f000000, 0x40000000], 0x7f000000), ([0xff000000, 0x40000000], 0xff000000),
                               ([0x3f800001, 0x3f800001], d2_pos), ([0xbf800001, 0x3f800001], d2_neg)],
                   instruction=(offset, lo, hi & ~(7 << 20) | 3 << 20))
        yield case(mul, 'm8', [([0x00800000, 0x3e000000], 0x00800000), ([0x80800000, 0x3e000000], 0x80800000)],
                   instruction=(offset, lo, hi & ~(7 << 20) | 7 << 20))

        # Use the trig literal as an ordinary multiply. The next instruction stores it.
        offset, lo, hi = instruction('fmul_by_imm', 0x020)
        imm_rows = {'rm': ([0xc0400000], 0xbef47645), 'rp': ([0x3fc00000], 0x3e747645), 'rz': ([0x3fa00000], 0x3e4bb7e3)}
        yield case('fmul_by_imm', mode, [imm_rows[mode]],
                   instruction=(offset, lo & 0xffffffff | 0x3e22f983 << 32, hi & ~(3 << 14) | code << 14))

        # a*b overflows separately, but a*b+c is 2^127 + 2^104 - 2^81.
        # Also check exact cancellation and a fused result one tiny step above it.
        yield case(fma, 'cancellation', [
            ([0x7f7fffff, 0x3f800001, 0xff000000], 0x7f000001 if mode == 'rp' else 0x7f000000),
            ([0x3f800000, 0x3f800000, 0xbf800000], 0x80000000 if mode == 'rm' else 0),
            ([0x3f800001, 0x3f800001, 0xbf800002], 0x28800000),
        ])

    for name, opcode, rows in (
        ('fadd_rm', 0x021, [([1, 0], 0), ([0x80000001, 0], 0x80000000)]),
        ('fmul_rm', 0x020, [([0x80000001, 0x3f800000], 0x80000000), ([0x80800000, 0x3f000000], 0x80000000)]),
        ('ffma_rm', 0x023, [([0, 0x3f800000, 1], 0), ([0, 0x3f800000, 0x80000001], 0x80000000)]),
    ):
        offset, lo, hi = instruction(name, opcode)
        yield case(name, 'ftz', rows, instruction=(offset, lo, hi | 1 << 16))


def i2f64_boundaries(ops):
    """Exact binary32 outputs for zero, rounding ties, word carries and integer limits."""
    template = next(op for op in ops if op['name'] == 'i2f_s64')
    ((offset, lo, hi),) = [
        (i * 16, lo, hi) for i, (lo, hi) in enumerate(struct.iter_unpack('<QQ', template['program']))
        if lo & 0xfff == 0x312
    ]
    # Each row is the input followed by the expected RN, RM, RP and RZ bits.
    common = [
        (0,                 0,          0,          0,          0),
        (1,                 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000),
        (0x01000001,        0x4b800000, 0x4b800000, 0x4b800001, 0x4b800000),
        (0x01000003,        0x4b800002, 0x4b800001, 0x4b800002, 0x4b800001),
        (0xffffffff,        0x4f800000, 0x4f7fffff, 0x4f800000, 0x4f7fffff),
        (0x1_00000000,      0x4f800000, 0x4f800000, 0x4f800000, 0x4f800000),
        (0x1_00000001,      0x4f800000, 0x4f800000, 0x4f800001, 0x4f800000),
        # Set just the low word's sticky bit to move from a tie to above the midpoint.
        (0x800000_80000000, 0x5b000000, 0x5b000000, 0x5b000001, 0x5b000000),
        (0x800000_80000001, 0x5b000001, 0x5b000000, 0x5b000001, 0x5b000000),
    ]
    signed_cases = [
        (-1,         0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000),
        (-2,         0xc0000000, 0xc0000000, 0xc0000000, 0xc0000000),
        (-0x1000001, 0xcb800000, 0xcb800001, 0xcb800000, 0xcb800000),
        (-(1 << 31), 0xcf000000, 0xcf000000, 0xcf000000, 0xcf000000),
        (-(1 << 32), 0xcf800000, 0xcf800000, 0xcf800000, 0xcf800000),
        (-(1 << 63), 0xdf000000, 0xdf000000, 0xdf000000, 0xdf000000),
        ((1 << 63) - 1, 0x5f000000, 0x5effffff, 0x5f000000, 0x5effffff),
    ]
    unsigned_cases = [
        (1 << 63,       0x5f000000, 0x5f000000, 0x5f000000, 0x5f000000),
        ((1 << 63) + 1, 0x5f000000, 0x5f000000, 0x5f000001, 0x5f000000),
        ((1 << 64) - 1, 0x5f800000, 0x5f7fffff, 0x5f800000, 0x5f7fffff),
    ]
    for signed, extra in ((True, signed_cases), (False, unsigned_cases)):
        rows = common + extra
        kind = 's64' if signed else 'u64'
        inputs = [[row[0] & 0xffffffff, (row[0] >> 32) & 0xffffffff] for row in rows]
        for mode, suffix in enumerate(('rn', 'rm', 'rp', 'rz')):
            program = bytearray(template['program'])
            modifiers = (hi & ~((3 << 14) | (1 << 10))) | (mode << 14) | (signed << 10)
            struct.pack_into('<QQ', program, offset, lo, modifiers)
            yield dict(template, name=f'i2f_{kind}_boundary_{suffix}', program=bytes(program), cases=len(rows),
                       rows_in=inputs, rows_gpu=[[row[mode + 1]] for row in rows], exact_bits=True,
                       oracle='synthetic integer-to-binary32 contract', derived_from=template['name'])


def f2fp_boundaries(ops):
    """Derived PACK_AB forms use exact packed outputs from the conversion contract."""
    by_name = {o['name']: o for o in ops}

    def case(template, suffix, modifiers, rows):
        op = by_name[template]
        return dict(op, name=template + '_' + suffix, program=half_variant(op, 0x03e, modifiers=modifiers),
                    cases=len(rows), rows_in=[inputs for inputs, _ in rows], rows_gpu=[[packed] for _, packed in rows],
                    exact_bits=True, oracle='synthetic F2FP packing contract', derived_from=template)

    bf16 = 'f2fp_pack_bf16x2'
    rz = 3 << 15    # bits 79..80 of the full instruction
    relu = 1 << 11  # bit 75
    # Each row has its input pair, nearest-even pack and toward-zero pack.
    bf16_rows = [
        ([0x3f808000, 0x3f818000], 0x3f803f82, 0x3f803f81),  # midpoint with even and odd retained bit
        ([0x3f807fff, 0x3f808001], 0x3f803f81, 0x3f803f80),  # immediately below and above a midpoint
        ([0xbf818000, 0x00008001], 0xbf820001, 0xbf810000),
        ([0x007f8000, 0x00008000], 0x00800000, 0x007f0000),  # subnormal rounding at both ends
        ([0x7f7fffff, 0xff7fffff], 0x7f80ff80, 0x7f7fff7f),
        ([0x80000000, 0xff800001], 0x80007fff, 0x80007fff),
    ]
    bf16_relu_rows = [
        ([0x3f818000, 0x80000000], 0x3f820000, 0x3f810000),
        ([0xff800001, 0xff800000], 0x7fff0000, 0x7fff0000),
        ([0x7f800000, 0xbf800000], 0x7f800000, 0x7f800000),
        ([0x80008001, 0x00008001], 0x00000001, 0x00000000),
        ([0x7fc00001, 0x3f808000], 0x7fff3f80, 0x7fff3f80),
    ]
    for name, modifier, rows in (('ties', 0, bf16_rows), ('relu', relu, bf16_relu_rows)):
        yield case(bf16, name + '_rn', modifier, [(values, near) for values, near, _ in rows])
        yield case(bf16, name + '_rz', modifier | rz, [(values, trunc) for values, _, trunc in rows])

    yield case('f2fp_pack_f16x2', 'boundary_rz', rz, [
        ([0x3f803000, 0xbf803000], 0x3c01bc01),
        ([0x33800000, 0xb37fffff], 0x00018000),
        ([0x387fffff, 0xb8800000], 0x03ff8400),
        ([0x7f7fffff, 0xc7800000], 0x7bfffbff),
        ([0x7f800000, 0xff800000], 0x7c00fc00),
        ([0x00000000, 0x80000000], 0x00008000),
    ])
    yield case('f2fp_pack_f16x2_relu', 'boundary_rz', rz, [
        ([0x3f803000, 0xbf803000], 0x3c010000),
        ([0xff800001, 0x7fc00001], 0x7fff7fff),
        ([0x7f800000, 0xff800000], 0x7c000000),
        ([0x33800000, 0x80000000], 0x00010000),
    ])


def alu_cbuf_variant(op, opcode, source, bank, offset, *, modifiers=0):
    """Change a register ALU instruction to read its B or C from a fixed constant-buffer offset."""
    program = bytearray(op['program'])
    (position, lo, hi), = [(i * 16, lo, hi) for i, (lo, hi) in enumerate(struct.iter_unpack('<QQ', program))
                           if lo & 0x1ff == opcode]
    form = 5 if source == 'b' else 3
    if source == 'c':
        hi = (hi & ~0xff) | ((lo >> 32) & 0xff)  # B moves into the register field formerly used by C.
    lo = (lo & 0xffffffff & ~(7 << 9)) | (form << 9) | (bank << 54) | (offset << 38)
    struct.pack_into('<QQ', program, position, lo, hi | modifiers)
    return bytes(program)


def idp_constant_buffer_cases(ops):
    """Bind a nonzero word at c[3][5] and exercise its use in either operand slot."""
    by_name = {op['name']: op for op in ops}
    bank, offset = 3, 20
    rows = [[0x80017fff, 0x807f0201, 0xfffffff0],
            [0xfffffffe, 0xff7f8001, 0x7fffffff],
            [0x00010002, 0x807ffe01, 0x80000000],
            [0x80808080, 0x00000000, 0xabcdef02]]

    def expected(row, width, signed_a, signed_b, first_byte):
        a, b, c = row
        parts_a = struct.unpack({(1, False): '<4B', (1, True): '<4b', (2, False): '<2H', (2, True): '<2h'}[width, signed_a],
                                a.to_bytes(4, 'little'))
        parts_b = struct.unpack('<4b' if signed_b else '<4B', b.to_bytes(4, 'little'))
        return (c + sum(x * y for x, y in zip(parts_a, parts_b[first_byte:]))) & 0xffffffff

    for name, source, value, width, signed_a, signed_b, first_byte in (
        ('dp4a_su', 'b', 0x807f02fe, 1, True, False, 0),
        ('dp4a_s32', 'c', 0x7ffffffe, 1, True, True, 0),
        ('dp2a_hi', 'b', 0xf18102fe, 2, False, False, 2),
        ('dp2a_lo', 'c', 0xfffffff0, 2, True, True, 0),
    ):
        template = by_name[name]
        resolved = [row.copy() for row in rows]
        for row in resolved:
            row[1 if source == 'b' else 2] = value
        yield dict(template, name=f'{name}_cbuf_{source}', program=alu_cbuf_variant(template, 0x026, source, bank, offset),
                   cases=len(rows), rows_in=rows,
                   rows_gpu=[[expected(row, width, signed_a, signed_b, first_byte)] for row in resolved],
                   cbuf=(bank, offset, value), exact_bits=True, derived_from=name, oracle='synthetic IDP constant-buffer contract')


def vabsdiff_cases(ops):
    """Compare full-width signed and unsigned distances, including sums that wrap."""
    template = next(op for op in ops if op['name'] == 'isad')
    rows = [[0x80000000, 0x7fffffff, 1],
            [0x7fffffff, 0x80000000, 0],
            [0xffffffff, 0x00000000, 1],
            [0x00000000, 0x80000000, 1],
            [0x80000000, 0xffffffff, 0x80000003],
            [0xffffffff, 0x7fffffff, 0x80000000],
            [0x7fffffff, 0x00000001, 0xffffffff],
            [0xffffffff, 0xffffffff, 0xabcdef01]]

    def case(suffix, program, signed, resolved, **extra):
        expected = []
        for a, b, c in resolved:
            if signed:
                a, b = struct.unpack('<ii', struct.pack('<II', a, b))
            expected.append([(abs(a - b) + c) & 0xffffffff])
        return dict(template, name='isad_' + suffix, program=program, cases=len(rows), rows_in=rows,
                    rows_gpu=expected, exact_bits=True, derived_from=template['name'], oracle='synthetic VABSDIFF contract', **extra)

    signed_bit = 1 << (73 - 64)
    yield case('u32_edges', template['program'], False, rows)
    yield case('s32_edges', half_variant(template, 0x014, modifiers=signed_bit), True, rows)
    bank, offset = 3, 20
    for source, value, signed in (('b', 0x7fffffff, True), ('c', 0xffffffff, False)):
        resolved = [row.copy() for row in rows]
        for row in resolved:
            row[1 if source == 'b' else 2] = value
        program = alu_cbuf_variant(template, 0x014, source, bank, offset, modifiers=signed_bit if signed else 0)
        yield case('cbuf_' + source, program, signed, resolved, cbuf=(bank, offset, value))


def chain_rewrites():
    """the MSL rewrites dmsh_build.py's finish() makes of every shader, in its order: each `src = f(src)` at its top level or in a try there (the ones under a
    stage's `if` are another stage's). The rewrites live in importable modules;
    importing dmsh_build itself would start a game build."""
    path = TOOLS / 'dmsh_build.py'
    tree = ast.parse(path.read_text())
    xc = next(n for n in tree.body if isinstance(n, ast.Assign) and any(getattr(t, 'id', None) == 'XC' for t in n.targets))
    scope = {'dmsh_chain': dmsh_chain}
    exec(compile(ast.Module(body=[xc], type_ignores=[]), str(path), 'exec'), scope)
    finish = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'finish')
    steps = []

    def visit(body):
        for st in body:
            if isinstance(st, ast.Try):
                visit(st.body)
            elif (isinstance(st, ast.Assign) and len(st.targets) == 1 and getattr(st.targets[0], 'id', None) == 'src' and isinstance(st.value, ast.Call)
                  and len(st.value.args) == 1 and getattr(st.value.args[0], 'id', None) == 'src' and not st.value.keywords):
                steps.append(ast.unparse(st.value.func))
    visit(finish.body)
    fns = []
    for name in steps:
        mod, _, attr = name.rpartition('.')
        fns.append((name, getattr(importlib.import_module(mod), attr)))
    return fns, scope['XC']


def harness(msl, w):
    """main0 seeds each thread's W words of the shader's shared array from buffer 30 and writes them back where it returns"""
    i = msl.index('kernel void main0(')
    sig = msl[i:msl.index(')', i)]
    lid = re.search(r'uint (\w+) \[\[thread_index_in_threadgroup\]\]', sig)
    lid3 = re.search(r'uint3 (\w+) \[\[thread_position_in_threadgroup\]\]', sig)
    tg = re.search(r'uint3 (\w+) \[\[threadgroup_position_in_grid\]\]', sig)
    add = ['device uint* dm_io [[buffer(30)]]']
    if lid:
        lid_e = lid.group(1)
    elif lid3:
        lid_e = lid3.group(1) + '.x'
    else:
        add.append('uint dm_lid [[thread_index_in_threadgroup]]')
        lid_e = 'dm_lid'
    if tg:
        tg_e = tg.group(1) + '.x'
    else:
        add.append('uint3 dm_tg [[threadgroup_position_in_grid]]')
        tg_e = 'dm_tg.x'
    rest = msl[i + len('kernel void main0('):]
    msl = msl[:i] + 'kernel void main0(' + ', '.join(add) + (', ' if not rest.lstrip().startswith(')') else '') + rest
    start = msl.index('{', msl.index(')', msl.index('kernel void main0('))) + 1
    end = msl.rindex('}')
    body = msl[start:end]
    shm = re.search(r'threadgroup spvUnsafeArray<uint, (\d+)> (\w+);|threadgroup uint (\w+)\[(\d+)\];', body)
    if not shm:
        raise ValueError('no shared array in main0')
    arr, words = shm.group(2) or shm.group(3), int(shm.group(1) or shm.group(4))
    if words < 32 * w:
        raise ValueError(f'shared array of {words} words, {32 * w} needed')
    base = f'(({tg_e}) * 32u + ({lid_e})) * {w}u'
    dump = f'for (uint dm_k = 0; dm_k < {w}u; dm_k++) dm_io[{base} + dm_k] = {arr}[({lid_e}) * {w}u + dm_k];'
    body = (body[:shm.end()] + f'\n    for (uint dm_k = 0; dm_k < {w}u; dm_k++) {arr}[({lid_e}) * {w}u + dm_k] = dm_io[{base} + dm_k];\n'
            + body[shm.end():])
    body = re.sub(r'\breturn;', '{ ' + dump + ' return; }', body).rstrip() + '\n    ' + dump + '\n'
    return msl[:start] + body + msl[end:]


RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
// run <metallib> <io> <groups> [cbuf]: main0 over groups of 32 threads, io at buffer 30 and optional constants at argument buffer 0, id 0.
int main(int argc, char **argv) { @autoreleasepool {
  id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
  if (!d) { fprintf(stderr, "no Metal device\n"); return 1; }
  id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e]; if (!lib) { fprintf(stderr, "library: %s\n", e.description.UTF8String); return 1; }
  id<MTLFunction> f = [lib newFunctionWithName:@"main0"]; if (!f) { fprintf(stderr, "no main0\n"); return 1; }
  id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:f error:&e]; if (!ps) { fprintf(stderr, "pipeline: %s\n", e.description.UTF8String); return 1; }
  NSData *in = [NSData dataWithContentsOfFile:@(argv[2])]; if (!in) { fprintf(stderr, "no input\n"); return 1; }
  id<MTLBuffer> b = [d newBufferWithBytes:in.bytes length:in.length options:MTLResourceStorageModeShared];
  id<MTLBuffer> zero = [d newBufferWithLength:65536 options:MTLResourceStorageModeShared];
  id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
  if (!q || !cb || !ce || !b || !zero) { fprintf(stderr, "Metal objects\n"); return 1; }
  [ce setComputePipelineState:ps];
  for (int i = 0; i < 30; i++) [ce setBuffer:zero offset:0 atIndex:i];
  [ce setBuffer:b offset:0 atIndex:30];
  if (argc == 5) {
    NSData *data = [NSData dataWithContentsOfFile:@(argv[4])];
    id<MTLArgumentEncoder> args = [f newArgumentEncoderWithBufferIndex:0];
    if (!data || !args) { fprintf(stderr, "constant buffer layout\n"); return 1; }
    id<MTLBuffer> constants = [d newBufferWithBytes:data.bytes length:data.length options:MTLResourceStorageModeShared];
    id<MTLBuffer> table = [d newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
    if (!constants || !table) { fprintf(stderr, "constant buffer allocation\n"); return 1; }
    [args setArgumentBuffer:table offset:0];
    [args setBuffer:constants offset:0 atIndex:0];
    [ce useResource:constants usage:MTLResourceUsageRead];
    [ce setBuffer:table offset:0 atIndex:0];
  }
  [ce dispatchThreadgroups:MTLSizeMake(atoi(argv[3]), 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
  if (cb.error) { fprintf(stderr, "gpu: %s\n", cb.error.description.UTF8String); return 1; }
  [[NSData dataWithBytes:b.contents length:in.length] writeToFile:@(argv[2]) atomically:YES]; return 0; } }
'''


def run_op(o, work, runner, steps, xc, env):
    """('ok', metal rows) or (stage that failed, its message)"""
    import array
    import dmsh_geometry
    d = work / o['name']
    d.mkdir()
    (d / 'k.bin').write_bytes(o['program'])
    w = len(o['ins']) + len(o['outs'])
    arch = o.get('arch', 'sm86')
    job = [dict(key='k', stage='cs', arch=arch, code=str(d / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, reduced=True,
                shared=32 * w * 4)]
    if arch == 'sm86':
        job[0]['nvn2_cs'] = [0, 0, 3, 0x40, 0]
    (d / 'jobs.json').write_text(json.dumps(job))
    r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(d / 'jobs.json'), str(d)], capture_output=True, text=True, env=env)
    if r.returncode or not (d / 'k.spv').exists():
        return 'translator', next((ln for ln in (r.stdout + r.stderr).splitlines() if '[RED]' in ln), (r.stdout + r.stderr).strip()[-300:])
    dmsh_geometry.flatten_sets(d / 'k.spv')
    r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), *xc, str(d / 'k.spv')], capture_output=True, text=True)
    if r.returncode:
        return 'spirv-cross', r.stderr.strip()[-300:]
    src = r.stdout
    try:
        for _, fn in steps:
            src = fn(src)
        src = harness(src, w)
    except Exception as e:
        return 'MSL rewrite', f'{type(e).__name__}: {e}'
    (d / 'k.metal').write_text(src)
    r = subprocess.run(MAC_METAL + [str(d / 'k.metal'), '-o', str(d / 'k.air')], capture_output=True, text=True)
    if r.returncode:
        return 'Metal compile', r.stderr.strip()[-300:]
    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 'k.air'), '-o', str(d / 'k.metallib')], check=True, capture_output=True)
    groups = (len(o['rows_in']) + 31) // 32
    io = array.array('I', [0xcdcdcdcd]) * (groups * 32 * w)
    for c, row in enumerate(o['rows_in']):
        io[c * w:c * w + len(row)] = array.array('I', row)
    (d / 'io.bin').write_bytes(io.tobytes())
    args = [str(runner), str(d / 'k.metallib'), str(d / 'io.bin'), str(groups)]
    if 'cbuf' in o:
        bank, offset, value = o['cbuf']
        cbufs = json.loads((d / 'k.res.json').read_text())['cbufs']
        if [(c['slot'], c.get('fixed')) for c in cbufs] != [(bank, offset + 4)]:
            return 'reflection', f'expected c[{bank}] through byte {offset + 4}, got {cbufs}'
        data = bytearray(65536)
        struct.pack_into('<I', data, offset, value)
        (d / 'cbuf.bin').write_bytes(data)
        args.append(str(d / 'cbuf.bin'))
    r = subprocess.run(args, capture_output=True, text=True, timeout=120)
    if r.returncode:
        return 'Metal run', r.stderr.strip()[-300:]
    out = array.array('I')
    out.frombytes((d / 'io.bin').read_bytes())
    return 'ok', [list(out[c * w + len(o['ins']):(c + 1) * w]) for c in range(len(o['rows_in']))]


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs a Mac with Xcode')
class AgainstAmpere(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = load_fixture()
        only = set(filter(None, os.environ.get('AMPERE_ORIN_ONLY', '').split(',')))
        ops = [o for o in cls.doc['ops'] if not only or o['name'] in only or o['family'] in only]
        steps, xc = chain_rewrites()
        cls.tmp = tempfile.TemporaryDirectory(prefix='ampere_orin_')
        work = Path(cls.tmp.name)
        (work / 'run.m').write_text(RUNNER)
        subprocess.run(['clang', '-fobjc-arc', '-O1', str(work / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(work / 'run')], check=True)
        env = dict(os.environ)
        with concurrent.futures.ThreadPoolExecutor(max(2, (os.cpu_count() or 4) - 2)) as ex:
            futs = {o['name']: ex.submit(run_op, o, work, work / 'run', steps, xc, env) for o in ops}
        cls.results = {}
        for o in ops:
            stage, got = futs[o['name']].result()
            if stage == 'ok':
                causes, lanes, worst, examples = compare.classify(o, o['rows_in'], o['rows_gpu'], got)
                patterns = collections.Counter()
                for expected, actual in zip(o['rows_gpu'], got):
                    for kind, a, b in zip(o['outs'], expected, actual):
                        if kind == 'u':
                            continue
                        for (av, k), (bv, _) in zip(compare.lanes(a, kind), compare.lanes(b, kind)):
                            if av != bv and compare.is_nan(av, k) and compare.is_nan(bv, k):
                                patterns[(f'{av:#x}', f'{bv:#x}')] += 1
                cls.results[o['name']] = dict(stage='ok', lanes=lanes, causes=dict(causes), worst_ulp=worst, examples=examples,
                                             nan_patterns=[dict(gpu=a, metal=b, lanes=n) for (a, b), n in sorted(patterns.items())])
            else:
                cls.results[o['name']] = dict(stage=stage, message=got)
            if 'derived_from' in o:
                cls.results[o['name']]['derived_from'] = o['derived_from']
            if 'oracle' in o:
                cls.results[o['name']]['oracle'] = o['oracle']
        cls.ops = ops
        if os.environ.get('AMPERE_ORIN_REPORT'):
            Path(os.environ['AMPERE_ORIN_REPORT']).write_text(json.dumps({'device': cls.doc['device'], 'rewrites': [n for n, _ in steps], 'ops': cls.results}, indent=1))
        cls.print_summary()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @classmethod
    def print_summary(cls):
        lines = [f"\nAmpere ops against the {cls.doc['device'].get('gpu', 'GPU')} (sm_{cls.doc['device'].get('sm', '?')}, CUDA driver "
                 f"{cls.doc['device'].get('cuda_driver', '?')}): lanes that differ, by cause"]
        for o in cls.ops:
            r = cls.results[o['name']]
            tag = 'known' if o['name'] in KNOWN else 'approx' if o['name'] in APPROX else ''
            if r['stage'] != 'ok':
                lines.append(f"  {o['name']:22s} {tag:6s} {r['stage']} failed: {r['message'][:120]}")
                continue
            diff = sum(r['causes'].values())
            cs = ', '.join(f'{k} {v}' for k, v in sorted(r['causes'].items(), key=lambda kv: -kv[1]))
            lines.append(f"  {o['name']:22s} {tag:6s} {diff:5d} of {r['lanes']:6d}" + (f'  ({cs})' if cs else '') + (f"  max {r['worst_ulp']} ULP" if r['worst_ulp'] else '')
                         + ('  [synthetic contract]' if 'oracle' in o else ''))
        print('\n'.join(lines), file=sys.stderr)

    def test_matches_the_gpu(self):
        for o in self.ops:
            n, r = o['name'], self.results[o['name']]
            with self.subTest(op=n):
                beyond = {k: v for k, v in r.get('causes', {}).items() if k not in compare.PLATFORM}
                if o.get('exact_bits'):
                    beyond = r.get('causes', {})
                if n in APPROX:
                    self.assertEqual(r['stage'], 'ok', r.get('message'))
                elif n in KNOWN:
                    self.assertTrue(r['stage'] != 'ok' or beyond, f'{n} now matches the GPU: take it out of KNOWN ({KNOWN[n]})')
                else:
                    self.assertEqual(r['stage'], 'ok', f"{n}: {r['stage']} failed: {r.get('message')}")
                    self.assertFalse(beyond, f"{n} differs from the GPU: {beyond}; for example {r['examples'][:2]}")

    def test_every_listed_op_is_recorded(self):   # a KNOWN or APPROX entry for an op the fixture lacks is a typo
        names = {o['name'] for o in self.doc['ops']}
        self.assertFalse((set(KNOWN) | APPROX) - names)


class NaNClassification(unittest.TestCase):
    def test_nan_payloads_do_not_hide_nan_versus_infinity(self):
        op = dict(name='hmul2_ftz', ins='hh', outs='h')
        causes, lanes, _, _ = compare.classify(op, [[0x03ff03ff, 0x7c007c00]], [[0x7fff7fff]], [[0x7e007c00]])
        self.assertEqual(lanes, 2)
        self.assertEqual(causes, {'nan-bits': 1, 'nan-one': 1})
        self.assertNotIn('nan-one', compare.PLATFORM)


if __name__ == '__main__':
    unittest.main()
