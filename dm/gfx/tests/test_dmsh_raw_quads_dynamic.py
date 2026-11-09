"""Legacy quad lowering plus fail-closed handling of newer translator helper ABIs."""
import ast
import subprocess
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import dmsh_wide  # noqa: E402


def quad_fixture():
    """Build four adjacent reads using a helper translated from synthetic instructions."""
    # Quad lowering recognizes the pre-cache scalar helper. Keep its positive GPU
    # oracle independent of newer native-cache and pointer-site signatures.
    translated = (Path(__file__).parent / 'fixtures/raw_global_dynamic_52c623609245.metal').read_text()
    helper = next(dmsh_wide._RAW_FUNCTION.finditer(translated))
    typ, table, name = (helper[k] for k in ('type', 'table', 'name'))
    src = f'''#include <metal_stdlib>
using namespace metal;
struct {typ} {{ uint _m0[1]; }};
struct spvDescriptorSetBuffer0 {{
 device {typ}* {table} [[id(0)]];
}};
{helper[0]}
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], device const ulong* addresses [[buffer(1)]], device int4* results [[buffer(2)]], uint tid [[thread_position_in_grid]]) {{
 int _900000 = as_type<int>(uint(addresses[tid]));
 int _900001 = as_type<int>(uint(addresses[tid] >> 32));
'''
    for i, offset in enumerate((4, 8, 12), 1):
        lo = 900000 + 4 * i
        src += f' int _{lo} = _900000 + {offset};\n bool _{lo+1} = uint(_{lo}) < uint(_900000);\n int _{lo+2} = int(_{lo+1});\n int _{lo+3} = _900001 + _{lo+2};\n'
    for i, (lo, hi) in enumerate(((900000, 900001), (900004, 900007), (900008, 900011), (900012, 900015))):
        a = 900100 + 3 * i
        src += f' int _{a} = _{lo};\n int _{a+1} = _{hi};\n int _{a+2} = {name}(_{a}, _{a+1}, (*spvDescriptorSet0.{table}));\n'
    return src + ' results[tid] = int4(_900102, _900105, _900108, _900111);\n}\n', int(table.split('_s')[1])


class DynamicRawQuads(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source, cls.slot = quad_fixture()

    def test_dynamic_search_and_hint_are_preserved(self):
        out = dmsh_wide.raw_load_quads(self.source, self.slot, 4096, 32)
        self.assertEqual(out.count('_words = dm_raw_load4'), 1)
        self.assertIn('index < (table._m0[4] - 1u) ?', out)
        self.assertIn('next > address + 15ul', out)
        self.assertIn('end - address >= 16ul', out)
        self.assertEqual(dmsh_wide.raw_load_quads(out, self.slot, 4096, 32), out)

    def test_unknown_search_semantics_stay_scalar(self):
        for old, new in ((' << 11;', ' << 12;'), ('>> uint(21)', '>> uint(22)'),
                         ('& 4095;', '& 8191;'), ('< 2097152u;', '<= 2097152u;'),
                         ('= 0 + 4;', '= 0 + 5;'), ('return 0;', 'return 1;')):
            with self.subTest(change=new):
                changed = self.source.replace(old, new, 1)
                self.assertNotEqual(changed, self.source)
                self.assertEqual(dmsh_wide.raw_load_quads(changed, self.slot, 4096, 32), changed)

    def test_intervening_effects_stay_scalar(self):
        for effect in ('threadgroup_barrier(mem_flags::mem_device);',
                       'results[tid] = int4(0);', '_900000 += 4;'):
            changed = self.source.replace(' int _900103 =', ' ' + effect + '\n int _900103 =')
            self.assertNotEqual(changed, self.source)
            self.assertEqual(dmsh_wide.raw_load_quads(changed, self.slot, 4096, 32), changed)

    def test_renamed_identifiers(self):
        renamed = re.sub(r'_(\d+)\b', lambda m: '_' + str(int(m[1]) + 1000000), self.source)
        out = dmsh_wide.raw_load_quads(renamed, self.slot, 4096, 32)
        self.assertEqual(out.count('_words = dm_raw_load4'), 1)


def gpu_fixture(out):
    """Compile both synthetic variants through the production Metal pipeline."""
    source, slot = quad_fixture()
    quads = dmsh_wide.raw_load_quads(source, slot, 4096, 32)
    if quads.count('_words = dm_raw_load4') != 1:
        raise RuntimeError('The synthetic quad fixture was not transformed')
    tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
    flags = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                 and any(isinstance(t, ast.Name) and t.id == 'MAC_METAL' for t in n.targets))
    out = Path(out)
    for name, text in (('scalar', source), ('quads', quads)):
        msl = out / (name + '.metal')
        msl.write_text(text)
        air = out / (name + '.air')
        subprocess.run([*flags, str(msl), '-o', str(air)], check=True)
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(out / (name + '.metallib'))], check=True)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture':
        gpu_fixture(sys.argv[2])
    else:
        unittest.main()
