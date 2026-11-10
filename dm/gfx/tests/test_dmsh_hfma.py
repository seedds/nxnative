"""HFMA2 on Metal (dmsh_build.py hfma_msl): the rewrite of the `fma` on unpacked halves that ends in a pack to half, and on this Mac's GPU the rewritten kernel against hfma_orin_fixture.u32, the output
of PTX fma.rn.f16x2 on a Jetson Orin (sm_87) for 4570 (a, b, c) half pairs (the 70 of 1,048,576 random triples where the float fma + pack of the old translation differs from the Orin, 3000 other random ones and
1500 of the 24,389 edge triples: denormals, Inf, NaN, powers of two). The old kernel rounds twice (float, then half) and is one half ULP off on the 70 the fixture
keeps; the rewritten one is the Orin's bits on all of them, with the chain's flags and with fast, safe and relaxed math spelled out"""
import array
import ast
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
FIXTURE = Path(__file__).resolve().parent / 'hfma_orin_fixture.u32'
NAMES = {'hfma_msl', 'HFMA_HELPERS', 'HFMA_DEF', 'HFMA_WRAP'}
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in NAMES)
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in NAMES for t in n.targets))]
scope = {'re': re, 'struct': struct}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
hfma_msl = scope['hfma_msl']

HEAD = '#include <metal_stdlib>\nusing namespace metal;\n\n'
# HFMA2 as spirv-cross writes it (ids made up): the three sources unpacked, one fma a lane, the pack. The second kernel adds .SAT (precise::clamp) and a negated source, the third an FFMA whose result is packed
KERNEL = '''kernel void check(device const uint *ia [[buffer(0)]], device const uint *ib [[buffer(1)]], device const uint *ic [[buffer(2)]], device uint *out [[buffer(3)]], uint gid [[thread_position_in_grid]])
{
    uint _1 = ia[gid];
    uint _2 = ib[gid];
    uint _3 = ic[gid];
    float _10 = float2(as_type<half2>(_1)).x;
    float _11 = float2(as_type<half2>(_1)).y;
    float _12 = float2(as_type<half2>(_2)).x;
    float _13 = float2(as_type<half2>(_2)).y;
    float _14 = float2(as_type<half2>(_3)).x;
    float _15 = float2(as_type<half2>(_3)).y;
    float _16 = fma(_10, _12, _14);
    float _17 = fma(_11, _13, _15);
    uint _18 = as_type<uint>(half2(float2(_16, _17)));
    out[gid] = _18;
}
'''
OTHER = '''kernel void check(device const uint *ia [[buffer(0)]], device const float *fb [[buffer(1)]], device uint *out [[buffer(3)]], uint gid [[thread_position_in_grid]])
{
    uint _1 = ia[gid];
    float _10 = float2(as_type<half2>(_1)).x;
    float _11 = float2(as_type<half2>(_1)).y;
    float _12 = fb[gid];
    float _13 = -_10;
    float _14 = fma(_10, _12, _11);
    float _15 = fma(_13, 0.5, _11);
    float _16 = fma(_10, _11, _10);
    float _17 = precise::clamp(_15, 0.0, 1.0);
    float _18 = fma(_11, _10, _10);
    float _19 = max(_18, 0.0);
    float _20 = fma(_11, 1.0009765625, _10);
    float _21 = fma(_11, 0.1, _10);
    float _22 = fma(_10, _10, _11);
    uint _30 = as_type<uint>(half2(float2(_14, _17)));
    uint _31 = as_type<uint>(half2(float2(_16, _19)));
    uint _32 = as_type<uint>(half2(float2(_20, _21)));
    out[gid] = _30 + _31 + _32 + as_type<uint>(_22);
}
'''
RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 2; }
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        id<MTLComputePipelineState> ps = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&e] : nil;
        if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
        NSData *d = [NSData dataWithContentsOfFile:@(argv[2])]; NSUInteger n = ((const uint32_t *)d.bytes)[0];
        const uint32_t *w = (const uint32_t *)d.bytes + 1;
        id<MTLBuffer> in[3]; for (int k = 0; k < 3; k++) in[k] = [dev newBufferWithBytes:w + k * n length:n * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> out = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
        [c setComputePipelineState:ps]; for (int k = 0; k < 3; k++) [c setBuffer:in[k] offset:0 atIndex:k]; [c setBuffer:out offset:0 atIndex:3];
        [c dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [c endEncoding]; [cb commit]; [cb waitUntilCompleted];
        [[NSData dataWithBytes:out.contents length:n * 4] writeToFile:@(argv[3]) atomically:YES];
    }
    return 0;
}
'''
MODES = ['', '-fmetal-math-mode=fast', '-fmetal-math-mode=safe', '-fmetal-math-mode=relaxed']


def fixture():
    w = array.array('I'); w.frombytes(FIXTURE.read_bytes())
    n = w[0]
    return n, list(w[1:1 + n]), list(w[1 + n:1 + 2 * n]), list(w[1 + 2 * n:1 + 3 * n]), list(w[1 + 3 * n:1 + 4 * n])


def gpu(test, msl, mode):   # the kernel's output word for each fixture triple
    with tempfile.TemporaryDirectory() as t:
        d = Path(t)
        (d / 'h.metal').write_text(msl)
        (d / 'run.m').write_text(RUNNER)
        (d / 'in.bin').write_bytes(FIXTURE.read_bytes())
        flags = ['-O1', '-fpreserve-invariance'] + ([mode] if mode else [])
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', *flags, str(d / 'h.metal'), '-o', str(d / 'h.air')], check=True, capture_output=True)
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 'h.air'), '-o', str(d / 'h.metallib')], check=True)
        subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)
        r = subprocess.run([str(d / 'run'), str(d / 'h.metallib'), str(d / 'in.bin'), str(d / 'out.bin')], capture_output=True, text=True)
        if r.returncode == 2: test.skipTest(r.stdout.strip())
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        o = array.array('I'); o.frombytes((d / 'out.bin').read_bytes())
        return list(o)


def is_nan16(h): return (h >> 10) & 31 == 31 and h & 1023 != 0
def same_lane(x, y): return x == y or (is_nan16(x) and is_nan16(y))
def lanes_equal(got, orin):   # lanes that differ from the Orin (any NaN equals any NaN)
    return sum(not same_lane(g >> s & 0xFFFF, o >> s & 0xFFFF) for g, o in zip(got, orin) for s in (0, 16))


class HfmaRewrite(unittest.TestCase):
    def test_fma_of_unpacked_halves_that_is_packed(self):
        out = hfma_msl(HEAD + KERNEL)
        self.assertIn('float _16 = dmsh_hfma(_10, _12, _14);', out)
        self.assertIn('float _17 = dmsh_hfma(_11, _13, _15);', out)

    def test_helper_once_after_the_using(self):
        out = hfma_msl(HEAD + KERNEL)
        self.assertEqual(out.count('static inline float dmsh_hfma('), 1)
        self.assertLess(out.index('using namespace metal;'), out.index('static inline float dmsh_hfma('))
        self.assertLess(out.index('dmsh_hfma(float'), out.index('kernel void check'))
        self.assertIn('math_mode(safe)', out)

    def test_saturate_negation_and_literals_count(self):
        out = hfma_msl(HEAD + OTHER)
        self.assertIn('float _15 = dmsh_hfma(_13, 0.5, _11);', out)   # negated half, literal, then precise::clamp, then the pack
        self.assertIn('float _18 = dmsh_hfma(_11, _10, _10);', out)   # max(x, 0) (.RELU) then the pack
        self.assertIn('float _20 = dmsh_hfma(_11, 1.0009765625, _10);', out)   # 1 + 2^-10 is a half

    def test_relu_that_keeps_a_nan(self):
        # .RELU as it keeps a NaN result (the Orin gives the NaN): isnan(v) ? v : max(v, 0.0), then the pack
        relu = HEAD + '''kernel void check(device const uint *ia [[buffer(0)]], device uint *out [[buffer(3)]], uint gid [[thread_position_in_grid]])
{
    uint _1 = ia[gid];
    float _10 = float2(as_type<half2>(_1)).x;
    float _11 = float2(as_type<half2>(_1)).y;
    float _63 = fma(_10, _11, _10);
    bool _69 = isnan(_63);
    float _71 = fast::max(_63, 0.0);
    float _73 = _69 ? _63 : _71;
    uint _81 = as_type<uint>(half2(float2(_73, _10)));
    out[gid] = _81;
}
'''
        self.assertIn('float _63 = dmsh_hfma(_10, _11, _10);', hfma_msl(relu))

    def test_pragmas_survive_a_line_at_the_start_of_the_body(self):
        # a rewrite that puts a line at the start of every function (loop_guard) must leave the helper's pragmas at the start of a compound statement
        out = hfma_msl(HEAD + KERNEL)
        body = out[out.index('static inline float dmsh_hfma'):]
        body = body.replace('{\n', '{\n    uint dm_lg = 0u; (void)dm_lg;\n', 1)
        self.assertRegex(body, r'\{[^\n]*\n#pragma METAL fp math_mode\(safe\)')

    def test_what_is_not_a_half_fma_stays(self):
        out = hfma_msl(HEAD + OTHER)
        self.assertIn('float _14 = fma(_10, _12, _11);', out)   # a float read from memory is not a half
        self.assertIn('float _16 = dmsh_hfma(_10, _11, _10);', out)
        self.assertIn('float _21 = fma(_11, 0.1, _10);', out)   # 0.1 is not a half
        self.assertIn('float _22 = fma(_10, _10, _11);', out)   # kept as a float (HFMA2.F32): not packed

    def test_other_shaders_unchanged(self):
        plain = HEAD + 'kernel void k() { float _1 = 0.0; float _2 = fma(_1, _1, _1); }\n'
        self.assertEqual(hfma_msl(plain), plain)

    def test_no_using_namespace_refused(self):
        with self.assertRaises(ValueError):
            hfma_msl(KERNEL)


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang') and FIXTURE.exists(), 'needs Xcode and the Orin fixture')
class HfmaOnGpu(unittest.TestCase):
    def test_matches_the_orin_in_every_math_mode(self):
        n, a, b, c, orin = fixture()
        msl = hfma_msl(HEAD + KERNEL)
        for mode in MODES:
            with self.subTest(mode=mode or 'default'):
                self.assertEqual(lanes_equal(gpu(self, msl, mode), orin), 0, 'lanes that differ from the Orin [%s]' % mode)

    def test_without_the_rewrite_the_same_lanes_fail(self):
        n, a, b, c, orin = fixture()
        wrong = lanes_equal(gpu(self, HEAD + KERNEL, ''), orin)
        self.assertGreaterEqual(wrong, 60, 'the old fma + pack is a half ULP off on the 70 lanes the fixture keeps (%d here)' % wrong)
        self.assertLessEqual(wrong, 80)


if __name__ == '__main__':
    unittest.main()
