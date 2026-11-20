"""dmsh_reflect.py pool_arrays and reflect on synthetic MSL: Ryujinx's arrays for a handle traced to no slot, sized to the whole texture / sampler ID ranges (Program.cs), become pointers to
the pools the replay keeps (nvn_mtl.m pool_gpu), declared with one element and read through a pointer to it with the run-time bound; the argument IDs are renumbered as the host lays the buffer out; 32-entry slot arrays stay; members with
attributes after their id (raster_order_group) are read; a member or a use the rewrite cannot read stops the shader instead of leaving the host's argument buffer longer than the reflection."""
from pathlib import Path
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402
import dmsh_reflect   # noqa: E402

NS = vars(dmsh_reflect)
MSL = '''struct spvDescriptorSetBuffer0
{
    constant _10* support_buffer [[id(0)]];
    constant _16* fp_c5 [[id(1)]];
    array<texture2d<float>, 1048576> fp_t2d_cb0_40000000 [[id(2)]];
    array<sampler, 4096> fp_s_cb0_40000000 [[id(1048578)]];
    array<texture2d<float>, 32> fp_t2d_cb14_0 [[id(1052674)]];
    texture2d<uint> fp_i_cb2_48_rg32ui [[id(1052706), raster_order_group(0)]];
};

fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    float4 a = spvDescriptorSet0.fp_t2d_cb0_40000000[int(_9[2u])].sample(spvDescriptorSet0.fp_s_cb0_40000000[int(_7)], uv);
}
'''


class PoolArrays(unittest.TestCase):
    def test_rewrite(self):
        out = NS['pool_arrays'](MSL)
        self.assertIn('struct spvPool_fp_t2d_cb0_40000000\n{\n    uint n;\n    ulong seen;\n    array<texture2d<float>, 1> e;\n};', out)
        self.assertIn('struct spvPool_fp_s_cb0_40000000\n{\n    uint n;\n    ulong seen;\n    array<sampler, 1> e;\n};', out)
        self.assertLess(out.index('struct spvPool_'), out.index('struct spvDescriptorSetBuffer0'))
        ids = re.findall(r'(\w+) \[\[id\((\d+)\)', out)
        self.assertEqual(ids, [('support_buffer', '0'), ('fp_c5', '1'), ('fp_t2d_cb0_40000000', '2'), ('fp_s_cb0_40000000', '3'), ('fp_t2d_cb14_0', '4'), ('fp_i_cb2_48_rg32ui', '36')])
        self.assertIn('constant spvPool_fp_t2d_cb0_40000000* fp_t2d_cb0_40000000 [[id(2)]];', out)
        self.assertIn('fp_i_cb2_48_rg32ui [[id(36), raster_order_group(0)]];', out)
        self.assertIn('(&spvDescriptorSet0.fp_t2d_cb0_40000000->e[0])[dm_pool_mark(spvDescriptorSet0.fp_t2d_cb0_40000000, min(uint(int(_9[2u])), spvDescriptorSet0.fp_t2d_cb0_40000000->n))]', out)
        self.assertLess(out.index('dm_pool_mark(constant P* p'), out.index('struct spvPool_'))
        self.assertIn('.sample((&spvDescriptorSet0.fp_s_cb0_40000000->e[0])[min(uint(int(_7)), spvDescriptorSet0.fp_s_cb0_40000000->n)], uv)', out)

    def test_mark_tests_with_a_plain_load(self):
        # an atomic load goes to the shared cache every time, and a pass's pixels queue on the few words its IDs share (frame-rate.md); only a clear bit takes the atomic
        mark = NS['POOL_MARK']
        self.assertNotIn('atomic_load', mark)
        self.assertIn('if (!(*w & b)) atomic_fetch_or_explicit(', mark)

    def test_mark_keeps_depth_test_early(self):
        pool = 'struct spvDescriptorSetBuffer0\n{\n    array<texture2d<float>, 1048576> fp_t_cb0_40000000 [[id(0)]];\n};\n'
        use = 'fragment main0_out main0(constant spvDescriptorSetBuffer0& d [[buffer(0)]])\n{\n    float4 c = d.fp_t_cb0_40000000[i].sample(s, u);%s\n}\n'
        out = NS['pool_arrays'](pool + use % '')
        self.assertIn('[[early_fragment_tests]] fragment main0_out main0(', out)   # the mark is its only store
        for keeps_late in (' discard_fragment();', ' out.gl_FragDepth [[depth(any)]];', ' d.img.write(c, p);', ' atomic_fetch_add_explicit(q, 1u, memory_order_relaxed);', '\n    device uint* q [[id(9)]];'):
            self.assertNotIn('early_fragment_tests', NS['pool_arrays'](pool + use % keeps_late))
        twice = NS['pool_arrays'](pool + '[[early_fragment_tests]] ' + use % '')
        self.assertEqual(twice.count('early_fragment_tests'), 1)

    def test_slot_arrays_stay(self):
        src = MSL.replace('1048576', '32').replace('4096', '32')
        self.assertEqual(NS['pool_arrays'](src), src)

    def test_uncovered_use(self):
        with self.assertRaises(ValueError): NS['pool_arrays'](MSL.replace('    float4 a', '    f(spvDescriptorSet0.fp_t2d_cb0_40000000);\n    float4 a'))


