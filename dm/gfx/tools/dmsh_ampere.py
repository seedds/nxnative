#!/usr/bin/env python3
"""Switch 2 (NVN2) shaders: what the build reads of their control section. The code itself is Ampere machine code, which the translator decodes and emits
natively (dmsh_xlate job arch sm86: Ryujinx.Graphics.Shader's Decoders/DecoderAmpere.cs and Instructions/InstEmitAmpere*.cs).

An NVN2 control section has version 0xc9 (NVN's have version 1 and are 0x880 bytes); its length at +0x30 in BNSH files (0 elsewhere), the register count at +0x4c,
compute's thread group at +0x58. +0x38 is the length of the instructions (the 0x80-byte SPH version 4 of the graphics stages included), +0x40 that rounded up to
0x100, +0x44 the code area's: between the last two lies the shader's c1 constant segment (0x100 bytes in vertex shaders, none in pixel shaders), as NVN's
+0x700 / +0x704 delimit it."""
import struct

CTL_MAGIC, CTL_VERSION = 0x98761234, 0xc9


def is_nvn2(ctl):
    """An NVN2 control section: magic, version 0xc9"""
    return len(ctl) >= 0x48 and struct.unpack_from('<II', ctl) == (CTL_MAGIC, CTL_VERSION)


def code_len(ctl):   # the code area
    return struct.unpack_from('<I', ctl, 0x44)[0]


def inst_len(ctl):   # the instructions (SPH included)
    return struct.unpack_from('<I', ctl, 0x38)[0]


def c1_segment(ctl, code):   # the c1 constant segment at the end of the code area
    a, e = struct.unpack_from('<II', ctl, 0x40)
    return code[a:e] if a <= e <= len(code) else b''


EARLY_FRAGMENT_TESTS = 0x5b   # a pixel shader's early fragment tests byte (see early_fragment_tests)
EARLY_FRAGMENT_TESTS_FROM = 11   # the first control sub-version (+0x08) where the byte is shown to be the flag


def early_fragment_tests(ctl):
    """An NVN2 pixel shader's early fragment tests flag: control byte +0x5b (NVN1 keeps it at +0x718; dmsh_build.early_fragment_tests reads that one). The NVN2 driver
    reads the pixel shader's flags as single bytes at +0x58..+0x62, a getter each (ldr x8, [x0, #8]; ldrb w0, [x8, #0x5b]; ret, in the sdk's seven vtables of control
    section versions). +0x58 is set exactly when the SPH writes depth (word 19 bit 1), in all 70,763 pixel shaders of four Switch 2 games (Ocarina of Time, Donkey Kong
    Bananza, PRAGMATA, Orbitals), and +0x5b never is together with it. The early tests are not an SPH bit on Ampere (NAK writes none; NVK sends the fragment shader's
    early_fragment_tests as the 3D method SET_API_MANDATED_EARLY_Z, which Ryujinx / Eden read as EarlyZForce / mandated_early_z). On PRAGMATA's cabin route (248 pixel shaders) it is set on all
    81 pixel shaders whose only blocker of early tests is their own store (the streaming feedback's atomic_fetch_min), on 11 of 84 that discard, and on none of the 75
    without side effects or the 5 depth writers. Shown for control sub-versions 11, 0x11 and 0x14, and read from 11 on (the getter reads +0x5b in all seven vtables); the 77 pixel shaders on hand with older sub-versions (0, 2, 7) set
    neither +0x58 nor +0x5b, so their layout is not shown and they keep late tests, which are never wrong, only slower"""
    return len(ctl) > EARLY_FRAGMENT_TESTS and struct.unpack_from('<I', ctl, 8)[0] >= EARLY_FRAGMENT_TESTS_FROM and ctl[EARLY_FRAGMENT_TESTS] != 0
