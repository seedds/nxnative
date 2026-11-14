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
AMPERE_ORIN_REPORT=<file> writes every op's causes as JSON."""
import ast
import bisect
import concurrent.futures
import gzip
import json
import os
import re
import shutil
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

# Ops that still differ from the GPU beyond PLATFORM: name -> why. A fix takes its ops out of here.
KNOWN = {
    # results that differ
    'fadd_rz': 'FADD .RZ / .RM / .RP: the rounding mode is dropped, Metal rounds to nearest', 'fadd_rm': '(as fadd_rz)', 'fadd_rp': '(as fadd_rz)',
    'fmul_rz': 'FMUL .RZ / .RM / .RP: the rounding mode is dropped', 'fmul_rm': '(as fmul_rz)', 'fmul_rp': '(as fmul_rz)',
    'ffma_rz': 'FFMA .RZ / .RM / .RP: the rounding mode is dropped', 'ffma_rm': '(as ffma_rz)', 'ffma_rp': '(as ffma_rz)',
    'i2f_s32_rz': 'I2FP.F32.S32.RZ rounds to nearest (PR #1800 has the fix)',
    'i2f_f16_rz': 'I2F to F16 with .RZ / .RM / .RP rounds to nearest (PR #1800 has the fix)', 'i2f_f16_rm': '(as i2f_f16_rz)', 'i2f_f16_rp': '(as i2f_f16_rz)',
    'i2f_f16_u32_rz': '(as i2f_f16_rz)', 'f2fp_pack_f16x2_rz': 'F2FP.PACK_AB.RZ rounds to nearest (PR #1800 has the fix)',
    'shfl_up_clamp': 'SHFL.UP ignores a non-zero clamp (PR #1800 has the fix)',
    'fmax_nan': 'FMNMX.NAN: a NaN operand gives the other operand, not NaN (PR #1800 has the fix)', 'fmin_nan': '(as fmax_nan)',
    'fsetp': 'FSETP.NE (ordered) becomes MSL !=, which is unordered: true with a NaN operand', 'fsetp_ftz': '(as fsetp)',
    'shfl_idx': 'SHFL: a lane in a register is not masked to its low 5 bits', 'shfl_bfly': '(as shfl_idx)', 'shfl_idx_seg': '(as shfl_idx)',
    'f2fp_pack_bf16x2': 'F2FP.BF16.PACK_AB packs f16, not bf16', 'f2fp_pack_f16x2_relu': 'F2FP.RELU: negative results are not clamped to 0',
    'i2f_s64': 'I2F.S64 rounds twice (the high and low words apart)',
    'fmax': 'FMNMX: Ampere orders -0 below +0 (max(+0, -0) is +0); Metal returns either', 'hmax2': '(as fmax, HMNMX2)',
    'hadd2_ftz': 'HADD2 / HMUL2 .FTZ: f16 denormals are not flushed (Metal keeps them)', 'hmul2_ftz': '(as hadd2_ftz)',
    'hmma_16816_f32': 'HMMA.16816.F32: the f32 accumulator does not add the products as the tensor core does (about 1 ULP)',
    # the translator refuses these
    'hfma2': 'HFMA2.MMA (0x235) is not implemented', 'hfma2_ftz': '(as hfma2)', 'hfma2_sat': '(as hfma2)', 'hfma2_relu': '(as hfma2)',
    'mufu_rcp': 'HFMA2.MMA with an immediate (0x435) is not implemented', 'f2f_tf32': '(as mufu_rcp)', 'idiv_u32': '(as mufu_rcp)',
    'idiv_u32_by_7': '(as mufu_rcp)', 'irem_s32': '(as mufu_rcp)', 'fdiv_rn': '(as mufu_rcp)', 'f2i_s64': '(as mufu_rcp)', 'iadd64': '(as mufu_rcp)',
    'isub64': '(as mufu_rcp)', 'imul64': '(as mufu_rcp)', 'imul24': '(as mufu_rcp)', 'dadd': '(as mufu_rcp; then DADD)', 'dmul': '(as mufu_rcp; then DMUL)',
    'hmin2_nan': 'HMNMX2.NAN is not implemented', 'bmin2': 'HMNMX2.BF16_V2 is not implemented', 'bmax2_nan': '(as bmin2)',
    'bfma2': 'HFMA2.BF16_V2 is not implemented', 'hmma_16816_bf16': 'HMMA .BF16 is not implemented', 'hmma_1688_tf32': 'HMMA .TF32 is not implemented',
    'imma_16832_s8': 'IMMA is not implemented', 'dp4a_u32': 'IDP.4A is not implemented', 'dp4a_s32': '(as dp4a_u32)', 'dp4a_su': '(as dp4a_u32)',
    'dp2a_lo': 'IDP.2A is not implemented', 'dp2a_hi': '(as dp2a_lo)', 'i2i_s8_sat': 'I2I .SAT is not implemented', 'i2i_u16_sat': '(as i2i_s8_sat)',
    'isad': 'VABSDIFF is not implemented', 'iadd_sat': 'PLOP3 with .SIGN register sources (0x21f) is not implemented', 'isub_sat': '(as iadd_sat)',
    'prmt': 'PRMT modes other than the index form (.F4E .B4E .RC8 .ECL .ECR .RC16) are not implemented',
    'mufu_tanh': 'MUFU.TANH is not implemented', 'mufu_tanh_f16x2': '(as mufu_tanh)',
    # the program calls a routine of the CUDA driver's own (CALL.ABS to a relocation the JIT resolves at load), which a game's shader never does
    'frcp_rn': 'not testable: CALL.ABS into the CUDA driver', 'fsqrt_rn': '(as frcp_rn)',
}
# Ops that run Metal's own approximation (MUFU, and the divisions built on it): compared and reported, never failed
APPROX = {'mufu_ex2', 'mufu_ex2_ftz', 'mufu_lg2', 'mufu_lg2_ftz', 'mufu_rcp_ftz', 'mufu_rsqrt', 'mufu_rsqrt_ftz', 'mufu_sqrt', 'mufu_sqrt_ftz', 'mufu_sin',
          'mufu_cos', 'mufu_ex2_f16x2', 'fdiv_full', 'fdiv_approx',
          # HMMA with an f16 accumulator: one block of 8 rounded once (PR #1897) matches 99.97 %; the rest are 1-2 ULP where the sum nearly cancels
          'hmma_16816_f16', 'hmma_1688_f16'}