class Reflect(unittest.TestCase):
    def test_pools_and_attributes(self):
        b = {e['msl_name']: e for e in NS['reflect']('k', 'ps', NS['pool_arrays'](MSL), {'sbufs': [], 'cbufs': [{'slot': 5, 'fixed': 64}]})}
        self.assertEqual((b['fp_c5']['kind'], b['fp_c5']['reg'], b['fp_c5']['fixed']), (abi.KIND_CBUF, 2, 64))   # the translator's count of the slot's bytes read at fixed offsets
        self.assertEqual((b['fp_t2d_cb0_40000000']['kind'], b['fp_t2d_cb0_40000000']['elem'], b['fp_t2d_cb0_40000000']['arg_id']), (abi.KIND_TEX_POOL, 'texture2d<float>', 2))
        self.assertEqual((b['fp_s_cb0_40000000']['kind'], b['fp_s_cb0_40000000']['arg_id']), (abi.KIND_SAMP_POOL, 3))
        self.assertEqual((b['fp_i_cb2_48_rg32ui']['kind'], b['fp_i_cb2_48_rg32ui']['reg'], b['fp_i_cb2_48_rg32ui']['arg_id']), (abi.KIND_IMG, 0, 36))

    def test_table_of_uniform_handles(self):   # Kirby Air Riders' light probes: texture words in c[4] at element 390, the sampler word at c2 byte 0x570; the index stays the element's
        src = '''struct spvDescriptorSetBuffer0
{
    array<texture2d_array<float>, 8192> cp_t2da_cb196612_10570000 [[id(0)]];
    array<sampler, 8192> cp_t2da_cb196612_10570000Smplr [[id(8192)]];
    array<texture2d<float>, 32> fp_t2d_cb14_0 [[id(16384)]];
};

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    float3 v = spvDescriptorSet0.cp_t2da_cb196612_10570000[_7].sample(spvDescriptorSet0.cp_t2da_cb196612_10570000Smplr[_7], p, 0u, level(0.0)).xyz;
}
'''
        out = NS['pool_arrays'](src)
        self.assertIn('(&spvDescriptorSet0.cp_t2da_cb196612_10570000->e[0])[min(uint(_7), spvDescriptorSet0.cp_t2da_cb196612_10570000->n)]', out)   # a table's index is its element, not a texture ID: not marked
        self.assertNotIn('dm_pool_mark', out)
        self.assertNotIn('early_fragment_tests', NS['pool_arrays'](src.replace('kernel void', 'fragment main0_out')))   # nothing marked, nothing stored: the test stays where Metal puts it
        self.assertIn('(&spvDescriptorSet0.cp_t2da_cb196612_10570000Smplr->e[0])[min(uint(_7), spvDescriptorSet0.cp_t2da_cb196612_10570000Smplr->n)]', out)   # a sampler read does not
        self.assertIn('array<texture2d<float>, 32> fp_t2d_cb14_0 [[id(2)]];', out)   # a pointer takes one ID; an array of 32 stays an array (nothing in the game's buffers is that short now but images and buffer textures)
        b = {e['msl_name']: e for e in NS['reflect']('k', 'cs', out, {'sbufs': [], 'cbufs': []})}
        t, s = b['cp_t2da_cb196612_10570000'], b['cp_t2da_cb196612_10570000Smplr']
        self.assertEqual((t['kind'], t['elem'], t['reg'], t['off'], t['arg_id'], t.get('soff')), (abi.KIND_TEX_HTAB, 'texture2d_array<float>', 1, 0, 0, 0x570))
        self.assertEqual((s['kind'], s['reg'], s['off'], s['arg_id'], s['soff'], s['sreg']), (abi.KIND_SAMP_HTAB, 1, 0, 1, 0x570, abi.SREG_C2))

    def test_unreadable_member(self):
        with self.assertRaises(ValueError): NS['reflect']('k', 'ps', MSL.replace('raster_order_group(0)', 'raster_order_group'), {'sbufs': [], 'cbufs': []})


