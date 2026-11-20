# Adapted from ohxiaoguang/Touchline23, MIT; original authorship retained.
"""Reader for FIFA's NXSHADER .fxo shader containers (FIFA 23 Switch).

Layout (verified on data/fifarna/shader.big):
  0x00  8   b'NXSHADER'
  0x08  8   version words (01 08 00 00 ...)
  0x10  8   b'GLSL' + u32 block size (2015.. / big-endian? no: little-endian)
   ...      N GLSLC output blobs (magic 0x19866891) back to back, starting at 0x18
  end    b'REFL' + reflection data (outside the GLSL block)

Each GLSLC output is a normal NVN program: a control section (magic 0x98761234,
+0x20 own length, +0x704 code length) followed by the code area (magic
0x12345678 graphics / 0x12345679 compute, 0x30 header + 0x50 SPH + instructions).
A graphics program can have a 0x100-byte constants block between the control
section and the code header, so the code area is located by its magic.
"""
import hashlib
import struct

CTL_MAGIC = bytes.fromhex("34127698")
GFX_MAGIC = bytes.fromhex("78563412")
CS_MAGIC = bytes.fromhex("79563412")
GLSLC_MAGIC = bytes.fromhex("91688619")
SPH_STAGE = {1: "vs", 2: "hs", 3: "ds", 4: "gs", 5: "ps"}


def ctl_len(ctl):
    n = struct.unpack_from("<I", ctl, 0x20)[0]
    return n if 0x760 <= n < 0x880 else 0x880


def skey(ctl, code):
    return hashlib.sha256(ctl[:ctl_len(ctl)] + code).hexdigest()[:16]


def stage_of(ctl, code):
    headerless_cs = (code[:4] not in (CS_MAGIC, GFX_MAGIC) and len(ctl) >= 0x754
                     and all(struct.unpack_from("<3I", ctl, 0x748)))
    if code[:4] == CS_MAGIC or headerless_cs:
        return "cs"
    if code[:4] == GFX_MAGIC and len(code) >= 0x34:
        return SPH_STAGE.get((struct.unpack_from("<I", code, 0x30)[0] >> 10) & 0xF)
    return None


def glsl_block(d):
    """-> (start, size) of the GLSL block inside an fxo."""
    if len(d) < 0x18:
        raise ValueError("truncated NXSHADER header")
    if d[:8] != b"NXSHADER":
        raise ValueError("not NXSHADER: %r" % d[:8])
    if d[0x10:0x14] != b"GLSL":
        raise ValueError("no GLSL block")
    return 0x10, struct.unpack_from("<I", d, 0x14)[0]


def programs(d):
    """-> [(stage, key, ctl, code, offset)] for every GLSLC output in an fxo."""
    start, size = glsl_block(d)
    end = start + size
    if size < 8 or end > len(d):
        raise ValueError("NXSHADER GLSL block exceeds file")
    out, p = [], start
    while True:
        c = d.find(CTL_MAGIC, p, end)
        if c < 0:
            break
        if c + 0x708 > end:
            break
        nctl = struct.unpack_from("<I", d, c + 0x20)[0]
        ncode = struct.unpack_from("<I", d, c + 0x704)[0]
        if not (0x760 <= nctl <= 0x20000) or not ncode:
            p = c + 4
            continue
        # code area: after the control section, sometimes behind a small
        # constants block of variable size, so search a window for the header;
        # compute shaders may have no header at all (instructions start at +0)
        base = c + nctl
        co = -1
        for cand in range(base, min(base + 0x400, end - 3)):
            if d[cand:cand + 4] in (GFX_MAGIC, CS_MAGIC):
                co = cand
                break
        if co < 0 and c + 0x754 <= c + nctl and all(struct.unpack_from("<3I", d, c + 0x748)):
            # headerless compute: the code area begins where the control section's code-offset field
            # (+0x6f0) says, not right after the control section; the two differ by the shader's c1
            # constant block (+0x1c bytes), which sits between them (sh_irradiance_prefilter.cs.fxo:
            # 0x180 bytes of constants). Taking the control-section end truncated the code, produced a
            # key the game never asks for, and made the dispatch fail at run time.
            co = c + struct.unpack_from("<I", d, c + 0x6f0)[0]
        if co < 0:
            p = c + 4
            continue
        if co < base or c + nctl > end or co + ncode > end:
            p = c + 4
            continue
        ctl, code = d[c:c + nctl], d[co:co + ncode]
        st = stage_of(ctl, code)
        if st:
            out.append((st, skey(ctl, code), ctl, code, c))
        p = co + ncode
    return out