MAC_METAL = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', '-fwrapv']   # dmsh_build.py MAC_METAL


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
    return doc


def chain_rewrites():
    """the MSL rewrites dmsh_build.py's finish() makes of every shader, in its order: each `src = f(src)` at its top level or in a try there (the ones under a
    stage's `if` are another stage's). dmsh_build.py runs as a script, so its functions and constants are taken from its source, as test_dmsh_hmma.py does"""
    import dmsh_atomic
    import dmsh_wide
    path = TOOLS / 'dmsh_build.py'
    tree = ast.parse(path.read_text())
    scope = {'re': re, 'bisect': bisect, 'os': os, 'json': json, 'dmsh_atomic': dmsh_atomic, 'dmsh_wide': dmsh_wide}
    for n in tree.body:
        pure = isinstance(n, ast.Assign) and all(isinstance(t, ast.Name) for t in n.targets) and not any(
            isinstance(c, ast.Call) and getattr(c.func, 'attr', '') != 'compile' for c in ast.walk(n.value))
        # the script's imports of standard modules too (struct, math, ...), which a rewrite may use; its own modules are imported above or not needed
        stdlib = isinstance(n, (ast.Import, ast.ImportFrom)) and all(
            (a.name if isinstance(n, ast.Import) else (n.module or '')).split('.')[0] in sys.stdlib_module_names for a in n.names)
        if isinstance(n, ast.FunctionDef) or pure or stdlib:
            try:
                exec(compile(ast.Module(body=[n], type_ignores=[]), str(path), 'exec'), scope)
            except Exception:   # a constant built from what only a build has (the game, the chain version) is not needed here
                pass
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
        fns.append((name, getattr(scope[mod], attr) if mod else scope[name]))
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
// run <metallib> <io> <groups>: main0 over <groups> groups of 32 threads, io at buffer 30 (the other slots a zero buffer), written back over io
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
  [ce dispatchThreadgroups:MTLSizeMake(atoi(argv[3]), 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
  if (cb.error) { fprintf(stderr, "gpu: %s\n", cb.error.description.UTF8String); return 1; }
  [[NSData dataWithBytes:b.contents length:in.length] writeToFile:@(argv[2]) atomically:YES]; return 0; } }
'''


def run_op(o, work, runner, steps, xc, env):
    """('ok', metal rows) or (stage that failed, its message)"""
    import array
    import dmsh_atomic
    import dmsh_geometry
    d = work / o['name']
    d.mkdir()
    (d / 'k.bin').write_bytes(o['program'])
    w = len(o['ins']) + len(o['outs'])
    job = [dict(key='k', stage='cs', arch='sm86', code=str(d / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, reduced=True,
                nvn2_cs=[0, 0, 3, 0x40, 0], shared=32 * w * 4)]
    (d / 'jobs.json').write_text(json.dumps(job))
    r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(d / 'jobs.json'), str(d)], capture_output=True, text=True, env=env)
    if r.returncode or not (d / 'k.spv').exists():
        return 'translator', next((ln for ln in (r.stdout + r.stderr).splitlines() if '[RED]' in ln), (r.stdout + r.stderr).strip()[-300:])
    dmsh_geometry.flatten_sets(d / 'k.spv')
    r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), *dmsh_atomic.cross_args(xc, str(d / 'k.spv')), str(d / 'k.spv')], capture_output=True, text=True)
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
    io = array.array('I', [0xcdcdcdcd]) * (len(o['rows_in']) * w)
    for c, row in enumerate(o['rows_in']):
        io[c * w:c * w + len(row)] = array.array('I', row)
    (d / 'io.bin').write_bytes(io.tobytes())
    r = subprocess.run([str(runner), str(d / 'k.metallib'), str(d / 'io.bin'), str(len(o['rows_in']) // 32)], capture_output=True, text=True, timeout=120)
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
                cls.results[o['name']] = dict(stage='ok', lanes=lanes, causes=dict(causes), worst_ulp=worst, examples=examples)
            else:
                cls.results[o['name']] = dict(stage=stage, message=got)
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
            lines.append(f"  {o['name']:22s} {tag:6s} {diff:5d} of {r['lanes']:6d}" + (f'  ({cs})' if cs else '') + (f"  max {r['worst_ulp']} ULP" if r['worst_ulp'] else ''))
        print('\n'.join(lines), file=sys.stderr)

    def test_matches_the_gpu(self):
        for o in self.ops:
            n, r = o['name'], self.results[o['name']]
            with self.subTest(op=n):
                beyond = {k: v for k, v in r.get('causes', {}).items() if k not in compare.PLATFORM}
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


if __name__ == '__main__':
    unittest.main()
