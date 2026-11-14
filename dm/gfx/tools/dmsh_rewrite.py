#!/usr/bin/env python3
# The rewrites dmsh_build.py makes in a translated shader's MSL before it compiles it, where Metal or the translator falls short of what the Switch code does: precise
# arithmetic (precise), memory barriers as fences (membar_fence, tg_fence), 64-bit products (mul_extended), uniform words tested against constants, which the front end would
# fold into switches (uniform_tests), Ampere's HMMA as simdgroup matrices (hmma_msl), first stores and shared-memory stores (first_store, shared_store_msl), threadgroup memory
# past Metal's limit (tg_spill), texture gathers with offsets (gather_offsets), border colours Metal has no sampler state for (border_emu), a pixel shader's depth snapped to
# its position (frag_z_snap) and the 0 / -1 mask of a tested bit 0 without its compare (bit0_mask). Functions of the MSL text and the library ABI only, with no build state,
# so tests import them. Part of the translation chain (dmsh_chain.FILES) and of every shader's cache key (dmsh_build.py CODE_FP).
import bisect, collections, itertools, re
import dmsh_abi as abi, dmsh_side

# spirv-cross writes the translator's FMin / FMax / FClamp as fast::min / max / clamp, whose result for a NaN operand is the GPU's choice; Maxwell's FMNMX returns the other operand
# and its saturation gives 0, as Metal's precise:: functions (IEEE fmin / fmax) do on every GPU. The M4 Pro's fast ones happen to agree, an A20 Pro's do not: Super Mario Galaxy 2's
# GX lighting takes max(0, NaN) from light directions left at zero, and the half of Mario its fill light faces came out white on the iPhone (GitHub issue 72; vk_shaders.py PRECISE)
def precise(msl): return re.sub(r'\bfast::(min|max|clamp)\(', r'precise::\1(', msl)
_MEMBAR = 'threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture);'
def membar_fence(msl):
    """MEMBAR as a fence, not a barrier. The translator turns BAR.SYNC into OpControlBarrier (workgroup scope; SPIRV-Cross prints threadgroup_barrier(mem_flags::mem_threadgroup)) and
    MEMBAR.GPU / .SYS into OpMemoryBarrier at device scope (Ryujinx CodeGen/Spirv/Instructions.cs GenerateMemoryBarrier), which SPIRV-Cross prints as a threadgroup_barrier with device,
    threadgroup and texture memory: an execution barrier the Switch code does not have. Where the threads of a group are not converged it waits for threads that never come.
    PRAGMATA's GPU culling queue 12a52b683eec95c1 has no BAR.SYNC at all, only MEMBAR.SC.GPU; ERRBAR inside its persistent work loop. Each becomes the fence it is,
    atomic_thread_fence at device scope, sequentially consistent; two in a row (MEMBAR.SC and ERRBAR) become one. From uncraftablexd's PRAGMATA bring-up (PR #1407)"""
    if _MEMBAR not in msl: return msl
    fence = 'atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture, memory_order_seq_cst, thread_scope_device);   // MEMBAR (membar_fence)'
    msl = re.sub(r'^([ \t]*)' + re.escape(_MEMBAR) + r'\n\1' + re.escape(_MEMBAR) + r'$', lambda m: m[1] + _MEMBAR, msl, flags=re.M)
    return msl.replace(_MEMBAR, fence)

PER_VERTEX_BASE = 64   # the translator's Declarations.PerVertexLocationBase: dm_pv<L>_<v> is at user(locn<64 + 3L + v>), past every attribute's own location
PER_VERTEX_IN = re.compile(r'\n    float4 dm_pv(\d+)_([012]) \[\[user\(locn(\d+)\)(?:, flat)?\]\];')
VERTEX_INDEX = ('first', 'second', 'third')
METAL4 = 'vertex_value<'   # in a source that needs MSL 4.0 (ios_metal.compile)
def per_vertex(msl):
    """A user attribute as each vertex of the triangle holds it, which Ampere's LDTRAM reads in a pixel shader (fragment shader barycentrics: Resident Evil 4's shaders weigh
    the three values with the barycentric coordinates themselves). SPIRV-Cross has no MSL form of SPIR-V's PerVertexKHR, so the translator declares each vertex read as a flat
    input of its own, dm_pv<L>_<v> at user(locn<64 + 3L + v>) (InstEmitAmpereMem ALdTram). Here the reads of attribute L become one vertex_value<float4> at user(locn<L>), which the
    vertex shader's output there feeds, and each read that vertex's get(vertex_index::...): the vertices the [[barycentric_coord]] weights refer to, as on NVIDIA.
    vertex_value is MSL 4.0 (macOS 26, iOS 26): the Mac's deployment target has it, and ios_metal.compile compiles a source using it with -std=metal4.0.
    Raises ValueError when attribute L is also an ordinary input of the shader, which one location cannot be at once (no game known)"""
    found = list(PER_VERTEX_IN.finditer(msl))
    if not found: return msl
    for m in found:
        if int(m[3]) != PER_VERTEX_BASE + 3 * int(m[1]) + int(m[2]): raise ValueError(f'per-vertex input dm_pv{m[1]}_{m[2]} at user(locn{m[3]}), not at the translator\'s location')
    locs = sorted({int(m[1]) for m in found})
    st = re.search(r'\nstruct main0_in\n\{(\n.*?)\n\};', msl, re.S)
    if not st or not all(st.start(1) <= m.start() < st.end(1) for m in found): raise ValueError('per-vertex inputs outside main0_in')
    both = [l for l in locs if re.search(rf'\[\[user\(locn{l}\)', st[1])]
    if both: raise ValueError(f'attribute {both[0]} read both per vertex and as an ordinary input')
    decls = ''.join(f'\n    vertex_value<float4> dm_pv{l} [[user(locn{l})]];' for l in locs)
    out = msl[:found[0].start()] + decls + ''.join(msl[a.end():b.start()] for a, b in zip(found, found[1:])) + msl[found[-1].end():]
    return re.sub(r'\.dm_pv(\d+)_([012])\b', lambda m: f'.dm_pv{m[1]}.get(vertex_index::{VERTEX_INDEX[int(m[2])]})', out)

BIT0_AND = re.compile(r'^[ \t]*(u?int) (_\d+) = ([^;\n]+) & 1u?;$', re.M)
BIT0_LOOSER = re.compile(r'\|\||&&|[|^?,]')   # an operator binding looser than &: `a | b & 1` is a | (b & 1), not bit 0
def bit0_operand(e):   # the left operand outside its parentheses binds at least as tightly as &
    while True:
        f = re.sub(r'\([^()]*\)', '', e)
        if f == e: return not BIT0_LOOSER.search(e)
        e = f
BIT0_TEST = re.compile(r'^[ \t]*bool (_\d+) = (_\d+) != 0u?;$', re.M)
BIT0_MASK = re.compile(r'= (_\d+) \? \(-1\) : 0;')
BIT0_WRITE = re.compile(r'\b(_\d+)\b[ \t]*(?:<<|>>|[-+*/%&|^])?=(?!=)')   # an assignment, plain or compound, a declaration's too
BIT0_OTHER = re.compile(r'(?:\+\+|--)[ \t]*(_\d+)\b|\b(_\d+)[ \t]*(?:\+\+|--)|&(_\d+)\b')   # an increment, or the temporary's address
BIT0_BODY = re.compile(r'^\{\n.*?^\}$', re.M | re.S)   # a function's body: its braces alone in column 0
def bit0_mask(msl):
    """The 0 / -1 mask of a tested bit 0, written from the bit and not from its compare. The translator makes a predicate of `x & 1` twice over: the compare
    `bool b = (x & 1) != 0`, and where the Switch code keeps the predicate in a register, its integer form `b ? -1 : 0`. Where the shader also branches on b the compare has
    two users in the AIR, a sign extension and a branch, and Apple's GPU compiler for the M3 and later (applegpu_g15 and up, macOS 26 and 27) can pick a bit-test jump it has
    no encoding for: `LLVM ERROR: Failed to encode.` under llvm::MCObjectStreamer::emitInstruction, which ends the compiler service while a pipeline with the shader is built
    (XPC_ERROR_CONNECTION_INTERRUPTED). It needs x in a uniform the compiler's per-draw constant program has loaded and no room left there for the test itself, so one
    shader in thousands with the idiom meets it: PRAGMATA's pixel shader c4ac3ca598cea32e (`AND` of a constant buffer word with 1, its predicate kept and branched on).
    With the mask as `-(x & 1)`, the same value, the compare is left with its branch alone. Only a declaration `int a = ... & 1;` with a declaration `bool b = a != 0;`
    counts, neither assigned again (SPIRV-Cross temporaries); b is in scope where the mask is taken, so a is. Other shaders come back as the same object"""
    if ' ? (-1) : 0;' not in msl: return msl
    def body(f):   # one function: SPIRV-Cross numbers its temporaries by function, and the build's extra entry points repeat a body
        src = f[0]
        if ' ? (-1) : 0;' not in src: return src
        bits = {m[2]: m[1] for m in BIT0_AND.finditer(src) if bit0_operand(m[3])}
        tests = {m[1]: m[2] for m in BIT0_TEST.finditer(src) if m[2] in bits}
        writes = collections.Counter(m[1] for m in BIT0_WRITE.finditer(src))   # a temporary written once, in its declaration
        other = {m[1] or m[2] or m[3] for m in BIT0_OTHER.finditer(src)}
        tests = {b: a for b, a in tests.items() if writes[a] == 1 and writes[b] == 1 and a not in other and b not in other}
        def mask(m):
            a = tests.get(m[1])
            if a is None: return m[0]
            return '= -%s;   // bit 0 as a mask (bit0_mask)' % (a if bits[a] == 'int' else 'int(%s)' % a)
        return BIT0_MASK.sub(mask, src) if tests else src
    out = BIT0_BODY.sub(body, msl)
    return out if out != msl else msl