class CallablePools(unittest.TestCase):
    TEXTURE = 'fp_t2d_cb0_40000000'
    SAMPLER = 'fp_s_cb0_40000000'

    def source(self):
        header = MSL[:MSL.index('fragment ')]
        parameters = f'constant array<texture2d<float>, 1048576>& {self.TEXTURE}, constant array<sampler, 4096>& {self.SAMPLER}'
        return header + f'''float4 leaf(uint index, {parameters})
{{
    return {self.TEXTURE}[int(index + offsets[2])].sample({self.SAMPLER}[index], uv);
}}
float4 relay(uint index, {parameters})
{{
    return leaf(index, {self.TEXTURE}, {self.SAMPLER});
}}
fragment main0_out main0(constant spvDescriptorSetBuffer0& d [[buffer(0)]])
{{
    float4 value = relay(0u, d.{self.TEXTURE}, d.{self.SAMPLER});
}}
'''

    def test_parameters_reads_and_nested_forwarding(self):
        out = dmsh_reflect.pool_arrays(self.source())
        self.assertEqual(out.count(f'constant spvPool_{self.TEXTURE}* {self.TEXTURE}'), 3)
        self.assertEqual(out.count(f'constant spvPool_{self.SAMPLER}* {self.SAMPLER}'), 3)
        self.assertIn(f'(&{self.TEXTURE}->e[0])[dm_pool_mark({self.TEXTURE}, min(uint(int(index + offsets[2])), {self.TEXTURE}->n))]', out)
        self.assertIn(f'(&{self.SAMPLER}->e[0])[min(uint(index), {self.SAMPLER}->n)]', out)
        self.assertIn(f'return leaf(index, {self.TEXTURE}, {self.SAMPLER});', out)
        self.assertIn(f'relay(0u, d.{self.TEXTURE}, d.{self.SAMPLER});', out)
        self.assertNotIn('constant array<texture2d<float>, 1048576>&', out)
        self.assertIn(f'constant spvPool_{self.SAMPLER}* {self.SAMPLER} [[id(3)]];', out)

    def test_uniform_handle_table_keeps_unmarked_indices(self):
        source = self.source().replace(self.TEXTURE, 'cp_t2da_cb196612_10570000').replace(self.SAMPLER, 'cp_t2da_cb196612_10570000Smplr')
        source = source.replace('texture2d<float>', 'texture2d_array<float>').replace('1048576', '8192').replace('4096', '8192')
        out = dmsh_reflect.pool_arrays(source)
        self.assertNotIn('dm_pool_mark', out)
        self.assertIn('min(uint(index), cp_t2da_cb196612_10570000Smplr->n)', out)

    def test_unsupported_parameter_forms(self):
        source = self.source()
        for old, new in [
            ('constant array<texture2d<float>, 1048576>&', 'constant array<texture2d<float>, 8192>&'),
            ('constant array<texture2d<float>, 1048576>&', 'constant array<texture2d<uint>, 1048576>&'),
            ('constant array<texture2d<float>, 1048576>&', 'thread const array<texture2d<float>, 1048576>&'),
        ]:
            with self.subTest(new=new), self.assertRaises(ValueError):
                dmsh_reflect.pool_arrays(source.replace(old, new))

    def test_alias_unknown_callee_and_wrong_forwarding(self):
        source = self.source()
        bad = [
            source.replace('return leaf(index,', f'auto alias = {self.TEXTURE}; return leaf(index,'),
            source.replace('return leaf(index,', 'return unknown(index,'),
            source.replace(f'return leaf(index, {self.TEXTURE}, {self.SAMPLER})', f'return leaf(index, {self.SAMPLER}, {self.TEXTURE})'),
            source.replace(f'return leaf(index, {self.TEXTURE},', f'return leaf(index, reinterpret_cast<constant uint*>({self.TEXTURE}),'),
            source.replace(f'return {self.TEXTURE}[', f'return (&{self.TEXTURE}[').replace('].sample(', '])->sample('),
        ]
        for index, text in enumerate(bad):
            with self.subTest(index=index), self.assertRaises(ValueError): dmsh_reflect.pool_arrays(text)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1', 'set DMSH_TEST_GPU=1 for Metal execution')
    def test_resource_id_pools_through_nested_calls_on_gpu(self):
        if platform.system() != 'Darwin' or not shutil.which('clang'):
            self.skipTest('needs macOS and clang')
        texture, sampler = self.TEXTURE, self.SAMPLER
        parameters = f'constant array<texture2d<float>, 1048576>& {texture}, constant array<sampler, 4096>& {sampler}'
        source = f'''#include <metal_stdlib>
using namespace metal;
struct spvDescriptorSetBuffer0
{{
    device float4* output [[id(0)]];
    array<texture2d<float>, 1048576> {texture} [[id(1)]];
    array<sampler, 4096> {sampler} [[id(1048577)]];
}};
float4 leaf(uint index, {parameters})
{{
    return {texture}[index].sample({sampler}[0], float2(0.5), level(0.0));
}}
float4 relay(uint index, {parameters})
{{
    return leaf(index, {texture}, {sampler});
}}
kernel void main0(constant spvDescriptorSetBuffer0& d [[buffer(0)]], uint index [[thread_position_in_grid]])
{{
    d.output[index] = relay(index, d.{texture}, d.{sampler});
}}
'''
        with tempfile.TemporaryDirectory(prefix='dm-callable-pools-') as temporary:
            work = Path(temporary)
            shader, executable = work / 'fixture.metal', work / 'fixture'
            shader.write_text(dmsh_reflect.pool_arrays(source))
            fixture = Path(__file__).parent / 'fixtures/pool_callables.m'
            compile = subprocess.run(['clang', '-O1', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', str(fixture),
                                      '-framework', 'Foundation', '-framework', 'Metal', '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(compile.returncode, 0, compile.stdout + compile.stderr)
            result = subprocess.run([str(executable), str(shader)], capture_output=True, text=True,
                                    env=dict(os.environ, MTL_DEBUG_LAYER='1'))
            if result.returncode == 77: self.skipTest('needs tier2 Metal argument buffers')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('nested forwarding, read bitmap and guards passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
