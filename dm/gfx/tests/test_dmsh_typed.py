"""Typed Maxwell exports follow runtime attachments without a per-shader fact."""
import os, pathlib, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_entry, dmsh_side, pso_offline
BASE = '''#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint i [[vertex_id]]) { return float4(i==1?3.f:-1.f,i==2?3.f:-1.f,0,1); }
struct main0_out {
float4 a [[color(0)]];
uint b [[color(1)]];
int c [[color(2)]];
};
fragment main0_out main0() { return {float4(0.25f,-0.5f,1.0f,0.25f),0xbf000000u,0x3f800000}; }
'''
def full(src=BASE):
    return dmsh_side.shared_body(src) + (dmsh_entry.uint_entry(src) or '') + dmsh_entry.fs_typed(src)

def metal_compiler(sdk):
    return bool(shutil.which('xcrun')) and subprocess.run(
        ['xcrun', '-sdk', sdk, 'metal', '--version'], capture_output=True).returncode == 0


class Typed(unittest.TestCase):
    def test_constants_do_not_overlap_existing_variants(self):
        import dmsh_abi as abi
        old = set(range(abi.UINT_FC, abi.UINT_FC + 8)) | set(range(abi.LOGIC_FC, abi.LOGIC_FC + 8)) | {abi.BORDER_FC}
        typed = {abi.FOUT_UINT_FC, abi.FOUT_SINT_FC, abi.FOUT_ACTIVE_FC}
        self.assertEqual(len(typed), 3)
        self.assertFalse(typed & old)

    def test_original_classes_and_preserved_members(self):
        s = BASE.replace('int c [[color(2)]];', 'int c [[color(2)]];\nfloat z [[depth(any)]];\nuint mask [[sample_mask]];')
        generated = dmsh_entry.fs_typed(s)
        self.assertIn('out.z = raw.z;', generated)
        self.assertIn('out.mask = raw.mask;', generated)
        self.assertIn('as_type<int4>(raw.a)', generated)
        self.assertIn('out.dm_uint1 = raw.b;', generated)
        self.assertEqual(dmsh_entry.typed_metadata(full()), dict(entry='main0_t', mask=7, uint=2, sint=4))

    def test_unknown_shapes_are_rejected(self):
        for old, new in [('float4 a', 'half4 a'), ('color(1)', 'color(1), index(1)'), ('int c', 'double c'), ('color(2)', 'color(1)')]:
            with self.subTest(new=new): self.assertIsNone(dmsh_entry.fs_typed(BASE.replace(old,new)))

    def test_alpha_reads_original_before_optional_conversion(self):
        s, info = dmsh_side.unit(full())
        self.assertIn('main0_a', info['alpha'])
        self.assertIn('main0_t_a', info['alpha'])
        e = dmsh_side.entry(s, 'main0_t_a'); body = s[e['head']:e['end']]
        self.assertIn('main0_out dm_r = dm_body_main0()', body)
        self.assertLess(body.index('dm_alpha_pass'), body.index('dm_typed_convert'))

    @unittest.skipUnless(all(metal_compiler(sdk) for sdk in ('macosx', 'iphoneos')), 'requires installed macOS and iOS Metal compilers')
    def test_typed_and_readable_tile_share_compile_on_both_platforms(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        from test_dmsh_fetch import FS2, BINDINGS, compile_lib
        import dmsh_fetch
        source = dmsh_side.shared_body(FS2) + dmsh_entry.fs_typed(FS2)
        source, info = dmsh_side.unit(source)
        folded, meta = dmsh_fetch.fold(source, BINDINGS)
        self.assertIsNotNone(folded, meta)
        typed = {'main0_t', 'main0_t_a'}
        side = set(info['alpha'] + info['logic']) - typed
        with tempfile.TemporaryDirectory(prefix='typed-tile-') as t:
            d = pathlib.Path(t)
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    lib = compile_lib(folded, d, sdk, sdk)
                    pieces = dmsh_side.cut(lib.read_bytes(), side, {'main0_f'}, typed)
                    for i, piece in enumerate(pieces): (d/f'{i}.metallib').write_bytes(piece)
                    self.assertEqual(pso_offline.functions(d/'0.metallib'), {'main0': False})
                    self.assertEqual(pso_offline.functions(d/'2.metallib'), {'main0_f': True})
                    self.assertEqual(pso_offline.functions(d/'3.metallib'), dict.fromkeys(typed, True))

    @unittest.skipUnless(os.environ.get('DMSH_TEST_TYPED_GPU'), 'set DMSH_TEST_TYPED_GPU to t_fragment_types')
    def test_compiled_split_and_host_on_gpu(self):
        with tempfile.TemporaryDirectory(prefix='typed-exports-') as t:
            d = pathlib.Path(t); src, info = dmsh_side.unit(full()); (d/'unit.metal').write_text(src)
            subprocess.run(['xcrun','metal','-c','-O1','-w',str(d/'unit.metal'),'-o',str(d/'unit.air')],check=True)
            subprocess.run(['xcrun','metallib',str(d/'unit.air'),'-o',str(d/'unit.metallib')],check=True)
            typed = {'main0_t','main0_t_a'}; side = set(info['alpha']+info['logic'])-typed
            pieces = dmsh_side.cut((d/'unit.metallib').read_bytes(),side,typed)
            for name,data in zip(('base','side','typed'),pieces): (d/(name+'.metallib')).write_bytes(data)
            self.assertEqual(pso_offline.functions(d/'base.metallib'), {'v':False,'main0':False,'main0_u':True})
            self.assertEqual(pso_offline.functions(d/'typed.metallib'),dict.fromkeys(typed,True))
            for metal4 in ('0','1'):
                subprocess.run([os.environ['DMSH_TEST_TYPED_GPU'],str(d)],check=True,env={**os.environ,'MTL_DEBUG_LAYER':'1','DM_PSO_MTL4':metal4,'DM_PSO_ARCHIVE':'off'})

if __name__ == '__main__': unittest.main()