TG_FENCE = 'atomic_thread_fence(mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_simdgroup);'
TG_FENCE_FN = re.compile(r'\bvoid dmsh_tg_fence\(\)\s*\{')
def tg_fence(src):
    """Warp-synchronous shared memory. Ampere code relies on lockstep warps and leaves the barrier out of small scans (Unreal's GPU radix sort downsweep d032ce5090303e01
    at 0x2eb0: STS / LDS [x-4] / STS / LDS [x-8]); for one thread the load and the earlier store do not alias, so Metal moves the load above the store and reads another
    lane's old value (Orbitals: Niagara's sorted index list wrong every frame, PR #1775). The translator calls dmsh_tg_fence() before each shared memory load that some path
    reaches from a shared memory store with no barrier between (SharedMemoryFence.cs). Its body there is a memory barrier at workgroup scope, which SPIRV-Cross prints as a
    threadgroup_barrier (and membar_fence as a device fence); here it becomes a simdgroup-scope fence on threadgroup memory, which is the order a warp needs and, not being
    an execution barrier, safe in divergent code. A kernel whose arrays spill to device memory gets the device scope too (tg_spill, after this). The text rule before it put a
    fence after every threadgroup store: 25,108 in 699 of Ocarina of Time's shaders, and Apple's compiler crashed on a foliage mesh pipeline on an M2 (GitHub issue #2481)"""
    m = TG_FENCE_FN.search(src)
    if not m: return src
    return src[:m.end()] + '\n    ' + TG_FENCE + '\n' + src[block_end(src, m.end() - 1):]

# spirv-cross writes OpUMulExtended / OpSMulExtended as calls of an optnone template, spvMulExtended, as Metal's compiler may fail on an inlined mulhi (spirv_msl.cpp,
# SPVFuncImplMulExtended). The barrier keeps every product a call that is never folded: Switch 2 shaders divide by constants through the high word of a product (Ocarina of Time
# has 348,587 such calls, all scalar uint by a literal, 98% by a power of two), and its NDVE title movie ran 6.9% faster on an M1 Pro with its 5,679 others inline (PR #1741).
# Scalar operands get overloads that take the halves of the 64-bit product, with no mulhi; overload resolution prefers them to the template, and vectors keep the template.
def mul_extended(msl):
    helper = re.compile(r'template<typename T, typename U, typename V>\s*\[\[clang::optnone\]\] T spvMulExtended\(V l, V r\)\s*\{\s*return T\{U\(l \* r\), U\(mulhi\(l, r\)\)\};\s*\}\n')
    found = list(helper.finditer(msl)) if 'spvMulExtended' in msl else []
    if len(found) != 1 or 'spvMulExtended(uint l, uint r)' in msl: return msl
    scalar = ''.join(f'template<typename T, typename U> static inline __attribute__((always_inline)) T spvMulExtended({t} l, {t} r) {{ {w} p = {w}(l) * {w}(r); return T{{U(p), U(p >> 32)}}; }}\n'
                     for t, w in (('uint', 'ulong'), ('int', 'long')))
    return msl[:found[0].end()] + scalar + msl[found[0].end():]

# A game uniform word tested against integer constants, as the translator writes a GLSL switch (Ryujinx's structurizer: one test of the word per case, flags for the fall-throughs,
# the cases nested in each other): bool _5 = as_type<int>((*spvDescriptorSet0.fp_c5)._m0[16].w) != 102. The Metal front end's SimplifyCFG (at -O1 too) folds a chain of blocks
# that each hold only such a test of one value into a switch, whose cases jump into the nested blocks past their joins. Apple's GPU compiler then takes a pixel shader with dozens of
# such switches for hours and tens of gigabytes (applegpu-nt, metal-tt 32023.922): Nintendo Switch Sports Resort's material pixel shader 692008b305ce264f (42 switches) ran past
# 10 minutes and 13 GB alone where it had taken 7 s, and the Mac app's pipeline archive spent 15 hours on four pipelines. The reads were plain since the host pads a short binding
# (dmsh_fetch.select_guards): before that, each read's bounds test sat in the test's block and nothing was folded. Each tested read becomes a volatile load of its word
# (dm_uword), which the front end can neither merge with the next test's nor move out of its block, so no chain is folded; a uniform load per test is less than the bounds test
# each read had then. Only words of the game's uniform slots (c3 and up) tested against a constant more than once: a single test is no chain, and the driver's banks (c0 to c2)
# are left as written, since later rewrites look for their reads (dmsh_entry.vs_base_instance) and the host fills them. Returns the shader unchanged when there are none
_UTEST_READ = r'as_type<(u?int)>\((\(\*spvDescriptorSet0\.[a-z]+_c(?:[3-9]|\d\d)\)\._m0\[\d+\])\.([xyzw])\)'   # game slots c3 and up, not the driver's banks
_UTEST = re.compile(r'(?<![\w.])%s(?= [!=]= -?\d+u?(?![\w.]))|(?<=[!=]= )%s' % (_UTEST_READ, _UTEST_READ.replace('(u?int)', '(?P<t2>u?int)').replace(r'(\(\*', r'(?P<r2>\(\*').replace('([xyzw])', '(?P<c2>[xyzw])')))
_UTEST_LEFT = re.compile(r'(?<![\w.])-?\d+u? [!=]= $')
UWORD = ('#ifndef DM_UWORD\n#define DM_UWORD\ntemplate<typename T, int N> static inline __attribute__((always_inline)) T dm_uword(const constant vec<T, N>& v, int i) '
         '{ return ((const volatile constant T*)&v)[i]; }\n#endif\n')
def uniform_tests(msl):
    if 'spvDescriptorSet0' not in msl or 'as_type<' not in msl: return msl
    def parts(m):
        if m.group(1): return m.group(1), m.group(2), m.group(3)
        if not _UTEST_LEFT.search(msl, max(0, m.start() - 16), m.start()): return None   # [!=]= after something other than a constant
        return m.group('t2'), m.group('r2'), m.group('c2')
    found = [(m, p) for m in _UTEST.finditer(msl) for p in [parts(m)] if p]
    tested = collections.Counter(r + c for _, (_, r, c) in found)
    chains = {w for w, n in tested.items() if n > 1}
    if not chains: return msl
    out, at = [], 0
    for m, (t, r, c) in found:
        if r + c not in chains: continue
        out += [msl[at:m.start()], 'as_type<%s>(dm_uword(%s, %d))' % (t, r, 'xyzw'.index(c))]; at = m.end()
    body = ''.join(out) + msl[at:]
    head = re.search(r'^using namespace metal;\n', body, re.M)
    if not head: raise ValueError('uniform tests: no "using namespace metal;" to put dm_uword after')
    return body[:head.end()] + UWORD + body[head.end():]

