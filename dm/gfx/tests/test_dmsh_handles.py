import unittest,random,json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from dmsh_handles import Analyze,Parser,Unknown,prove_pools,reflection
MASK=0xffffffff

def signed(x):return x-(1<<32) if x & (1<<31) else x

def evaluate(n, read):
    op=n[0]
    if op=='const':return n[1]
    if op=='cb':return read('cb',n[1],evaluate(n[2],read)*16+n[3]*4)
    if op=='sb':return read('sb',n[1],evaluate(n[2],read)*4+n[3]*4)
    if op=='select':return evaluate(n[2] if evaluate(n[1],read) else n[3],read)
    a=evaluate(n[1],read)
    if op=='not':return int(not a)
    if op=='inv':return a^MASK
    if op.startswith('neg_'):
        if op=='neg_i' and a==0x80000000:raise Unknown('signed overflow')
        return (-a)&MASK
    b=evaluate(n[2],read)
    if op.endswith('_i'):a,b=signed(a),signed(b)
    name=op.split('_')[0]
    if name=='and':r=a&b
    elif name=='or':r=a|b
    elif name=='xor':r=a^b
    elif name=='eq':r=a==b
    elif name=='ne':r=a!=b
    elif name=='lt':r=a<b
    elif name=='le':r=a<=b
    elif name=='gt':r=a>b
    elif name=='ge':r=a>=b
    elif name=='land':r=bool(a) and bool(b)
    elif name=='lor':r=bool(a) or bool(b)
    elif name=='min':r=min(a,b)
    elif name=='max':r=max(a,b)
    elif name=='add':r=a+b
    elif name=='sub':r=a-b
    elif name=='mul':r=a*b
    elif name in ('shl','shr'):
        if not 0<=b<32:raise Unknown('shift count')
        if name=='shl' and op.endswith('_i') and a<0:raise Unknown('negative signed shift')
        r=(a<<b) if name=='shl' else (a>>b)
    else:raise Unknown(op)
    if op.endswith('_i') and name in ('add','sub','mul','shl') and not -(1<<31)<=r<(1<<31):raise Unknown('signed overflow')
    return int(r)&MASK

