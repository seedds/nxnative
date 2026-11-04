"""Numerical GPU comparison of original shaders and main's shared pixel compile units."""
import os, pathlib, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_side, dmsh_pch, dmsh_mtlcache

@unittest.skipUnless(os.environ.get('DM_TEST_ONELIB'), 'run make check_t_onelib for GPU numerical comparisons')
class Numerical(unittest.TestCase):
    def test_merged_outputs_match_original(self):
        head = '#include <metal_stdlib>\n#include <simd/simd.h>\nusing namespace metal;\n'
        vertex = head + 'struct main0_out { float4 m_0 [[position]]; float2 m_1 [[user(locn0)]]; };\nvertex main0_out vertex_main(uint i [[vertex_id]]) { return main0_out{float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0.0, 1.0), float2(0.0)}; }\n'
        expressions = [
            'float4(a * b + c, fma(a, b, c), precise::min(a, b), precise::max(a, b))',
            'sin(float4(a,b,c,d)) * cos(float4(b,c,d,a)) + sqrt(abs(float4(c,d,a,b)))',
            'float4(a > b ? a * c : b + d, a < 0.0 ? -a : a, floor(b), fract(c))',
            't.sample(s, p.xy / 32.0) * a + t.read(uint2(uint(p.x) & 15u, uint(p.y) & 15u))',
            'float4(dfdx(p.x * p.y), dfdy(p.x * p.y), a + b * c, (a + b) * c)',
            'float4(precise::clamp(a, -1.0, 1.0), precise::min(b, c), precise::max(c, d), a / b)',
            'float4(as_type<float>(as_type<uint>(a) ^ 0x00100000u), b, c, d)',
            'float4(dot(float3(a,b,c), float3(b,c,d)), length(float3(a,b,c)), a-b, c+d)',
            'uint4(as_type<uint>(a), as_type<uint>(b) ^ as_type<uint>(c), uint(p.x), uint(p.y))',
        ]
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            flags = ['-c', '-O1', '-fpreserve-invariance']
            metal = ['xcrun', '-sdk', 'macosx', 'metal', *flags]
            def cached_compile(msl, air, lib):
                dmsh_pch.compile(metal, msl, air)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', lib], check=True, capture_output=True)
            cache = dmsh_mtlcache.Cache(str(root / 'cache'), {'mac': ('macosx', 'numerical O1 invariance', cached_compile)}, d)
            vsrc = root / 'vertex-input.metal'; vsrc.write_text(vertex)
            cache.compile('mac', str(vsrc), str(root / 'vertex.metallib'))
            for i, expression in enumerate(expressions):
                ty = 'uint4' if i == 8 else 'float4'
                src = head + f'struct main0_out {{ {ty} m_0 [[color(0)]]; }};\n'
                src += 'fragment main0_out main0(float4 p [[position]], constant float* values [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])\n{\n'
                src += '    main0_out out = {};\n    uint j = (uint(p.x) + uint(p.y) * 32u) & 255u;\n'
                src += '    float a = values[j], b = values[(j+1u)&255u], c = values[(j+2u)&255u], d = values[(j+3u)&255u];\n'
                if i == 2: src += '    if (p.x < 4.0) discard_fragment();\n'
                src += f'    out.m_0 = {expression};\n    return out;\n}}\n'
                merged = dmsh_side.unit(src)
                self.assertIsNotNone(merged[0])
                for variant, text in enumerate((src, merged[0], merged[0])):
                    msl = root / f'{i}_{variant}.metal'; msl.write_text(text)
                    air = root / f'{i}_{variant}.air'; lib = root / f'{i}_{variant}.metallib'
                    if variant == 2: cache.compile('mac', str(msl), str(lib))
                    else:
                        subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', *flags, str(msl), '-o', str(air)], check=True, capture_output=True)
                        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(lib)], check=True, capture_output=True)
            result = subprocess.run([os.environ['DM_TEST_ONELIB'], str(root)], capture_output=True, text=True)
            print(result.stdout, end='')
            self.assertEqual(result.returncode, 0, result.stderr)



if __name__ == '__main__':
    unittest.main()