# Ampere HMMA.16816 with an f16 accumulator (agl's upscaler networks, the NDVE video networks) reaches the MSL as calls of dmsh_hmma16816_f16(lane, block, A0..A3, B0, B1, C0, C1,
# flags, A's LDSM address, A0..A3's offsets, A's row stride, out D1) → D0, the subgroup's 256-word block of the scratch shared memory as the last argument (InstEmitAmpereTensor.AHmma,
# TensorPass, HelperFunctionManager.GenerateHmma16816F16). The translator's body computes D lane by lane from the block; here it becomes simdgroup_matrix code over the same block:
# A row-major (halves 0..255, 16 a row), B transposed (256..383, row n, 16 halves of k, loaded transposed), C / D row-major (384..511, 8 a row). Per HMMA on an M4 Pro: 1.08 ns
# a SIMD-group against 5.4 for the shuffle emulation. TensorPass's flags (constants at each call) let a call skip the block where a matrix is already in the SIMD-group's
# registers: each function that calls the helper holds 8 accumulator slots (acc, two matrices each) and A's four matrices (am) and passes them by reference, so the Metal
# compiler, inlining the helper, keeps them in registers. A chain's accumulator stays in its slot from one call to the next (FlagCSlot / FlagDSlot), a call with the previous
# call's A loads none (FlagAReuse), C = 0 is a multiply (FlagCZero), and A an LDSM read is loaded from shared memory where it read it (FlagADirect: row 0 of matrix i at lane 0's
# row address + offset i, rows stride bytes apart, transposed for .MT88), which leaves the LDSM's own per-lane code unused. That needs the caller's shared memory array: the
# one threadgroup array the calling functions declare besides the scratch, passed as one more argument; with no such array FlagADirect is ignored and A goes through the block.
# On the NDVE networks of Ocarina of Time's movies (14 dispatches a frame) the frame's compute went from 26.7 to 18.9 ms on an M4 (8 GPU cores)
# (shaders.md, HMMA chains keep their matrices in the SIMD-group)
HMMA_BODY = '''{
    threadgroup uint* s = &%(scr)s[uint(%(blk)s)];
    threadgroup half* h = (threadgroup half*)s;
    const int f = int(%(fl)s), k = (f >> 8) * 2;
    const bool areuse = f & 1, cslot = f & 2, dslot = f & 4, czero = f & 8, adirect = %(adirect)s;
    const bool btake = bmode >= 0 && (bmode & 1), bfill = bmode >= 0 && !(bmode & 1); const int bk = (bmode >> 1) * 2;
    uint g = uint(%(lane)s) >> 2, t = uint(%(lane)s) & 3;
    if (!areuse && !adirect) { s[g * 8 + t] = uint(%(a0)s); s[(g + 8) * 8 + t] = uint(%(a1)s); s[g * 8 + 4 + t] = uint(%(a2)s); s[(g + 8) * 8 + 4 + t] = uint(%(a3)s); }
    if (!btake) { s[128 + g * 8 + t] = uint(%(b0)s); s[128 + g * 8 + 4 + t] = uint(%(b1)s); }
    if (!cslot && !czero) { s[192 + g * 4 + t] = uint(%(acc0)s); s[192 + (g + 8) * 4 + t] = uint(%(acc1)s); }
    if (!btake || (!areuse && !adirect) || (!cslot && !czero)) simdgroup_barrier(mem_flags::mem_threadgroup);
    if (!areuse && adirect) {
        threadgroup half* x = %(shm_base)s + (int(simd_broadcast(uint(%(aaddr)s), ushort(0))) >> 1);
        const ulong st = ulong(uint(%(astr)s) >> 1); const bool tr = f & 32;
        simdgroup_load(am[0], x + (int(%(ao0)s) >> 1), st, ulong2(0, 0), tr); simdgroup_load(am[1], x + (int(%(ao2)s) >> 1), st, ulong2(0, 0), tr);
        simdgroup_load(am[2], x + (int(%(ao1)s) >> 1), st, ulong2(0, 0), tr); simdgroup_load(am[3], x + (int(%(ao3)s) >> 1), st, ulong2(0, 0), tr);
    } else if (!areuse) {
        simdgroup_load(am[0], h, 16, ulong2(0, 0)); simdgroup_load(am[1], h, 16, ulong2(8, 0)); simdgroup_load(am[2], h, 16, ulong2(0, 8)); simdgroup_load(am[3], h, 16, ulong2(8, 8));
    }
    // C and D in the call's own matrices: a slot is read only for a chain's C (cslot) and written only for its D (dslot). A call outside every chain has slot 0 in its
    // flags (no slot bits); working in acc[0..1] it overwrote the accumulator a chain held there between its two calls (Ocarina of Time's agl_ndsr_vr networks: 2,515 such
    // calls in 30 shaders, dotted rows across fog and light, GitHub issue #1537). The flags are constants at each inlined call, so the copies cost nothing
    // Ampere adds each 8 products to the accumulator and rounds once to f16, k 0-7 then k 8-15 (N_FMA 8 in Khattak and Mikaitis's A100 model, which matches recorded
    // A100 output in 5,000 of 5,000 cases), where simdgroup_half8x8 rounds after every product (2,420 of them). So each block runs in f32 (each product of two halves
    // exact, the sum in order) and its result is rounded to f16 (RNE): an f16 C, the first block's result and D are the only roundings
    // simdgroup_multiply_accumulate takes a float result from half operands and a half C. A lane's thread_elements() are the same two places in a half and a
    // float matrix, so the rounding is two conversions a lane. Rounding through a multiply-accumulate with zero A and B into a half result gives the same bits but
    // made the network twice as slow (NDSR's fa3033d05c0a41d6: 0.59 ms against 0.32 on an M5 Max). dm/gfx/msl/ndve_engine.metal's hmma() rounds the same way
    simdgroup_half8x8 b0, b1, c0, c1;
    simdgroup_float8x8 f0, f1;
    if (btake) { b0 = bm[bk]; b1 = bm[bk + 1]; }
    else { simdgroup_load(b0, h + 256, 16, ulong2(0, 0), true); simdgroup_load(b1, h + 256, 16, ulong2(8, 0), true); if (bfill) { bm[bk] = b0; bm[bk + 1] = b1; } }
    if (czero) { c0 = make_filled_simdgroup_matrix<half, 8, 8>(0.0h); c1 = c0; }
    else if (cslot) { c0 = acc[k]; c1 = acc[k + 1]; }
    else { simdgroup_load(c0, h + 384, 8, ulong2(0, 0)); simdgroup_load(c1, h + 384, 8, ulong2(0, 8)); }
    simdgroup_multiply_accumulate(f0, am[0], b0, c0); simdgroup_multiply_accumulate(f1, am[2], b0, c1);
    for (int i = 0; i < 2; i++) { c0.thread_elements()[i] = half(f0.thread_elements()[i]); c1.thread_elements()[i] = half(f1.thread_elements()[i]); }   // the first block's rounding
    simdgroup_multiply_accumulate(f0, am[1], b1, c0); simdgroup_multiply_accumulate(f1, am[3], b1, c1);
    for (int i = 0; i < 2; i++) { c0.thread_elements()[i] = half(f0.thread_elements()[i]); c1.thread_elements()[i] = half(f1.thread_elements()[i]); }
    if (dslot) { acc[k] = c0; acc[k + 1] = c1; simdgroup_barrier(mem_flags::mem_threadgroup); %(d1)s = 0; return 0; }
    simdgroup_store(c0, h + 384, 8, ulong2(0, 0)); simdgroup_store(c1, h + 384, 8, ulong2(0, 8));
    simdgroup_barrier(mem_flags::mem_threadgroup);
    uint d0 = s[192 + g * 4 + t], d1 = s[192 + (g + 8) * 4 + t];
    simdgroup_barrier(mem_flags::mem_threadgroup);
    %(d1)s = as_type<%(dt)s>(d1);
    return as_type<%(rt)s>(d0);
}'''

HMMA_ARGS = ['lane', 'blk', 'a0', 'a1', 'a2', 'a3', 'b0', 'b1', 'acc0', 'acc1', 'fl', 'aaddr', 'ao0', 'ao1', 'ao2', 'ao3', 'astr', 'd1']

HMMA_CALL = re.compile(r'\bdmsh_hmma16816_f16\(([^()]*)\)')

TG_ARRAY = re.compile(r'^[ \t]+threadgroup (spvUnsafeArray<uint, \d+>) (\w+);[ \t]*$', re.M)

def split_params(text):   # commas inside template arguments (spvUnsafeArray<uint, N>) do not separate parameters
    params, depth, cur = [], 0, ''
    for ch in text + ',':
        depth += {'<': 1, '>': -1}.get(ch, 0)
        if ch == ',' and depth == 0: params.append(cur.strip()); cur = ''
        else: cur += ch
    return params

BRACE = re.compile(r'[{}]')

FUNC_OPEN = re.compile(r'(?=\n\{\n)')   # where a line holding only a brace starts, overlapping ones included

def block_end(src, start):   # the index of the brace closing the block opened at src[start] (start itself when it is no brace); the regular expression steps from brace to brace
    if start < len(src) and src[start] not in '{}': return start
    depth = 0
    for b in BRACE.finditer(src, start):
        depth += 1 if b.group() == '{' else -1
        if depth == 0: return b.start()
    raise ValueError('dmsh_hmma16816_f16: block not closed')

# B kept in the SIMD-group (hmma_bslots). The lanes hold B in registers and the matrix hardware fills its matrices only from memory: a lane writing its elements through
# thread_elements() from SIMD shuffles (the 8x8 lane layout measured on an M6: lane l holds row (l/16)*4 + (l/2)%4, columns 2(l%2) + 4((l/8)%2) and the next) took a chain of
# calls 1.716 ms against 1.448 through the scratch, and every operand that way 8.9 ms. So B's trip through the block (2 stores, a barrier, 2 loads) cannot get cheaper; it can only
# be left out where it repeats. B's trip is a fifth of a chain whose A and C stay in the SIMD-group (1.147 ms with none, 24 threadgroups of 4 SIMD-groups, as these networks run)
HMMA_COPY = re.compile(r'(?:as_type<\w+>|int|uint|float|half|short|ushort)?\((\w+)\)|(\w+)')

