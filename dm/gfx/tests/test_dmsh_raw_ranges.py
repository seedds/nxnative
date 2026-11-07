"""PR #1464 range-cache tests, adapted to the pinned dynamic-search translator."""
from pathlib import Path
import ast, os, re, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_dmsh_raw_quads_dynamic import quad_fixture
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import dmsh_wide as wide

@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'requires pinned translator')
class RawRanges(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.source,slot=quad_fixture()
  cls.source=cls.source.replace(']]) {\n',']])\n{\n')
  cls.table=f'cp_s{slot}'
 def rewrite(self,s):return wide.raw_ranges(s,{self.table},4096,32)
 def test_search_behind_cache(self):
  out=self.rewrite(self.source)
  self.assertNotEqual(out,self.source)
  self.assertIn('struct dm_raw_range',out)
  self.assertIn('if (_ra - _rr.s < _rr.e - _rr.s)',out)
  self.assertIn('thread dm_raw_range& _rr',out)
  self.assertEqual(out.count('= {0ul, 0ul, 0};'),1)
  self.assertEqual(out.count(', _rr_'),4)
  self.assertEqual(self.rewrite(out),out)
 def test_const_table_and_pointer(self):
  s=self.source.replace('device _','const device _').replace('reinterpret_cast<device uint*>','reinterpret_cast<const device uint*>')
  self.assertNotEqual(self.rewrite(s),s)
 def test_unproven_shapes_unchanged(self):
  for a,b in [(' << 11;',' << 12;'),('>> uint(21)','>> uint(22)'),('& 4095;','& 8191;'),('< 2097152u;','<= 2097152u;'),('= 0 + 4;','= 0 + 5;'),('return 0;','return 1;')]:
   with self.subTest(change=b):
    s=self.source.replace(a,b,1);self.assertNotEqual(s,self.source);self.assertEqual(self.rewrite(s),s)
 def test_table_provenance(self):
  self.assertEqual(wide.raw_ranges(self.source,set(),4096,32),self.source)
  self.assertEqual(wide.raw_ranges(self.source,{'cp_s999'},4096,32),self.source)
  self.assertEqual(wide.raw_ranges(self.source,{self.table},4096,64),self.source)
  self.assertEqual(wide.raw_ranges(self.source,{self.table},4095,32),self.source)
  s=self.source.replace('(*spvDescriptorSet0.'+self.table+')','(*spvDescriptorSet0.cp_s999)',1)
  self.assertEqual(self.rewrite(s),s)
 def test_multiple_entries_keep_separate_caches(self):
  start=self.source.index('kernel void main0')
  s=self.source+'\n'+self.source[start:].replace('main0','main1')
  out=self.rewrite(s)
  self.assertEqual(out.count('= {0ul, 0ul, 0};'),2)
  self.assertEqual(out.count(', _rr_'),8)
 def test_memory_effects_are_retained(self):
  effect='threadgroup_barrier(mem_flags::mem_device);'
  s=self.source.replace(' int _900103 =',' '+effect+'\n int _900103 =')
  out=self.rewrite(s);self.assertNotEqual(out,s);self.assertIn(effect,out)
  # Translation is cached; the data load in the helper is still executed each call.
  self.assertEqual(out.count('reinterpret_cast<device uint*>'),s.count('reinterpret_cast<device uint*>'))
 @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') and shutil.which('xcrun'),'requires Metal GPU')
 def test_gpu_boundaries_gaps_and_hint_collisions(self):
  self.check_gpu(self.source, 'raw_range_gpu.m')
 def check_gpu(self, source, harness):
  root=Path(__file__).resolve().parents[1];tree=ast.parse((root/'tools/dmsh_build.py').read_text())
  flags=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='MAC_METAL' for t in n.targets))
  sys.path.insert(0,str(root.parents[0]/'ios/tools'));import ios_metal
  with tempfile.TemporaryDirectory() as tmp:
   tmp=Path(tmp)
   for name,text in [('scalar',source),('cached',self.rewrite(source))]:
    f=tmp/(name+'.metal');f.write_text(text);ios_metal.compile(f,tmp/(name+'.air'),tmp/(name+'.metallib'),flags)
   binary=tmp/'check'
   subprocess.run(['xcrun','clang','-fobjc-arc','-framework','Foundation','-framework','Metal',str(Path(__file__).with_name(harness)),'-o',str(binary)],check=True,capture_output=True)
   r=subprocess.run([str(binary),str(tmp/'scalar.metallib'),str(tmp/'cached.metallib')],capture_output=True,text=True)
   print(r.stdout);self.assertEqual(r.returncode,0,r.stderr)

 @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') and shutil.which('xcrun'),'requires Metal GPU')
 def test_gpu_reads_after_intervening_writes(self):
  m=next(wide._RAW_RANGE_HELPER.finditer(self.source));name=m[1];table=m[6]
  s=self.source[:self.source.index('kernel void main0')]+f'''kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], device uint4* output [[buffer(2)]], device uint* data [[buffer(3)]], uint tid [[thread_position_in_grid]])
{{
 int _900000=int(tid*4u); int _900001=32;
 int _900002={name}(_900000, _900001, (*spvDescriptorSet0.{table}));
 data[tid]=as_type<uint>(_900002)^0xdeadbeefu;
 threadgroup_barrier(mem_flags::mem_device);
 int _900003={name}(_900000, _900001, (*spvDescriptorSet0.{table}));
 output[tid]=uint4(as_type<uint>(_900002),as_type<uint>(_900003),0,0);
}}
'''
  self.assertNotEqual(self.rewrite(s),s)
  self.check_gpu(s, 'raw_range_update_gpu.m')

if __name__=='__main__':unittest.main()
