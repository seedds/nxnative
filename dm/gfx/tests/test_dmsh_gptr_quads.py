"""Synthetic-input coverage for pointer-site quad grouping; no game shader data."""
from pathlib import Path
import ast
import re
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_wide  # noqa: E402

FIXTURE = Path(__file__).parent / 'fixtures/raw_global_gptr_3d38aaee9dc0.metal'


def fixture(offset=0):
    translated = FIXTURE.read_text()
    helper = next(dmsh_wide._GPTR_HELPER.finditer(translated))
    typ, table, sites, name = (helper[k] for k in ('type', 'table', 'sites', 'name'))
    qual = (helper['const'] or '') + 'device'
    source = f'''#include <metal_stdlib>
using namespace metal;
struct {typ} {{ uint _m0[1]; }};
struct spvDescriptorSetBuffer0 {{ {qual} {typ}* {table}; {qual} {typ}* {sites}; }};
{helper[0]}
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], device const ulong* addresses [[buffer(1)]], device int4* results [[buffer(2)]], uint tid [[thread_position_in_grid]]) {{
 int _900000 = as_type<int>(uint(addresses[tid]));
 int _900001 = as_type<int>(uint(addresses[tid] >> 32));
 uint _900002 = 0;
'''
    pairs = []
    for i in range(4):
        delta = offset + 4 * i
        lo = 900010 + 4 * i
        if delta:
            source += f' int _{lo} = _900000 + {delta};\n bool _{lo+1} = uint(_{lo}) < uint(_900000);\n int _{lo+2} = int(_{lo+1});\n int _{lo+3} = _900001 + _{lo+2};\n'
            pairs.append((lo, lo+3))
        else:
            pairs.append((900000, 900001))
    for i, (lo, hi) in enumerate(pairs):
        a = 900100 + 4 * i
        source += f' int _{a} = _{lo};\n int _{a+1} = _{hi};\n int _{a+2} = 3;\n int _{a+3} = {name}(_{a}, _{a+1}, _{a+2}, (*spvDescriptorSet0.{table}), (*spvDescriptorSet0.{sites}), _900002);\n'
    return source + ' results[tid] = int4(_900103, _900107, _900111, _900115);\n}\n'


class PointerSiteQuads(unittest.TestCase):
    def test_adjacent_words(self):
        for offset in (0, 16):
            with self.subTest(offset=offset):
                source = fixture(offset)
                result = dmsh_wide.gptr_load_quads(source)
                self.assertEqual(len(dmsh_wide._gptr_groups(source)), 1)
                self.assertEqual(result.count('_quad = dm_gptr_quad_'), 1)
                self.assertEqual(dmsh_wide.gptr_load_quads(result), result)
                self.assertIn(next(dmsh_wide._GPTR_HELPER.finditer(source))[0], result)

    def test_intervening_effects(self):
        effects = ('threadgroup_barrier(mem_flags::mem_device);', 'results[tid] = int4(0);',
                   'unknown_call();', '_900000 += 4;', 'mutate(_900000);',
                   '++_900000;', 'int _900200 = mutate(_900000);')
        for offset in (0, 16):
            for effect in effects:
                with self.subTest(offset=offset, effect=effect):
                    source = fixture(offset).replace(' int _900104 =', ' '+effect+'\n int _900104 =')
                    self.assertEqual(dmsh_wide.gptr_load_quads(source), source)

    def test_different_site_or_cache(self):
        for old, new in (('int _900106 = 3;', 'int _900106 = 4;'),
                         (', _900002);', ', _900201);')):
            with self.subTest(change=new):
                source = fixture().replace(old, new, 1)
                self.assertEqual(dmsh_wide.gptr_load_quads(source), source)

    def test_unknown_helper_semantics(self):
        source = fixture().replace('return 0;', 'return 1;', 1)
        self.assertEqual(dmsh_wide.gptr_load_quads(source), source)

    def test_identifiers_are_not_fixed(self):
        source = re.sub(r'_(\d+)\b', lambda m: '_'+str(int(m[1])+1000000), fixture())
        self.assertEqual(dmsh_wide.gptr_load_quads(source).count('_quad = dm_gptr_quad_'), 1)

    def test_nonadjacent_words_stay_scalar(self):
        source = fixture().replace('_900000 + 12;', '_900000 + 16;')
        self.assertEqual(dmsh_wide.gptr_load_quads(source), source)