HMMA_DECL = re.compile(r'^[ \t]*(?:const[ \t]+)?[A-Za-z_]\w*(?:<[^<>;]*>)?[ \t]+(\w+)[ \t]*=[ \t]*([^;\n]+);', re.M)

HMMA_WRITE = re.compile(r'\b(\w+)[ \t]*(?:<<|>>|[-+*/%&|^])?=(?!=)|\+\+[ \t]*(\w+)|\b(\w+)[ \t]*\+\+|--[ \t]*(\w+)|\b(\w+)[ \t]*--|&[ \t]*(\w+)')

def hmma_bslots(msl, open_at, end, fcalls, writers):
    """The calls of one function (open_at, end: its braces; fcalls: (match, arguments) in text order) → {call start: slot * 2 (keeps its B) or slot * 2 + 1 (takes it)}.
    Two calls have the same B when their B arguments are the same variables after following copies (x, as_type<T>(x), int(x) ...) down to a variable written once in the function
    and declared at its top level, so that its value is the same everywhere after its declaration. A variable a call writes (an out argument: the helper's D1, an argument of a
    function with a non-const thread reference, one whose address is taken) is written more than once by that count. The first call of a B that stands at the function's top level
    keeps its two matrices in a slot, and the calls of that B after it in the text take them: a statement at the top level runs before everything written after it"""
    body = msl[open_at:end]
    writes = collections.Counter(next(g for g in w.groups() if g) for w in HMMA_WRITE.finditer(body))
    for c, args in fcalls: writes[args[HMMA_ARGS.index('d1')]] += 1
    for name in writers:
        for w in re.finditer(r'\b%s\(' % name, body):
            paren_depth, j = 1, w.end()
            while paren_depth and j < len(body): paren_depth += {'(': 1, ')': -1}.get(body[j], 0); j += 1
            writes.update(re.findall(r'\b[A-Za-z_]\w*\b', body[w.end():j]))
    braces = [(b.start() + open_at, 1 if b.group() == '{' else -1) for b in BRACE.finditer(body)]
    at, depth_after = [p for p, _ in braces], list(itertools.accumulate(d for _, d in braces))
    def depth(pos): k = bisect.bisect_left(at, pos) - 1; return depth_after[k] if k >= 0 else 0   # 1: the function's top level
    decls = {}
    for d in HMMA_DECL.finditer(body): decls.setdefault(d.group(1), (d.group(2).strip(), d.start(1) + open_at))
    def canon(name):
        for _ in range(64):
            if writes[name] != 1 or name not in decls: return None
            init, pos = decls[name]
            m = HMMA_COPY.fullmatch(init)
            inner = m and (m.group(1) or m.group(2))
            if not inner or inner[0].isdigit(): return name if depth(pos) == 1 else None
            name = inner
        return None
    by_b = collections.defaultdict(list)
    for c, args in fcalls:
        k = (canon(args[HMMA_ARGS.index('b0')]), canon(args[HMMA_ARGS.index('b1')]))
        if None not in k: by_b[k].append(c.start())
    modes, slot = {}, 0
    for starts in by_b.values():
        first = next((i for i, st in enumerate(starts) if depth(st) == 1), None)
        if first is None or first == len(starts) - 1: continue
        modes[starts[first]] = slot * 2
        modes.update((st, slot * 2 + 1) for st in starts[first + 1:])
        slot += 1
    return modes

