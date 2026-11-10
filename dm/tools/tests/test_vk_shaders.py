"""Vulkan shader-table tests: HLSL entry points and push constants, dual-source variants, compute sizes, and bindless argument-buffer rows."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings
from types import SimpleNamespace
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import vk_shaders as vs  # noqa: E402


def fragment(entry='main', pc_type=None, ubo=False, outs=('v4float',)):
    """Assembly of a fragment shader with constant outputs, an optional UBO and optional push constants."""
    asm = f'''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "{entry}" {" ".join(f"%out{i}" for i in range(len(outs)))}
OpExecutionMode %main OriginUpperLeft
OpName %main "{entry}"
'''
    if pc_type: asm += f'OpName %PCT "{pc_type}"\nOpMemberName %PCT 0 "tint"\nOpName %pc "pc"\n'
    if ubo: asm += 'OpName %UBT "type.ubo"\nOpMemberName %UBT 0 "scale"\nOpName %ub "ub"\n'
    for i in range(len(outs)): asm += f'OpDecorate %out{i} Location {i}\n'
    if pc_type: asm += 'OpDecorate %PCT Block\nOpMemberDecorate %PCT 0 Offset 0\n'
    if ubo: asm += 'OpDecorate %UBT Block\nOpMemberDecorate %UBT 0 Offset 0\nOpDecorate %ub DescriptorSet 0\nOpDecorate %ub Binding 0\n'
    asm += '''%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%int = OpTypeInt 32 1
%int0 = OpConstant %int 0
%half = OpConstant %float 0.5
%c2 = OpConstantComposite %v2float %half %half
%c4 = OpConstantComposite %v4float %half %half %half %half
%o_v2 = OpTypePointer Output %v2float
%o_v4 = OpTypePointer Output %v4float
'''
    if pc_type: asm += '%PCT = OpTypeStruct %v4float\n%pc_p = OpTypePointer PushConstant %PCT\n%pc_m = OpTypePointer PushConstant %v4float\n%pc = OpVariable %pc_p PushConstant\n'
    if ubo: asm += '%UBT = OpTypeStruct %v4float\n%ub_p = OpTypePointer Uniform %UBT\n%ub_m = OpTypePointer Uniform %v4float\n%ub = OpVariable %ub_p Uniform\n'
    for i, t in enumerate(outs): asm += f'%out{i} = OpVariable %o_{t[:2]} Output\n'
    asm += '%main = OpFunction %void None %fn\n%entry = OpLabel\n%v0 = OpCopyObject %v4float %c4\n'
    if pc_type: asm += '%pa = OpAccessChain %pc_m %pc %int0\n%pv = OpLoad %v4float %pa\n%v0b = OpFMul %v4float %v0 %pv\n'
    else: asm += '%v0b = OpCopyObject %v4float %v0\n'
    if ubo: asm += '%ua = OpAccessChain %ub_m %ub %int0\n%uv = OpLoad %v4float %ua\n%v0c = OpFMul %v4float %v0b %uv\n'
    else: asm += '%v0c = OpCopyObject %v4float %v0b\n'
    for i, t in enumerate(outs): asm += f'OpStore %out{i} ' + ('%v0c' if i == 0 and t == 'v4float' else '%c2' if t == 'v2float' else '%c4') + '\n'
    return asm + 'OpReturn\nOpFunctionEnd\n'


def spirv_with_bindings(bindings):
    words = [vs.MAGIC, 0x00010000, 0, 64, 0]
    for variable, descriptor_set, binding in bindings:
        words.extend(((4 << 16) | vs.OP_DECORATE, variable, vs.DEC_BINDING, binding))
        words.extend(((4 << 16) | vs.OP_DECORATE, variable, vs.DEC_SET, descriptor_set))
    return struct.pack(f'<{len(words)}I', *words)


class VkShaders(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(shutil.which('spirv-as'), 'spirv-as required to assemble the test shaders')
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def translate(self, asm):
        """vk_shaders.one() on the assembled shader: (key, stage, resource rows, [MSL text of each variant])"""
        spv = self.tmp / 'shader.spv'
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-o', str(spv), '-'], input=asm, text=True, check=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)], check=True)
        out = self.tmp / 'out'; out.mkdir(exist_ok=True)
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', ResourceWarning)   # the tool leaves its files to be closed at collection
            key, stage, res, msls, _ = vs.one(str(spv), str(out), str(self.tmp))
        self.assertEqual(key, hashlib.sha256(spv.read_bytes()).hexdigest()[:16])
        return key, stage, res, [Path(m).read_text() for m in msls]

    def test_entry_point_of_an_hlsl_shader(self):   # DXC names it PSMain (or ps_*), spirv-cross renames that one to vk_<key>
        key, stage, _, srcs = self.translate(fragment(entry='PSMain'))
        self.assertEqual(stage, 'frag')
        self.assertEqual(len(srcs), 1)
        self.assertIn('fragment ', srcs[0]); self.assertIn(f'vk_{key}(', srcs[0])
        self.assertNotIn('PSMain', srcs[0])

    def test_entry_point_named_main(self):   # GLSL's name keeps working
        key, _, _, srcs = self.translate(fragment())
        self.assertIn(f'vk_{key}(', srcs[0])

    def test_push_constant_under_dxcs_type_name(self):   # type.PushConstant.RootConstants: spirv-cross writes it type_PushConstant_RootConstants
        _, _, res, srcs = self.translate(fragment(pc_type='type.PushConstant.RootConstants', ubo=True))
        self.assertEqual(res, ['u:0.0=0', 'p:0.0=1'])
        self.assertIn('constant type_PushConstant_RootConstants& pc [[buffer(1)]]', srcs[0])

    def test_push_constant_under_a_plain_type_name(self):   # GLSL's type name is already an identifier
        _, _, res, srcs = self.translate(fragment(pc_type='PushBlock', ubo=True))
        self.assertEqual(res, ['u:0.0=0', 'p:0.0=1'])
        self.assertIn('constant PushBlock& pc [[buffer(1)]]', srcs[0])

    def test_dual_source_variant_for_outputs_of_one_type(self):   # Super Mario Galaxy's GX shaders: two vec4 outputs, the second the blend source
        key, _, _, srcs = self.translate(fragment(outs=('v4float', 'v4float')))
        self.assertEqual(len(srcs), 2)
        self.assertIn(f'vk_{key}_d(', srcs[1]); self.assertIn('[[color(0), index(1)]]', srcs[1])
        self.assertNotIn('index(1)', srcs[0])

    def test_no_dual_source_variant_for_a_second_target_of_another_type(self):
        _, _, _, srcs = self.translate(fragment(outs=('v4float', 'v2float')))
        self.assertEqual(len(srcs), 1)
        self.assertIn('[[color(1)]]', srcs[0])

    def test_no_dual_source_variant_for_a_single_output(self):
        self.assertEqual(len(self.translate(fragment())[3]), 1)

    def test_no_dual_source_variant_past_two_outputs(self):
        self.assertEqual(len(self.translate(fragment(outs=('v4float', 'v4float', 'v4float')))[3]), 1)

    def test_texel_buffer_is_a_native_texture_buffer(self):   # Re:ZERO's ClusteredLight kernel: the host binds a buffer texture, the MSL must declare one
        asm = '''OpCapability Shader
OpCapability SampledBuffer
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 8 1 1
OpName %lights "lights"
OpDecorate %lights DescriptorSet 0
OpDecorate %lights Binding 0
OpDecorate %SB Block
OpMemberDecorate %SB 0 Offset 0
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%v4uint = OpTypeVector %uint 4
%int0 = OpConstant %int 0
%uint5 = OpConstant %uint 5
%img = OpTypeImage %uint Buffer 0 0 0 1 Unknown
%img_p = OpTypePointer UniformConstant %img
%lights = OpVariable %img_p UniformConstant
%SB = OpTypeStruct %v4uint
%sb_p = OpTypePointer StorageBuffer %SB
%out = OpVariable %sb_p StorageBuffer
%m_p = OpTypePointer StorageBuffer %v4uint
%main = OpFunction %void None %fn
%entry = OpLabel
%i = OpLoad %img %lights
%t = OpImageFetch %v4uint %i %uint5
%dst = OpAccessChain %m_p %out %int0
OpStore %dst %t
OpReturn
OpFunctionEnd
'''
        _, stage, _, srcs = self.translate(asm)
        self.assertEqual(stage, 'comp:8,1,1')
        self.assertIn('texture_buffer<uint> lights', srcs[0])
        self.assertNotIn('spvTexelBufferCoord', srcs[0])


class ShaderTable(unittest.TestCase):
    # T20: losing array kinds or offset rows breaks bindless lookup; a wrong stage or local size breaks compute dispatch.
    def test_argument_arrays_dynamic_offsets_and_discrete_sets(self):
        reflection = {
            'entryPoints': [{'name': 'PSMain', 'mode': 'frag'}],
            'separate_images': [
                {'set': 1, 'binding': 0, 'array': [512], 'name': 'textures'},
                {'set': 3, 'binding': 0, 'array': [20], 'name': 'smallArray'},
            ],
            'separate_samplers': [{'set': 0, 'binding': 0, 'array': [64], 'name': 'samplers'}],
            'ubos': [
                {'set': 0, 'binding': 80, 'name': 'dynamicUniform'},
                {'set': 2, 'binding': 0, 'name': 'ordinaryUniform'},
            ],
        }
        bindings = [(10, 1, 0), (11, 0, 0), (12, 0, 80), (13, 2, 0), (14, 3, 0)]
        msl = """struct Ubo { uint value; };\nstruct spvDescriptorSetBuffer0 { array<sampler, 64> samplers [[id(0)]]; device const uint* dynamicUniform [[id(80)]]; };\nstruct spvDescriptorSetBuffer1 { array<texture2d<float>, 512> textures [[id(0)]]; };\nfragment float4 PSMain(constant Ubo& ordinaryUniform [[buffer(0)]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(1)]], constant spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(2)]], constant uint* spvDynamicOffsets [[buffer(3)]], texture2d<float> smallTexture [[texture(0)]]) { auto dynamic = spvDescriptorSet0.dynamicUniform + spvDynamicOffsets[0]; return float4(float(dynamic[0])); }\n"""
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'bindless.spv'
            path.write_bytes(spirv_with_bindings(bindings))

            def cross(args, **kwargs):
                if '--reflect' in args:
                    return SimpleNamespace(stdout=json.dumps(reflection), stderr='', returncode=0)
                name = args[args.index('--rename-entry-point') + 2]
                output = Path(args[args.index('--output') + 1])
                output.write_text(msl.replace('PSMain', name))
                return SimpleNamespace(stdout='', stderr='', returncode=0)

            with patch.object(vs.subprocess, 'run', side_effect=cross) as run:
                row = vs.one(str(path), td, td)

        self.assertEqual(row[1], 'frag')
        self.assertEqual(set(row[2]), {
            'S:0.0=1*64', 'T:1.0=2*512', 'U:0.80=1', 'u:2.0=0', 't:3.0=0*20', 'q:0.0=3', 'o:0.80=0',
        })
        command = run.call_args_list[1].args[0]
        self.assertIn('--msl-argument-buffers', command)
        tier = command.index('--msl-argument-buffer-tier')
        self.assertEqual(command[tier + 1], '1')
        dynamic = command.index('--msl-dynamic-buffer')
        self.assertEqual(command[dynamic + 1:dynamic + 3], ['0', '80'])
        discrete = [command[i + 1] for i, arg in enumerate(command) if arg == '--msl-discrete-descriptor-set']
        self.assertEqual(discrete, ['2', '3', '4', '5', '6', '7'])
        self.assertIn('PSMain', command)

    def test_compute_local_size_and_specialization_skip(self):
        with tempfile.TemporaryDirectory() as td:
            fixed = Path(td) / 'compute.spv'
            fixed.write_bytes(spirv_with_bindings([]))
            specialized = Path(td) / 'specialized.spv'
            specialized.write_bytes(spirv_with_bindings([(9, 0, 1)]))
            fixed_reflection = {'entryPoints': [{'name': 'CSMain', 'mode': 'comp', 'workgroup_size': [8, 8, 1]}]}
            spec_reflection = {'entryPoints': [{'name': 'CSMain', 'mode': 'comp', 'workgroup_size': [8, 8, 1], 'workgroup_size_is_spec_constant_id': [True, False, False]}]}

            reflect_index = 0
            def cross(args, **kwargs):
                nonlocal reflect_index
                if '--reflect' in args:
                    reflection = fixed_reflection if reflect_index == 0 else spec_reflection
                    reflect_index += 1
                    return SimpleNamespace(stdout=json.dumps(reflection), stderr='', returncode=0)
                name = args[args.index('--rename-entry-point') + 2]
                output = Path(args[args.index('--output') + 1])
                output.write_text(f'kernel void {name}() {{ }}\n')
                return SimpleNamespace(stdout='', stderr='', returncode=0)

            with patch.object(vs.subprocess, 'run', side_effect=cross) as run:
                row = vs.one(str(fixed), td, td)
                skipped = vs.one(str(specialized), td, td)

        self.assertEqual(row[1], 'comp:8,8,1')
        self.assertEqual(skipped, 'comp with a local size from specialization constants')
        self.assertEqual(run.call_count, 3, 'the specialized-size module must not be translated')
        self.assertIn('comp', run.call_args_list[1].args[0])
        self.assertNotIn('--msl', run.call_args_list[2].args[0])


if __name__ == '__main__':
    unittest.main()
