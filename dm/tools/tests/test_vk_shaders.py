"""vk_shaders.py on synthetic SPIR-V fragment shaders assembled here with spirv-as (the committed spirv-cross translates them; no Metal compile, no game files): an entry point
named as HLSL compilers name it, a push-constant block under DXC's type name, and the dual-source variant only for outputs 0 and 1 of one type.
Requires spirv-as on PATH (the tool-tests job installs spirv-tools for spirv-val)."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import warnings

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools')); import vk_shaders as vs


def fragment(entry='main', pc_type=None, ubo=False, outs=('v4float',)):
    """Assembly of a fragment shader that writes one constant per output location (the locations in `outs` order, each of the type named); with `ubo` it multiplies
    output 0 by a vec4 of a uniform buffer (set 0, binding 0), with `pc_type` by a vec4 of a push-constant block whose type carries that name (OpName)."""
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
        self.assertEqual(res, ['u:0.0=0', 'p:0.0=1'])   # the push constants take the buffer slot after the uniform buffer, where spirv-cross put them at 0
        self.assertIn('constant type_PushConstant_RootConstants& pc [[buffer(1)]]', srcs[0])

    def test_push_constant_under_a_plain_type_name(self):   # GLSL: the type's name is already an identifier
        _, _, res, srcs = self.translate(fragment(pc_type='PushBlock', ubo=True))
        self.assertEqual(res, ['u:0.0=0', 'p:0.0=1'])
        self.assertIn('constant PushBlock& pc [[buffer(1)]]', srcs[0])

    def test_dual_source_variant_for_outputs_of_one_type(self):   # Super Mario Galaxy's GX shaders: two vec4 outputs, the second the blend source
        key, _, _, srcs = self.translate(fragment(outs=('v4float', 'v4float')))
        self.assertEqual(len(srcs), 2)
        self.assertIn(f'vk_{key}_d(', srcs[1]); self.assertIn('[[color(0), index(1)]]', srcs[1])
        self.assertNotIn('index(1)', srcs[0])

    def test_no_dual_source_variant_for_a_second_target_of_another_type(self):   # a float2 output beside the float4 colour is a real attachment: Metal refuses the variant
        _, _, _, srcs = self.translate(fragment(outs=('v4float', 'v2float')))
        self.assertEqual(len(srcs), 1)
        self.assertIn('[[color(1)]]', srcs[0])

    def test_no_dual_source_variant_for_a_single_output(self):
        self.assertEqual(len(self.translate(fragment())[3]), 1)

    def test_no_dual_source_variant_past_two_outputs(self):   # three targets of one type are three targets
        self.assertEqual(len(self.translate(fragment(outs=('v4float', 'v4float', 'v4float')))[3]), 1)


if __name__ == '__main__':
    unittest.main()