def hmma_msl(msl):
    m = re.search(r'\n(\w+) dmsh_hmma16816_f16\(([^)]*)\)\n\{', msl)
    if not m: return msl
    params = split_params(m.group(2))
    scr = [x for x in params if x.startswith('threadgroup ')]
    plain = [x for x in params if not x.startswith('threadgroup ')]
    if len(scr) != 1 or len(plain) != len(HMMA_ARGS): raise ValueError('dmsh_hmma16816_f16: unexpected parameters ' + m.group(2)[:200])
    def group1(pattern, text, how=re.search):   # the first group of a match the helper's signature must have
        found = how(pattern, text)
        if not found: raise ValueError('dmsh_hmma16816_f16: unexpected parameter ' + text[:100])
        return found.group(1)
    name = lambda x: group1(r'(\w+)$', x)
    v = dict(zip(HMMA_ARGS, [name(x) for x in plain]), scr=name(scr[0]), dt=group1(r'thread (\w+)&', plain[-1], re.match), rt=m.group(1))
    body_end = block_end(msl, m.end() - 1)
    # The calls, and the functions they are in: where each opens its body (a brace at the start of a line), and the shared memory array it declares besides the scratch.
    # A function's opening and end are found once (opens, ends): a network's function holds hundreds of calls in up to 1.8 MB of MSL, and a scan per call took Ocarina of Time's 71 HMMA
    # shaders 578 s of one core. A call's function opens at the last line holding only a brace that ends before the call (overlapping ones included, as rfind would find them)
    opens = [o.start() for o in FUNC_OPEN.finditer(msl)]
    calls, callers, ends = [], {}, {}
    for c in HMMA_CALL.finditer(msl):
        if m.start() <= c.start() <= body_end: continue
        k = bisect.bisect_right(opens, c.start() - 3) - 1
        open_at = opens[k] + 1 if k >= 0 else 0
        if open_at > 0 and open_at not in ends: ends[open_at] = block_end(msl, open_at)
        if open_at <= 0 or ends[open_at] < c.start(): raise ValueError('dmsh_hmma16816_f16: a call outside a function body')
        if open_at not in callers:
            scratch = split_params(c.group(1))[-1]
            arrays = [(t, n) for t, n in TG_ARRAY.findall(msl, open_at, ends[open_at]) if n != scratch]
            callers[open_at] = arrays[0] if len(arrays) == 1 else None
        calls.append((c, open_at))
    if not calls: return msl
    shared = set(callers.values())
    direct = len(shared) == 1 and None not in shared
    writers = {f.group(1) for f in re.finditer(r'\b(\w+)\(([^()]*)\)\s*\{', msl) if re.search(r'\bthread (?!const\b)[\w<>, ]+&', f.group(2)) and f.group(1) != 'dmsh_hmma16816_f16'}
    modes = {}
    for open_at in callers:
        modes.update(hmma_bslots(msl, open_at, ends[open_at], [(c, split_params(c.group(1))) for c, o in calls if o == open_at], writers))
    nb = max([2] + [(v // 2 + 1) * 2 for v in modes.values()])
    extra = (', thread simdgroup_half8x8 (&acc)[16], thread simdgroup_half8x8 (&am)[4], thread simdgroup_half8x8 (&bm)[%d], const int bmode' % nb
             + (', threadgroup %s& dm_shm' % next(iter(shared))[0] if direct else ''))
    v.update(adirect='(f & 16) != 0' if direct else 'false', shm_base='(threadgroup half*)&dm_shm[0]' if direct else '(threadgroup half*)nullptr')
    # Edits of the original text, applied from its end: the helper itself, each call's added arguments (the caller's matrices and shared memory), each caller's matrices
    edits = [(m.start(), body_end + 1, '\n%s dmsh_hmma16816_f16(%s%s)\n%s' % (m.group(1), m.group(2), extra, HMMA_BODY % v))]   # spirv-cross has it inlined (always_inline, the line before)
    edits += [(c.end() - 1, c.end() - 1, ', dm_acc, dm_am, dm_bm, %d' % modes.get(c.start(), -1) + (', ' + callers[open_at][1] if direct else '')) for c, open_at in calls]
    edits += [(open_at + 2, open_at + 2, '    simdgroup_half8x8 dm_acc[16], dm_am[4], dm_bm[%d];\n' % nb) for open_at in callers]
    for a, b, text in sorted(edits, key=lambda e: e[0], reverse=True):
        msl = msl[:a] + text + msl[b:]
    return msl

# A store only the very first invocation of a dispatch makes (a compute shader's preamble: `if (-local.x == workgroup.x << log2(local size)) buffer[k] = value`). Hoian_Proc in Splatoon Raiders resets its append counter
# that way, with the other invocations adding to the counter at their end, hundreds of instructions later. On the Switch the first workgroup starts ahead of them. Apple's GPUs start workgroups in no order: a workgroup that had appended before
# the reset lost its plants to the next one's, a different few each frame, and they flickered. So the store leaves the shader and the host makes it, in order, before the dispatch (the reflection's first_store, nvn_mtl.m). Only this exact
# condition counts: it holds for one invocation of a dispatch with a flat local size. The stores may come one guarded block at a time, and a value may be a local set under the same guard, as in
# Mario Kart World's scenery lists (2ce9071c2f22d74f, 5fea9b7ddb30e8a8: `int a = -local; if (first) a = 1; int b = a; if (first) x[0] = 0; if (first) x[2] = b; ...`): the race took a different
# block of trees out of the list at each rebuild. A guarded store left over that is not understood leaves the whole shader as it is. Returns the shader and [(storage binding's MSL name, word, value)]
FIRST_STORE = re.compile(r'float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*int (\w+) = as_type<int>\(\1\) << (\d+);\s*'
                         r'int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \5 \+ 1;\s*bool (\w+) = \6 != \3;\s*')

# A local size with rows: the same test on y and x, and workgroup z zero (Mario Kart World's 0823b08c2027a38d, local size 8x8)
FIRST_STORE_2D = re.compile(r'float (\w+) = as_type<float>\(gl_WorkGroupID\.y\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.y\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*'
                            r'float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_WorkGroupID\.z\);\s*'
                            r'int (\w+) = as_type<int>\(\1\) << (\d+);\s*int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \8 \+ 1;\s*bool (\w+) = \9 != \6;\s*'
                            r'int (\w+) = as_type<int>\(\4\) << (\d+);\s*int (\w+) = ~as_type<int>\(\3\);\s*int (\w+) = \13 \+ 1;\s*bool (\w+) = \14 == \11;\s*'
                            r'bool (\w+) = !\10;\s*bool (\w+) = \15 && \16;\s*bool (\w+) = (?:as_type<int>\(\5\) == 0|0 == as_type<int>\(\5\));\s*bool (\w+) = \18 && \17;\s*(?:bool (\w+) = !\19;\s*)?')

def first_store(msl, local):
    if not local or local[2:] != [1]: return msl, []
    m = FIRST_STORE.search(msl)
    if m:
        if local[1] != 1 or 1 << int(m.group(4)) != local[0]: return msl, []
        guard = r'!' + re.escape(m.group(7))
    else:
        m = FIRST_STORE_2D.search(msl)
        if not m or 1 << int(m.group(7)) != local[1] or 1 << int(m.group(12)) != local[0]: return msl, []
        guard = r'!' + re.escape(m.group(20)) if m.group(20) else re.escape(m.group(19))
    held = re.compile(r'int (\w+) = \w+;\s*if \(%s\)\s*\{\s*\1 = (\d+);\s*\}\s*int (\w+) = \1;\s*' % guard)   # a local that holds a constant in the first invocation
    block = re.compile(r'if \(%s\)\s*\{\s*((?:\(\*\w+\.\w+\)\._m0\[\d+\] = (?:\d+u|uint\(\w+\));\s*)+)\}\s*' % guard)
    plain = re.compile(r'int \w+ = [^;{}]*;\s*')   # arithmetic the compiler left between them
    at, out, stores, values = m.end(), msl[:m.end()], [], {}
    while True:
        h = held.match(msl, at)
        if h: values[h.group(3)] = int(h.group(2)); out += msl[at:h.end()]; at = h.end(); continue
        b = block.match(msl, at)
        if b:
            for n, w, v in re.findall(r'\(\*\w+\.(\w+)\)\._m0\[(\d+)\] = (\d+u|uint\(\w+\));', b.group(1)):
                if v.endswith('u'): stores.append((n, int(w), int(v[:-1])))
                elif v[5:-1] in values: stores.append((n, int(w), values[v[5:-1]]))
                else: return msl, []
            at = b.end(); continue
        p = plain.match(msl, at)
        if not p or held.match(msl, p.start()): break
        out += msl[at:p.end()]; at = p.end()
    rest = msl[at:]
    if not stores or len(stores) > 4 or re.search(r'if \(%s\)' % guard, rest): return msl, []
    return out + rest, stores

# Shared memory byte and half-word stores (STS.U8 / STS.U16, and the bytes a task or mesh shader writes to its stage memory: its primitive indices and count) reach the MSL as calls of
# dmsh_shared_store8_<id> / dmsh_shared_store16_<id>(byte offset, value, the shared memory array) (HelperFunctionManager.GenerateSharedStore). The translator's body is upstream Ryujinx's
# compare-and-swap loop on the word, as SPIR-V has no narrower store into a uint array; Metal's threadgroup memory takes byte stores, so the body becomes one store of the byte or half-word.
# That is the console's store: no atomic, and two lanes writing the same byte leave one of their values. In the loop, the 32 lanes of a mesh shader writing neighbouring bytes contended four
# to a word and retried; Ocarina of Time's foliage mesh shaders (ten such stores a lane) took 8.3-8.6 ms of GPU a frame on an M3 Pro (shaders.md, Stage memory byte stores through a
# compare-and-swap loop). The narrow pointer aliases the uint array for the compiler's type-based alias analysis (uchar, and the may_alias ushort, are "omnipotent char"), so the word loads
# around the store keep their order. tg_spill later turns the address space to device with the rest of the function
SHARED_STORE = re.compile(r'\nvoid (dmsh_shared_store(8|16)_\d+)\(([^)]*)\)\n\{')

SHARED_ALIAS = 'typedef ushort __attribute__((may_alias)) dm_ushort_alias;   // a half-word that may alias the shared memory\'s words (see shared_store_msl)\n'

def shared_store_msl(msl):
    if 'dmsh_shared_store' not in msl: return msl
    found = list(SHARED_STORE.finditer(msl))
    if not found: raise ValueError('dmsh_shared_store: no definition of the expected form')
    out, at = [], 0
    for m in found:
        params = split_params(m.group(3))
        scalar = [re.fullmatch(r'thread const u?int& (\w+)', p) for p in params[:2]]
        arr = re.fullmatch(r'(threadgroup|device) spvUnsafeArray<uint, \d+>& (\w+)', params[2]) if len(params) == 3 else None
        if not arr or not scalar[0] or not scalar[1]: raise ValueError('dmsh_shared_store: unexpected parameters ' + m.group(3)[:160])
        off, val = scalar[0].group(1), scalar[1].group(1)
        space, mem = arr.groups()
        store = (f'(({space} uchar*)&{mem}[0])[uint({off})] = uchar(uint({val}));' if m.group(2) == '8' else
                 f'(({space} dm_ushort_alias*)&{mem}[0])[uint({off}) >> 1u] = ushort(uint({val}));')   # a half-word store is aligned (Maxwell faults on a misaligned one)
        out.append(msl[at:m.end()] + '\n    ' + store + '\n')
        at = block_end(msl, m.end() - 1)
    out.append(msl[at:])
    src = ''.join(out)
    if any(m.group(2) == '16' for m in found):
        u = src.find('using namespace metal;\n')
        if u < 0: raise ValueError('dmsh_shared_store: no using namespace metal')
        u += len('using namespace metal;\n'); src = src[:u] + SHARED_ALIAS + src[u:]
    return src

# A compute shader's local size as the kernel's [[max_total_threads_per_threadgroup]]. A Metal compute pipeline's maxTotalThreadsPerThreadgroup is not fixed at 1,024: on GPUs without
# Dynamic Caching (the M1 and M2, applegpu_g13* / g14*, and the iPhone GPUs of their generation) it falls with the registers the kernel's code takes, 1,024 threads at up to 52 32-bit
# registers, 512 at up to 104, 384 at 128, and the GPU compiler chooses the registers without knowing the size the host will dispatch. The host dispatches the Switch shader's own local size
# (nvn_mtl.m), so a kernel compiled past that size's budget gets a dispatch larger than its pipeline allows, which runs no invocation: Ocarina of Time's froxel light injection 9ed48d1011f1b48b
# (8x8x8) took 128 registers on applegpu_g14g once its 195 uniform reads at computed offsets were bounded as selects (918e9f34) and 104, exactly the 512-thread budget, as plain reads, and its
# 128x72x96 volumes stayed zero on M2s, so the forest haze was gone (issue #2425); on the M3 and later (g15 / g16) every size up to 1,024 fits and the code is the same with the attribute.
# With it, the compiler fits the kernel to the size (spilling where it must), as it does for a pipeline descriptor's maxTotalThreadsPerThreadgroup. Only sizes above 384 get it: 128
# registers, the most a thread has, still allow 384, so no smaller size can be refused, and on g13 / g14 the attribute changes the code of most kernels that already fit (a 64-thread kernel's
# registers and spill go down a little), which those sizes are left out of. The line goes before the kernel's, which tools match at the start of a line. local: the translator's [x, y, z];
# a missing or out-of-range size leaves the shader as it is
MAX_THREADS = 1024   # Metal's limit for a compute threadgroup on every Apple GPU
ALWAYS_FITS = 384   # threads a pipeline allows at the most registers a thread can take (128) on g13 / g14

def max_threads(msl, local):
    if not local or len(local) != 3 or any(not isinstance(v, int) or v < 1 for v in local): return msl
    n = local[0] * local[1] * local[2]
    if n <= ALWAYS_FITS or n > MAX_THREADS or '[[max_total_threads_per_threadgroup(' in msl: return msl
    return re.sub(r'^(kernel void main0\()', '[[max_total_threads_per_threadgroup(%d)]]\n\\1' % n, msl, count=1, flags=re.M)

# Compute shared memory past Metal's limit: Apple's GPUs give a kernel 32 KB of threadgroup memory (maxThreadgroupMemoryLength), and Metal refuses a pipeline that declares more, while Ampere
# gives up to 100 KB. Mario Kart World's DrivePassShaderDefault has 168 compute shaders of 48 KB (spvUnsafeArray<uint, 12288>), seven of them run on the way to the first race; refused, their
# dispatches were skipped. Such a kernel takes its threadgroup arrays from a device buffer the host binds (abi.TG_SLOT): one slice of the arrays' total per workgroup, at the workgroup's index in
# the whole grid, so no two workgroups of a dispatch share one (the host sizes the buffer from the grid: nvn_mtl.m dispatch). A slice index modulo 512 shared them: Apple's GPUs start workgroups in
# no order, and an M6 ran groups 512 apart at the same time from 1,024 groups up (shaders.md, Compute shared memory past 32 KB). Its threadgroup barriers order device memory too, and threadgroup
# atomics become device atomics. Returns the shader and the bytes per workgroup (0: unchanged)
TG_DECL = re.compile(r'^([ \t]*)threadgroup (spvUnsafeArray<(\w+), (\d+)>|\w+) (\w+)(?:\[(\d+)\])?;[ \t]*$', re.M)

TG_BYTES = {'uint': 4, 'int': 4, 'float': 4, 'uint2': 8, 'int2': 8, 'float2': 8, 'uint3': 16, 'int3': 16, 'float3': 16, 'uint4': 16, 'int4': 16, 'float4': 16,
            'half': 2, 'short': 2, 'ushort': 2, 'half2': 4, 'half4': 8, 'char': 1, 'uchar': 1}

def tg_spill(msl):
    m = re.search(r'\bkernel\s+void\s+main0\s*\(', msl)
    if not m: return msl, 0
    decls, total = [], 0
    for d in TG_DECL.finditer(msl, m.end()):
        elem = d.group(3) or d.group(2); n = int(d.group(4) or d.group(6) or 1)
        if elem not in TG_BYTES: raise ValueError(f'threadgroup array of {elem}')
        decls.append((d, total)); total += (TG_BYTES[elem] * n + 15) & ~15
    if total <= abi.TG_MAX: return msl, 0
    depth = 1; i = m.end()   # the parameter list's end: its attributes hold parentheses ([[buffer(0)]])
    while depth: depth += {'(': 1, ')': -1}.get(msl[i], 0); i += 1
    sig_end = i - 1
    params = msl[m.end():sig_end]
    g = re.search(r'uint3 (\w+) \[\[threadgroup_position_in_grid\]\]', params)
    gid = g.group(1) if g else 'dm_tgid'
    add = ('' if g else ', uint3 dm_tgid [[threadgroup_position_in_grid]]') + f', uint3 dm_ntg [[threadgroups_per_grid]], device uchar* dm_tg [[buffer({abi.TG_SLOT})]]'
    out, at = [], 0
    for k, (d, off) in enumerate(decls):
        out.append(msl[at:d.start()])
        t = d.group(2) + (f'[{d.group(6)}]' if d.group(6) else '')
        if k == 0: out.append(f'{d.group(1)}ulong dm_tgbase = ((ulong({gid}.z) * dm_ntg.y + {gid}.y) * dm_ntg.x + {gid}.x) * {total}ul;\n')   # in 64 bits: past 87,381 workgroups of 48 KB the byte offset takes more than 32
        if d.group(6): out.append(f'{d.group(1)}device {d.group(2)} (&{d.group(5)})[{d.group(6)}] = *(device {d.group(2)} (*)[{d.group(6)}])(dm_tg + dm_tgbase + {off});')
        else: out.append(f'{d.group(1)}device {t}& {d.group(5)} = *(device {t}*)(dm_tg + dm_tgbase + {off});')
        at = d.end()
    out.append(msl[at:])
    src = ''.join(out)
    e = src.index(params, m.start()) + len(params)
    src = src[:e] + add + src[e:]
    lines = src.split('\n')
    for n, ln in enumerate(lines):   # the rest of the address space (helper parameters, atomics); the array template's own threadgroup overloads stay
        if 'operator [] (size_t pos)' in ln: continue
        ln = re.sub(r'\bthreadgroup\b(?=\s)', 'device', ln)
        ln = re.sub(r'(?<!mem_device \| )mem_flags::mem_threadgroup\b', 'mem_flags::mem_device | mem_flags::mem_threadgroup', ln)
        lines[n] = ln
    return '\n'.join(lines), total

# textureGatherOffsets: spirv-cross's spvGatherConstOffsets passes coffsets[i] from a loop to gather(), whose offset Metal takes only as a constant expression. At -O1 the loop stays and
# the compiler takes it anyway: the gathers then read texels at offsets that differ between lanes and between runs, with no error. agl's NDSR upscaler builds its input tile this way, so
# some of its lanes saw zeros and the network drew dotted rows in smooth areas of the picture (the same frame, three pictures). Each call becomes its four gathers, offsets written out.
# Offsets that form a 2x2 square (b, b+(1,0), b+(0,1), b+(1,1), in any order) take one gather at b instead: each offset's texel is the (i0, j0) corner of its own footprint, and with
# integer offsets those corners are the four texels of b's footprint, each wrapped on its own either way (Metal's order: x (i0, j1), y (i1, j1), z (i1, j0), w (i0, j0)). agl's NDSR
# networks build their input tile from such squares: 27 of Ocarina of Time's 32 shaders with gather offsets, all compute, 3 to 15 squares each
GATHER_QUAD = {(0, 0): 'w', (1, 0): 'z', (0, 1): 'x', (1, 1): 'y'}

def gather_quad(off):
    """off: the four offsets as written (int2(a), int2(a, b)) → (the square's corner b as MSL, the swizzle that puts its texels in the offsets' order), or None"""
    xy = []
    for o in off:
        m = re.fullmatch(r'int2\(\s*(-?\d+)\s*(?:,\s*(-?\d+)\s*)?\)', o)
        if not m: return None
        xy.append((int(m.group(1)), int(m.group(2) if m.group(2) is not None else m.group(1))))
    bx, by = min(x for x, _ in xy), min(y for _, y in xy)
    rel = [(x - bx, y - by) for x, y in xy]
    if sorted(rel) != sorted(GATHER_QUAD): return None
    return f'int2({bx}, {by})', ''.join(GATHER_QUAD[r] for r in rel)

GATHER4 = 'template<typename T> static inline __attribute__((always_inline)) vec<T, 4> dm_gather4(T a, T b, T c, T d) { return vec<T, 4>(a, b, c, d); }\n'

def gather_offsets(msl):
    if not re.search(r'\bspvGather(Compare)?ConstOffsets\(spvDesc|= spvGather(Compare)?ConstOffsets\(', msl): return msl
    arrays = {}
    for m in re.finditer(r'\bconstant spvUnsafeArray<int2, 4> (\w+) = spvUnsafeArray<int2, 4>\(\{ (.*?) \}\);', msl):
        off = re.findall(r'int2\([^()]*\)', m.group(2))
        if len(off) != 4: raise ValueError('gather offsets: unexpected array ' + m.group(0)[:120])
        arrays[m.group(1)] = off
    out, i = [], 0
    for m in re.finditer(r'\bspvGather(Compare)?ConstOffsets\((?=[^)]*?\w)', msl):
        if m.start() < i or msl[m.end():m.end() + 6] == 'const ': continue   # the wrappers' own definitions
        depth, args, cur, j = 1, [], '', m.end()
        while depth:
            ch = msl[j]; j += 1
            depth += {'(': 1, ')': -1}.get(ch, 0)
            if depth == 0 or (ch == ',' and depth == 1): args.append(cur.strip()); cur = ''
            else: cur += ch
        cmp = bool(m.group(1)); base = 3 if cmp else 4
        if len(args) <= base or args[2] not in arrays: raise ValueError('gather offsets: unexpected call ' + msl[m.start():j][:160])
        tex, smp, off, rest = args[0], args[1], arrays[args[2]], ', '.join(args[base:])
        tail = '' if cmp else ', ' + args[3]
        fn = 'gather_compare' if cmp else 'gather'
        quad = gather_quad(off)
        if quad: out += [msl[i:m.start()], f'({tex}.{fn}({smp}, {rest}, {quad[0]}{tail}).{quad[1]})']
        else: out += [msl[i:m.start()], 'dm_gather4(' + ', '.join(f'{tex}.{fn}({smp}, {rest}, {o}{tail}).w' for o in off) + ')']
        i = j
    out.append(msl[i:])
    msl = ''.join(out)
    if 'dm_gather4(' not in msl: return msl
    k = msl.index('\n', msl.index('using namespace metal;')) + 1
    return msl[:k] + GATHER4 + msl[k:]

# Border colours: Maxwell's sampler takes any RGBA border colour, Metal's only transparent black, opaque black or opaque white, and the host's sampler gets the nearest of them
# (nvn_border.h). A filtered sample is linear in the border colour, r = Σ w·texel + w_border·B, so two samples through samplers that differ only in their border colour give the rest:
# with Metal colours Bn (the sampler) and Bw (its twin), r = s_n + (s_w - s_n)·k, k = (B - Bn) / (Bw - Bn) per channel. Each sampler member a shader samples gets a twin member
# <member>_w and a bit in the block the shader reads (abi.KIND_BORDER: a mask word, then k per bit); the host sets the bit only for a sampler whose colour Metal lacks and whose wrap
# mode reaches the border, so every other sample costs a uniform branch and the twin sample runs only there. Tears of the Kingdom's grass bend map (ff9def5b3c418851) samples the
# previous map with clamp-to-border and border 0.5 (upright): read as transparent black, every row that scrolled in flattened the grass. Samples through sampler arrays, compares
# and gathers keep the nearest Metal colour
BORDER_HELPERS = f'''
struct dm_border_t {{ uint mask; float4 k[{abi.BORDER_SAMPLERS}]; }};
static inline __attribute__((always_inline)) float4 dm_bmix(float4 r, float4 w, float4 k) {{ return r + (w - r) * k; }}
static inline __attribute__((always_inline)) float dm_bmix(float r, float w, float4 k) {{ return r + (w - r) * k.x; }}
template<typename V, typename T, typename... A> static inline __attribute__((always_inline)) V dm_bs(V r, constant dm_border_t* b, uint i, T t, sampler w, A... a)
{{
    if (DM_BORDER_LIVE && (b->mask >> i & 1u)) r = dm_bmix(r, t.sample(w, a...), b->k[i]);
    return r;
}}
'''

# The corrections also depend on the function constant abi.BORDER_FC: the host sets it (true: off) for a pipeline whose draw or dispatch binds no sampler with a twin, the usual case,
# and the twin samples are compiled out with the registers they kept live. A uniform branch still costs them: Ocarina of Time's 1080p material passes spilled 300-400 bytes a thread,
# 1.5 ms of a 26.7 ms frame on a MacBook Air M4, and its kernels, NDSR's network among them, 0.25 ms more (PR #1929); Splatoon Raiders' c0ae76e0ca76b574 spilled 592 bytes a thread
# and none without them (PR #1203). A pipeline the host does not specialize leaves the constant undefined: every sample corrects as its bit says
BORDER_LIVE_FC = (f'constant bool dm_border_off [[function_constant({abi.BORDER_FC})]];\n'
                  'constant bool dm_border_live = !is_function_constant_defined(dm_border_off) || !dm_border_off;\n#define DM_BORDER_LIVE dm_border_live\n')

def border_emu(msl):
    """The samples of the argument buffer's textures through its sampler members, T.sample(S, args) → dm_bs(T.sample(S, args), block, bit, T, S_w, args) (see BORDER_HELPERS);
    the source unchanged when it samples through none. Only samples of float textures: an integer texture is not filtered, and its border colour is Maxwell's integer one, which
    the host does not correct (it keeps Metal's nearest)"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    if not body: return msl
    # members with attributes after their id too (see reflect): the twins' ids go after every one of them
    members = [(m.group(1).strip(), m.group(2), int(m.group(3))) for m in re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];', body.group(1), re.M)]
    samplers = [n for ty, n, _ in members if ty == 'sampler']
    ftex = {n for ty, n, _ in members if re.fullmatch(r'(texture|depth)\w*<float(,.*)?>', ty)}
    call = re.compile(r'spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+), ')
    used = {m.group(2) for m in call.finditer(msl) if m.group(1) in ftex}
    bit = {n: i for i, n in enumerate([n for n in samplers if n in used][:abi.BORDER_SAMPLERS])}
    if not bit: return msl
    def width(ty):   # argument buffer entries a member takes
        a = re.fullmatch(r'array<.*,\s*(\d+)>', ty); return int(a.group(1)) if a else 1
    nid = max(aid + width(ty) for ty, _, aid in members)   # the new members go after every entry spirv-cross gave out
    add = ''.join(f'    sampler {n}{abi.BORDER_TWIN} [[id({nid + i})]];\n' for n, i in bit.items()) + f'    constant dm_border_t* {abi.BORDER_MEMBER} [[id({nid + len(bit)})]];\n'
    end = body.end(1)
    msl = msl[:end] + ('' if msl[end - 1] == '\n' else '\n') + add + msl[end:]
    for m in reversed([m for m in call.finditer(msl) if m.group(2) in bit and m.group(1) in ftex]):   # the last first: a sample inside another's arguments is rewritten before the outer one copies them
        depth, i = 1, m.end()
        while depth:
            depth += {'(': 1, ')': -1}.get(msl[i], 0); i += 1
        args = msl[m.end():i - 1]
        twin = f'spvDescriptorSet0.{m.group(2)}{abi.BORDER_TWIN}'
        msl = msl[:m.start()] + f'dm_bs({msl[m.start():i]}, spvDescriptorSet0.{abi.BORDER_MEMBER}, {bit[m.group(2)]}u, spvDescriptorSet0.{m.group(1)}, {twin}, {args})' + msl[i:]
    return msl.replace('using namespace metal;\n', 'using namespace metal;\n' + BORDER_LIVE_FC + BORDER_HELPERS, 1)

# gl_FragCoord.z against a depth the shader read. NVIDIA stores a fragment's depth as the z its pixel shader reads, so a shader can test a fragment against a depth image of its own surface and
# find them equal. Apple's GPU stores a value below the [[position]].z its fragment shader reads at some pixels: by up to one ulp of z plus a fraction of the depth's step across a pixel
# (dm/gfx/tests/t_zsnap.m: 0.2% of a perspective triangle's pixels, 2% of flat ones, none above, the largest gap one ulp plus 2^-23 of fwidth(z)); writing [[position]].z as the depth would
# keep them equal, but takes the depth test after the shader, and the depth a prepass stores would then fail an EQUAL pass on the same geometry (Metroid Dread's G-buffer).
# Ocarina of Time's mist discards a fragment whose gl_FragCoord.z is above the depth its depth-only pass stored, and lost its own surface at scattered pixels: dark dots flickering in the mist.
# So where gl_FragCoord.z and a value read from a texture meet in one comparison of main0, through any arithmetic on either side (the mist's z > d; z * k > d * k), the z read takes that value
# when it lies in the window the stored depth of the fragment's own surface can take, [z - ulp(z) - 2^-20 fwidth(z), z): the comparison then sees the equality NVIDIA's does, whatever its
# operator. A value outside the window, a read whose texture value comes after it or in a block it does not see, and z read in another function stay as they were
ZSNAP = ('static inline __attribute__((always_inline)) float dm_zsnap(float z, float d, float zw)   // see frag_z_snap\n'
         '{\n    float lo = as_type<float>(as_type<int>(z) - 1) - zw * 0x1p-20f;\n    return (z > 0.0f && d < z && d >= lo) ? d : z;\n}\n')

ZSNAP_WIDTH = 'dm_zw'

def frag_z_snap(msl):
    """→ (msl with each paired gl_FragCoord.z read in main0 as dm_zsnap(gl_FragCoord.z, <texture value>, dm_zw) and dm_zw = fwidth(gl_FragCoord.z) first in main0's body, the number of reads).
    main0's body is dm_body_main0's when it has one (dmsh_side.shared_body: a library's sources), the build's rewrite comes before that"""
    if 'gl_FragCoord.z' not in msl: return msl, 0
    shared = dmsh_side.body(msl)
    try: e = None if shared else dmsh_side.entry(msl, 'main0')
    except ValueError: return msl, 0
    if shared: head, end = shared[1:]; params = re.findall(r'[^,]+', msl[shared[0]:head])
    elif e: head, end, params = e['head'], e['end'], e['params']
    else: return msl, 0
    if not any(re.search(r'\bfloat4 gl_FragCoord( \[\[position\]\])?\)?$', p.strip()) for p in params): return msl, 0
    first = msl.find('{', head) + 1
    lines = msl[first:end - 1].split('\n')
    zr, xr, scope, line_of = {}, {}, {}, {}   # each local's gl_FragCoord.z reads and texture values it is computed from; the blocks it is declared in; its line
    stack, blocks, pairs = [0], 0, {}
    for i, ln in enumerate(lines):
        m = re.match(r'\s*(?:const )?(float|bool|int|uint)\s+(_\d+) = (.+);$', ln)
        if m:
            ty, v, ex = m.groups(); refs = re.findall(r'\b_\d+\b', ex)
            z = set().union(*(zr.get(r, set()) for r in refs)); x = set().union(*(xr.get(r, set()) for r in refs))
            if ex == 'gl_FragCoord.z': z = {v}
            elif ty == 'float' and re.search(r'\.(sample|read)\(', ex) and not z: x = {v}   # a scalar read from a texture (a depth image); not sample_compare or gather
            zr[v], xr[v], scope[v], line_of[v] = z, x, tuple(stack), i
            c = re.fullmatch(r'(_\d+) (?:<=|>=|<|>|==|!=) (_\d+)', ex) if ty == 'bool' else None
            if c:
                a, b = c.groups()
                for zs, xs in ((a, b), (b, a)):
                    if zr.get(zs) and xr.get(xs) and not xr.get(zs) and not zr.get(xs):
                        for zv in zr[zs]: pairs.setdefault(zv, set()).update(xs_ for xs_ in xr[xs])
        for ch in re.findall(r'[{}]', ln):
            if ch == '{': blocks += 1; stack.append(blocks)
            elif len(stack) > 1: stack.pop()
    n = 0
    for zv in sorted(pairs, key=lambda v: line_of[v]):
        xs = [x for x in sorted(pairs[zv], key=lambda v: line_of[v]) if line_of[x] < line_of[zv] and scope[zv][:len(scope[x])] == scope[x]]   # declared before the read, in a block it sees
        if not xs: continue
        expr = 'gl_FragCoord.z'
        for x in xs: expr = f'dm_zsnap({expr}, {x}, {ZSNAP_WIDTH})'
        i = line_of[zv]; lines[i] = lines[i].replace(f'{zv} = gl_FragCoord.z;', f'{zv} = {expr};'); n += 1
    if not n: return msl, 0
    body = f'\n    float {ZSNAP_WIDTH} = fwidth(gl_FragCoord.z);' + '\n'.join(lines)   # in uniform control flow, before anything the body does
    msl = msl[:first] + body + msl[end - 1:]
    return msl.replace('using namespace metal;\n', 'using namespace metal;\n\n' + ZSNAP, 1), n


# NVIDIA's float blender with "zero times anything is zero" on. The 3D engine has a method for it (SET_BLEND_FLOAT_OPTION, 0x670, bit ZERO_TIMES_ANYTHING_IS_ZERO in NVIDIA's published class
# headers), and NVN's driver sets it in its start-up command list: the word 0x80010670 (an immediate write of 1) is in the sdk module of Ocarina of Time and of 12 of 14 other games looked at.
# So a blend term whose weight is 0 is 0 whatever the colour, where IEEE (and Direct3D 11, which wants the NaN kept "including 0.0 blend weights") gives 0 x NaN = 0 x Inf = NaN: a pixel
# shader may leave NaN in the colour of a pixel it adds nothing to and say so with alpha 0, and under SRC_ALPHA blending the target keeps its value. Apple's blender multiplies as IEEE does,
# stores the NaN (0 x Inf too; only a factor of ZERO itself leaves its term out: dm/gfx/tests/t_blend_zero.m, on an M6), and every later pass that reads the target spreads it.
# Ocarina of Time's half-resolution mist shader 5ad65c0eefd09c7f averages hit positions over a layer whose density is 0 there (a positive hit count, a weight sum of 0: 1/0 x 0; its FMULs
# carry no .FMZ, so the translation is exact) and writes them with alpha 0 into an RGBA16F target blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA. The mist composite carried the NaN into the scene
# and bloom turned each into a white cloud.
# The rule belongs to the blender, so it depends on the draw's blend state, which a shader does not know: the host gives it (nvn_mtl.m bz_bind) as a word at fragment buffer
# DMSH_BLEND_BUFFER, DMSH_BLEND_ZERO_BITS bits per colour output: where the output's source colour weight is zero (dmsh_abi.h has the cases), a non-finite rgb is returned as 0, which is
# what the zero weight makes of it. With blending off, on a target that is not floating point, or with a weight the alpha does not make zero, the output's bits are 0 and it is returned as
# it is: NVIDIA stores that NaN or Inf too. This part never changes a finite colour, nor the alpha.
# Not covered, as a shader cannot see it: a NaN or Inf already in the target under a zero destination weight, and a non-finite weight on a colour of 0.
# The guard goes into main0 before the build makes its other entry points, so each of them has it: they call main0's body (dmsh_side.shared_body: main0_u, main0_d, main0_f and
# the side library's) and take main0's parameters, the word's among them. It is written as a call, not as stores to out, which dmsh_entry.fs_uint reads to tell what a shader writes
# The same call rounds what goes into a normalized target as NVIDIA's colour unit does (the struct's second word, round, DMSH_BLEND_ROUND_BITS a colour output, from nvn_mtl.m rop_word).
# The two convert a float colour differently where it lies within rounding of a half step: Apple stores the nearest integer to the exact x * (2^n - 1) (dm/gfx/tests/t_rop_round.m
# measures it, an M6), NVIDIA's rounds the float32 product to nearest even. Sonic Frontiers writes its G-buffer material flags as the constant 66.5 / 255 in float32
# (0.2607843279838562): exactly 66.5 once multiplied in float32, so 66 on the Switch, but 66.5000036 exactly, so 67 on the Mac, whose bit 0 sends the lighting to an ambient
# the G-buffer leaves black. Here the colour becomes rint(float32(saturate(c) * scale)) / scale, which Apple's conversion stores as that integer. Only where the stored value is the
# conversion of the shader's colour: blending off, MIN or MAX (they pick one of two stored values) or a logic operation; a blended sum is rounded after the blend, which a shader cannot reach.
BLEND_ZERO = '''struct dm_blend_state { uint zero; uint round; };   // see dmsh_rewrite.py blend_zero
static inline __attribute__((always_inline)) float4 dm_bz(float4 c, uint m)
{
    m &= %du;
    if (!((m == 1u && c.w == 0.0f) || (m == 2u && c.w == 1.0f))) return c;
    return float4(select(c.xyz, float3(0.0f), (as_type<uint3>(c.xyz) & uint3(0x7f800000u)) == uint3(0x7f800000u)), c.w);   // NaN or Inf by its bits: fast math may assume there is none
}
static inline __attribute__((always_inline)) float4 dm_rop(float4 c, uint m)
{
    m &= %du;
    if (m == 0u) return c;
    float4 s = m == %du ? float4(255.0f) : (m == %du ? float4(65535.0f) : float4(1023.0f, 1023.0f, 1023.0f, 3.0f));
    return rint(precise::clamp(c, 0.0f, 1.0f) * s) / s;   // precise: NaN goes to 0, as both store it; the product is float32's, rint rounds it to nearest even
}
static inline __attribute__((always_inline)) void dm_blend0(thread main0_out& dm_o, constant dm_blend_state& dm_s)
{
    uint m = dm_s.zero, r = dm_s.round;
    if ((m | r) == 0u) return;
%s}
'''
BLEND_ZERO_PARAM = f'constant dm_blend_state& dm_blend [[buffer({abi.BLEND_BUFFER})]]'


def blend_zero(msl):
    """→ (msl with dm_blend0(out, dm_blend); before each `return out;` line of main0, the word as main0's last parameter and the helpers before main0, the number of returns guarded).
    Unchanged (0) when main0 has no float4 colour output, is not of a shape dmsh_side.entry knows, has its body shared already, or returns other than on a line of its own"""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    cols = [(nm, int(c)) for ty, nm, c in re.findall(r'^\s*(\w+)\s+(\w+)\s*\[\[color\((\d+)\)\]\];', st.group(1), re.M) if ty == 'float4'] if st else []   # not index(1): a second blend source
    if not cols or 'dm_blend0(' in msl or f'[[buffer({abi.BLEND_BUFFER})]]' in msl or dmsh_side.body(msl): return msl, 0
    try: e = dmsh_side.entry(msl, 'main0')
    except ValueError: return msl, 0
    if not e or e['ret'] != 'main0_out': return msl, 0
    body = msl[e['head']:e['end']]
    n = len(re.findall(r'\breturn\b', body))
    body, k = re.subn(r'^([ \t]*)return out;$', lambda m: f'{m.group(1)}dm_blend0(out, dm_blend);\n{m.group(1)}return out;', body, flags=re.M)
    if not k or k != n: return msl, 0
    bits, rb = abi.BLEND_ZERO_BITS, abi.BLEND_ROUND_BITS
    helper = BLEND_ZERO % ((1 << bits) - 1, (1 << rb) - 1, abi.ROUND_8, abi.ROUND_16,
                           ''.join(f'    dm_o.{nm} = dm_rop(dm_bz(dm_o.{nm}, m >> {bits * c}u), r >> {rb * c}u);\n' for nm, c in cols))
    return msl[:e['start']] + helper + '\n' + msl[e['start']:e['head'] - 1] + (', ' if e['params'] else '') + BLEND_ZERO_PARAM + ')' + body + msl[e['end']:], k