def gpu_fixture(out):
    """Compile the synthetic differential kernel through the production pipeline."""
    P = Path(out); P.mkdir(parents=True, exist_ok=True)
    S = TOOLS.parents[2]
    h=next(dmsh_wide._GPTR_HELPER.finditer(FIXTURE.read_text()))
    typ,table,sites,scalar=(h[k] for k in ('type','table','sites','name'))
    qual=(h['const'] or '')+'device'
    s=f'''#include <metal_stdlib>
    using namespace metal;
    struct {typ} {{ uint _m0[1]; }};
    {h[0]}
    static inline __attribute__((always_inline))
    int4 scalar4(ulong address, int site, {qual} {typ}& table, {qual} {typ}& sites, thread uint& last) {{
     int4 result;
     for(uint k=0;k<4;k++) {{ ulong at=address+4ul*k; int lo=as_type<int>(uint(at)),hi=as_type<int>(uint(at>>32)); result[k]={scalar}(lo,hi,site,table,sites,last); }}
     return result;
    }}
    kernel void gptr_test({qual} {typ}& tab [[buffer(0)]], {qual} {typ}& sites [[buffer(1)]], const device int4* c [[buffer(2)]], device int* out [[buffer(3)]], uint i [[thread_position_in_grid]]) {{
     uint a=0,b=0; int site=c[i].z; ulong address=as_type<ulong>(uint2(uint(c[i].x),uint(c[i].y)));
     if(c[i].w) {{ int lo=1048584,hi=1,site=-1; (void){scalar}(lo,hi,site,tab,sites,a); b=a; }}
     int4 reference=scalar4(address,site,tab,sites,a),candidate;
     struct LocalBindings {{ {qual} {typ}* {table}; {qual} {typ}* {sites}; }};
     LocalBindings spvDescriptorSet0={{&tab,&sites}};
    '''
    for slot in range(6):
     base=900000+slot*100
     def name(off, base=base):return '_'+str(base+off)
     s+=f'{"if" if slot==0 else "else if"}(site=={slot}) {{\n int {name(0)} = c[i].x;\n int {name(1)} = c[i].y;\n uint {name(2)} = b;\n'
     for j in range(1,4):
      q=4*j
      s+=f' int {name(q)} = {name(0)} + {q};\n bool {name(q+1)} = uint({name(q)}) < uint({name(0)});\n int {name(q+2)} = int({name(q+1)});\n int {name(q+3)} = {name(1)} + {name(q+2)};\n'
     for j in range(4):
      q=20+j*4;lo=name(0 if j==0 else j*4);hi=name(1 if j==0 else j*4+3)
      s+=f' int {name(q)} = {lo};\n int {name(q+1)} = {hi};\n int {name(q+2)} = {slot};\n int {name(q+3)} = {scalar}({name(q)}, {name(q+1)}, {name(q+2)}, (*spvDescriptorSet0.{table}), (*spvDescriptorSet0.{sites}), {name(2)});\n'
     s+=f' candidate=int4({", ".join(name(23+j*4) for j in range(4))}); b={name(2)};\n}}\n'
    s+='else {candidate=scalar4(address,site,tab,sites,b);}\nfor(uint k=0;k<4;k++){out[i*10+k]=reference[k];out[i*10+4+k]=candidate[k];}out[i*10+8]=int(a);out[i*10+9]=int(b);\n}\n'
    assert len(dmsh_wide._gptr_groups(s))==6
    rewritten=dmsh_wide.gptr_load_quads(s)
    assert rewritten.count('_quad = dm_gptr_quad_')==6
    assert dmsh_wide.gptr_load_quads(rewritten)==rewritten
    (P/'synthetic-gpu.metal').write_text(rewritten)
    sys.path.insert(0,str(S/'dm/ios/tools'));import ios_metal
    ast_tree=ast.parse((S/'dm/gfx/tools/dmsh_build.py').read_text())
    flags=next(ast.literal_eval(n.value) for n in ast_tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='MAC_METAL' for t in n.targets))
    ios_metal.compile(P/'synthetic-gpu.metal',P/'synthetic-gpu.air',P/'synthetic-gpu.metallib',flags)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture':
        gpu_fixture(sys.argv[2])
    else:
        unittest.main()
