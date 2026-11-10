#!/usr/bin/env python3
# Route B offline build: original Switch shaders → SPIR-V (dmsh_xlate, the Ryujinx translator) → spirv-cross MSL → metallib + replay reflection JSON.
# No translation at runtime (iOS has no JIT): build output goes into the shader cache directory, file name = Switch control segment key (dm_ctl_sha16, same as column 4 of shader_map.tsv) + "_b".
# Source of query answers: the answer table in the game profile, dm/games/<id>/shader_facts.tsv (dmsh_facts.py) - texture types, vertex attribute / fragment output component types, compute thread group size and shared memory.
# Usage: dmsh_build.py <Switch package dir> <output cache dir> [key...] (no keys = all); build output is the image <output cache dir>/b-<chain version>.img (same for IOSOUT; dm/tools/shader_lib.py), for the chain version see dmsh_chain.py.
# The folder b-<chain version>/ beside it holds only the shaders in flight: a finished shader's files go into the shader cache (cas.db, under stage/), and the image is written from there
# at the end (publish_library), so a build, stopped or not, leaves a few files and not one per shader output (Octopath Traveler 0, stopped in its compile: 68,230 files). The translator's
# working files go into a folder per batch of shaders, removed when the batch is done; a working folder whose build is gone is removed at the next start (sweep_work).
# A full build (no keys) also stamps both directories with the game and the digest of its profile inputs (inputs.tsv) and writes the profile's c1_full.tsv and cb_extent.tsv (c1_guard.py, cb_extent.py)
# from its Mac outputs. One output directory per game: the chain version is the same for every game   Environment: DM_GAME=game profile (required, dm/games/<id>), XLATE=path to the translation tool (default the in-repo dmsh_xlate_bin), DMSH_CAS=the compile cache, one file (shader_cache.py; default <output cache dir>/cas.db), DMSH_PREFILL=Mac library root[=iOS library root] (separated by :, their b-*/ go into the MSL-keyed compile cache first; sibling b-*/ always do), J=concurrency
# Build output: <key>_b.metallib, <key>_b.json (replay reflection, including drv_reads, the runtime read offsets into driver segments c0 / c2), <key>_b.metal (MSL)
# Environment IOSOUT=dir: the same MSL also goes through dm/ios/tools/ios_metal.compile to produce an iphoneos metallib + reflection (chain_metal records the iOS compile convention)
# Final summary: success / failure counts and the distribution of reasons; distribution of driver segment read offsets (those outside the range the replay layer can fill are listed separately);
# a [RED] line per shader that built but lost something (shader_regressions.report_losses), which does not change the exit status
import atexit, bisect, inspect, itertools, os, re, sys, json, mmap, struct, hashlib, shutil, subprocess, tempfile, time, collections, threading, concurrent.futures as cf
import dmsh_geometry, dmsh_atomic, dmsh_wide, dmsh_meshstage, dmsh_abi as abi   # abi: the driver segment layout and the binding kinds the host shares (dm/gfx/nvn/dmsh_abi.h)
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # Workspace root (game files, build outputs and run artifacts are all under it)
HOME = os.path.expanduser('~')
dm_dir, out = sys.argv[1:3]; want = set(sys.argv[3:]); cache_root = out
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_chain, dmsh_fetch, dmsh_facts, dmsh_mtlcache, dmsh_pch, dmsh_vin, dmsh_pull, dmsh_side, dmsh_mesh, dmsh_ampere, shader_regressions, shader_cache, tool_lock   # translation chain version: verify the tools against the lock file first; build output goes into b-<V>/
_bad = tool_lock.check_tools()
if _bad: sys.exit('\n'.join('[RED] translation chain tools do not match the lock file: ' + b for b in _bad) + '\n(if the tools changed, run tool_lock.py --update-lock and commit the lock file; if the translator just was not rebuilt, rebuild it)')
CHAIN = dmsh_chain.version(); out = os.path.join(out, 'b-' + CHAIN)
GAME = dmsh_chain.load_game()   # the current game profile (DM_GAME): the tables come from here
if f'"{CHAIN}"' not in open(dmsh_chain.HEADER).read(): sys.exit(f'[RED] {dmsh_chain.HEADER} does not hold the current translation chain version {CHAIN}: run dmsh_chain.py --header first and commit it (otherwise the host cannot find these libraries)')
INPUTS = dmsh_chain.inputs(GAME)   # the digest of the profile's inputs, which a full build stamps on its libraries (b-<V>/inputs.tsv)
XLATE = os.environ.get('XLATE', os.path.join(dmsh_chain.XLATE_DIR, 'dmsh_xlate'))
SPIRV_CROSS = dmsh_chain.SPIRV_CROSS   # the in-repo build the lock records
FACTS = dmsh_facts.load()
if not os.path.exists(dmsh_facts.TABLE): sys.exit(f'[RED] no answer table {dmsh_facts.TABLE} (it comes with the game profile; an empty table = every query answered with defaults)')
IOSOUT = os.environ.get('IOSOUT')   # destination for requested iOS libraries (iphoneos metallib + reflection)
# Which metallibs this run builds: DMSH_TARGETS=mac,ios (default: both when IOSOUT is set, else mac). The Mac
# sources and reflection are written even when 'mac' is left out (the profile's c1_full / cb_extent tables are
# made from them), so an iOS-only build skips only the macOS compile, about half of the Metal work.
TARGETS = {t.strip() for t in (os.environ.get('DMSH_TARGETS') or ('mac,ios' if IOSOUT else 'mac')).split(',') if t.strip()}
if not TARGETS or not TARGETS <= {'mac', 'ios'}: sys.exit(f'[RED] DMSH_TARGETS takes mac and/or ios: {",".join(sorted(TARGETS)) or "empty"}')
if 'ios' in TARGETS and not IOSOUT: sys.exit('[RED] DMSH_TARGETS asks for iOS libraries without IOSOUT')
if 'ios' not in TARGETS: IOSOUT = None   # every side library, cache, cleanup and stamp follows the same selection
IOS_PULL = os.environ.get('DMSH_IOS_PULL') != '0'   # DMSH_IOS_PULL=0: no iOS pulling libraries, when nothing in the app reads them (the NSP script's ios_wants_pulling). The reflection is the same: its 'pull' comes from the variant, not from the compile
if IOSOUT:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); import ios_metal
    IOSOUT = os.path.join(IOSOUT, 'b-' + CHAIN); os.makedirs(IOSOUT, exist_ok=True)
import shader_lib, romfs   # dm/tools (on the path shader_regressions puts it): the library's image, which this build writes (publish_library)
for _d in [out] + ([IOSOUT] if IOSOUT else []):   # one chain version serves every game, so b-<V>/ alone does not keep two games' libraries apart
    _g = shader_lib.read_stamp(_d).get('game')
    if _g and _g != GAME.id: sys.exit(f'[RED] {_d} holds the shader libraries of game {_g}, not {GAME.id}: give each game its own output directory')