class Tests(unittest.TestCase):
    def expr(self,s,src='',bindings=()):return Parser(Analyze(src,bindings),s).run().node
    def val(self,s):return evaluate(self.expr(s),lambda *args:self.fail('unexpected memory read'))
    def test_integer_semantics(self):
        cases={'uint(0xffffffff) + 2u':1,'int(0xffffffff) >> 1':0xffffffff,'uint(0xffffffff) >> 1u':0x7fffffff,'1 + 2 * 3':7,'false ? 3 : 4':4,'as_type<int>(as_type<float>(uint(123)))':123,'-1 < 0':1,'uint(-1) < 0u':0,'min(uint(9), 2u)':2,'!(7 & 3)':0,'~0u':MASK,'-1 == int(0xffffffff)':1}
        for x,want in cases.items():
            with self.subTest(x=x):self.assertEqual(self.val(x),want)
    def test_unknown_expressions(self):
        for x in ('float(1)','int(as_type<float>(1))','1.0 + 2.0','sin(1)','in.m_1.x','1 / 2','1 % 2','min(1,2u)','_missing','spvDescriptorSet0.unknown'):
            with self.subTest(x=x),self.assertRaises(Unknown):self.expr(x)
    def test_zero_float_bit_selection(self):
        # Includes signed zero, infinities and NaN payloads: selection must
        # preserve raw bits rather than numerically convert the float.
        for bits in (0, 1, 0x80000000, 0x7f800000, 0x7fc01234, 0xffffffff):
            for zero in ('0.0', '0.0f', '0.0F'):
                for condition in ('true', 'false'):
                    expr = f'as_type<uint>({condition} ? as_type<float>(0x{bits:x}u) : {zero})'
                    self.assertEqual(self.val(expr), bits if condition == 'true' else 0)
        for expr in ('uint(0.0)', 'int(0.0)', '0.0 + 0.0', '-0.0',
                     'as_type<uint>(0.01)', 'as_type<uint>(0.0e1)',
                     'as_type<uint>(0.0u)', 'as_type<uint>(00.0)',
                     'true ? 0u : 0.0'):
            with self.subTest(expr=expr), self.assertRaises(Unknown):self.expr(expr)
    def test_invalid_arithmetic(self):
        for x in ('2147483647 + 1','1 << 32','1u >> 32u','-1 << 1','-int(0x80000000)'):
            with self.subTest(x=x),self.assertRaises(Unknown):self.val(x)
    def test_mutations(self):
        for rest in ('_v = 2;','_v += 2;','_v <<= 2;','++_v;','_v++;','--_v;','_v--;','use(&_v);'):
            with self.subTest(rest=rest),self.assertRaises(Unknown):self.expr('_v','uint _v = 1u;\n'+rest)
        self.assertEqual(evaluate(self.expr('_v','uint _v = 1u;'),lambda *a:None),1)
    def test_memory_types(self):
        bindings=[{'msl_name':'fp_c4','kind':'cbuf','reg':1},{'msl_name':'fp_s0','kind':'sbuf','reg':3,'rw':False}]
        node=self.expr('as_type<uint>((*spvDescriptorSet0.fp_c4)._m0[8].y)',bindings=bindings)
        self.assertEqual(evaluate(node,lambda *a:42 if a==('cb',1,132) else self.fail(str(a))),42)
        node=self.expr('(*spvDescriptorSet0.fp_s0)._m0[3]',bindings=bindings)
        self.assertEqual(evaluate(node,lambda *a:71 if a==('sb',3,12) else self.fail(str(a))),71)
        bindings[1]['rw']=True
        with self.assertRaises(Unknown):self.expr('(*spvDescriptorSet0.fp_s0)._m0[3]',bindings=bindings)
    def test_whole_pool_coverage(self):
        src='uint _v = 7u;\nfloat x = (&spvDescriptorSet0.pool->e[0])[min(uint(int(_v)), spvDescriptorSet0.pool->n)].read(uint2(0)).x;'
        self.assertEqual(len(Analyze(src,[]).pool_indices('pool')),1)
        for rest in ('\nuse(spvDescriptorSet0.pool);','\n_v = 1u;'):
            with self.assertRaises(Unknown):Analyze(src+rest,[]).pool_indices('pool')
    def test_entry_scopes_and_helper_uses(self):
        bindings=[{'msl_name':'pool','kind':'tex_pool'}]
        body='uint _v = VALUEu;\nfloat x = (&spvDescriptorSet0.pool->e[0])[min(uint(int(_v)), spvDescriptorSet0.pool->n)].read(uint2(0)).x;'
        src='fragment float main0() { '+body.replace('VALUE','7')+' }\nfragment uint main0_u() { '+body.replace('VALUE','11')+' }'
        proof=prove_pools(src,bindings)['pool'];self.assertEqual(len(proof['indices']),2)
        self.assertEqual([evaluate(n,lambda *a:None) for n in proof['indices']],[7,11])
        self.assertIn('unknown',prove_pools(src+'\nvoid helper(){ use(spvDescriptorSet0.pool); }',bindings)['pool'])
        self.assertIn('unknown',prove_pools(src.replace('VALUE','7').replace('VALUEu','7u').replace('uint _v = 11u;','uint _v = 11u; _v = 8u;'),bindings)['pool'])
