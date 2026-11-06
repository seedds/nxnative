"""Range reuse for grouped raw loads: real Metal boundary and write oracles."""
from pathlib import Path
import os,re,shutil,sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
import test_dmsh_raw_ranges as ranges
from test_dmsh_raw_quads_dynamic import quad_fixture
import dmsh_wide as wide

@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'),'requires pinned translator')
class RawQuadRanges(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  source,slot=quad_fixture();cls.table=f'cp_s{slot}'
  cls.source=wide.raw_load_quads(source,slot,4096,32).replace(']]) {\n',']])\n{\n')
 def rewrite(self,s):return wide.raw_ranges(s,{self.table},4096,32)
 check_gpu=ranges.RawRanges.check_gpu
 def test_exact_helper_and_per_function_cache(self):
  out=self.rewrite(self.source);self.assertNotEqual(out,self.source)
  self.assertEqual(out.count('dm_raw_quad_range _rquad ='),1)
  self.assertIn('cache.e = min(end, next);',out)
  self.assertEqual(self.rewrite(out),out)
  start=self.source.index('kernel void main0');s=self.source+'\n'+self.source[start:].replace('main0','main1')
  self.assertEqual(self.rewrite(s).count('dm_raw_quad_range _rquad ='),2)
 def test_unproven_inputs_stay_unchanged(self):
  self.assertEqual(wide.raw_ranges(self.source,set(),4096,32),self.source)
  self.assertEqual(wide.raw_ranges(self.source,{'cp_s999'},4096,32),self.source)
  for old,new in [('next > address + 15ul','next >= address + 15ul'),('dm_raw_load4(_900100,','dm_raw_load4(1,'),('return 0;','return 1;')]:
   with self.subTest(change=new):
    s=self.source.replace(old,new,1);self.assertNotEqual(s,self.source);self.assertEqual(self.rewrite(s),s)
 @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') and shutil.which('xcrun'),'requires Metal GPU')
 def test_gpu_cache_hits_gaps_boundaries(self):
  start=self.source.index('kernel void main0')
  s=self.source[:start]+f'''kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], device const ulong* addresses [[buffer(1)]], device int4* results [[buffer(2)]], uint tid [[thread_position_in_grid]])
{{
 ulong previous=addresses[tid]-16ul;
 int _900000=as_type<int>(uint(previous)); int _900001=as_type<int>(uint(previous>>32));
 int4 first=dm_raw_load4(_900000, _900001, (*spvDescriptorSet0.{self.table}));
 int _900002=as_type<int>(uint(addresses[tid])); int _900003=as_type<int>(uint(addresses[tid]>>32));
 int4 second=dm_raw_load4(_900002, _900003, (*spvDescriptorSet0.{self.table}));
 uint4 bits=as_type<uint4>(first);
 results[tid]=as_type<int4>(as_type<uint4>(second)^((bits<<1)|(bits>>31)));
}}
'''
  self.assertNotEqual(self.rewrite(s),s);self.check_gpu(s,'raw_quad_range_gpu.m')
 @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') and shutil.which('xcrun'),'requires Metal GPU')
 def test_gpu_reads_after_intervening_write(self):
  start=self.source.index('kernel void main0')
  s=self.source[:start]+f'''kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], device uint4* output [[buffer(2)]], device uint* data [[buffer(3)]], uint tid [[thread_position_in_grid]])
{{
 int _900000=int(tid*16u); int _900001=32;
 int4 first=dm_raw_load4(_900000, _900001, (*spvDescriptorSet0.{self.table}));
 data[tid*4u]=as_type<uint>(first.x)^0xdeadbeefu;
 threadgroup_barrier(mem_flags::mem_device);
 int4 second=dm_raw_load4(_900000, _900001, (*spvDescriptorSet0.{self.table}));
 output[tid]=uint4(as_type<uint>(first.x),as_type<uint>(second.x),0,0);
}}
'''
  self.check_gpu(s,'raw_quad_range_update_gpu.m')
if __name__=='__main__':unittest.main()