MAP = str(GAME.table('shader_map.tsv'))
BF_WANT_FILE = str(GAME.table('bf_want.tsv'))
BF_WANT = {line.strip() for line in open(BF_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(BF_WANT_FILE) else set()
DUAL_WANT_FILE = str(GAME.table('dual_want.tsv'))   # pixel shaders a pipeline blends with their output 1 as the second source (see fs_dual), produced by the host (DM_NVN_DUAL_WANT)
DUAL_WANT = {line.split()[0] for line in open(DUAL_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(DUAL_WANT_FILE) else set()
XC = ['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native', '--msl-pad-fragment-output']
# spirv-cross writes the translator's FMin / FMax / FClamp as fast::min / max / clamp, whose result for a NaN operand is the GPU's choice; Maxwell's FMNMX returns the other operand
# and its saturation gives 0, as Metal's precise:: functions (IEEE fmin / fmax) do on every GPU. The M4 Pro's fast ones happen to agree, an A20 Pro's do not: Super Mario Galaxy 2's
# GX lighting takes max(0, NaN) from light directions left at zero, and the half of Mario its fill light faces came out white on the iPhone (GitHub issue 72; vk_shaders.py PRECISE)
def precise(msl): return re.sub(r'\bfast::(min|max|clamp)\(', r'precise::\1(', msl)

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
            depth, j = 1, w.end()
            while depth and j < len(body): depth += {'(': 1, ')': -1}.get(body[j], 0); j += 1
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
    if (b->mask >> i & 1u) r = dm_bmix(r, t.sample(w, a...), b->k[i]);
    return r;
}}
'''
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
    return msl.replace('using namespace metal;\n', 'using namespace metal;\n' + BORDER_HELPERS, 1)
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
STORE = shader_cache.Store(os.environ.get('DMSH_CAS', os.path.join(cache_root, 'cas.db')))
atexit.register(STORE.close)   # folds its log into the one file
CAS_LOCKS, CAS_LOCKS_GUARD = {}, threading.Lock()
PRE_STATS = collections.Counter()
# -fpreserve-invariance: vertex positions are marked [[invariant]], but without this flag Metal does not promise that two different shaders compute the same expression to the same value; on Maxwell the same multiply-adds always agree.
# Super Mario Odyssey draws a depth pre-pass with alRenderDepthOnly, then the G-buffer with another vertex shader and depth test less-or-equal: a hair off and the test failed, leaving those pixels with old content (Cap Kingdom's ground in 32x32 blocks)
# -O1: the front end's -O2 (CorrelatedValuePropagation, then SimplifyCFG) folds the translated shaders' repeated tests of one value into switches whose cases jump past their own join.
# Apple's GPU backend then exceeds its control-flow ID budget ("Max Unstruct ID 758 ... exceeds supported limit", a note, not an error) and falls back to a slower lowering:
# Odyssey's alRenderMaterialUber took 25 minutes and 17.3 MB of GPU code, 31 s and 1.3 MB at -O1. Ordinary shaders come out within a few percent either way (the backend optimizes again)
# -fwrapv: signed integer overflow wraps, as SPIR-V's integer arithmetic and the Switch's GPUs do; in MSL it is undefined, and the front end folds around it. Ocarina of Time's mesh shader
# 35785cddc971290c sign-extends a packed 16-bit vertex field as a = w * 65536, then (a >> 31) << 16 | uint(a) >> 16; without the flag the compiler took the sign bits from bits 15-30 of w,
# so the lightmap / probe UV's x came out far too large and models drawn with mesh shaders took their ambient light from the wrong texels (the bright band on the Deku Tree's walls).
# The flag also drops inbounds from pointer arithmetic (Metal's clang has no -fwrapv-pointer) and keeps loop bounds that may wrap (test_dmsh_wrapv.py)
MAC_METAL = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', '-fwrapv']
MAC_RECORD = ' '.join(MAC_METAL) + '; xcrun -sdk macosx metallib'
# The compiles run the tools xcrun names, found once: each xcrun launch looks them up again, about 4 ms of a compile's two launches (8 ms a shader). -isysroot is what xcrun -sdk passes
# (as SDKROOT), and the outputs are the same bytes (.air and .metallib, checked on Odyssey's shaders); without it the driver targets the running macOS, not the SDK, and they differ.
# MAC_RECORD names the xcrun command, which runs the same compiler
def xcrun_out(*args): return subprocess.run(['xcrun', '-sdk', 'macosx', *args], capture_output=True, text=True, check=True).stdout.strip()
MAC_RUN = [xcrun_out('-f', 'metal'), '-isysroot', xcrun_out('--show-sdk-path'), *MAC_METAL[4:]]
MAC_LINK = [xcrun_out('-f', 'metallib')]
# -fbracket-depth: the front end stops at 256 nested brackets ("bracket nesting level exceeded maximum of 256"). Ocarina of Time's compute shaders 1f78730053feb036 and e172bd7ec218ab8e
# (3.5 MB of MSL each) nest 259 deep in one expression and were left out. A source within the default limit compiles to the same bytes with it (checked on macOS and iOS), so, like the
# precompiled header, the records do not name it and the Metal cache keeps its libraries. ios_metal.DEPTH is the same
METAL_DEPTH = ['-fbracket-depth=1024']
# The outer shader cache restores compiled libraries directly, before the MSL compile cache can check their compiler. Keep override builds separate there too.
METAL_FP = dmsh_mtlcache.compiler_id('macosx') + ('\0iphoneos\0' + dmsh_mtlcache.compiler_id('iphoneos') if IOSOUT else '')
def mac_compile(msl, air, lib):   # same interface as ios_metal.compile; the precompiled header and METAL_DEPTH change no output, so MAC_RECORD does not name them (dmsh_pch.py)
    dmsh_pch.compile(MAC_RUN + METAL_DEPTH, msl, air)
    subprocess.run([*MAC_LINK, air, '-o', lib], check=True)
# The NVN driver's constant segments have one layout in every game, which dm/gfx/nvn/dmsh_abi.h holds for the host and this build (through dmsh_abi.py); nvn_mtl.m fills them the same way at run time.
#   c2 C2_TEX + 8 r: handles bound with nvnCommandBufferBindTexture (combined texture and sampler), slot r. The driver has 32 such slots, but a game may bind higher ones, whose words lie in the
#     image table (Hades II binds slot 34, and no image), so a texture instruction's handle anywhere below the separate texture table names a combined slot. In the image
#     table that slot reads the image when one is bound there (nvn_mtl.m tex_slot_h): a size query of the image a compute shader writes (Unreal's GetDimensions on a RWTexture3D),
#     whose type the translator takes from the shader's surface instructions on that handle (dmsh_xlate ImageDims)
#   c2 C2_IMG + 8 r: images (nvnCommandBufferBindImage), slot r < C2_IMG_SLOTS
#   c2 C2_SEPTEX + 8 r: separate textures (nvnCommandBufferBindSeparateTexture), reported as slot SEPTEX_SLOT0 + r, where the host binds them (hle_nvn.c, NVN_SEPTEX_SLOT0)
#   c2 C2_SAMP + 8 r: separate samplers (nvnCommandBufferBindSeparateSampler), slot r < C2_SEP_SLOTS
#   c0 base + 0x10 r: storage buffer r (64-bit address, 32-bit size), base C0_SSBO_<stage> (the hull / domain stages' bases were never seen)
TEX_BASE, IMG_BASE, IMG_SLOTS, SEPTEX_BASE, SEPTEX_SLOT0, SAMP_BASE = abi.C2_TEX, abi.C2_IMG, abi.C2_IMG_SLOTS, abi.C2_SEPTEX, abi.SEPTEX_SLOT0, abi.C2_SAMP
SSBO = {'vs': abi.C0_SSBO_VS, 'cs': abi.C0_SSBO_CS, 'gs': abi.C0_SSBO_GS, 'ps': abi.C0_SSBO_PS, 'ts': abi.C0_SSBO_TS, 'ms': abi.C0_SSBO_MS}

def record_index(path):   # → (an open descriptor, {a record's key: its bytes' (start, end)}): shader_split.py writes each record's key at +0x08, its bytes from its marker to the next one
    # Scanned through a read-only mapping that is closed again, so no package stays in this process. Pokopia's ps_rdb.bin (4.25 GB) read and cut into its 105,118
    # records left the process at 8.7 GB, and each fork (every spirv-cross and Metal compile) then took 75 ms instead of 3 with the GIL held: one shader worker at a time (GitHub issue #1036)
    fd = os.open(path, os.O_RDONLY); n = os.fstat(fd).st_size
    if not n: return fd, {}
    with mmap.mmap(fd, n, access=mmap.ACCESS_READ) as m:
        offs = [x.start() for x in re.finditer(b'BTCDBTCD', m)] + [n]
        return fd, {m[o + 8:o + 16].hex(): (o, e) for o, e in zip(offs, offs[1:])}

def clone(src, dst):   # APFS copy-on-write, else a plain copy; the clonefile call, not a cp: a fork of this large process costs about 6 ms
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    dmsh_mtlcache.clone(src, dst)

def atomic_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    with open(tmp, 'w') as f: json.dump(value, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)

def source_without_rule_bodies():   # this file's code form (dmsh_chain.code_form): no comments, docstrings or formatting, and the rule functions' bodies blanked
    return dmsh_chain.code_form(open(__file__, 'r').read(), {f.__name__ for f, _ in RULES})

def shader_rule_source(name):
    return b''.join(inspect.getsource(f).encode() for f, applies in RULES if applies(name))

def tool_fingerprint():
    sc = subprocess.run([SPIRV_CROSS, '--version'], capture_output=True, text=True, timeout=30)
    return ('lock\0' + open(dmsh_chain.LOCK).read() + '\0options\0' + dmsh_chain.options() +
            '\0xlate\0' + dmsh_chain.xlate_source_sha() + '\0spirv-cross\0' + sc.stdout + sc.stderr +
            '\0xc\0' + repr(XC) + '\0mac\0' + MAC_RECORD + '\0ios\0' + (ios_metal.RECORD if IOSOUT else '') + '\0metal\0' + METAL_FP)

# The profile keys that change how a shader is built (nvn_*, shader_*), as dmsh_chain.inputs takes them: part of every shader's cache key
PROFILE_ROWS = dmsh_chain.profile_rows(GAME)
def shader_key(key, name, rec):
    """The per-shader cache key. It covers the profile's build keys and the shader's pairs; a vertex shader with pairs also takes its pixel shaders' keys (pair_keyed), since vs_pad reads their
    MSL. Before, these were only in the chain version, so a build after changing only them hit the old entries and filed them under the new version (with an empty DMSH_CAS as the workaround)"""
    pc = dmsh_facts.row(key, FACTS[key]).encode() if key in FACTS else 'missing'.encode()
    map_row = MAP_ROWS.get(key, 'missing'.encode()) + MAP_CONFLICT_ROWS.get(key, b'')
    payload = (b'record\0' + rec + b'\0map\0' + map_row + b'\0pc\0' + pc +
               b'\0tools\0' + TOOL_FP + b'\0code\0' + CODE_FP.encode() +
               b'\0rule\0' + shader_rule_source(name) + b'\0profile\0' + PROFILE_ROWS + b'\0pairs\0' + ','.join(sorted(PAIRS.get(key, ()))).encode() +
               b'\0dual\0' + (b'1' if key in DUAL_WANT else b'0'))   # fs_dual emits another entry point for listed shaders; the cache must track that choice
    return hashlib.sha256(payload).hexdigest()
def pair_keyed(digest, pixel_digests):   # a vertex shader's key with its pixel shaders' keys (vs_pad)
    return hashlib.sha256((digest + '\0' + ','.join(pixel_digests)).encode()).hexdigest()

def cache_meta_path(key): return f'index/{key}.json'
def cache_path(digest): return f'shader/{digest}'
CAS_USED = '_b_cas.txt'   # in the Mac library directory b-<V>/: the cache_path digests of its last full build, one per line

# A finished shader's outputs are staged in the cache, under a name of this output folder's, and leave the disk (commit); the image is written from them at the end (publish_library).
# A stopped build leaves them there, and the next build of this folder clears them first: its shaders come back from the cache entries publish_cache wrote
STAGE = 'stage/' + hashlib.sha256(os.path.realpath(out).encode()).hexdigest()[:16]
def label_dir(label) -> str: return out if label == 'mac' else IOSOUT or ''   # '' when this build makes no iPhone libraries

def output(label, name):
    """An output file's bytes: on disk while its shader is in flight, else staged; for a partial build, a shader it does not build is read from the image it builds on. None when there is none"""
    try:
        if label_dir(label):
            with open(os.path.join(label_dir(label), name), 'rb') as f: return f.read()
    except FileNotFoundError: pass
    data = STORE.get(f'{STAGE}/{label}/{name}')   # put before its file is removed (commit), so a file gone meanwhile is found here
    if data is not None or not want: return data
    m = OUT_KEY.match(name); old = OLD_IMAGE.get(label)
    if old is None or not m or m.group(1) in want: return None   # a key this build makes has no older outputs: the start removed them
    try: return (old / name).read_bytes()
    except OSError: return None

def output_of(label, name) -> bytes:   # an output its shader has just written or restored: there is one
    data = output(label, name)
    if data is None: raise FileNotFoundError(f'{label}/{name}: no such output')
    return data

def commit(key='', names=None):
    """A finished shader's files (<key>_*, or the names given) into the cache's stage and off the disk. A file the cache could not take (a full drive) stays on disk, where publish_library finds it too"""
    with STORE.batch():
        for label in ('mac', 'ios'):
            d = label_dir(label)
            if not d: continue
            for n in (names if names is not None else [n for n in os.listdir(d) if n.startswith(key + '_')]):
                p = os.path.join(d, n)
                if n.startswith('.') or '.tmp' in n or not os.path.isfile(p): continue
                STORE.put_file(f'{STAGE}/{label}/{n}', p)
                if not STORE.full: os.unlink(p)

def publish_library(label):
    """The library's image <dir>.img from the staged outputs and the files left on disk (these win), and for a partial build the image it builds on, less the outputs of the keys it built
    (and its stamp, when the profile's inputs changed). Then the folder and the staged entries go. When the image cannot be written, the staged outputs go back into the folder, which works as it is"""
    d = label_dir(label); image = shader_lib.image_of(d); prefix = f'{STAGE}/{label}/'
    files = {}
    old = OLD_IMAGE.get(label)
    if old is not None:
        for p in old.iterdir():
            m = OUT_KEY.match(p.name)
            if (m and m.group(1) in want) or (p.name == dmsh_chain.STAMP and STAMP_BEFORE != INPUTS): continue
            files[p.name] = str(p)
    for n in STORE.names(prefix): files[n[len(prefix):]] = (lambda n=n: STORE.get(n))
    for n in os.listdir(d):
        if not n.startswith('.') and '.tmp' not in n and os.path.isfile(os.path.join(d, n)): files[n] = os.path.join(d, n)
    try: shader_lib.write(files, image)
    except (OSError, ValueError) as e:
        print(f'[RED] could not write {image}, the library stays the folder {d}: {e}', flush=True)
        for n in STORE.names(prefix): STORE.to_file(n, os.path.join(d, n[len(prefix):]))
        return
    STORE.delete_prefix(prefix)
    shutil.rmtree(d, ignore_errors=True)

def cache_outputs(key, digest, cached=False):
    root = cache_path(digest); files = {}
    for label, directory in (('mac', out), ('ios', IOSOUT)):
        if not directory: continue
        # fin(), paired vertex padding and the profile tables also need these on an iOS-only cache hit.
        required = [key + '_b.json'] + ([key + '_b.metal'] if label == 'mac' else [])
        if label in TARGETS: required.append(key + '_b.metallib')
        have = STORE.names(f'{root}/{label}/') if cached else set()
        names = required + [key + s + ext for s in ('_bg0', '_bg1', '_bg2', '_bg3', '_bg4', '_bgv', '_bl0', '_bl1', '_bl3') for ext in ('.metallib', '.json', '.metal')
                            if (label == 'mac' or ext != '.metal') and (ext != '.metallib' or label in TARGETS)]
        found = [n for n in names if (f'{root}/{label}/{n}' in have if cached else os.path.exists(os.path.join(directory, n)))]
        if not all(n in found for n in required): return None
        files[label] = found
    return root, files

def restore_cache(key, digest):
    found = cache_outputs(key, digest, cached=True)
    if not found: return False
    root, files = found
    for label, names in files.items():
        directory = out if label == 'mac' else IOSOUT
        for name in names:
            src = f'{root}/{label}/{name}'; dst = os.path.join(directory, name)
            if name.endswith('.json'):
                data = STORE.get(src)
                if data is None: return False   # pruned by another build since the look above: a miss, which builds it
                tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
                with open(tmp, 'wb') as f: f.write(re.sub(rb'("chain"\s*:\s*")[^"]*"', rb'\g<1>' + CHAIN.encode() + b'"', data, count=1))
                os.replace(tmp, dst)
            elif not STORE.to_file(src, dst): return False
    return True

def cache_lookup(key, digest, rule_fp, code_fp):   # whether the cache holds the shader's outputs; they are restored when the shader is finished (fin), so they are on disk only while it is in flight
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('shader', key), threading.Lock())
    with lock:
        if cache_outputs(key, digest, cached=True): PRE_STATS['hit'] += 1; return True, 'hit'
        old = json.loads(STORE.get(cache_meta_path(key)) or b'null')
        if old and old.get('code_fp') != code_fp: reason = 'code fingerprint changed: all rebuilt'
        elif old and old.get('rule_fp') != rule_fp: reason = 'rule changed'
        else: reason = 'new key'
        PRE_STATS['miss'] += 1; PRE_STATS[reason] += 1; return False, reason

def publish_cache(key, digest, rule_fp, code_fp):
    # Paired pixel MSL is an input to vs_pad; a diagnostic build without it must not cache an incomplete vertex shader under a complete dependency key.
    if any(output('mac', ps + '_b.metal') is None for ps in PAIRS.get(key, ())): return
    found = cache_outputs(key, digest)
    if not found: return
    root, files = found
    with STORE.batch():   # a stop leaves the whole entry or none of it
        for label, names in files.items():
            for name in names:
                src = os.path.join(out if label == 'mac' else IOSOUT, name)
                if name.endswith('.json'):
                    value = json.load(open(src)); value.pop('fetch', None); value.pop('vin', None); value.pop('pull', None); value.pop('alpha', None); value.pop('logic', None)   # the side libraries are made again from the restored MSL (ps_libraries, vin_finish, pull_finish)
                    STORE.put(f'{root}/{label}/{name}', json.dumps(value, ensure_ascii=False, indent=1).encode())
                else: STORE.put_file(f'{root}/{label}/{name}', src)
        STORE.put(cache_meta_path(key), json.dumps({'digest': digest, 'rule_fp': rule_fp, 'code_fp': code_fp}).encode())

def write_refl(key, refl):
    for target, d in (('mac', out),) + ((('ios', IOSOUT),) if IOSOUT else ()):
        atomic_json(os.path.join(d, key + '_b.json'), MTL.with_compiler(target, refl))

VIN_STATS = collections.Counter()
PULL_STATS = collections.Counter()
SIDE_STATS = collections.Counter()
BF_STATS = collections.Counter()

def pull_finish(key, full, refl):
    """The pulling library <key>_bp of a vertex shader with stage inputs (dmsh_pull.py: no [[stage_in]], the inputs read through the per-draw layout block), compiled through the
    MSL-keyed Metal cache with the chain's flags; reflection 'pull' names its entry points, the layout block's buffer index and the inputs. Shaders without stage inputs get none
    (their pipelines have no vertex descriptor anyway), nor does a shader the variant does not fit (stale libraries removed). Changes refl; the caller writes it"""
    if refl['stage'] != 'vs': return refl
    libs = [os.path.join(out, key + '_bp.metallib')] + ([os.path.join(IOSOUT, key + '_bp.metallib')] if IOSOUT else [])
    refl.pop('pull', None)
    def drop():
        for p in libs:
            try: os.unlink(p)
            except FileNotFoundError: pass
    v, info = dmsh_pull.variant(full)
    if not v:
        PULL_STATS['no stage input' if info == 'no stage input' else 'not generated'] += 1
        if info != 'no stage input': print(f'[RED] {key}: no pulling library ({info}): its pipelines are always built for the draw\'s vertex layout', flush=True)
        drop(); return refl
    msl = os.path.join(w, key + '_bp.metal'); open(msl, 'w').write(v)
    try:
        if 'mac' in TARGETS: MTL.compile('mac', msl, libs[0])
        if IOSOUT and IOS_PULL: MTL.compile('ios', msl, libs[1])
    except subprocess.CalledProcessError as e:
        PULL_STATS['failed'] += 1; print(f'[RED] {key}: the pulling library does not compile ({str(e)[:120]}): its pipelines are always built for the draw\'s vertex layout', flush=True)
        drop(); return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    PULL_STATS['generated'] += 1
    refl['pull'] = {k: v_ for k, v_ in info.items() if v_ is not None}
    return refl

def vin_finish(key, full, refl):
    """The shadow-input library <key>_bv of a vertex shader with integer inputs (dmsh_vin.py), compiled through the MSL-keyed Metal cache; reflection 'vin' names its entry points and
    the shadow attribute indices. Other shaders get none (stale libraries removed). Changes refl; the caller writes it"""
    if refl['stage'] != 'vs': return refl
    libs = [os.path.join(out, key + '_bv.metallib')] + ([os.path.join(IOSOUT, key + '_bv.metallib')] if IOSOUT else [])
    refl.pop('vin', None)
    def drop():
        for p in libs:
            try: os.unlink(p)
            except FileNotFoundError: pass
    v, shadows = dmsh_vin.variant(full)
    if not v:
        if shadows != 'no integer input': VIN_STATS['not generated'] += 1; print(f'[RED] {key}: no shadow-input library ({shadows}): its integer inputs keep the CPU conversion', flush=True)
        drop(); return refl
    VIN_STATS['generated'] += 1
    msl = os.path.join(w, key + '_bv.metal'); open(msl, 'w').write(v)
    try:
        if 'mac' in TARGETS: MTL.compile('mac', msl, libs[0])
        if IOSOUT: MTL.compile('ios', msl, libs[1])
    except subprocess.CalledProcessError as e:
        VIN_STATS['failed'] += 1; print(f'[RED] {key}: the shadow-input library does not compile ({str(e)[:120]}): its integer inputs keep the CPU conversion', flush=True)
        drop(); return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    refl['vin'] = {'entry': 'main0', 'entry_z': 'main0_z', 'shadow': shadows}
    VIN_STATS['inputs'] += len(shadows); VIN_STATS['float only'] += sum(1 for s in shadows if 'sint' not in s and 'uint' not in s)
    return refl

def ps_libraries(key, msl, full, bindings):
    """A pixel shader's libraries, cut from one compile through the MSL-keyed Metal cache (dmsh_side.cut): <key>_b with its own entry points, <key>_bx with its alpha test and logic
    operation ones (dmsh_side.unit), <key>_bf with its readable-tile version main0_f (dmsh_fetch.fold; a shader without a rewritable sample only when bf_want.tsv lists it). The readable-tile
    version was a source and a Metal compile of its own (13,666 of Mario Kart 8 Deluxe's 29,651 shaders). msl: the file of its plain source, full; bindings: its reflection's.
    What does not build is left out, and only that: a unit whose readable-tile version does not build is compiled again without it, one that does not build at all gives <key>_b from the
    plain source, each with a red line. A library a shader does not get is removed (a stale one of an earlier build), and the host draws without it. → what the reflection says of them.
    Raises CalledProcessError when the plain source does not compile"""
    targets = [(t, d) for t, d in (('mac', out), ('ios', IOSOUT)) if d and t in TARGETS]
    def drop(*suffixes):
        for _, d in targets:
            for s in suffixes:
                try: os.unlink(os.path.join(d, key + s + '.metallib'))
                except FileNotFoundError: pass
    src, info = dmsh_side.unit(full)
    side = set(info['alpha'] + info['logic']) if src else set()
    folded, fmeta = dmsh_fetch.fold(src or full, bindings, om_only_ok=key in BF_WANT)   # also without side entry points: main0_f then shares the compile with main0 alone
    for unit, fetch in ([(folded, fmeta)] if folded else []) + ([(src, None)] if src else []):
        names = ['_b'] + (['_bx'] if side else []) + (['_bf'] if fetch else [])
        groups = ([side] if side else []) + ([{'main0_f'}] if fetch else [])
        u = os.path.join(w, key + '_bu.metal'); open(u, 'w').write(unit)
        try:
            for t, d in targets: MTL.compile_split(t, u, [os.path.join(d, key + n + '.metallib') for n in names], lambda lib, groups=groups: dmsh_side.cut(lib, *groups))
        except (subprocess.CalledProcessError, ValueError) as e:
            if fetch: BF_STATS['failed'] += 1; print(f'[RED] {key}: the readable-tile version does not build ({str(e)[:120]}): its draws go without the tile read', flush=True)
            else: SIDE_STATS['failed'] += 1; print(f'[RED] {key}: the alpha test and logic operation entry points do not build ({str(e)[:120]}): its draws go without them', flush=True)
            continue
        finally:
            try: os.unlink(u)
            except FileNotFoundError: pass
        drop(*{'_bx', '_bf'} - set(names))
        SIDE_STATS['generated'] += bool(side); BF_STATS['generated'] += bool(fetch)
        return {**{f: {'entries': info[f]} for f in ('alpha', 'logic') if side and info[f]}, **({'fetch': fetch} if fetch else {})}
    SIDE_STATS['not generated'] += not src
    drop('_bx', '_bf')
    for t, d in targets: MTL.compile(t, msl, os.path.join(d, key + '_b.metallib'))
    return {}

pc_of, loc = {}, {}
MAP_ROWS, MAP_CONFLICT_ROWS = {}, {}
for raw in open(MAP, 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 4: MAP_ROWS.setdefault(c[3].decode(), raw)
for raw in open(MAP + '.conflicts.tsv', 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 1 and not raw.startswith((b'#', b'dm_')): MAP_CONFLICT_ROWS.setdefault(c[0].decode(), raw)
for ln in open(MAP):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[1])))
for ln in open(MAP + '.conflicts.tsv'):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 1 and not ln.startswith('#') and not ln.startswith('dm_') and c[0] in pc_of: pc_of[c[0]] = c[1]

def vertex_ab(key, code, ctl):
    """A vertex shader in two parts, VertexA + VertexB, as one program (Catherine: Full Body's GLSLC, SDK 8.2.1, GPU code version 1.15, writes every vertex shader so; the other games' are one
    program of shader type 1). Part A sits at code +0, its SPH with shader type 0 (Maxwell's VERTEX_CULL_BEFORE_FETCH slot): it loads the position attribute and writes only the position.
    Part B starts at control +0x700 (its own 0x30-byte header, then an SPH of shader type 1) and writes the other outputs from the other attributes. The hardware runs A, then B; translated
    alone, A left every varying unwritten (a full-screen pass read its texture coordinate as 0 and the whole screen came out one texel's colour). Control +0x748 lists both parts as
    {SPH offset, length from the SPH, ?} for A, then for B. The merge is what Ryujinx's TranslatorContext.Translate(VertexA) builds: A's instructions, each unconditional EXIT a branch to
    B's first instruction, then B's, under B's SPH with A's flags and input / output maps ORed in and the larger local memory sizes. Measured on all 1,244 of the game's vertex shaders: A
    writes no generic output (so none of B's loads reads one of A's), A ends in two EXITs (one predicated on CC FCSM_TR, which Ryujinx never takes, left as it is) and a BRA to itself,
    and neither part has an absolute or table jump, so moving B keeps every branch. → (code area with its 0x30 header, c1), c1 being what follows part B; None for any other vertex shader"""
    if len(code) < 0x80 or len(ctl) < 0x760 or code[:4] != bytes.fromhex('78563412'): return None
    w0 = struct.unpack_from('<I', code, 0x30)[0]
    if w0 & 0x1f != 1 or (w0 >> 10) & 0xf != 0: return None
    a_sph, a_len, _, b_sph, b_len, _ = struct.unpack_from('<6I', ctl, 0x748)
    if not any((a_sph, a_len, b_sph, b_len)): return None   # one program of shader type 0, no part B: translated as it is (Ryujinx reads type 0 as a vertex shader)
    bw0 =struct.unpack_from('<I', code, b_sph)[0] if 0x80 <= b_sph <= len(code) - 0x50 else 0
    if a_sph != 0x30 or b_sph != struct.unpack_from('<I', ctl, 0x700)[0] + 0x30 or bw0 & 0x1f != 1 or (bw0 >> 10) & 0xf != 1 or (a_len - 0x50) % 32 or (b_len - 0x50) % 32 or b_sph + b_len > len(code):
        sys.exit(f'[RED] {key}: vertex shader of shader type 0 without a VertexB part in the expected place (control +0x748 {a_sph:#x} {a_len:#x} {b_sph:#x} {b_len:#x})')
    sa, sb = list(struct.unpack_from('<20I', code, a_sph)), list(struct.unpack_from('<20I', code, b_sph))
    sph = [sb[0] | sa[0] & ~(0xf << 10)] + [max(sa[i] & 0xffffff, sb[i] & 0xffffff) | sb[i] & ~0xffffff for i in (1, 2, 3)] + [sb[4]] + [sa[i] | sb[i] for i in range(5, 20)]
    a = bytearray(code[a_sph + 0x50:a_sph + a_len]); b = code[b_sph + 0x50:b_sph + b_len]
    for o in range(0, len(a), 8):
        if o % 32 == 0: continue   # the scheduling word of each group of three instructions
        ins = struct.unpack_from('<Q', a, o)[0]
        if ins >> 52 == 0xe30 and ins & 0x1f == 0xf:   # EXIT with CC T (any predicate) → BRA to part B, keeping the predicate and CC bits; the offset counts from the next instruction
            struct.pack_into('<Q', a, o, 0xe24 << 52 | ((len(a) - (o + 8)) & 0xffffff) << 20 | ins & 0xfffff)
    return code[:0x30] + struct.pack('<20I', *sph) + bytes(a) + b, code[b_sph + b_len:]

def early_fragment_tests(ctl, code):
    """Early fragment tests: control +0x718 of a pixel shader (the NVN driver's control accessor reads that byte beside the other fragment flags at +0x719..+0x71d, the same accessor in
    the sdk of all 21 games here; Ryujinx gets it as the 3D engine's EarlyZForce). On the Switch such a shader is tested against depth before it runs; Metal otherwise tests a shader that
    stores to memory after it, and the late depth differs by an ulp from a prepass's along contours, so an EQUAL pass fails there (MacNativeGames ae8f4f4: Tears of the Kingdom's dotted
    lattice on the Great Sky Island; 1,677 of its 13,646 pixel shaders set it, none writes depth). A shader whose SPH (code, after the 0x30 header) writes depth (word 19 bit 1, OmapDepth)
    is left as it is: tested early, the depth it computes would not be the one tested. NVN2 (Switch 2) keeps the flag at control +0x5b (dmsh_ampere.early_fragment_tests), its
    0x80-byte SPH starting the code as here, with word 19 where Maxwell's has it"""
    flag = dmsh_ampere.early_fragment_tests(ctl) if dmsh_ampere.is_nvn2(ctl) else len(ctl) > 0x718 and ctl[0x718] != 0
    return flag and len(code) >= 0x50 and not struct.unpack_from('<I', code, 19 * 4)[0] & 2

def ps_omap(code):
    """A pixel shader's output map: SPH word 18 (Maxwell's 0x50-byte SPH and NVN2's 0x80-byte one alike, at the start of the job's code; Ryujinx ShaderHeader OmapTargets),
    4 bits per colour output, R G B A from the low bit. The GPU writes only these components of a bound target and leaves the others as they were; the translator stores only
    them, and Metal writes the whole attachment under its write mask, so the host masks each target with its nibble (nvn_mtl.m). Lichii (a Switch 2 emulator) traced a band of
    NaN above Ocarina of Time's horizon to the missing per-component mask (their commit 79c1516; Ocarina's agl_common depth pass has map 0x10f: output 0 RGBA, output 2 R only)"""
    return struct.unpack_from('<I', code, 18 * 4)[0] if len(code) >= 0x50 else None

def job_for(key, name, rec, w):
    ci = rec.find(bytes.fromhex('34127698'), 0x38); code = rec[0x38:ci]; stage = name[:2]
    extra = {}
    if dmsh_ampere.is_nvn2(rec[ci:]):
        # NVN2 (Switch 2): Ampere code (a 0x80-byte SPH for the graphics stages, then the instructions, control +0x38 long), which the translator's Ampere
        # frontend reads (job arch sm86); its c1 segment lies between control +0x40 and +0x44. The NVN control fields the NVN1 branch reads (VertexA / B,
        # transform feedback) are not in an NVN2 section; early fragment tests are, at +0x5b (early_fragment_tests, after both branches)
        c1 = dmsh_ampere.c1_segment(rec[ci:], code)
        code = code[:dmsh_ampere.inst_len(rec[ci:])]
        extra['arch'] = 'sm86'
        # compute lays out its driver constants in c[0] per shader: the control section gives its push constants' words (+0xa9), counts its uniform buffer, storage
        # buffer, texture, separate texture and separate sampler entries (+0xb1, +0xb2, +0xb3, +0xb4, +0xb5; images, +0xb6, come last) and gives the size (+0xba);
        # the translator maps them to c[24] and NVN's fixed tables (InstEmitAmpere.cs AMapC0). Graphics keeps NVN's tables
        if stage == 'cs': extra['nvn2_cs'] = [rec[ci + 0xb1], rec[ci + 0xb2], rec[ci + 0xb3], struct.unpack_from('<H', rec, ci + 0xba)[0], rec[ci + 0xa9] * 4, rec[ci + 0xb4], rec[ci + 0xb5]]
        if stage in ('ts', 'ms'):
            # A task or mesh shader: translated as compute, its SPH taken off and its stage memory in shared memory, then wrapped as the Metal stage function (dmsh_meshstage.py).
            # Graphics stages' c[0] and c[2] tables stay: the translator reads their addresses out of the code as for a vertex shader
            mesh = dmsh_meshstage.info(rec[ci:], rec[0x38:ci])
            lay, size = dmsh_meshstage.memory(mesh)
            if dmsh_meshstage.own_shared(code[0x80:]) or size > dmsh_meshstage.METAL_MAX_SHARED: mesh['refused'] = 'shared memory of its own, or stage memory past Metal\'s limit'
            extra.update(mesh_stage=mesh, stage_memory=lay, local=[mesh['threads'], 1, 1], shared=size); code = code[0x80:]; stage = 'cs'
    else:
        c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)   # control segment +0x700: c1 relative to the start of the instruction area, +0x704: total length of the instruction area (c1 = what lies between the two; lengths across the whole library are 0 / 256 / 512 / 768)
        c1 = rec[0x38 + c1o:0x38 + c1t]
        ab = vertex_ab(key, code, rec[ci:]) if stage == 'vs' else None
        if ab: code, c1 = ab
        if stage != 'cs': code = code[0x30:]   # strip the 0x30 header (the 0x50 SPH after it is read by the translator itself)
        elif code[:4] == bytes.fromhex('79563412'):
            # Compute in this game has a header (magic 0x12345679; D2R's package records have none): 0x30 header + zero padding, instructions start at code area +0x100 (measured on all 71; the BRA-to-self and the control words are 32-byte aligned to this start).
            # Stripping only 0x30 made the translator decode from +0x30: the control word every 32 bytes is off by 0x10 and the jump tables of indirect BRX (relative to the program start) are all misplaced → particle / post-effect compute shaders loop forever and hang the GPU.
            if any(code[0x30:0x100]): sys.exit(f'[RED] {key}: compute code area +0x30..+0x100 is not all zero, so the instructions do not start at +0x100')
            code = code[0x100:]
        tf = tf_layout(key, rec[ci:]) if stage in ('vs', 'gs') else None
        if tf: extra['tf'] = tf
    if stage == 'ps' and early_fragment_tests(rec[ci:], code): extra['early_fragment_tests'] = True
    cp = os.path.join(w, key + '.bin'); open(cp, 'wb').write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(), **extra,
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}   # translation options (see dmsh_xlate/Program.cs); relaxed precision is on by default (GPU float last-bit error does not enter game state, whole frame −26% on the second Mac)   # c1: used by the translator to read indirect jump tables
    if extra.get('mesh_stage'): return j, c1   # its threads and memory come from its control section (above)
    if key not in FACTS: j['no_facts'] = True; return j, c1   # not in the answer table: queries use defaults (listed separately in the summary)
    j.update(FACTS[key])
    return j, c1

TF_SLOT = 8   # Metal vertex buffer index of transform feedback buffer 0 (1-3 follow): slot 0 is the argument buffer, 16-29 the vertex streams, 30 the zero buffer (nvn_mtl.m tf_draw)
def tf_layout(key, ctl):
    """Decode the four feedback streams in the program's control section.
    Each stream has up to 128 output-location bytes and matching component-count
    bytes, captured from x on and interleaved in that order. Pokémon Legends:
    Z-A uses one output per stream, Tropical Freeze two in one stream, and
    Xenoblade Chronicles 3 uses up to eleven.
    Unknown layouts are reported and left without a capture entry."""
    strides = struct.unpack_from('<4H', ctl, 0x6dc)
    if not any(strides): return None
    out = []
    for b in range(4):
        if not strides[b]: out.append([]); continue
        count = ctl[0x6e8 + b]
        locs = list(ctl[0xdc + 0x80 * b:0xdc + 0x80 * b + count])
        sizes = list(ctl[0x4dc + 0x80 * b:0x4dc + 0x80 * b + count])
        if not 1 <= count <= 128 or any(not 0x20 <= x < 0x40 for x in locs) or any(not 1 <= n <= 4 for n in sizes) or strides[b] != 4 * sum(sizes):
            print(f'[RED] {key}: transform feedback buffer {b} has a layout of unknown form (stride {strides[b]}, outputs {locs}, components {sizes}): no capture entry point; see tf_layout', flush=True)
            return None
        out.append({'stride': strides[b], 'outputs': [[o - 0x20, n] for o, n in zip(locs, sizes)]})
    return out

def vs_tf(src, tf, geometry=False):
    """Capture interleaved output bits, using offsets prepared on the GPU.
    Geometry readers gather the ordered, decomposed primitive indices before
    reading their outputs. Their capture index remains linear, so duplicated
    strip vertices occupy distinct feedback records."""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{', src, re.S)
    if not m: sys.exit('[RED] vertex shader without a main0 main function, cannot add the transform feedback entry point')
    params, body = m.group(1), src[m.end():]   # the main function is the last function in the file
    vm = re.search(r'uint\s+(\w+)\s*\[\[vertex_id\]\]', params); vid = vm.group(1) if vm else 'dm_vid'
    im = re.search(r'uint\s+(\w+)\s*\[\[instance_id\]\]', params); iid = im.group(1) if im else 'dm_iid'
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    names = {int(l): nm for nm, l in re.findall(r'float4 (\w+) \[\[user\(locn(\d+)\)\]\]', st.group(1) if st else '')}
    stores = []
    for b, e in enumerate(tf):
        if not e: continue
        offset = 0
        for o, n in e['outputs']:
            for i, c in enumerate('xyzw'[:n]):
                value = f'as_type<uint>(out.{names[o]}.{c})' if o in names else ('0x3f800000u' if c == 'w' else '0u')
                stores.append(f'dm_tf{b}[dm_tf.offset[{b}] / 4 + dm_linear * {e["stride"] // 4} + {offset + i}] = {value};')
            offset += n
    extra = ('' if vm else ', uint dm_vid [[vertex_id]]') + ('' if im else ', uint dm_iid [[instance_id]]')
    extra += ''.join(f', device uint* dm_tf{b} [[buffer({TF_SLOT + b})]]' for b, e in enumerate(tf) if e)
    extra += ', constant dm_tf_capture& dm_tf [[buffer(14)]]'
    pro = f'uint dm_linear = ({iid} - dm_tf.base_instance) * dm_tf.count + {vid} - dm_tf.first;\n'
    if geometry: pro += f'{vid} = dm_tf.order[dm_linear];\n'
    decl = '\nstruct dm_tf_capture { uint4 offset; uint4 args; device const uint* order; uint first, count, base_instance, pad; ulong reserved; };\n'
    return decl + f'\nvertex void main0_tf({params}{extra})\n{{\n' + pro + body.replace('return out;', '{ if (dm_linear < dm_tf.args.x) { ' + ' '.join(stores) + ' } return; }')


def vs_writes(src, binds):
    """Whether a vertex shader has side effects that a draw with rasterizer discard still runs, so that it needs main0_discard: storage buffer writes (Xenoblade Chronicles 3's
    vertex-only work before its skinned models) or image stores (Metroid Prime Remastered's volumetric fog: 1cbc19f806a08166 fills its 100x57x64 RGBA16F froxel volume from
    5,700 points with rasterization off; without the entry the host skipped the draw, the volume stayed zero and the fog pass blended every scene to black)"""
    return any(b.get('rw') for b in binds) or bool(re.search(r'access::(read_)?write\b', src))


def vs_discard(src):
    """Metal requires a void vertex entry when rasterization is disabled.
    Preserve all original side effects and early returns; omit only outputs."""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{', src, re.S)
    if not m: raise ValueError('vertex shader has no main0 entry for rasterizer discard')
    return '\nvertex void main0_discard(' + m.group(1) + ')\n{' + src[m.end():].replace('return out;', 'return;')

def attributes(src):
    """The vertex attribute declarations, as matches of (type, name, location). Searched only in the lines from the first to the last: from any word of a 220 KB source the pattern costs 9 ms"""
    i, j = src.find('[[attribute('), src.rfind('[[attribute(')
    if i < 0: return []
    end = src.find('\n', j)
    return list(re.finditer(r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', src[src.rfind('\n', 0, i) + 1:end if end >= 0 else len(src)]))

def in_word(src, i):   # whether a word character comes just before src[i]
    return i > 0 and (src[i - 1] == '_' or src[i - 1].isalnum())

def has_word(src, text):
    """Whether text, which starts with a word character, is in src with no word character before it: re.search(r'\\b' + text, src) found as text. A pattern that starts with \\b has no literal prefix to skip to"""
    i = src.find(text)
    while i >= 0:
        if not in_word(src, i): return True
        i = src.find(text, i + 1)
    return False

def raster_facts(stage, src):
    """Static facts needed for the runtime proof that the load can be skipped (full-coverage first draw) (nvn_mtl.m first_draw_covers):
        Vertex: whether position x / y / z are taken unchanged from the .x / .y / .z of one and the same vertex attribute (w is the constant 1 or the same attribute's .w), with no branches and no clip distances in the main function - if so, pos_attr = attribute slot;
        Fragment: whether it may discard (discard_fragment), whether it writes sample_mask, and which color indices it writes. Anything unrecognized is treated as "unprovable" (pos_attr = -1 / discard = 1).
        Also, not for that proof: whether a fragment shader reads the primitive ID, which a specialized Metal 4 pipeline does not give it (nvn_mtl.m pso_build_ex), and a vertex shader's
        clip → screen varyings (screen, dmsh_fetch.py screen_varyings), the vertex side of the same-pixel proof for a projective sample's tile read (nvn_mtl.m fb_tile_plan)"""
    if stage == 'ps':
        out = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
        return {'discard': int('discard_fragment' in src), 'sample_mask': int('sample_mask' in (out.group(1) if out else '')), 'primitive_id': int('[[primitive_id]]' in src),
                'colors': sorted({int(x) for x in re.findall(r'\[\[color\((\d+)\)\]\]', out.group(1) if out else '')})}
    if stage != 'vs': return {}
    scr = {'screen': dmsh_fetch.screen_varyings(src)}   # clip → screen varyings (dmsh_fetch.py): the vertex side of a projective sample's tile read
    body = re.search(r'\bvertex\s+\w+\s+\w+\s*\(.*?\)\s*\{(.*)\}\s*$', src, re.S)   # the main function is the last function in the file
    if not body or re.search(r'\b(if|for|while|switch|do)\b|clip_distance|ClipDistance', body.group(1)): return {'pos_attr': -1, **scr}
    b = dmsh_fetch.unguard(body.group(1))   # a bounded uniform word read bare (the host's proof reads it only from inside its binding)
    attr = {m.group(2): int(m.group(3)) for m in attributes(src)}
    var = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);', b)}
    last = {}
    for m in re.finditer(r'out\.gl_Position\.([xyzw])\s*=\s*([^;]+);', b): last[m.group(1)] = m.group(2).strip()
    src_of = {c: var.get(v) for c, v in last.items()}
    m0 = src_of.get('x')
    if not m0 or m0[0] not in attr or any(src_of.get(c) != (m0[0], c) for c in 'xyz'): return {'pos_attr': -1, **scr}
    if last.get('w') == '1.0': w = 'one'
    elif src_of.get('w') == (m0[0], 'w'): w = 'attr'
    else: return {'pos_attr': -1, **scr}
    # uv pass-through (used for the "this-pixel addressing" proof in merge B step 2): the .x / .y of interpolated output locn K are taken unchanged from the .x / .y of vertex attribute A → uv_from_attr[K] = A
    ost = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'(m_\d+) \[\[user\(locn(\d+)\)\]\]', ost.group(1) if ost else '')}
    olast = {}
    for m in re.finditer(r'out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);', b): olast[(m.group(1), m.group(2))] = m.group(3).strip()
    uv, aff = {}, {}
    for mem, k in locn.items():
        sx, sy = var.get(olast.get((mem, 'x'), '')), var.get(olast.get((mem, 'y'), ''))
        if sx and sy and sx[0] == sy[0] and sx[0] in attr and sx[1] == 'x' and sy[1] == 'y': uv[str(k)] = attr[sx[0]]; continue
        # Affine: component = fma(attribute component, uniform[slot].vector[i].component, uniform[slot].vector[j].component) (vp_cN is uniform slot N − 3)
        def fm(v_):
            m = re.search(r'float %s = fma\((_\w+), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])\);' % re.escape(v_), b)
            if not m or m.group(1) not in var: return None
            return var[m.group(1)], [int(m.group(2)) - 3, int(m.group(3)), 'xyzw'.index(m.group(4))], [int(m.group(5)) - 3, int(m.group(6)), 'xyzw'.index(m.group(7))]
        fx, fy = fm(olast.get((mem, 'x'), '')), fm(olast.get((mem, 'y'), ''))
        if fx and fy and fx[0][0] == fy[0][0] and fx[0][0] in attr and fx[0][1] == 'x' and fy[0][1] == 'y' and min(fx[1][0], fx[2][0], fy[1][0], fy[2][0]) >= 0:
            aff[str(k)] = {'attr': attr[fx[0][0]], 'sx': fx[1], 'ox': fx[2], 'sy': fy[1], 'oy': fy[2]}
    return {'pos_attr': attr[m0[0]], 'pos_w': w, 'uv_from_attr': uv, 'uv_affine': aff, **scr}

def decode_tex(name):
    """fp_t_cb{slots}_{handle} / fp_t_tcb_{handle}: texture slot, sampler slot (Ryujinx TextureHandle encoding; c[2] handle table). Handle type 3 (SeparateConstantSamplerHandle) carries a constant
    sampler ID instead of a second offset, so only the first offset can name the texture (const_sampler gives the ID)"""
    m = re.match(r'[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    slots = int(m.group(1)) if m.group(1) else -1; h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2; sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    def slot(word, base, cb): return (word * 4 - base) // abi.C2_ROW if cb == 2 and word * 4 >= base and (word * 4 - base) % abi.C2_ROW == 0 else None
    words = [o0] + ([o1] if ty else [])
    if re.match(r'[a-z]+_i', name):   # image
        return next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < IMG_BASE + abi.C2_ROW * IMG_SLOTS and slot(x, IMG_BASE, tb) is not None), None), None, ty
    tw = [o0] if ty == 3 else words
    t = next((slot(x, TEX_BASE, tb) for x in tw if slot(x, TEX_BASE, tb) is not None and x * 4 < SEPTEX_BASE), None)
    if t is None: t = next((SEPTEX_SLOT0 + slot(x, SEPTEX_BASE, tb) for x in tw if slot(x, SEPTEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None) if ty != 3 else None
    return t, s, ty

def const_sampler(name):
    """The constant sampler ID of a type-3 handle (fp_t_cb65538_3041005C: the texture handle at c2 word 0x5C ORed with sampler 0x104), or None"""
    m = re.match(r'[a-z]+_t\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    h = int(m.group(2), 16)
    return (h >> 14) & 0x3fff if h >> 28 == 3 else None

def decode_ubo_tex(name):
    """Bindless texture: the handle is written by the game into its own uniform buffer (fp_t_cb7_2A = c[7] word 0x2A). Returns (texture: (uniform slot, byte offset), sampler: (slot, offset));
    handle type 0 = combined handle (one 32-bit word: low 20 bits texture ID, high 12 bits sampler ID), 1 = the sampler handle is in another word (its slot is the high 16 bits of slots − 1). This game's whole library uses only type 0 (1850 shaders)
    Type 3: the sampler is a constant ID (Monster Hunter Rise: a texture handle at c[5] word 0x28 with sampler 0x104); the sampler part is then ('id', ID).
    Type 1 with the sampler word in the driver table: the texture handles are the game's, one separate sampler the shader's own (Kirby Air Riders: array<depth2d<float>, 32> fp_t2ds_cb196614_10570000, c[6] and the
    word at c2 byte 0x570, about 5,700 pixel and compute shaders, its shadows and lights); the sampler part is then ('c2', byte offset)"""
    m = re.match(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name)
    if not m: return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    tb = slots & 0xffff; sb = (slots >> 16) - 1 if slots >> 16 else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    if not 3 <= tb <= 16 or ty not in (0, 1, 3) or (ty == 1 and sb != 2 and not 3 <= sb <= 16): return None
    return (tb - 3, o0 * 4), (('id', o1) if ty == 3 else ('c2', o1 * 4) if ty and sb == 2 else (sb - 3, o1 * 4) if ty else (tb - 3, o0 * 4))

def decode_ubo_img(name):
    """An image whose handle the game keeps in its own uniform buffer (<stage>_i_cb<n>_<word>_<format>: Unreal Engine 5.4 on the Switch 2 keeps every image in c[3]). Returns (uniform slot, byte offset)"""
    m = re.match(r'[a-z]+_i\w*?_cb(\d+)_([0-9A-F]+)', name)
    return (int(m.group(1)) - 3, int(m.group(2), 16) * 4) if m and 3 <= int(m.group(1)) <= 16 else None

def c2_array(name, ty):
    """An array of handles in the driver table c2 that the shader indexes at run time (Ryujinx's texture array from a buffer, here bank 2: <stage>_t<type>_cb<slots>_<handle>, declared
    array<element, n>, one element per c2 handle word; the index is the handle's byte offset ÷ 8). Luigi's Mansion 3: 586 pixel shaders sample their shadow cascades from
    array<texture2d_array<float>, n> fp_t2das_cb2_0, the index (0x80 + a cascade offset) ÷ 8. Handle type 1 ORs one fixed separate sampler word into every element's handle:
    EA SPORTS FC 27's lights, fp_t2d_cb196610_10570000 (texture words from c2 byte 0, the sampler word at c2 byte 0x570), indexed by separate texture i at element 45 + i.
    → (the c2 byte offset of element 0 (element k = the handle at that offset + 8k), the c2 byte offset of the sampler word or None), or None"""
    m = re.fullmatch(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name[:-5] if name.endswith('Smplr') else name)
    if not m or not ty.startswith('array<'): return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    if h >> 28 == 0 and slots == 2: return (h & 0x3fff) * 4, None
    if h >> 28 == 1 and slots & 0xffff == 2 and (slots >> 16) - 1 == 2: return (h & 0x3fff) * 4, ((h >> 14) & 0x3fff) * 4
    return None

POOL_MIN = 33   # arrays at least this long are pools: Program.cs sizes Ryujinx's handle fallback by the pools, the vendored BindlessElimination keeps 32-entry slot arrays (NvnSlotArrayLength)

# The test is a plain load: an atomic one, even relaxed, goes to the shared cache every time, and the reads of a pass name a few IDs, so its pixels queue on the same word.
# A 1536x864 pass of 16 marks a pixel over 16 IDs: 2.60 ms atomic, 0.34 plain, 0.18 with no mark (Apple M6). A stale 0 costs an OR that changes nothing; a bit only goes
# from 0 to 1 while a submission runs, as the host clears one (pool_seen_clear) only for an ID registered anew, which work committed after it reads
POOL_MARK = """template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)
{
    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);
    uint b = 1u << (i & 31u);
    if (!(*w & b)) atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(w), b, memory_order_relaxed);
    return i;
}

"""

def pool_arrays(src):
    """Ryujinx's arrays for a texture handle it cannot trace to one slot (<stage>_<t|s>_cb0_40000000, indexed by the handle's texture / sampler ID), sized to the whole ID ranges
    (Program.cs QueryTextureArrayLengthFromPool, 2^20 and 2^12) → a pointer to a pool the replay keeps per element type (nvn_mtl.m pool_gpu): struct spvPool_<name> { uint n; ulong seen;
    array<element, 1> e; }, read as (&e[0])[min(index, n)], n being the highest ID the host filled. A read of a texture pool also sets the ID's bit in the bitmap at seen (dm_pool_mark), as which IDs a
    shader reads is known only here: the host makes those textures instead of all of them. The word is read first, with a plain load: a bit is set once, so nearly every read costs a load, not an atomic. The struct declares one element: Metal's argument encoder walks every element
    of a declared array (AGX IndirectArgumentLayoutGen2::parseArray), and for 2^20 textures that took 57 ms and 66 MB per encoder (9.7 GB in Mario Party Superstars' prewarm);
    the elements are 8-byte resource IDs, so the pointer reaches the rest (checked on the GPU). Mario Party Superstars' characters read their light textures' handles from a
    storage buffer (6,370 pixel shaders). The argument IDs are then renumbered in member order, a pointer taking one and any other array its length, as the host lays the buffer out.
    Buffer textures and images keep their 32-entry slot arrays (tex_dyn_slots). ValueError on a use of a pool array the rewrite does not cover.
    An array of the handles in one of the game's uniform buffers (Ryujinx's texture array from a buffer, as long as a 64 KB buffer's handles, Program.cs QueryTextureArrayLengthFromBuffer) takes
    the same form: the host keeps a table of that buffer's handles, as long as its last handle (nvn_mtl.m htab_gpu), so the element index is unchanged and only the textures the table names are made.
    A table's reads are not marked: its index is an element of the table, not a texture ID, and the host has made every texture the table names already (it leaves seen 0). Marked through that
    null address, Fire Emblem Fortune's Weave's lighting pass took 197 ms a frame on an M1 Pro (PR #1738)"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', src, re.S)
    if not body: return src
    pools = {}
    def to_pointer(m):
        if int(m.group(3)) < POOL_MIN or not (m.group(4).endswith('_cb0_40000000') or decode_ubo_tex(m.group(4).removesuffix('Smplr'))): return m.group(0)
        pools[m.group(4)] = (m.group(2), int(m.group(3)), m.group(2) != 'sampler' and m.group(4).endswith('_cb0_40000000'))   # element, length, marked (a texture pool's reads)
        return f'{m.group(1)}constant spvPool_{m.group(4)}* {m.group(4)} [[id('
    members = re.sub(r'^(\s*)array<(.+), (\d+)> (\w+) \[\[id\(', to_pointer, body.group(1), flags=re.M)
    if not pools: return src
    aid = 0
    def renumber(m):
        nonlocal aid
        a = re.fullmatch(r'array<.+, (\d+)>', m.group(2).strip())
        out = f'{m.group(1)}{m.group(2)} {m.group(3)} [[id({aid}){m.group(5)}]];'
        aid += int(a.group(1)) if a else 1
        return out
    members = re.sub(r'^(\s*)(.+?)\s+(\w+)\s*\[\[id\((\d+)\)((?:\s*,\s*\w+\(\d+\))*)\]\];', renumber, members, flags=re.M)
    structs = ''.join(f'struct spvPool_{nm}\n{{\n    uint n;\n    ulong seen;\n    array<{el}, 1> e;\n}};\n\n' for nm, (el, n, _) in pools.items())
    marked = any(mk for _, _, mk in pools.values())
    if marked: structs = POOL_MARK + structs
    i = body.start()
    src = src[:i] + structs + src[i:body.start(1)] + members + src[body.end(1):]
    for nm in pools:   # base.name[index] → (&base.name->e[0])[min(uint(index), base.name->n)], the index taken with its brackets balanced
        out, pos = [], 0
        for m in re.finditer(r'([\w.]+)\.%s\[' % re.escape(nm), src):
            if m.start() < pos: continue
            depth, j = 1, m.end()
            while depth:
                if j >= len(src): raise ValueError(f'{nm}: unbalanced index')
                depth += {'[': 1, ']': -1}.get(src[j], 0); j += 1
            base = f'{m.group(1)}.{nm}'
            index = f'min(uint({src[m.end():j - 1]}), {base}->n)'
            out.append(src[pos:m.start()] + f'(&{base}->e[0])[{f"dm_pool_mark({base}, {index})" if pools[nm][2] else index}]'); pos = j
        src = ''.join(out) + src[pos:]
        names = 3 if pools[nm][2] else 2   # each read names it twice, and once more in a mark; the member once (spvPool_<name> is one word)
        left = len(re.findall(r'\b%s\b' % re.escape(nm), src)) - names * len(re.findall(r'\.%s->e\[0\]\)\[' % re.escape(nm), src)) - 1
        if left: raise ValueError(f'{nm}: {left} uses of the pool array the rewrite does not cover')
    # The mark stores to memory, so Metal would test this fragment shader against depth after running it: every covered pixel is shaded (no hidden surface removal) and the depth it tests differs by
    # an ulp from a prepass's, which fails an EQUAL pass along contours. The mark is the host's own, so it must not change when the test runs: early, unless the shader discards or writes depth, which
    # a test before it would get wrong (then it stays late, and only costs the shading)
    own = src.replace(POOL_MARK, '')   # what the shader does itself: a store of its own keeps the test where the game's own shader has it
    if marked and not re.search(r'\bdiscard_fragment\b|\[\[\s*depth\(|\[\[\s*sample_mask\s*\]\]|\[\[\s*stencil\s*\]\]|\.write\(|\batomic_\w+_explicit\b|^\s*device\s', own, re.M):
        src = re.sub(r'^(?<!\]\] )fragment\s', '[[early_fragment_tests]] fragment ', src, flags=re.M)
    return src

def reflect(key, stage, msl, res):
    """MSL argument buffer struct member → replay reflection: kind (same as nvn_refl.h) + NVN slot number + argument buffer member index"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    sb_cb = {b['binding']: b for b in res['sbufs']}
    binds = []
    # A member may carry attributes after its id: SPIRV-Cross marks the resources a fragment shader writes inside its interlock [[id(n), raster_order_group(0)]]
    # (Mario Party Superstars' light_primitive_point_cluster.bnsh, a storage buffer and two images); a member the pattern skipped left the host's argument buffer longer than the reflection
    members = list(re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];', body.group(1) if body else '', re.M))
    if body and len(members) != body.group(1).count('[[id('): raise ValueError(f'{key}: an argument buffer member the reflection cannot read')
    for m in members:
        ty, nm, aid = m.group(1).strip(), m.group(2), int(m.group(3))
        e = {'msl_name': nm, 'msl_type': ty, 'arg_id': aid}
        pool = re.search(r'struct spvPool_%s\s*\{\s*uint n;\s*ulong seen;\s*array<(.+), \d+> e;' % re.escape(nm), msl) if ty == f'constant spvPool_{nm}*' else None
        if ty == 'sampler' and nm.endswith('Smplr' + abi.BORDER_TWIN):   # a sampler's twin (border_emu): the same sampler, its bit in the block in the order of the twins
            nm = nm[:-len(abi.BORDER_TWIN)]; e['border_bit'] = sum(1 for b in binds if 'border_bit' in b)
        if nm == abi.BORDER_MEMBER: e.update(kind=abi.KIND_BORDER)
        elif nm in dmsh_geometry.HELPERS: e.update(kind=abi.KIND_GEOMETRY, reg=dmsh_geometry.HELPERS[nm])
        elif pool and not nm.endswith('_cb0_40000000'):   # a table of a uniform buffer's handles (pool_arrays): reg and off as an array of them, below
            ut, us = decode_ubo_tex(nm.removesuffix('Smplr'))   # pyright: ignore[reportGeneralTypeIssues]  (not None: pool_arrays made this pool only for a name it decodes)
            e.update(kind=abi.KIND_SAMP_HTAB if pool.group(1) == 'sampler' else abi.KIND_TEX_HTAB, elem=pool.group(1), reg=ut[0], off=ut[1], **({'soff': us[1], 'sreg': abi.SREG_C2} if us[0] == 'c2' else {}))
        elif pool: e.update(kind=abi.KIND_SAMP_POOL if pool.group(1) == 'sampler' else abi.KIND_TEX_POOL, elem=pool.group(1))   # see pool_arrays
        elif nm == 'support_buffer': e.update(kind=abi.KIND_SUPPORT)
        elif re.fullmatch(r'[a-z]+_c(\d+)', nm):
            n = int(re.fullmatch(r'[a-z]+_c(\d+)', nm).group(1))
            if 3 <= n <= 16: e.update(kind=abi.KIND_CBUF, reg=n - 3)
            elif n == abi.CBSIZE_BANK: e.update(kind=abi.KIND_CBSIZE)   # the uniform slots' bound sizes, which the translator's bounded reads compare with
            else: e.update(kind=abi.KIND_C1 if n == 1 else abi.KIND_DRVCB, bank=n)
        elif re.fullmatch(r'[a-z]+_s(\d+)', nm):
            e.update(kind=abi.KIND_SBUF, index=int(re.fullmatch(r'[a-z]+_s(\d+)', nm).group(1)))
        elif decode_ubo_tex(nm[:-5] if nm.endswith('Smplr') else nm):   # bindless: the handle is in a uniform buffer (reg = uniform slot, off = byte offset)
            ut, us = decode_ubo_tex(nm[:-5] if nm.endswith('Smplr') else nm)
            other = () if us[0] in ('id', 'c2') or us == ut else (ut, us)   # handle type 1: the word of the other half, which the host ORs in (the compiler puts texture and sampler words in either order)
            if nm.endswith('Smplr') and us[0] == 'c2': e.update(kind=abi.KIND_SAMP_UBO, reg=ut[0], off=ut[1], soff=us[1], sreg=abi.SREG_C2)
            elif nm.endswith('Smplr'): e.update(kind=abi.KIND_SAMP_ID, reg=us[1]) if us[0] == 'id' else e.update(kind=abi.KIND_SAMP_UBO, reg=us[0], off=us[1], **({'soff': ut[1], 'sreg': ut[0]} if other else {}))
            elif 'texture_buffer' in ty: e.update(kind=abi.KIND_TBUF_UBO, reg=ut[0], off=ut[1])
            else: e.update(kind=abi.KIND_TEX_UBO, reg=ut[0], off=ut[1], **({'soff': us[1], 'sreg': us[0]} if other else {}))
        elif decode_ubo_img(nm):
            e.update(kind=abi.KIND_IMG_UBO, reg=decode_ubo_img(nm)[0], off=decode_ubo_img(nm)[1])   # pyright: ignore[reportOptionalSubscript]  (not None: tested above)
            if has_word(msl, nm + '.atomic_'): e['atomic'] = True   # as KIND_IMG below
        elif c2_array(nm, ty) is not None:   # an array of the driver table's handles (c2_array): the replay fills element k from the slot whose c2 word is at off + 8k
            off, soff = c2_array(nm, ty)   # pyright: ignore[reportGeneralTypeIssues]  (not None: tested above)
            e.update(kind=abi.KIND_SAMP_C2A if nm.endswith('Smplr') else abi.KIND_TEX_C2A, off=off, **({'soff': soff} if soff is not None else {}))
        elif nm.endswith('Smplr'):   # handle type 0 = texture and sampler combined in one handle: the sampler index is taken from the high bits of that texture slot's handle
            t, s, ty = decode_tex(nm[:-5]); e.update(kind=abi.KIND_SAMP_ID, reg=const_sampler(nm[:-5])) if ty == 3 else e.update(kind=abi.KIND_SAMP, reg=s) if ty else e.update(kind=abi.KIND_SAMP_TEX, reg=t)
        else:
            d = decode_tex(nm)
            if not d: e.update(kind=abi.KIND_UNKNOWN)
            else: e.update(kind=abi.KIND_IMG if re.match(r'[a-z]+_i', nm) else (abi.KIND_TBUF if 'texture_buffer' in ty else abi.KIND_TEX), reg=d[0])
            # native texture atomics (dmsh_atomic.py): Metal allows them only on an R32Uint / R32Sint texture made with the shader-atomic usage, which the host checks for this slot
            if e.get('kind') == abi.KIND_IMG and has_word(msl, nm + '.atomic_'): e['atomic'] = True
            # a 1D integer fetch the translator read as 2D rows of 16384 texels (Program.cs RecordTexelFetch1D): the host binds a texture buffer in this slot as such rows (nvn_mtl.m tbuf_as_2d)
            if e.get('kind') == abi.KIND_TEX and e.get('reg') in res.get('fetch1d', ()):
                e['fetch1d'] = True
                if has_word(msl, nm + '.get_width('): e['fetch1d_size'] = True   # also size-queried: a texture buffer of more than 16384 elements reports 16384 there
        binds.append(e)
    # Storage buffers: SPIR-V binding number → c0 offset → NVN storage buffer slot (c0 base SSBO[stage], C0_ROW bytes per slot)
    base = SSBO.get(stage)   # storage buffer base for the hull / domain stages has not been measured: a storage buffer there is reported with no slot
    sb_sorted = sorted(res['sbufs'], key=lambda b: b['binding'])
    for e in binds:
        if e['kind'] == abi.KIND_SBUF:
            b = sb_sorted[e['index']] if e['index'] < len(sb_sorted) else None
            off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
            if b and b['cb_slot'] == abi.GMEM_MAP_CB_SLOT:   # not a game buffer: for addresses traced to no storage buffer, the host's pool table (offset 0) or the write log (DMSH_GMEM_LOG_CB_OFFSET)
                if b['cb_offset'] == abi.GMEM_LOG_CB_OFFSET: e.update(kind=abi.KIND_GMEM_LOG, rw=True); continue
                # rw: one of those accesses writes (the translator notes each one with its operation: Load, Store or an atomic)
                e.update(kind=abi.KIND_GMEM_MAP, rw=any(re.search(r'operation "(Store|Atomic\w*)" traced to no storage buffer', n) for n in res.get('notes', ()))); continue
            if stage == 'cs' and off is not None and abi.C0_CS_UBO <= off < abi.C0_CS_UBO + abi.C0_ROW * abi.C0_CS_UBO_SLOTS and (off - abi.C0_CS_UBO) % abi.C0_ROW == 0:
                e.update(kind=abi.KIND_UBO_PTR, reg=(off - abi.C0_CS_UBO) // abi.C0_ROW)   # compute-stage uniform slot r: address at c0[C0_CS_UBO + 0x10 r], read through global memory (checked against cs_lut_combine_c reading cb10[66])
            elif b and b['cb_slot'] >= 3:   # the address is in the game's own uniform buffer c[n] = uniform slot n - 3 (Monster Hunter Rise's compute shaders: c3 + 0x38); the replay reads it there
                e.update(kind=abi.KIND_SBUF_UBO, reg=b['cb_slot'] - 3, off=b['cb_offset'] * 4)
            else: e['reg'] = (off - base) // abi.C0_ROW if off is not None and base is not None and 0 <= off - base < abi.C0_ROW * abi.C0_SSBO_SLOTS and (off - base) % abi.C0_ROW == 0 else None
            e['rw'] = bool(b and b['flags'] & 1)   # BufferUsageFlags.Write; Program.cs serializes the translator enum unchanged (atomics count as writes too)
    return binds

DRV_FILL = {0: [(b, b + abi.C0_ROW * abi.C0_SSBO_SLOTS) for k, b in sorted(SSBO.items(), key=lambda kv: kv[1]) if k not in ('ts', 'ms')],   # c0: storage buffer address tables (vertex / compute / geometry / pixel; a task or mesh stage's: unfillable())
            2: [(TEX_BASE, SEPTEX_BASE), (IMG_BASE, IMG_BASE + abi.C2_ROW * IMG_SLOTS), (SEPTEX_BASE, SAMP_BASE), (SAMP_BASE, SAMP_BASE + abi.C2_ROW * abi.C2_SEP_SLOTS)]}   # c2: the handle tables above, all filled by the replay layer

def drv_reads(src):
    """Reads of driver segments c0 / c2 in the MSL: constant index → byte range; non-constant indices are recorded as dynamic"""
    out = {}
    for bank in (0, 2):
        offs, dyn = set(), False
        for m in re.finditer(r'fp_c%d\)\._m0\[([^\]]+)\]' % bank, src):
            idx = m.group(1).rstrip('u')
            if idx.isdigit(): offs.add(int(idx) * 16)
            else: dyn = True
        if offs or dyn: out[str(bank)] = {'vec4_offsets': sorted(offs), 'dynamic': dyn}
    return out

SAMPLE_ROWS = [(abi.C0_SAMPLE_TABLE, abi.C0_SAMPLE_TABLE + abi.C0_SAMPLE_BYTES), (abi.C0_SAMPLE_INFO, abi.C0_SAMPLE_INFO + abi.C0_ROW)]   # c0: the pixel stage's sample position rows

def unfillable(dr, stage):
    bad = []
    for bank, d in dr.items():
        fill = DRV_FILL[int(bank)] + (SAMPLE_ROWS if bank == '0' and stage == 'ps' else []) + ([(SSBO[stage], SSBO[stage] + abi.C0_ROW * abi.C0_SSBO_SLOTS)] if bank == '0' and stage in ('ts', 'ms') else [])
        # a computed index into c2 reads one of its handle tables, which the replay layer fills whole (nvn_mtl.m DRV2: EA SPORTS FC 27's lights read the separate texture words
        # at 0x168 + 8i to test each light's handle); c0 is filled only in its tables and words above, so a computed index there may read what the replay does not write.
        # The one computed c0 index known is GLSLC's sample position lookup in a pixel shader: a byte of the table at 0x620, at an index made of the sample index (PIXLD.MYINDEX)
        # and the pixel's place in the location grid masked by the words of the 0x630 row, at most 15 (Metroid Prime Remastered: 30 of its 438 shaders that read the sample index)
        if d['dynamic'] and bank == '0' and not (stage == 'ps' and abi.C0_SAMPLE_INFO in d['vec4_offsets']): bad.append(f'c{bank} dynamic index')
        for o in d['vec4_offsets']:
            # a read is a 16-byte row: overlapping any table is enough (Engage reads the c2+0x160 row for texture slot 0 at 0x168 in it)
            if not any(o < b and a < o + 16 for a, b in fill): bad.append(f'c{bank}+{o:#x}')
    return bad

RULES = []   # MSL rewrite rules attached by shader name (function, applicability test); the former dark-range remap toe has been removed (back to the original Switch brightness)
# CODE_FP hashes the code form of this file and its helper modules (dmsh_chain.code_form): editing a comment, a docstring or the layout does not change any shader's key.
# The library ABI enters as its values (dmsh_abi.VALUES), so a comment in dmsh_abi.h does not change a key either.
# It covers what runs only when a shader misses the cache, since a hit restores the output and skips it. dmsh_side.py is in it: fs_uint and fs_dual write main0's shared body (shared_body).
# dmsh_fetch.py is not, but for screen_varyings (raster_facts), nor are the other side libraries' modules: they are made again from the restored MSL on every build (ps_libraries;
# test_dmsh_code_fp.py checks this list).
CODE_FP = hashlib.sha256(source_without_rule_bodies() + b''.join(b'\0' + dmsh_chain.code_form(open(m.__file__, 'r').read()) for m in (dmsh_chain, dmsh_geometry, dmsh_atomic, dmsh_wide, dmsh_meshstage, dmsh_ampere, dmsh_side, abi) + ((ios_metal,) if IOSOUT else ()))
                         + b'\0' + inspect.getsource(dmsh_fetch.screen_varyings).encode()
                         + b'\0abi\0' + repr(sorted(abi.VALUES.items())).encode()).hexdigest()
TOOL_FP = tool_fingerprint().encode()

_CAST = ('as_type<int>(', 'as_type<uint>(')
def int_inputs(src):
    """Inputs that a vertex shader only uses as integers (under a float declaration every use is an as_type<int/uint> bit cast, or a plain copy) → {location: 'uint'}.
    Maxwell does no type conversion when reading vertex attribute registers: the bit pattern of integer formats (bone indices R8UI / RG8UI / RGBA8UI etc.) goes into the register as is and the shader uses it as an integer.
    The answer table has no attribute types (the machine code does not say them), so the translator's attribute type queries all defaulted to float: Metal converts the integer format to a float value, and after the bit cast it is garbage - every bone index is wrong and skinned models (the characters) are stretched into streaks or thrown off screen."""
    ins = {m.group(2): int(m.group(3)) for m in attributes(src)}
    i = src.find('vertex main0_out main0(')
    if i < 0: return {}
    body = src[i:]; out = {}
    for name, loc_ in ins.items():
        uses = list(re.finditer(r'in\.%s\b' % re.escape(name), body))
        nint = 0; bad = False
        for u in uses:
            ls = body.rfind('\n', 0, u.start()) + 1; le = body.find('\n', u.end())
            m = re.match(r'\s*float\s+(_\w+)\s*=\s*in\.%s\.[xyzw];$' % re.escape(name), body[ls:le])
            if not m:
                if body[:u.start()].endswith(_CAST): nint += 1; continue
                bad = True; break
            v = m.group(1)
            for o in list(re.finditer(r'\b%s\b' % re.escape(v), body))[1:]:
                if body[:o.start()].endswith(_CAST): nint += 1; continue
                l2s = body.rfind('\n', 0, o.start()) + 1; l2e = body.find('\n', o.end())
                if re.match(r'\s*[\w.\[\]]+\s*=\s*%s;$' % re.escape(v), body[l2s:l2e]): continue   # plain copy: the bit pattern is the same under either declaration
                bad = True; break
            if bad: break
        if not bad and nint: out[str(loc_)] = 'uint'
    return out

# Both pair tables feed pipelines, so both must feed padding, build order and shader cache keys.
def load_pairs(game, package):
    pairs = collections.defaultdict(set)
    for table in ('shader_pairs.tsv', 'pso_pairs.tsv'):
        path = str(game.table(table))
        if table == 'pso_pairs.tsv' and not os.path.isfile(path):
            path = os.path.join(os.path.dirname(os.path.abspath(package)), table)   # the split's copy for profiles predating this table
        if table == 'pso_pairs.tsv' and not os.path.isfile(path): continue
        with open(path) as f:
            for ln in f:
                if ln.strip() and not ln.startswith('#'): v, p = ln.split(); pairs[v].add(p)
    return pairs

PAIRS = load_pairs(GAME, dm_dir)

def build_keys(want, locations, pairs):
    """A partial vertex build needs its available paired pixel shaders before padding; a full build keeps every shader."""
    return sorted(set(want) | {ps for vs in want for ps in pairs.get(vs, ()) if ps in locations}) if want else sorted(locations)

def partner_inputs(key):
    """The user(locnN) locations the pixel shaders paired with a vertex shader read (vs_pad); pairs whose pixel MSL cannot be read are skipped."""
    want = set()
    for ps in PAIRS.get(key, ()):
        data = output('mac', ps + '_b.metal')   # finished before this shader (fin's phases), so staged in the cache by now
        if data is not None: want |= {int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', data.decode())}
    return want

def partner_reads(key):
    """{location: component mask} the pixel shaders paired with a mesh shader read (dmsh_meshstage.to_stage's reads), from their SPH input maps (dmsh_meshstage.pixel_reads), None without
    pairs. A location a partner's MSL declares that its input map does not name (a record not found, a fixed-function input the translator moved to a user location) counts as all four."""
    if not PAIRS.get(key): return None
    reads = collections.defaultdict(int)
    for ps in PAIRS[key]:
        got = {}
        if ps in loc:
            rec = rec_of(ps)[1]; ci = rec.find(bytes.fromhex('34127698'), 0x38); code = rec[0x38:ci]
            got = dmsh_meshstage.pixel_reads(code[:0x80] if dmsh_ampere.is_nvn2(rec[ci:]) else code[0x30:0x80])   # an Ampere SPH leads the code, a Maxwell one follows its 0x30 header
        data = output('mac', ps + '_b.metal')
        for n in ({int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', data.decode())} if data is not None else set()) - got.keys(): got[n] = 0xf
        for n, m in got.items(): reads[n] |= m
    return dict(reads)

def vs_pad(key, src):
    """Give a vertex shader the outputs its paired pixel shaders read but it does not write, with the value (0,0,0,1).
    Maxwell attributes are untyped 32-bit slots, and a pixel shader reading a location the vertex shader did not write only gets a default; Ryujinx also fills them with (0,0,0,1) when it links stages. Here every shader is translated
    offline on its own without knowing what the next stage reads, and Metal requires every pixel shader input to be a vertex output: 331 of 9043 pairs failed to link and their draws were skipped
    (over 10,000 "Fragment input(s) user(locnN) ... not written by vertex shader" in one iPhone session after the opening course). Returns (new source, padded locations); pairs whose pixel MSL cannot be read are skipped."""
    miss = sorted(partner_inputs(key) - {int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', src)})
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i); k = src.find('main0_out out = {};')
    if not miss or i < 0 or j < 0 or k < 0: return src, []
    src = src[:j] + ''.join(f'\n    float4 dm_pad{l} [[user(locn{l})]];' for l in miss) + src[j:]
    k = src.find('main0_out out = {};') + len('main0_out out = {};')
    return src[:k] + ''.join(f'\n    out.dm_pad{l} = float4(0.0, 0.0, 0.0, 1.0);' for l in miss) + src[k:], miss

def vs_init(src):
    """Every vertex shader output starts as (0,0,0,1) before the translated body runs: on the hardware that is what an unwritten component reads. The translator's source starts with out = {}, all zeros;
    Ryujinx initializes the components the next stage reads this way when it links stages, and since every shader is translated offline on its own without knowing what the next stage reads, all are
    initialized (components the body writes are overwritten as before). An output written only in xyz (a world position) is divided by its w in the pixel shader: with w = 0 every shadow-map
    compare value became infinite and Link's Awakening was in shadow everywhere outdoors (dim, no shadows from the fences)."""
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i); k = src.find('main0_out out = {};')
    if i < 0 or j < 0 or k < 0: return src
    outs = re.findall(r'\n    float4 (\w+) \[\[user\(locn\d+\)\]\];', src[i:j])
    k += len('main0_out out = {};')
    return src[:k] + ''.join(f'\n    out.{n} = float4(0.0, 0.0, 0.0, 1.0);' for n in outs) + src[k:]

VP_BASE_INSTANCE = '(*spvDescriptorSet0.vp_c0)._m0[100].y'   # c0 + 0x644, the driver's base-instance word (row 100, second component)

def vs_base_instance(src):
    """The draw's base instance from Metal, not from the driver segment. GLSLC adds c0 + 0x644 to Maxwell's instance ID where a shader needs the absolute instance (gl_InstanceID +
    gl_BaseInstance); the host writes that word from the draw command (nvn_mtl.m encode_args), but an indirect draw's base instance is in its argument record, which the GPU may write
    earlier in the same submission. Metal passes every draw's base instance, direct or indirect, as [[base_instance]], so a vertex function reads it there. Super Mario Party Jamboree's
    GPU-driven instances (276504dc5c987ecf writes the records, 4b05556737e9535f draws them: record = instance + c0 + 0x644, 80 bytes each) all took the first records' transforms
    with 0 there, and stray grey triangles crossed the board. The segment's rows are float4 holding raw bits, so the word becomes the float with the base instance's bits; a function without
    the parameter gets it"""
    if VP_BASE_INSTANCE not in src: return src
    src = src.replace(VP_BASE_INSTANCE, 'as_type<float>(gl_BaseInstance)')
    def add(m):
        return m.group(0) if '[[base_instance]]' in m.group(0) else m.group(0)[:-1] + ', uint gl_BaseInstance [[base_instance]])'
    return re.sub(r'\bvertex\s+\w+\s+main0\s*\((?:[^()]|\([^()]*\))*\)', add, src)   # the parameter list, whose attributes hold parentheses ([[buffer(0)]])

def drop_point_size(src):
    """Take the point size output out of vertex functions whose output struct no longer has it. A write goes; where the translated body takes the output by reference (_88(out.gl_Position, out.gl_PointSize)), that function still needs something to write, so the
    vertex function gets a local float for it"""
    src = re.sub(r'\n    out\.gl_PointSize = [^;\n]*;', '', src)
    while 'out.gl_PointSize' in src:
        use = src.find('out.gl_PointSize')
        a = [m for m in re.finditer(r'\bvertex\s+\w+\s+\w+\s*\(', src) if m.start() < use]
        if not a: sys.exit('[RED] a point size output used outside a vertex function')
        b, depth = src.find('{', a[-1].end()), 0
        for e in range(b, len(src)):
            depth += src[e] == '{'; depth -= src[e] == '}'
            if not depth: break
        src = src[:b + 1] + '\n    float dmsh_point_size = 1.0;' + src[b + 1:e].replace('out.gl_PointSize', 'dmsh_point_size') + src[e:]
    return src

def vs_layer(src):
    """A vertex shader that writes the layer (render_target_array_index) loses the point size output. The translator writes gl_PointSize = 1.0 in every vertex shader; Metal builds a pipeline
    whose vertex function writes the layer only with an input primitive class, and one that writes a point size only with the point class (or none), so with both no pipeline can be built.
    Pokémon Legends: Z-A's layered full-screen passes (57246f9a04af8529, b29f08619f379a83: a triangle per instance, layer = instance + offset) draw triangles, where the point size is unused;
    the host gives such pipelines the triangle class (nvn_mtl.m pso_get_). Shaders that do not write the layer are unchanged"""
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i)
    if i < 0 or j < 0 or '[[render_target_array_index]]' not in src[i:j] or '[[point_size]]' not in src[i:j]: return src
    src = src[:i] + re.sub(r'\n    float gl_PointSize \[\[point_size\]\];', '', src[i:j], count=1) + src[j:]
    return drop_point_size(src)

def gs_slice(spv):
    """A geometry shader that only picks the layer: triangles in, at most 3 vertices out, each vertex copying its input position and the input components it passes on, and gl_Layer taking the
    bits of one input component (Unreal Engine 4.18's FWriteToSliceGS, which writes one slice of a volume texture per instance: DRAGON QUEST XI S 0e7f58cd1f602f6d builds the 32x32x32 colour
    grading LUT the tone mapper reads, and without it the screen came out a flat blue). Metal has no geometry stage, so the host drops such a shader and draws with the vertex shader's
    layer entry points instead (vs_layer_entry), which write that component as the layer. Read from the SPIR-V as GLSL (SPIRV-Cross writes no MSL for a geometry stage).
    → {'loc': input location, 'comp': component} or None (any other geometry shader: one-pass cube shadows emit six transformed triangles)"""
    r = subprocess.run([SPIRV_CROSS, spv, '--version', '450'], capture_output=True, text=True)
    g = r.stdout
    if r.returncode or 'layout(triangles) in;' not in g or 'layout(max_vertices = 3, triangle_strip) out;' not in g or g.count('EmitVertex();') != 3 or 'EndPrimitive' in g: return None
    ins = {m.group(2): int(m.group(1)) for m in re.finditer(r'layout\(location = (\d+)\) in vec4 (\w+)\[3\];', g)}
    outs = {m.group(2): int(m.group(1)) for m in re.finditer(r'layout\(location = (\d+)\) out vec4 (\w+);', g)}
    body = g[g.find('void main()'):]
    var, layer = {}, set()
    for st in (s.strip() for s in re.split(r'[;{}]', body[body.find('{') + 1:])):
        if not st or st == 'if (false)' or re.fullmatch(r'int \w+ = -?\d+|\w+ = int\(0x80000000\)|int \w+ = \w+|EmitVertex\(\)', st): continue
        m = re.fullmatch(r'float (\w+) = (\w+)\[\w+\]\.([xyzw])', st)
        if m and m.group(2) in ins: var[m.group(1)] = (ins[m.group(2)], m.group(3)); continue
        m = re.fullmatch(r'float (\w+) = gl_in\[\w+\]\.gl_Position\.([xyzw])', st)
        if m: var[m.group(1)] = ('pos', m.group(2)); continue
        m = re.fullmatch(r'gl_Position\.([xyzw]) = (\w+)', st)
        if m and var.get(m.group(2)) == ('pos', m.group(1)): continue
        m = re.fullmatch(r'(\w+)\.([xyzw]) = (\w+)', st)
        if m and m.group(1) in outs and var.get(m.group(3)) == (outs[m.group(1)], m.group(2)): continue
        m = re.fullmatch(r'gl_Layer = floatBitsToInt\((\w+)\)', st)
        if m and m.group(1) in var and var[m.group(1)][0] != 'pos': layer.add(var[m.group(1)]); continue
        return None   # anything else: not a plain slice writer
    if len(layer) != 1 or g.count('gl_Layer =') != 3: return None
    (loc, comp), = layer
    return {'loc': loc, 'comp': comp}

def vs_layer_entry(full):
    """Layer entry points for a vertex shader that writes integer bits taken from the instance index into an output component (Unreal Engine 4.18's WriteToSliceMainVS: the slice = instance −
    base instance + its first slice, DRAGON QUEST XI S 7ceb7898171a9ae9). main0_l and main0_lz are main0 and main0_z writing that component as the layer (render_target_array_index), without
    the point size (see vs_layer); the host draws with them when the program's geometry shader is a slice writer reading that component (gs_slice). → (source with the two entry points added,
    {'loc', 'comp'}), or (source, None) when no output qualifies"""
    i = full.find('struct main0_out\n{\n'); j = full.find('\n};', i)
    if i < 0 or j < 0 or 'gl_InstanceIndex' not in full or '[[render_target_array_index]]' in full[i:j]: return full, None
    locs = {m.group(1): int(m.group(2)) for m in re.finditer(r'\n    float4 (\w+) \[\[user\(locn(\d+)\)\]\];', full[i:j])}
    m = next((m for m in re.finditer(r'\n    out\.(\w+)\.([xyzw]) = as_type<float>\(', full) if m.group(1) in locs), None)
    if not m: return full, None
    name, comp = m.group(1), m.group(2)
    st = re.sub(r'\n    float gl_PointSize \[\[point_size\]\];', '', full[i:j + 3]).replace('struct main0_out', 'struct main0l_out', 1)
    st = st[:-3] + '\n    uint gl_Layer [[render_target_array_index]];\n};'
    add = '\n' + st + '\n'
    for ep, new in (('main0', 'main0_l'), ('main0_z', 'main0_lz')):
        a = re.search(rf'\bvertex\s+main0_out\s+{ep}\s*\(', full)
        if not a: return full, None
        b, depth = full.find('{', a.end()), 0
        for e in range(b, len(full)):
            depth += full[e] == '{'; depth -= full[e] == '}'
            if not depth: break
        fn = full[a.start():e + 1].replace('main0_out', 'main0l_out').replace(f' {ep}(', f' {new}(', 1)
        fn = drop_point_size(fn)
        add += '\n' + fn.replace('return out;', f'out.gl_Layer = as_type<uint>(out.{name}.{comp});\n    return out;') + '\n'
    return full + add, {'loc': locs[name], 'comp': comp}

def vs_depth(src):
    """Two entry points for the depth convention: the translator's vertex shader leaves z unchanged (NVN's near=0 convention); the main function is copied here - main0 gets the GL convention's (near=-w) z' = z/2 + w/2 before every return out
    (bit-identical to the formula Ryujinx used to emit in the translator), main0_z stays as is. The convention a game sets is runtime state (nvnDeviceSetDepthMode); the host picks the entry point by it, with nothing to configure per game.
    Metal function constants could do this too, but a function with function constants must always be specialized before a pipeline can be built (measured about 2.4 ms per vertex shader); two entry points only compile the vertex shaders twice (about 30% of the library).
    Returns (the source with only main0: reflection / raster facts are computed from it as before, the full source written into the metallib)."""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\(', src)
    if not m: sys.exit('[RED] vertex shader without a main0 main function, cannot add the depth entry points')
    body = src[m.start():]   # the main function is the last function in the file
    gl = body.replace('return out;', 'out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);\n    return out;')
    single = src[:m.start()] + gl
    return single, single + '\n' + re.sub(r'\bmain0\s*\(', 'main0_z(', body, count=1)

def int_outputs(src, body, cols):
    """The colour outputs that hold integers, not float bits. The translator keeps registers in integer locals, so a float colour also reaches its output as as_type<float>(_n), with _n =
    as_type<int>(<a float>) somewhere above; Kirby Star Allies' G-buffer shaders write all four of colour 0 to 2 that way, and only colour 2 goes to an integer target (RGBA8Uint, its material bits:
    constants, shifts and ORs). So each write is followed back through the locals: a value computed by an integer operation or a small integer constant counts as integer, the bits of a computed
    float as float; zero, larger constants (the bits of a float constant) and words copied from a uniform or an input say nothing. A denormal float literal is a small integer constant SPIRV-Cross
    printed through the output's type. → the outputs with integers and no float bits, or, when none is that clean (a register reused by two branches), the outputs with integers"""
    body = dmsh_fetch.unguard(body)   # a bounded uniform word is a word copied from a uniform
    types = {v: t for t, v in re.findall(r'^\s*(u?int\d?|float\d?|bool)\s+(_\d+)\b', body, re.M)}
    defs = collections.defaultdict(list)
    for v, e in re.findall(r'^\s*(?:(?:u?int\d?|float\d?|bool)\s+)?(_\d+)\s*=\s*([^;]+);', body, re.M): defs[v].append(e.strip())
    def kinds(e, seen):
        if re.fullmatch(r'_\d+', e):
            if types.get(e, '').startswith('float'): return {'float'}
            if e in seen: return set()
            seen.add(e); return set().union(*(kinds(x, seen) for x in defs[e])) if defs[e] else set()
        if re.fullmatch(r'-?\d+u?', e): return {'int'} if 0 < abs(int(e.rstrip('u'))) < 0x800000 else set()
        t = re.fullmatch(r'[^?]+\?\s*([^:]+?)\s*:\s*(.+)', e)   # a choice between two values (a comparison's 1.0 or 0.0 in Ghost Trick's colour 0) is what its values are
        if t: return kinds(t[1], seen) | kinds(t[2], seen)
        a = re.fullmatch(r'as_type<u?int\d?>\((.*)\)', e)
        if a: return set() if re.search(r'fp_c\d|\bin\.|^-?[0-9.]', a[1]) else {'float'}
        return set() if e.startswith(('(*spvDescriptorSet0', 'spvDescriptorSet0')) else {'int'}
    found = {}
    for nm in cols:
        k = set()
        for w in re.findall(r'\bout\.%s(?:\.\w+)? = ([^;]+);' % nm, body):
            w = w.strip(); a = re.fullmatch(r'as_type<float\d?>\((.*)\)', w)
            if a: k |= kinds(a[1].strip(), set())
            elif re.fullmatch(r'-?[0-9.]+(?:e[-+]?\d+)?f?', w): k |= {'int'} if 0 < abs(float(w.rstrip('f'))) < 1.17549e-38 else set()
            else: k.add('float')
        found[nm] = k
    return {nm for nm, k in found.items() if k == {'int'}} or {nm for nm, k in found.items() if 'int' in k}

def fs_uint(src):
    """Integer colour targets. With no PC version the translator declares every fragment output float; a shader writes an integer as the float with its bits (as_type<float>(…)), and Metal
    refuses a pipeline whose float output meets an integer attachment. Luigi's Mansion 3 draws its whole scene into an R32Uint + RGBA8Uint G-buffer (1,377 of its 4,307 pixel shaders write
    an output from integer bits): a hand-written row per shader (shader_facts_manual.tsv) does not scale to that. A pixel shader that writes an output from integer bits gets a second entry
    point, main0_u, the same body returning every colour output as the unsigned integer vector of the same bits (as_type, nothing converted); the host takes it for a pipeline whose colour
    attachments are all unsigned integer formats (nvn_mtl.m).
    A G-buffer may mix the two: Pokémon Sword's scene shaders write colour 0 and 2 as floats and colour 1 from integer bits into an RGBA16Uint target. A shader whose outputs include both
    gets a third entry point, main0_m, that returns only the outputs written from integer bits as unsigned integers (their colour indices: the mask); the host takes it for a pipeline whose
    integer attachments are exactly those; which outputs those are comes from how their values are computed (int_outputs), as nearly every float output is written as integer bits too.
    Both call main0's body (dmsh_side.shared_body) instead of copying it. → (main0_u (and main0_m), to append to the source with its body shared, the main0_m mask or 0), or (None, 0)"""
    m = re.search(r'^.*\bfragment\s+main0_out\s+main0\s*\(', src, re.M); st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not m or not st: return None, 0
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    cols = {nm: ty for ty, nm, at in mem if 'color(' in at and ty.startswith('float')}
    # The hardware writes an output's bits to an integer target whatever the shader computed, so main0_u is right for any pixel shader; only those that look like integer writers get one
    # (the library stays small): an output from integer bits, nothing but constants (a clear drawn into an integer target, 35e1865e313f0553 writes 0 into Luigi's Mansion 3's R32Uint target),
    # or nothing but flat stage inputs passed through (the bits its vertex or mesh stage wrote: Mario Kart World's dcb0504c844f1ecc copies a primitive's id into an R8Uint target)
    stores = [m for m in re.finditer(r'out\.(\w+)(?:\.\w+)? = ([^;]+);', src) if m.group(1) in cols and not in_word(src, m.start())]   # one pass over the text, not one for each output and each question
    from_int = {m.group(1) for m in stores if re.match(r'as_type<float\d?>\(', m.group(2))}
    writes = [m.group(2) for m in stores]
    flat = set(re.findall(r'\b(\w+) \[\[user\(locn\d+\), flat\]\]', src)) if ', flat]]' in src else ()
    def passed(w):   # a flat input, or a local that holds one
        w = w.strip(); d = re.fullmatch(r'\w+', w) and re.search(r'\bfloat\d? %s = (in\.\w+(?:\.\w+)?);' % w, src)
        i = re.fullmatch(r'in\.(\w+)(?:\.\w+)?', d[1] if d else w)
        return bool(i and i[1] in flat)
    body = src[m.start():]; body = body[:body.find('return out;') + 1 or len(body)]
    ints = int_outputs(src, body, cols)
    if not from_int and not ints and not (writes and (all(re.fullmatch(r'-?[0-9.]+f?|float\d\([-0-9., ]*\)', w.strip()) for w in writes) or all(passed(w) for w in writes))): return None, 0
    uty = lambda ty: 'u' + ty.replace('float', 'int')   # float4 → uint4, float → uint
    try: e = dmsh_side.entry(src, 'main0')
    except ValueError: e = None
    if not e: return None, 0
    def entry(sfx, as_uint):   # main0_<sfx>: main0's body (dmsh_side.shared_body), its outputs in as_uint returned as the unsigned integer vectors of their bits
        struct = f'struct main0_out_{sfx}\n{{\n' + ''.join(f'    {uty(cols[nm]) if nm in as_uint else ty} {nm} {at};\n' for ty, nm, at in mem) + '};\n\n'
        conv = f'return main0_out_{sfx}{{' + ', '.join(f'as_type<{uty(cols[nm])}>(out.{nm})' if nm in as_uint else f'out.{nm}' for _, nm, _ in mem) + '};'
        return '\n' + struct + dmsh_side.calling(e, f'main0_{sfx}', f'main0_out_{sfx}', conv)
    out, mask = entry('u', set(cols)), 0
    as_int = ints or from_int
    if as_int and set(cols) - as_int:   # some outputs from integer bits, some not: the mixed entry point
        out += entry('m', as_int)
        mask = sum(1 << int(re.search(r'color\((\d+)\)', at).group(1)) for _, nm, at in mem if nm in as_int)
    return out, mask

def fs_dual(src):
    """Dual-source blending. NVN's SRC1 blend factors read the pixel shader's output 1 as target 0's second source (Maxwell feeds MRT 1 to it), and the translator declares that
    output [[color(1)]]; Metal reads the second source only from [[color(0), index(1)]], and its compiler crashes building a pipeline that blends with an absent one (Pokémon Legends:
    Arceus's ha_post_effects, f4f36a03b0c6691d). A shader in the profile's dual_want.tsv that writes exactly colours 0 and 1 gets a second entry point, main0_d, the same body with
    output 1 as index 1 of colour 0; the host takes it for a pipeline whose target 0 blends with a SRC1 factor (nvn_mtl.m pso_desc). Not every such shader gets one: most write a
    second render target (5,474 of Arceus's 6,156 pixel shaders write exactly colours 0 and 1). It calls main0's body (dmsh_side.shared_body): a copy of the body was compiled again, and
    a readable-tile rewrite of its samples would not build there (dmsh_fetch.fold). → main0_d, to append to the source with its body shared (with its other entry points), or None"""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not st: return None
    try: e = dmsh_side.entry(src, 'main0')
    except ValueError: e = None
    if not e: return None
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    if sorted(at for _, _, at in mem if 'color(' in at) != ['[[color(0)]]', '[[color(1)]]']: return None
    struct = 'struct main0_out_d\n{\n' + ''.join(f'    {ty} {nm} {"[[color(0), index(1)]]" if at == "[[color(1)]]" else at};\n' for ty, nm, at in mem) + '};\n\n'
    return '\n' + struct + dmsh_side.calling(e, 'main0_d', 'main0_out_d', 'return main0_out_d{' + ', '.join(f'out.{nm}' for _, nm, _ in mem) + '};')

SG_OPS = {333: 'true', 334: 'pred', 335: 'pred', 336: 'true', 337: 'value', 338: 'value', 339: 'ballot', 345: 'value', 346: 'value', 347: 'value', 348: 'value'}   # OpGroupNonUniform* → its one-lane value
SG_BUILTINS = {36: 'one', 41: 'zero', 4416: 'one4', 4417: 'one4', 4418: 'zero4', 4419: 'one4', 4420: 'zero4'}   # SubgroupSize, SubgroupLocalInvocationId, Eq / Ge / Gt / Le / Lt masks
def spv_single_lane(path):
    """Metal has no SIMD-group functions in vertex functions, and SPIRV-Cross's subgroup emulation covers only Elect. Monster Hunter Rise's vertex shaders (about 4%) scalarize uniform loads:
    ballot the active lanes, shuffle the first lane's value to all, and take a fast path when all agree. With a subgroup of one lane that is exact: the ballot is the lane's own bit, shuffles and
    broadcasts return the lane's own value, all / any are the predicate, all-equal and elect are true, the lane ID is 0, the size 1 and the masks follow. The SPIR-V is rewritten that way
    (the builtin variables, their decorations and the subgroup capabilities removed). → True when rewritten, False when there is nothing to do or an operation it does not cover"""
    with open(path, 'rb') as f: d = f.read()
    words = list(struct.unpack(f'<{len(d) // 4}I', d)); hdr, ins, i = words[:5], [], 5
    while i < len(words): n = words[i] >> 16; ins.append(words[i:i + n]); i += n
    op = lambda x: x[0] & 0xffff
    if not any(op(x) in SG_OPS or (op(x) == 71 and len(x) > 3 and x[2] == 11 and x[3] in SG_BUILTINS) for x in ins): return False
    if any(340 <= op(x) <= 366 and op(x) not in SG_OPS for x in ins): return False   # other subgroup operations (bit counts, arithmetic): not covered
    bvar = {x[1]: SG_BUILTINS[x[3]] for x in ins if op(x) == 71 and len(x) > 3 and x[2] == 11 and x[3] in SG_BUILTINS}
    # A mask component can be loaded through an input pointer (Xenoblade Chronicles 3's prefix ballot), rather than loading the whole vector first. Keep its index and extract
    # from the constant vector at the load; removing only the builtin variable would leave a dangling pointer. Other access-chain forms remain unsupported.
    bptr = {}
    for x in ins:
        if op(x) in (65, 66) and len(x) > 3 and x[3] in bvar:
            if len(x) != 5 or not bvar[x[3]].endswith('4'): return False
            bptr[x[2]] = (bvar[x[3]], x[4])
    if any(op(x) in (65, 66) and len(x) > 3 and x[3] in bptr for x in ins): return False
    uint = next((x[1] for x in ins if op(x) == 21 and x[2:4] == [32, 0]), None); boolt = next((x[1] for x in ins if op(x) == 20), None)
    uvec4 = next((x[1] for x in ins if op(x) == 23 and x[2] == uint and x[3] == 4), None)
    if uint is None or boolt is None or uvec4 is None: return False
    bound = hdr[3]; c = {}
    for nm in ('zero', 'one', 'zero4', 'one4', 'true'): c[nm] = bound; bound += 1
    consts = [[43 | 4 << 16, uint, c['zero'], 0], [43 | 4 << 16, uint, c['one'], 1], [44 | 7 << 16, uvec4, c['zero4'], c['zero'], c['zero'], c['zero'], c['zero']],
              [44 | 7 << 16, uvec4, c['one4'], c['one'], c['zero'], c['zero'], c['zero']], [41 | 3 << 16, boolt, c['true']]]
    out, placed = [], False
    for x in ins:
        o = op(x)
        if o == 17 and x[1] in (61, 62, 63, 64, 65, 66, 67, 68, 4423, 4431): continue   # GroupNonUniform* / SubgroupBallotKHR / SubgroupVoteKHR capabilities
        if o == 10 and (b'ballot' in struct.pack(f'<{len(x) - 1}I', *x[1:]) or b'subgroup' in struct.pack(f'<{len(x) - 1}I', *x[1:])): continue
        if o in (71, 5) and x[1] in bvar: continue   # the builtin's decoration and name
        if o == 59 and x[2] in bvar: continue        # the builtin variable itself
        if o == 15: x = _ep_drop(x, bvar)
        if o == 54 and not placed: out += consts; placed = True
        if o == 61 and x[3] in bvar: out.append([83 | 4 << 16, x[1], x[2], c[bvar[x[3]]]]); continue   # a load of a builtin → its constant
        if o in (65, 66) and x[2] in bptr: continue
        if o == 61 and x[3] in bptr:
            nm, index = bptr[x[3]]
            out.append([77 | 5 << 16, x[1], x[2], c[nm], index]); continue   # OpVectorExtractDynamic, including a constant index
        if o in SG_OPS:
            kind = SG_OPS[o]
            if kind == 'ballot': out.append([169 | 6 << 16, x[1], x[2], x[4], c['one4'], c['zero4']]); continue   # OpSelect(pred, own bit, 0)
            out.append([83 | 4 << 16, x[1], x[2], c['true'] if kind == 'true' else x[4]]); continue
        out.append(x)
    if not placed: return False
    hdr[3] = bound
    flat = hdr + [v for x in out for v in x]
    with open(path, 'wb') as f: f.write(struct.pack(f'<{len(flat)}I', *flat))
    return True
def _ep_drop(x, ids):   # OpEntryPoint without the given interface IDs: model, function, the name (a nul-terminated string in whole words), then the interface list
    k = 3
    while k < len(x):
        k += 1
        if any((x[k - 1] >> (8 * b)) & 0xff == 0 for b in range(4)): break   # the word holding the string's nul ends the name
    y = x[:k] + [v for v in x[k:] if v not in ids]
    y[0] = len(y) << 16 | (x[0] & 0xffff)
    return y

def geometry_finish(key, w, j, c1):
    """Build all five input-topology variants; the draw selects one at replay.
    Resources retain logical NVN stage 2 even though Metal executes a kernel.
    """
    code = open(j['code'], 'rb').read()
    dmsh_geometry.output_layout(code, 0)   # validate before running the translator
    jobs = [{**j, 'key': key + '_g' + str(t), 'geometry_topology': t} for t in range(5)]
    jp = os.path.join(w, key + '_geometry.json'); atomic_json(jp, jobs)
    rx = subprocess.run([XLATE, jp, w], capture_output=True, text=True)
    if rx.returncode: return 'geometry failed', rx.stderr.strip()[:160], None
    base = None
    for t, job in enumerate(jobs):
        layout = dmsh_geometry.output_layout(code, t)
        k = job['key']; spv = os.path.join(w, k + '.spv')
        dmsh_geometry.flatten_sets(spv)
        r = subprocess.run([SPIRV_CROSS, *dmsh_atomic.cross_args(XC, spv), spv], capture_output=True, text=True)
        if r.returncode: return 'geometry MSL failed', r.stderr.strip()[:160], None
        plain = dmsh_geometry.topology_pointer(pool_arrays(precise(dmsh_atomic.check_msl(r.stdout))))
        src = dmsh_geometry.capture_input(plain)
        res = json.load(open(os.path.join(w, k + '.res.json')))
        refl = {'key': key + '_bg' + str(t), 'route': 'B', 'chain': CHAIN, 'dm_ctl': key,
                'source': f'{loc[key][0]}#{loc[key][1]}', 'pc_paired': pc_of[key], 'stage': 'gs',
                'entry': 'main0', 'msl_entry_kind': 'kernel', 'threadgroup_size': [32, 1, 1],
                'geometry': dict(layout, stride=res['geometry_stride']),
                'c1': c1.hex(), 'notes': res['notes'], 'drv_reads': drv_reads(src), 'drv_unfillable': [],
                'bindings': reflect(key, 'gs', src, res), 'vertex_inputs': [], 'raster': {}}
        def save(suffix, text, meta):
            path = os.path.join(out, key + suffix + '.metal'); open(path, 'w').write(text)
            if 'mac' in TARGETS: MTL.compile('mac', path, os.path.join(out, key + suffix + '.metallib'))
            atomic_json(os.path.join(out, key + suffix + '.json'), MTL.with_compiler('mac', meta))
            if IOSOUT:
                MTL.compile('ios', path, os.path.join(IOSOUT, key + suffix + '.metallib'))
                atomic_json(os.path.join(IOSOUT, key + suffix + '.json'), MTL.with_compiler('ios', meta))
        save('_bg' + str(t), src, refl)
        # Passthrough input without adjacency: a lean kernel the host can pair with the game's own vertex shader (dmsh_geometry.lean)
        if dmsh_geometry.passthrough(code) and t in (0, 1, 3):
            try: lean, reads = dmsh_geometry.lean(plain, res['geometry_stride'])
            except ValueError as e: lean = None; refl['notes'] = refl['notes'] + [str(e)]
            if lean:
                save('_bl' + str(t), lean, {**refl, 'key': key + '_bl' + str(t), 'geometry': dict(layout, stride=1, lean=True, capture_reads=reads),
                                            'drv_reads': drv_reads(lean), 'bindings': reflect(key, 'gs', lean, res)})
        if t == 0:
            base = {**refl, 'key': key + '_b'}
            save('_b', src, base)
            spv = os.path.join(w, k + '.reader.spv'); dmsh_geometry.flatten_sets(spv)
            r = subprocess.run([SPIRV_CROSS, *dmsh_atomic.cross_args(XC, spv), spv], capture_output=True, text=True)
            if r.returncode: return 'geometry reader failed', r.stderr.strip()[:160], None
            reader = vs_layer(vs_init(dmsh_geometry.reader(pool_arrays(precise(dmsh_atomic.check_msl(r.stdout))))))
            analysis, full = vs_depth(reader)
            meta = {'key': key + '_bgv', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key,
                    'stage': 'vs', 'entry': 'main0', 'entry_z': 'main0_z', 'msl_entry_kind': 'vertex',
                    'geometry_reader': True, 'geometry_output': layout['output'], 'c1': '', 'notes': [], 'drv_reads': {}, 'drv_unfillable': [],
                    'bindings': reflect(key, 'vs', analysis, {'sbufs': []}), 'vertex_inputs': [], 'raster': {}}
            if j.get('tf'):
                full += vs_tf(analysis, j['tf'], geometry=True)
                meta.update(entry_tf='main0_tf', tf_strides=[e['stride'] if e else 0 for e in j['tf']])
            save('_bgv', full, meta)
    return 'ok', '', base


def finish(key, w, j, c1):
    name, k = loc[key]
    msl = os.path.join(w, key + '.metal'); lib = os.path.join(out, key + '_b.metallib')
    if j['stage'] == 'gs':   # keep the layer-only fold; other geometry runs as compute
        s = gs_slice(os.path.join(w, key + '.spv')) if not j.get('tf') else None
        if s:
            refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': 'gs', 'gs_slice': s,
                    'notes': [], 'drv_reads': '', 'drv_unfillable': [], 'bindings': []}   # no library: the host draws with the vertex shader's layer entry points
            write_refl(key, refl)
            return 'ok', '', refl
        try: return geometry_finish(key, w, j, c1)
        except (ValueError, subprocess.CalledProcessError) as e: return 'geometry failed', str(e)[:160], None
    if j['stage'] == 'vs': spv_single_lane(os.path.join(w, key + '.spv'))   # one-lane subgroups (see spv_single_lane)
    r = subprocess.run([SPIRV_CROSS, *dmsh_atomic.cross_args(XC, os.path.join(w, key + '.spv')), os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
    if r.returncode: return 'MSL failed', r.stderr.strip()[:160], None
    src = open(msl).read()
    if j['stage'] == 'vs':   # second pass: re-translate with inputs that are only used as integers reported as uint (see int_inputs)
        extra = {l: t for l, t in int_inputs(src).items() if l not in j['attr']}
        if extra:
            j['attr'].update(extra); jp = os.path.join(w, f'job1_{key}.json'); json.dump([j], open(jp, 'w'))
            rx = subprocess.run([XLATE, jp, w], capture_output=True, text=True)
            if rx.returncode or not os.path.exists(os.path.join(w, key + '.spv')): return 'translation failed', ('integer-input retranslation: ' + rx.stderr.strip())[:160], None
            spv_single_lane(os.path.join(w, key + '.spv'))
            r = subprocess.run([SPIRV_CROSS, *dmsh_atomic.cross_args(XC, os.path.join(w, key + '.spv')), os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
            if r.returncode: return 'MSL failed', r.stderr.strip()[:160], None
            src = open(msl).read()
    try: src = dmsh_atomic.check_msl(src)
    except ValueError as e: return 'image atomic MSL failed', str(e), None
    src = precise(src)   # min / max / clamp with Maxwell's NaN results (see precise)
    src = mul_extended(src)   # scalar extended products without the optnone wrapper (see mul_extended)
    try: src = gather_offsets(src)   # four gathers instead of a wrapper's loop over their offsets (see gather_offsets)
    except ValueError as e: return 'gather offsets rewrite failed', str(e)[:160], None
    try: src = hmma_msl(src)   # Ampere HMMA through simdgroup_matrix (see hmma_msl)
    except ValueError as e: return 'HMMA rewrite failed', str(e)[:160], None
    try: src = shared_store_msl(src)   # shared memory byte and half-word stores as Metal byte stores (see shared_store_msl)
    except ValueError as e: return 'shared store rewrite failed', str(e)[:160], None
    try: src = pool_arrays(src)   # handles traced to no slot read the pools the replay keeps (see pool_arrays)
    except ValueError as e: return 'pool rewrite failed', str(e)[:160], None
    src = dmsh_wide.wide_loads(src)   # storage words the translator split, read whole again (see dmsh_wide.py)
    src = border_emu(src)   # border colours Metal lacks (see border_emu)
    if j['stage'] == 'ps': src = frag_z_snap(src)[0]   # gl_FragCoord.z against a depth of its own surface (see frag_z_snap)
    open(msl, 'w').write(src)
    first, spill = [], 0
    mesh = j.get('mesh_stage')
    if mesh:   # the kernel as the Metal object or mesh function (dmsh_meshstage.py)
        if mesh.get('refused'): return 'mesh stage failed', mesh['refused'], None
        # outputs its paired pixel shaders read but its SPHs do not enable get (0,0,0,1), as vs_pad gives a vertex shader's (Ocarina of Time's foliage meshes); each output as wide as they read
        try: src = dmsh_meshstage.to_stage(src, mesh, partner_reads(key)); open(msl, 'w').write(src)
        except ValueError as e: return 'mesh stage failed', str(e)[:160], None
    elif j['stage'] == 'cs':
        src, first = first_store(src, j.get('local')); open(msl, 'w').write(src)   # the first invocation's store, for the host (see first_store)
        try: src, spill = tg_spill(src)   # shared memory past Metal's 32 KB in a device buffer (see tg_spill)
        except ValueError as e: return 'shared memory rewrite failed', str(e)[:160], None
        if spill: open(msl, 'w').write(src)
    pad = []
    if j['stage'] == 'vs':   # outputs start as (0,0,0,1) (see vs_init); no point size beside a layer output (see vs_layer); the base instance from Metal (see vs_base_instance)
        src = vs_base_instance(vs_layer(vs_init(src))); open(msl, 'w').write(src)
    if j['stage'] == 'vs' and PAIRS.get(key):   # outputs the pixel shader reads but the vertex shader does not write get (0,0,0,1) (see vs_pad)
        src, pad = vs_pad(key, src)
        if pad: open(msl, 'w').write(src)
    lay = None; discard = False
    res = json.load(open(os.path.join(w, key + '.res.json')))
    if j['stage'] == 'vs':   # the two depth-convention entry points (see vs_depth); the analysis below uses the source with only main0
        capture = dmsh_geometry.capture(src)
        src, full = vs_depth(src)
        full += capture
        discard = vs_writes(src, reflect(key, 'vs', src, res))
        if discard: full += vs_discard(src)
        if j.get('tf'): full += vs_tf(src, j['tf'])   # transform feedback capture (see vs_tf)
        plain = full
        full, lay = vs_layer_entry(full)   # layer entry points for a slice-writing geometry shader (see vs_layer_entry)
        open(msl, 'w').write(full)
    fu, fmask = fs_uint(src) if j['stage'] == 'ps' else (None, 0)   # the integer-output entry points (see fs_uint); the analysis below uses the source with only main0
    fd = fs_dual(src) if j['stage'] == 'ps' and key in DUAL_WANT else None   # the dual-source entry point (see fs_dual), from the source with only main0
    shared = dmsh_side.shared_body(src) if fu or fd else None   # they call main0's body; fs_uint and fs_dual give none for a main0 whose shape it does not know
    if shared: plain = src; open(msl, 'w').write(shared + (fu or '') + (fd or ''))
    else: fu = fd = None
    try: binds = reflect(key, mesh['kind'] if mesh else j['stage'], src, res)   # before the compile: a pixel shader's readable-tile version needs its texture slots (ps_libraries)
    except ValueError as e: return 'reflection failed', str(e)[:160], None
    side, lost = {}, []
    def compile_targets():   # the selected targets from the same MSL (MSL identical to one compiled before takes that library, dmsh_mtlcache.py)
        if j['stage'] == 'ps': side.clear(); side.update(ps_libraries(key, msl, open(msl).read(), binds)); return
        if 'mac' in TARGETS: MTL.compile('mac', msl, lib)
        if 'ios' in TARGETS and IOSOUT: MTL.compile('ios', msl, os.path.join(IOSOUT, key + '_b.metallib'))
    try: compile_targets()
    except subprocess.CalledProcessError as e:
        if not lay and not fu and not fd: return 'metal failed', str(e)[:160], None
        lost = [name for name, extra in (('layer', lay), ('integer output', fu), ('dual source', fd)) if extra]   # kept in the reflection, as a cache hit compiles nothing: every build reports it (shader_regressions.report_losses)
        open(msl, 'w').write(plain); lay = fu = fd = None   # the layer, integer-output and dual-source entry points are extras: a library that does not build with them is built without
        try: compile_targets()
        except subprocess.CalledProcessError as e: return 'metal failed', str(e)[:160], None
    clone(msl, os.path.join(out, key + '_b.metal'))
    entry = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', src)
    dr = drv_reads(src)
    refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': mesh['kind'] if mesh else j['stage'],
            **({'mesh_stage': {'kind': mesh['kind'], 'threads': mesh['threads'], 'payload': mesh['payload'], 'task': mesh.get('task', True)}, **({'entry_z': 'main0_z'} if mesh['kind'] == 'ms' else {}),
               **({'entry_object': 'main0_o'} if mesh['kind'] == 'ms' and not mesh.get('task', True) else {})} if mesh else {}),
            'entry': entry.group(2) if entry else 'main0', 'msl_entry_kind': entry.group(1) if entry else None, **({'entry_z': 'main0_z', 'entry_geometry_capture': 'main0_gs'} if j['stage'] == 'vs' else {}),
            **({'entry_discard': 'main0_discard'} if discard else {}),
            **({'entry_tf': 'main0_tf', 'tf_strides': [e['stride'] if e else 0 for e in j['tf']]} if j.get('tf') else {}),
            **({'entry_layer': 'main0_l', 'entry_layer_z': 'main0_lz', 'layer_from': lay} if lay else {}), **({'entry_u': 'main0_u'} if fu else {}), **({'entry_m': 'main0_m', 'uint_mask_m': fmask} if fu and fmask else {}), **({'entry_d': 'main0_d'} if fd else {}), **({'entries_lost': lost} if lost else {}),
            **({'first_store': [{'reg': next(b['reg'] for b in binds if b['msl_name'] == n), 'word': w, 'value': v} for n, w, v in first]} if first else {}),
            **({'tg_spill': spill} if spill else {}),
            'threadgroup_size': j.get('local'), 'c1': c1.hex(), 'notes': res['notes'] + (['not in the answer table, queries use defaults'] if j.get('no_facts') else []), 'drv_reads': dr, 'drv_unfillable': unfillable(dr, mesh['kind'] if mesh else j['stage']),
            'bindings': binds,
            'vertex_inputs': [{'location': int(m.group(3)), 'msl_type': m.group(1)} for m in attributes(src)],
            'raster': {**raster_facts(j['stage'], src), **({'omap': ps_omap(open(j['code'], 'rb').read())} if j['stage'] == 'ps' else {})}, 'vs_pad': pad}
    if j['stage'] == 'vs': full = open(msl).read(); vin_finish(key, full, refl); pull_finish(key, full, refl)   # the shadow-input and pulling libraries (dmsh_vin.py, dmsh_pull.py), from the source with every entry point
    refl.update(side)   # the alpha test and logic operation entry points and the readable-tile version (ps_libraries)
    write_refl(key, refl)
    return 'ok', '', refl

keys = build_keys(want, loc, PAIRS)
if want: want = set(keys)   # dependency outputs are cleaned too, so a failed pixel build cannot leave old MSL for padding
NJ = int(os.environ.get('J', '8'))
BATCH = 4 * NJ   # shaders translated and finished together: the next batch is translated while this one is finished, and a shader's working files go when it is (fin).
# Minecraft Dungeons II, 15,826 shaders: at most 3,279 files under the build folder with batches of 256 removed whole, 68,230 before batches (Octopath Traveler 0's stopped build)
# A full build starts from no shader outputs, a partial one from none of the keys it builds, so what a run leaves is what it built: every output is restored from the cache anyway.
# The chain version is the same whatever the profile's tables, so a key that left the map, a shader that no longer builds or a variant no longer made would keep an old library.
# The stamp goes too, and a full build writes it at the end; a partial build keeps it only while it names the profile's current inputs.
# A full build also removes the image (as the NSP script does before it): a stopped build leaves no library, rather than one with old shaders for those it had not reached. A partial build
# builds on the image (OLD_IMAGE), less the outputs of the keys it builds, which publish_library leaves out
OUT_KEY = re.compile(r'([0-9a-f]{16})_b')
STAMP_BEFORE = shader_lib.read_stamp(out).get('inputs')
OLD_IMAGE = {}
for _label, _d in (('mac', out), ('ios', IOSOUT)):
    if not _d: continue
    if not want:
        shutil.rmtree(_d, ignore_errors=True); shader_lib.remove_image(_d)
    elif os.path.isfile(shader_lib.image_of(_d)): OLD_IMAGE[_label] = romfs.Path(shader_lib.image_of(_d))
    os.makedirs(_d, exist_ok=True)
    for _n in os.listdir(_d):   # a partial build over a folder (an older chain's, or one a stopped build left)
        _m = OUT_KEY.match(_n)
        if _m and _m.group(1) in want: os.unlink(os.path.join(_d, _n))
    if STAMP_BEFORE != INPUTS and os.path.exists(os.path.join(_d, dmsh_chain.STAMP)): os.unlink(os.path.join(_d, dmsh_chain.STAMP))
STORE.delete_prefix(STAGE + '/')   # what a stopped build of this folder staged

def sweep_work(root):
    """The working folders (dmsh_*) a build left that is no longer running: each holds the process ID of its build (pid), one from before that is taken after a day.
    Octopath Traveler 0's build, stopped in its compile, left 38,376 files there, and every later build made another folder beside it"""
    for d in (os.path.join(root, n) for n in os.listdir(root) if n.startswith('dmsh_')) if os.path.isdir(root) else ():
        try:
            with open(os.path.join(d, 'pid')) as f: pid = int(f.read())
            try: os.kill(pid, 0); continue   # its build runs
            except ProcessLookupError: pass
            except PermissionError: continue
        except (OSError, ValueError):
            try:
                if time.time() - os.stat(d).st_mtime < 86400: continue
            except OSError: continue
        shutil.rmtree(d, ignore_errors=True)
_work = os.environ.get('WORK') or tempfile.gettempdir()
sweep_work(_work)
w = tempfile.mkdtemp(prefix='dmsh_', dir=_work)
with open(os.path.join(w, 'pid'), 'w') as _f: _f.write(str(os.getpid()))
MTL = dmsh_mtlcache.Cache(STORE, {'mac': ('macosx', MAC_RECORD, mac_compile), **({'ios': ('iphoneos', ios_metal.RECORD, ios_metal.compile)} if IOSOUT else {})}, w)
# first take the libraries earlier chain versions left on disk: the b-*/ and b-*.img beside the output (iOS: the folder of the same name) and the roots DMSH_PREFILL lists
for _spec in [cache_root + ('=' + os.path.dirname(IOSOUT) if IOSOUT else '')] + [x for x in os.environ.get('DMSH_PREFILL', '').split(os.pathsep) if x]:
    _mac, _, _ios = _spec.partition('=')
    for _b in sorted(os.listdir(_mac)) if os.path.isdir(_mac) else []:
        if _b.startswith('b-') and os.path.realpath(shader_lib.folder_of(os.path.join(_mac, _b))) != os.path.realpath(out):
            for _ln in MTL.fill(os.path.join(_mac, _b), os.path.join(_ios, _b) if _ios else None, targets=TARGETS): print(_ln, flush=True)

# Progress for the NSP script (DMSH_PROGRESS: a small JSON file it reads, build_ipa_from_nsp.py ShaderWatch), written at most every half second
PROGRESS = os.environ.get('DMSH_PROGRESS')
_prog = {'t': 0.0, 'lock': threading.Lock()}
def progress(step, done, total, force=False, **extra):
    if not PROGRESS: return
    with _prog['lock']:
        now = time.time()
        if not force and now - _prog['t'] < 0.5: return
        _prog['t'] = now
        try:
            with open(PROGRESS + '.tmp', 'w') as f: json.dump({'step': step, 'done': done, 'total': total, **extra}, f)
            os.replace(PROGRESS + '.tmp', PROGRESS)
        except OSError: pass

packs, jobs, c1s = {}, {}, {}
packs_lock = threading.Lock()
digests, rule_fps, cache_reasons, cache_hits = {}, {}, {}, set()
def rec_of(key):
    """→ (package name, the shader's record), found by its key and read from the package each time, not kept (see record_index). Not by the map row's index: a package from another
    shader_split.py run than the map translated every row after a dropped or added one from its neighbour's bytes (issue #1841: Ocarina of Time's present pass ran a terrain pixel
    shader and showed black). Every key is looked up before the first job starts, so a package that does not hold one stops the build here"""
    name = loc[key][0]
    with packs_lock:
        if name not in packs: packs[name] = record_index(os.path.join(dm_dir, name))
    fd, recs = packs[name]
    if key not in recs:
        old = ' (its records carry no keys: it was split before they did)' if recs and set(recs) == {'0' * 16} else ''
        sys.exit(f'[RED] {name} holds no record of shader {key}, which the shader map names: the package and the map come from different shader_split.py runs{old}; '
                 f'split the shaders again (the NSP script does when {dm_dir} is removed)')
    o, e = recs[key]
    return name, os.pread(fd, e - o, o)
for key in keys: digests[key] = shader_key(key, *rec_of(key))
def plain_digest(k):   # a paired pixel shader's key, also when it is not built this time
    if k in digests: return digests[k]
    return shader_key(k, *rec_of(k)) if k in loc else 'missing'
for key in keys:
    if PAIRS.get(key): digests[key] = pair_keyed(digests[key], [plain_digest(p) for p in sorted(PAIRS[key])])
for i, key in enumerate(keys):
    name = loc[key][0]
    rule_fps[key] = hashlib.sha256(shader_rule_source(name) + ','.join(sorted(PAIRS.get(key, ()))).encode()).hexdigest()   # the vertex shader must be rebuilt when its pairs change (vs_pad)
    hit, cache_reasons[key] = cache_lookup(key, digests[key], rule_fps[key], CODE_FP)
    if hit: cache_hits.add(key)
    progress('cache check', i + 1, len(keys), cached=len(cache_hits))
NMISS = sum(1 for k in keys if k not in cache_hits)
xerr = {}
def xlate_errors(r):
    for ln in r.stderr.splitlines():
        m = re.match(r'\[RED\] (\w+): (.*)', ln)
        if m: xerr[m.group(1)] = m.group(2)[:160]
def translate(batch):
    """A batch's misses translated into a working folder of its own (removed once the batch is finished) → that folder"""
    wb = tempfile.mkdtemp(prefix='batch_', dir=w)
    miss = [k for k in batch if k not in cache_hits]
    for k in miss: jobs[k], c1s[k] = job_for(k, loc[k][0], rec_of(k)[1], wb)
    def xl(ch):
        jp = os.path.join(wb, f'job_{abs(hash(ch[0]))}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
        return subprocess.run([XLATE, jp, wb], capture_output=True, text=True)
    with cf.ThreadPoolExecutor(NJ) as ex:
        for r in ex.map(xl, [c for c in (miss[i::NJ] for i in range(NJ)) if c]): xlate_errors(r)
    return wb
def restored(key, wb):
    """A cache hit's outputs back on disk while it is finished. When another build pruned them since the look (cache_lookup), the shader is translated now, in the batch's folder: False"""
    if restore_cache(key, digests[key]): return True
    PRE_STATS['hit'] -= 1; PRE_STATS['miss'] += 1; PRE_STATS['pruned meanwhile'] += 1
    jobs[key], c1s[key] = job_for(key, loc[key][0], rec_of(key)[1], wb)
    jp = os.path.join(wb, f'job1_{key}.json'); json.dump([jobs[key]], open(jp, 'w'))
    xlate_errors(subprocess.run([XLATE, jp, wb], capture_output=True, text=True))
    return False
DONE = collections.Counter()
def fin(key, wb):
    try:
        if key in cache_hits and restored(key, wb):
            refl = json.loads(output_of('mac', key + '_b.json'))
            src = output_of('mac', key + '_b.metal').decode() if refl['stage'] in ('ps', 'vs') else ''
            if refl['stage'] == 'vs': write_refl(key, pull_finish(key, src, vin_finish(key, src, refl)))
            if refl['stage'] == 'ps': refl.update(ps_libraries(key, os.path.join(out, key + '_b.metal'), src, refl['bindings'])); write_refl(key, refl)   # stored without them (publish_cache)
            return key, 'ok', '', refl
        if key in xerr or not os.path.exists(os.path.join(wb, key + '.spv')): return key, 'translation failed', xerr.get(key, 'no spv'), None
        result = (key,) + finish(key, wb, jobs[key], c1s[key])
        if result[1] == 'ok': publish_cache(key, digests[key], rule_fps[key], CODE_FP)
        return result
    finally:
        commit(key)
        with os.scandir(wb) as it:   # its working files (<key>.bin / .spv / .res.json / .metal, the geometry variants' <key>_g<n>.*, job1_<key>.json)
            for e in it:
                if e.name.startswith((key, 'job1_' + key)) and e.is_file(): os.unlink(e.path)
        with _prog['lock']: DONE['n'] += 1; n = DONE['n']
        progress('compiling', n, len(keys), cached=len(cache_hits), misses=NMISS)
# Two phases: paired vertex shaders read their pixel shaders' MSL (vs_pad), so they go last, in batches of their own. The next batch is translated while this one is finished
batches = [b[i:i + BATCH] for b in ([k for k in keys if k not in PAIRS], [k for k in keys if k in PAIRS]) for i in range(0, len(b), BATCH)]
res = []
with cf.ThreadPoolExecutor(1) as tx, cf.ThreadPoolExecutor(NJ) as ex:
    nxt = tx.submit(translate, batches[0]) if batches else None
    for i, batch in enumerate(batches):
        assert nxt is not None
        wb = nxt.result()
        nxt = tx.submit(translate, batches[i + 1]) if i + 1 < len(batches) else None
        res += list(ex.map(lambda k, wb=wb: fin(k, wb), batch))
        shutil.rmtree(wb, ignore_errors=True)
# Passthrough geometry as mesh shaders (dmsh_mesh.py): <vertex key>_<geometry key>_m3 for each pair in the profile's geo_mesh.tsv whose two libraries exist; the host
# draws a lean triangle-list geometry draw with it when its pair has one, and through the compute path otherwise (nvn_geometry.inc geo_mesh_draw)
MESH_STATS = collections.Counter()
def mesh_finish(pair):
    vk, gk = pair
    vs, gs, gj = (output('mac', n) for n in (vk + '_b.metal', gk + '_bl3.metal', gk + '_bl3.json'))   # staged by now
    name = f'{vk}_{gk}_m3.metallib'
    libs = [os.path.join(out, name)] + ([os.path.join(IOSOUT, name)] if IOSOUT else [])
    if vs is None or gs is None or gj is None: MESH_STATS['missing a library'] += 1; return
    try: msl = dmsh_mesh.compose(vs.decode(), gs.decode(), json.loads(gj)['geometry']['capture_reads'])
    except ValueError as e: MESH_STATS['not composed'] += 1; print(f'[RED] mesh {vk} {gk}: {e}', flush=True); return
    path = os.path.join(w, f'{vk}_{gk}_m3.metal'); open(path, 'w').write(msl)
    try:
        if 'mac' in TARGETS: MTL.compile('mac', path, libs[0])
        if IOSOUT: MTL.compile('ios', path, libs[1])
        MESH_STATS['built'] += 1
    except subprocess.CalledProcessError as e: MESH_STATS['failed to compile'] += 1; print(f'[RED] mesh {vk} {gk}: does not compile ({str(e)[:120]})', flush=True)
    finally:
        try: os.unlink(path)
        except FileNotFoundError: pass
        commit(names=[name])
_mesh = GAME.table('geo_mesh.tsv')
if _mesh.exists():
    with cf.ThreadPoolExecutor(NJ) as ex: list(ex.map(mesh_finish, [tuple(ln.split()[:2]) for ln in _mesh.read_text().splitlines() if ln.strip() and not ln.startswith('#')]))
shutil.rmtree(w, ignore_errors=True)
st = collections.Counter(r[1] for r in res)
print(f'shaders {len(res)}: ok {st["ok"]}, ' + ', '.join(f'{k} {v}' for k, v in st.items() if k != 'ok'))
print(f'shader cache: hit {PRE_STATS["hit"]}, rebuilt {PRE_STATS["miss"]}')
for reason in ('new key', 'rule changed', 'code fingerprint changed: all rebuilt'):
    if PRE_STATS[reason]: print(f'  {reason} {PRE_STATS[reason]}')
print(f'readable-tile versions (<key>_bf, dmsh_fetch.py): {BF_STATS["generated"]} generated, {BF_STATS["failed"]} failed to compile')
print(f'shadow-input libraries (<key>_bv, dmsh_vin.py): {VIN_STATS["generated"]} generated, {VIN_STATS["failed"]} failed to compile, {VIN_STATS["not generated"]} not generated; '
      f'{VIN_STATS["inputs"]} integer inputs shadowed, {VIN_STATS["float only"]} of them with no other-sign shadow (attribute indices ran out)')
print(f'alpha test and logic operation libraries (<key>_bx, dmsh_side.py): {SIDE_STATS["generated"]} generated, {SIDE_STATS["failed"]} failed to compile, {SIDE_STATS["not generated"]} pixel shaders without either')
print(f'pulling libraries (<key>_bp, dmsh_pull.py): {PULL_STATS["generated"]} generated, {PULL_STATS["failed"]} failed to compile, {PULL_STATS["not generated"]} not generated, '
      f'{PULL_STATS["no stage input"]} vertex shaders without stage inputs (none needed)')
print('mesh libraries (<vertex>_<geometry>_m3, dmsh_mesh.py): ' + (', '.join(f'{k} {v}' for k, v in MESH_STATS.items()) or 'none'))
print(MTL.summary())
for lvl in [k for k in st if k != 'ok']:
    for e, n in collections.Counter(r[2] for r in res if r[1] == lvl).most_common(10): print(f'  [{lvl}] {n:5d}  {e}')
ok = [r[3] for r in res if r[1] == 'ok']
rd = [x for x in ok if x['drv_reads']]
print(f'shaders reading driver segments at run time {len(rd)} (c0 {sum("0" in x["drv_reads"] for x in rd)}, c2 {sum("2" in x["drv_reads"] for x in rd)}); reading what the replay layer cannot fill {sum(bool(x["drv_unfillable"]) for x in ok)}')
for e, n in collections.Counter(u for x in ok for u in x['drv_unfillable']).most_common(15): print(f'  cannot fill {n:5d}  {e}')
f1 = [b for x in ok for b in x['bindings'] if b.get('fetch1d')]
print(f'1D integer fetches read as 2D rows of 16384 texels (a texture buffer there is bound as rows, nvn_mtl.m tbuf_as_2d): {len(f1)} bindings in {sum(any(b.get("fetch1d") for b in x["bindings"]) for x in ok)} shaders, '
      f'{sum(1 for b in f1 if b.get("fetch1d_size"))} of them also size-queried')
SLOTK = {abi.KIND_CBUF, abi.KIND_SAMP, abi.KIND_SAMP_TEX, abi.KIND_TEX, abi.KIND_IMG, abi.KIND_TBUF, abi.KIND_SBUF, abi.KIND_UBO_PTR, abi.KIND_SBUF_UBO, abi.KIND_TEX_UBO, abi.KIND_SAMP_UBO, abi.KIND_SAMP_ID, abi.KIND_IMG_UBO, abi.KIND_TBUF_UBO}
noreg = [x for x in ok if any(b['kind'] in SLOTK and b.get('reg') is None for b in x['bindings'])]
unk = [x for x in ok if any(b['kind'] == abi.KIND_UNKNOWN for b in x['bindings'])]
print(f'shaders with a binding whose slot was not decoded {len(noreg)}, with an unrecognized binding kind {len(unk)} (loading them at run time prints [RED] lines)')
for e, n in collections.Counter(f"{b['kind']} {b['msl_name'][:28]}" for x in noreg + unk for b in x['bindings'] if b['kind'] == abi.KIND_UNKNOWN or (b['kind'] in SLOTK and b.get('reg') is None)).most_common(8): print(f'  {n:5d}  {e}')
nt = collections.Counter(nn.split(' (')[0] for x in ok for nn in x['notes'] if not nn.startswith('translator: '))
for e, n in nt.most_common(8): print(f'  queries without an answer {n:5d}  {e}')
tm = collections.Counter(nn[len('translator: '):] for x in ok for nn in x['notes'] if nn.startswith('translator: '))   # the translator's own messages (dmsh_xlate Log): bindless reads resolved or removed, invalid encodings
for e, n in tm.most_common(12): print(f'  translator {n:6d} in {sum(e in [nn[len("translator: "):] for nn in x["notes"]] for x in ok):5d} shaders  {e}')
shader_regressions.report_losses(ok)
json.dump([{'key': r[0], 'st': r[1], 'err': r[2]} for r in res], open(os.path.join(out, '_b_build.json'), 'w'), ensure_ascii=False)
if not want:   # a full build: the stamp, then the c1 guard and uniform read extent tables from these Mac outputs, into the profile folder (the host takes them only beside this stamp)
    # The per-shader cache entries this build used (cas/shader/<digest>), which the NSP script keeps when it prunes the others (prune_shader_cache). Only beside the Mac outputs:
    # every full build writes them, and the iOS folder goes whole into the IPA. Before the stamp, so a stamped folder always has its list
    with open(os.path.join(out, CAS_USED + '.tmp'), 'w') as _f: _f.write(''.join(d + '\n' for d in sorted(set(digests.values()))))
    os.replace(os.path.join(out, CAS_USED + '.tmp'), os.path.join(out, CAS_USED))
    for _d in [out] + ([IOSOUT] if IOSOUT else []): dmsh_chain.write_stamp(_d, GAME.id, INPUTS)
    print(f'library stamp: game {GAME.id}, inputs {INPUTS}', flush=True)
# The libraries' images, from what this build staged (and for a partial build the images it builds on): the folders go, and what reads a library from here on reads the image
for _label in ('mac', 'ios'):
    if label_dir(_label): publish_library(_label)
if PROGRESS:
    try: os.unlink(PROGRESS)
    except FileNotFoundError: pass
if not want:
    for _tool, _table in (('c1_guard.py', 'c1_full.tsv'), ('cb_extent.py', 'cb_extent.tsv')):
        _r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), _tool), out, str(GAME.table(_table))], capture_output=True, text=True)
        if _r.returncode: sys.exit(f'[RED] {_tool} failed ({_r.returncode}): {_r.stderr.strip()[-500:]}')
        print(f'{_table}: {(_r.stdout.strip().splitlines() or [""])[-1]}', flush=True)
elif STAMP_BEFORE != INPUTS: print(f'no library stamp: the profile\'s inputs changed since the last full build ({STAMP_BEFORE} → {INPUTS}), and a partial build does not rebuild the rest', flush=True)
