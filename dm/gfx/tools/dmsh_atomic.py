"""Image atomics run natively on the Metal texture (MSL 3.1 texture atomics).

SPIRV-Cross below MSL 3.1 emulates an image atomic with a raw device pointer
beside the texture (<image>_atomic), indexed by the bound texture's width: a
block-linear image is a deswizzled Metal copy whose pool memory has another
layout, so those atomics went to a scratch buffer that nothing read, and a 3D
image did not compile at all (an int3 subscript). Native texture atomics act on
the texture every other access of the image uses, for buffer, 2D, 2D-array and
3D images. The translator still guards each coordinate (IGN bounds) before the
atomic.

Only shaders with image atomics (OpImageTexelPointer) are written at MSL 3.1:
every other shader keeps its MSL 2.3 source byte for byte, so its library comes
from the MSL-keyed compile cache. The emulation must never come back silently:
check_msl fails the shader when SPIRV-Cross writes the pointer form anyway.
"""
import re
import struct

MSL_VERSION = '20300'
MSL_VERSION_ATOMICS = '30100'
OP_IMAGE_TEXEL_POINTER = 60


def uses_image_atomics(spv):
    """Whether the SPIR-V module (path) has an image texel pointer, the operand of every image atomic"""
    with open(spv, 'rb') as f:
        data = f.read()
    if len(data) < 20 or data[:4] != b'\x03\x02\x23\x07':
        raise ValueError('not a SPIR-V module: ' + spv)
    if b'\x3c\x00\x06\x00' not in data:   # the instruction's first word (always 6 words long, opcode 60), nowhere in the module: most shaders, without walking their instructions in Python
        return False
    words = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if op == OP_IMAGE_TEXEL_POINTER:
            return True
        if n == 0:
            raise ValueError('malformed SPIR-V instruction in ' + spv)
        i += n
    return False


def cross_args(xc, spv):
    """spirv-cross arguments for this module: xc with its --msl-version raised to 3.1 when the module has image atomics"""
    if not uses_image_atomics(spv):
        return list(xc)
    args = list(xc)
    args[args.index('--msl-version') + 1] = MSL_VERSION_ATOMICS
    return args


_EMULATED = re.compile(r'\bspvImage2DAtomicCoord\b|device\s+atomic_u?int\s*\*\s*\w+_atomic\b')


# 64-bit image atomics (after uncraftablexd's PRAGMATA bring-up, PR #1801). Ampere SURED.D.2D.MAX.U64 (PRAGMATA's visibility buffer: depth << 32 | ID, 336 compute
# shaders) is a 64-bit unsigned max with no result. Metal has it as texture2d<ulong, access::read_write>::atomic_max / atomic_min (MSL 3.1, an RG32Uint texture made with
# the shader-atomic usage, Apple GPU family 9: M3 / A17 Pro and later), with no fetch form and no other operation. SPIRV-Cross refuses every 64-bit atomic, so the translator
# (CodeGen/Spirv GenerateImageAtomic) writes the operation on its rg32ui image as two 32-bit atomics of the same operation on the same texel, the low word then the high word,
# inside the IGN bounds check. wide_atomics turns each such pair into the one 64-bit atomic and declares the image as ulong; the high word is the more significant half, as the
# RG32Uint texel's R channel holds the low 32 bits (Metal's ulong view of RG32Uint, little-endian as the guest's 64-bit texel). The image may be bound only for that: any
# other access of it (a read, a write, a lone 32-bit atomic) fails the shader rather than run on the wrong type
_WIDE_DECL = re.compile(r'\b(texture(?:2d|2d_array|3d))<uint, access::read_write>(\s+)(\w+_rg32ui)\b')
_WIDE_STMT = r'^([ \t]*)(?:uint _\d+ = )?([\w.]*?\b%s)\.atomic_fetch_(max|min)\((.*)\)(?:\.x)?;[ \t]*$'
_WIDE_QUERY = r'\.get_(?:width|height|depth|array_size)\(\)'


def _split_args(text):
    """The top-level comma-separated arguments of a call's argument text"""
    out, depth, cur = [], 0, ''
    for ch in text:
        depth += {'(': 1, '[': 1, ')': -1, ']': -1}.get(ch, 0)
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    out.append(cur.strip())
    return out


def wide_atomics(source):
    """The translator's 32-bit atomic pairs on rg32ui images as Metal's 64-bit texture atomics (see above); the source unchanged when there are none"""
    if '_rg32ui' not in source:
        return source
    for m in list(_WIDE_DECL.finditer(source)):
        name = m.group(3)
        if not re.search(r'\b%s\.atomic_' % re.escape(name), source):
            continue   # an rg32ui image the shader only reads or writes: not this case
        lines = source.split('\n')
        stmt = re.compile(_WIDE_STMT % re.escape(name))
        out, i = [], 0
        while i < len(lines):
            a = stmt.match(lines[i])
            if not a:
                out.append(lines[i]); i += 1; continue
            b = stmt.match(lines[i + 1]) if i + 1 < len(lines) else None
            if not b or b.group(2) != a.group(2) or b.group(3) != a.group(3):
                raise ValueError(f'{name}: a 32-bit atomic on a 64-bit image that is not the translator\'s low / high pair')
            x, y = _split_args(a.group(4)), _split_args(b.group(4))
            if len(x) < 2 or x[:-1] != y[:-1]:
                raise ValueError(f'{name}: the two halves of a 64-bit atomic name different texels')
            out.append(f'{a.group(1)}{a.group(2)}.atomic_{a.group(3)}({", ".join(x[:-1])}, ulong4(as_type<ulong>(uint2({x[-1]}, {y[-1]}))));')
            i += 2
        source = '\n'.join(out)
        left = re.sub(r'\b%s(?:%s|\.atomic_(?:max|min)\()' % (re.escape(name), _WIDE_QUERY), '', source)
        uses = len(re.findall(r'\b%s\b' % re.escape(name), left))
        if uses != 1:   # the declaration alone
            raise ValueError(f'{name}: {uses - 1} accesses of a 64-bit atomic image besides its atomics and size queries')
        source = re.sub(r'\b(texture(?:2d|2d_array|3d))<uint, access::read_write>(\s+%s)\b' % re.escape(name), r'\1<ulong, access::read_write>\2', source)
    return source


def check_msl(source):
    """Fail closed on SPIRV-Cross's pointer emulation of image atomics; return the source with 64-bit image atomics made native (wide_atomics)"""
    if 'tomic' in source and _EMULATED.search(source):   # (both alternatives hold the word; the pattern alone tries every character of the source)
        raise ValueError('image atomics emulated through a raw pointer; they must be native texture atomics (MSL 3.1)')
    return wide_atomics(source)
