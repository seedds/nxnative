import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from dmsh_wide import power_of_two_mulhi as rewrite
H='''template<typename T, typename U, typename V>
[[clang::optnone]] T spvMulExtended(V l, V r)
{
 return T{U(l * r), U(mulhi(l, r))};
}
struct _2 { uint _m0; uint _m1; };
'''
C='(spvMulExtended<_2, uint>(uint(_5), 16u))._m1'
class Rewrite(unittest.TestCase):
 def test_all_powers(self):
  for power in range(32):
   c=C.replace('16u',str(1<<power)+'u')
   expected='(uint(_5) & 0u)' if power==0 else f'(uint(_5) >> {32-power}u)'
   self.assertEqual(rewrite(H+c),H+expected)
 def test_nonpowers(self):
  for n in (0,3,15,17,4294967295,4294967296):
   s=H+C.replace('16u',str(n)+'u');self.assertEqual(rewrite(s),s)
 def test_strict_types(self):
  for s in (H.replace('uint _m1','int _m1')+C,H+C.replace('<_2, uint>','<_2, int>'),H+C.replace('uint(_5)','int(_5)'),H+C.replace('._m1','._m0')):
   self.assertEqual(rewrite(s),s)
 def test_no_side_effect_elision(self):
  s=H+C.replace('uint(_5)','uint(next())').replace('16u','1u');self.assertEqual(rewrite(s),s)
 def test_unknown_helper(self):
  s=H.replace('U(mulhi(l, r))','U(mulhi(l, r) + 1)')+C;self.assertEqual(rewrite(s),s)
 def test_missing_or_ambiguous_struct(self):
  for s in (H.replace('struct _2 { uint _m0; uint _m1; };','')+C,H+'struct _2 { int _m0; int _m1; };'+C):self.assertEqual(rewrite(s),s)
 def test_idempotent(self):
  s=rewrite(H+C);self.assertEqual(rewrite(s),s)
if __name__=='__main__':unittest.main()