class ReflectionTests(unittest.TestCase):
    def test_zero_float_material_select(self):
        src, bindings = self.fixture('as_type<uint>(_index != 0u ? as_type<float>(_handle) : 0.0)')
        proof = reflection(src, bindings)
        self.assertIsNotNone(proof)
        self.assertIn('select', [node[0] for node in proof['nodes']])

    def fixture(self, expr='_handle'):
        bindings = [
            {'kind':'cbuf','msl_name':'cb','msl_type':'constant C*','reg':1,'arg_id':0},
            {'kind':'sbuf','msl_name':'sb','msl_type':'const device B*','reg':2,'rw':False,'arg_id':1},
            {'kind':'tex_pool','msl_name':'pool','arg_id':2}]
        src = '''struct C { float4 _m0[4096]; }; struct B { uint _m0[1]; };
        fragment float4 main0(constant Args& spvDescriptorSet0) {
            uint _index = as_type<uint>((*spvDescriptorSet0.cb)._m0[0].x);
            packed_uint4 _data = *(const device packed_uint4*)&(*spvDescriptorSet0.sb)._m0[_index];
            uint _handle = _data.z & 1048575u;
            return (&spvDescriptorSet0.pool->e[0])[min(uint(int(EXPR)), spvDescriptorSet0.pool->n)].read(uint2(0));
        }'''.replace('EXPR',expr)
        return src,bindings

    def test_schema(self):
        src,bindings=self.fixture();proof=reflection(src,bindings)
        self.assertEqual(proof['version'],1)
        self.assertEqual(proof['pools'][0]['binding'],2)
        sb=next(n for n in proof['nodes'] if n[0]=='sb')
        self.assertEqual((sb[1],sb[3]),(2,2))  # packed component is a byte offset, not wrapping uint addition
        self.assertTrue(len(proof['nodes'])<256)

    def test_fail_closed(self):
        src,bindings=self.fixture()
        cases = [src.replace('_handle = _data.z','_handle = unknown(_data.z)'),
                 src.replace('return (','_data.z = 0u; return ('),
                 src.replace('return (','_data.z++; return ('),
                 src.replace('return (','++_data.z; return ('),
                 src.replace('return (','mutate(_handle); return (')+'void mutate(thread uint& x) { x++; }',
                 src.replace('return (','frexp(1.0, _handle); return ('),
                 src.replace('return (','external(_handle); return ('),
                 src.replace('return (','external((_handle)); return ('),
                 src.replace('return (','external(_data[0]); return ('),
                 src.replace('return (','fract(1.0, _handle); return ('),
                 src.replace('return (','_data[0] = 1u; return ('),
                 src.replace('return (','uint* p = &(_handle); *p=1u; return ('),
                 src.replace('return (','(true ? _handle : _index) = 1u; return ('),
                 src.replace('return (','helper(spvDescriptorSet0); return ('),
                 src.replace('uint _handle =','long _handle = 1; { uint _handle ='),
                 src.replace('_data.z','_data.xy'),
                 src.replace('uint _handle =','uint _handle; { uint _handle ='),
                 src.replace('float4 _m0','uint4 _m0'),
                 src.replace('uint _m0','float _m0'),
                 src.replace('1048575u','017u'),
                 src.replace('1048575u','2147483648')]
        for bad in cases:
            with self.subTest(bad=bad):self.assertIsNone(reflection(bad,bindings))
        for rw in (None,True):
            bs=[dict(b) for b in bindings];bs[1]['rw']=rw
            self.assertIsNone(reflection(src,bs))
        bs=bindings+[{'kind':'tex_pool','msl_name':'second','arg_id':3}]
        self.assertIsNone(reflection(src,bs))

    def test_unknown_opcode(self):
        src,bindings=self.fixture('_handle ^ 1u')
        self.assertIsNone(reflection(src,bindings))

    def test_material_addresses(self):
        bindings=[{'msl_name':'cb','kind':'cbuf','reg':1},{'msl_name':'sb','kind':'sbuf','reg':2,'rw':False}]
        src='''uint _a = as_type<uint>((*spvDescriptorSet0.cb)._m0[0].x);
        uint _z = as_type<uint>((*spvDescriptorSet0.cb)._m0[0].y);
        uint _off = ((_a & 3221225472u) == 2147483648u) ? ((_a & 1048575u) + _z) * 8u + 4u : 4u;
        uint _h = (*spvDescriptorSet0.sb)._m0[_off];'''
        n=Parser(Analyze(src,bindings),'_h & 1048575u').run().node
        rng=random.Random(744)
        for _ in range(1000):
            i,z=rng.randrange(1000),rng.randrange(1000)
            flags=rng.choice((0,0x40000000,0x80000000,0xc0000000));h=rng.randrange(1<<32)
            off=(i+z)*32+16 if flags==0x80000000 else 16
            def read(kind,reg,at):
                if (kind,reg,at)==('cb',1,0):return flags|i
                if (kind,reg,at)==('cb',1,4):return z
                self.assertEqual((kind,reg,at),('sb',2,off));return h
            self.assertEqual(evaluate(n,read),h&1048575)

if __name__=='__main__':unittest.main()
