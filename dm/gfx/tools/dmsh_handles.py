"""Prove bounded material texture-pool indices from translated MSL.

This is deliberately a recognizer, not a general MSL interpreter. Only immutable
32-bit integer expressions and explicitly read-only buffers are accepted. A
missing proof keeps the host's conservative depth feedback handling. The host
must separately prove that every buffer word is current and not GPU-written.
"""
import re
from collections import Counter
from dataclasses import dataclass

class Unknown(ValueError): pass
@dataclass(frozen=True)
class V:
    ty: str
    node: tuple

def const(n, ty='i'):
    return V(ty, ('const', n & 0xffffffff))

def integer(v):
    if v.ty not in ('i', 'u', 'b'): raise Unknown('numeric floating-point expression')
    return v

TOK = re.compile(r'\s*(0\.0[fF]?(?![\w.])|0x[0-9a-fA-F]+[uU]?|\d+[uU]?|[A-Za-z_]\w*|<<|>>|==|!=|<=|>=|&&|\|\||[().\[\],?:+*/%&|^!~<>-])')
PREC = {'||':1, '&&':2, '|':3, '^':4, '&':5, '==':6, '!=':6, '<':7, '>':7, '<=':7, '>=':7, '<<':8, '>>':8, '+':9, '-':9, '*':10}
OPS = {'|':'or','^':'xor','&':'and','==':'eq','!=':'ne','<':'lt','>':'gt','<=':'le','>=':'ge','<<':'shl','>>':'shr','+':'add','-':'sub','*':'mul','&&':'land','||':'lor'}


def call_escapes(src):
    """Bare local lvalues passed to an unknown call may be reference outputs.

    Calls nested in a value expression do not escape its locals. Recognize only
    a small set of value-only calls; an unfamiliar builtin/helper stays unknown.
    """
    safe = {'if', 'while', 'switch', 'for', 'int', 'uint', 'float', 'bool',
            'min', 'max', 'as_type'}
    escaped = set()
    for m in re.finditer(r'\b(\w+)\s*\(', src):
        if m[1] in safe:
            continue
        level, start, at = 1, m.end(), m.end()
        while level and at < len(src):
            c = src[at]
            if c == '(':
                level += 1
            elif c == ')':
                level -= 1
            if (c == ',' and level == 1) or level == 0:
                arg = src[start:at].strip()
                while arg.startswith('(') and arg.endswith(')'):
                    arg = arg[1:-1].strip()
                local = re.fullmatch(r'(_\w+)(?:\.[xyzw]+)?', arg) or re.match(r'(_\w+)(?:\.|\[)', arg)
                if local:
                    escaped.add(local[1])
                start = at + 1
            at += 1
    return escaped


class Analyze:
    def __init__(self, src, bindings):
        self.src, self.defs, self.cache, self.active = src, {}, {}, set()
        self.resources = {b['msl_name']:b for b in bindings}
        # A declaration and exactly one assignment. A branch/loop reassignment,
        # taking its address, or an increment makes the name unprovable.
        pat = r'\b(bool|uint|int|float|packed_uint[234])\s+(_\w+) = ([^;\n]+);'
        assignments = Counter(re.findall(r'(?<![\w.])(_\w+)(?:\.\w+|\[[^;{}]*?\])*\s*(?:(?:<<|>>|[+*/%&|^-])?=(?!=)|\+\+|--)', src))
        declarations = Counter(name for ty,name in re.findall(r'\b(\w+)\s+(_\w+)\s*(?=[=;,\[])',src) if ty not in ('return','case','goto'))
        escaped = set(re.findall(r'(?:&|\+\+|--)\s*(?:\(\s*)*(_\w+)\b', src)) | call_escapes(src)
        # An exotic lvalue (a conditional or function returning a reference) can
        # mutate a name without spelling it on the left of '='. Not recognized.
        if re.search(r'\)\s*(?:(?:<<|>>|[+*/%&|^-])?=(?!=)|\+\+|--)', src):
            escaped.update(declarations)
        for m in re.finditer(pat, src):
            name=m[2]
            if assignments[name]==1 and declarations[name]==1 and name not in escaped and name not in self.defs:
                self.defs[name]=(m[1],m[3])
    def variable(self, name):
        if name in self.cache:return self.cache[name]
        if name not in self.defs or name in self.active or len(self.active)>64:raise Unknown('non-SSA or unknown name '+name)
        ty,expr=self.defs[name];self.active.add(name)
        try:v=Parser(self,expr).run()
        finally:self.active.remove(name)
        target={'bool':'b','uint':'u','int':'i','float':'f','packed_uint2':'v2','packed_uint3':'v3','packed_uint4':'v4'}[ty]
        if target=='f' and v.ty!='f':raise Unknown('implicit float conversion')
        if target in ('i','u','b'):
            integer(v)
            if target=='b' and v.ty!='b':v=V('b',('ne',v.node,const(0).node))
        if target.startswith('v') and v.ty!=target:raise Unknown('vector conversion')
        v=V(target,v.node);self.cache[name]=v;return v
    def pool_indices(self, name):
        # Require every source occurrence outside declarations and the bound n
        # to be this canonical bounded array access, rather than an escaped pool.
        prefix='(&spvDescriptorSet0.'+name+'->e[0])['
        indices=[];start=0
        while (at:=self.src.find(prefix,start))>=0:
            pos=at+len(prefix);level=1;end=pos
            while end<len(self.src) and level:
                level += (self.src[end]=='[')-(self.src[end]==']');end+=1
            if level:raise Unknown('unterminated pool index')
            expr=self.src[pos:end-1]
            suffix=', spvDescriptorSet0.'+name+'->n)'
            if not expr.startswith('min(') or not expr.endswith(suffix):raise Unknown('noncanonical pool bound')
            expr=expr[4:-len(suffix)]
            v=integer(Parser(self,expr).run())
            if v.ty!='u':raise Unknown('pool index not unsigned')
            indices.append(v.node);start=end
        uses=self.src.count('spvDescriptorSet0.'+name)
        if not indices or uses!=2*len(indices):raise Unknown('unrecognized pool use')
        return list(dict.fromkeys(indices))

class Parser:
    def __init__(self, analyzer, src):
        self.a=analyzer;self.t=[];self.i=0;pos=0
        while pos<len(src):
            m=TOK.match(src,pos)
            if not m:raise Unknown('token near '+src[pos:pos+40])
            self.t.append(m[1]);pos=m.end()
        if len(self.t)>256:raise Unknown('expression too large')
    def peek(self):return self.t[self.i] if self.i<len(self.t) else None
    def take(self,x=None):
        t=self.peek()
        if t is None or (x is not None and t!=x):raise Unknown('expected '+str(x)+' got '+str(t))
        self.i+=1;return t
    def run(self):
        v=self.expr()
        if self.peek() is not None:raise Unknown('trailing token '+self.peek())
        return v
    def expr(self, low=0):
        v=self.primary()
        while self.peek() in PREC and PREC[self.peek()]>=low:
            op=self.take();w=self.expr(PREC[op]+1);integer(v);integer(w)
            ty='u' if 'u' in (v.ty,w.ty) else 'i';name=OPS[op]
            if op in ('+','-','*','<','>','<=','>='):name+='_'+ty
            if op in ('<<','>>'):name+='_'+('i' if v.ty=='b' else v.ty);ty='i' if v.ty=='b' else v.ty
            if op in ('==','!=','<','>','<=','>=','&&','||'):ty='b'
            v=V(ty,(name,v.node,w.node))
        if low==0 and self.peek()=='?':
            self.take();integer(v);yes=self.expr();self.take(':');no=self.expr()
            if yes.ty!=no.ty:raise Unknown('mixed ternary types')
            v=V(yes.ty,('select',v.node,yes.node,no.node))
        return v
    def resource(self, packed=0):
        self.take('(');self.take('*');self.take('spvDescriptorSet0');self.take('.');name=self.take();self.take(')');self.take('.');self.take('_m0');self.take('[');idx=integer(self.expr());self.take(']')
        b=self.a.resources.get(name,{})
        if b.get('kind')=='cbuf':
            if packed:raise Unknown('packed constant load')
            self.take('.');c=self.take()
            if c not in ('x','y','z','w'):raise Unknown('constant swizzle')
            return V('f',('cb',b['reg'],idx.node,'xyzw'.index(c)))
        if b.get('kind')=='sbuf' and b.get('rw') is False:
            return V('v'+str(packed) if packed else 'u',('sb'+str(packed) if packed else 'sb',b['reg'],idx.node,0))
        raise Unknown('unsupported storage source '+name)
    def primary(self):
        t=self.take()
        if t in ('!','~','-','+'):
            v=integer(self.primary())
            if t=='+':return v
            return V('b' if t=='!' else v.ty,({'!':'not','~':'inv','-':'neg_'+v.ty}[t],v.node))
        if t=='*':
            for x in ('(','const','device'):self.take(x)
            ty=self.take()
            if ty not in ('packed_uint2','packed_uint3','packed_uint4'):raise Unknown('packed load type')
            for x in ('*',')','&'):self.take(x)
            v=self.resource(int(ty[-1]))
        elif t=='(':
            if self.t[self.i:self.i+2]==['*','spvDescriptorSet0']:self.i-=1;v=self.resource()
            else:v=self.expr();self.take(')')
        # A float select between raw material bits and +0.0 preserves those
        # bits. Keep its float type so numeric conversions/arithmetic still
        # fail closed; only a later explicit bitcast can use this as an index.
        elif t in ('0.0','0.0f','0.0F'):v=V('f',('const',0))
        elif re.fullmatch(r'(?:0x[\da-fA-F]+|\d+)[uU]?',t):
            digits=t.rstrip('uU')
            if len(digits)>1 and digits[0]=='0' and not digits.startswith('0x'):raise Unknown('leading-zero literal')
            n=int(digits,0) if t.startswith('0x') else int(digits)
            if not t.startswith('0x') and not t.endswith(('u','U')) and n>0x7fffffff:raise Unknown('wide signed decimal')
            if n>0xffffffff:raise Unknown('wide constant')
            v=const(n,'u' if t[-1:] in 'uU' or n>0x7fffffff else 'i')
        elif t in ('true','false'):v=const(t=='true','b')
        elif t=='as_type':
            self.take('<');ty=self.take();self.take('>');self.take('(');v=self.expr();self.take(')')
            if ty not in ('int','uint','float') or v.ty not in ('i','u','f'):raise Unknown('bitcast type')
            v=V({'int':'i','uint':'u','float':'f'}[ty],v.node)
        elif t in ('int','uint','bool'):
            self.take('(');v=integer(self.expr());self.take(')')
            if t=='bool':v=V('b',('ne',v.node,const(0).node))
            else:v=V('i' if t=='int' else 'u',v.node)
        elif t in ('min','max'):
            self.take('(');v=integer(self.expr());self.take(',');w=integer(self.expr());self.take(')')
            if v.ty!=w.ty:raise Unknown('mixed min types')
            v=V(v.ty,(t+'_'+v.ty,v.node,w.node))
        elif t.startswith('_'):v=self.a.variable(t)
        else:raise Unknown('unsupported name '+t)
        if self.peek()=='.':
            self.take();c=self.take()
            if v.ty not in ('v2','v3','v4') or len(c)!=1 or c not in 'xyzw'[:int(v.ty[1])]:raise Unknown('vector access '+v.ty+' .'+c)
            _,reg,idx,_=v.node;v=V('u',('sb',reg,idx,'xyzw'.index(c)))
        return v

def prove_pools(src, bindings):
    """Analyze each fragment entry independently; generated integer-output clones
    reuse local names. Include every entry and reject pool uses in helper code.
    """
    src=re.sub(r'/\*.*?\*/|//[^\n]*','',src,flags=re.S)
    blocks=[];spans=[]
    for m in re.finditer(r'\bfragment\b[^{;]*\{',src):
        start=m.end()-1;depth=1;end=start+1
        while depth and end<len(src):
            depth+=(src[end]=='{')-(src[end]=='}');end+=1
        if depth:raise Unknown('unclosed fragment function')
        blocks.append(Analyze(src[start:end],bindings));spans.append((start,end))
    if not blocks:blocks=[Analyze(src,bindings)];outside=''
    else:
        at=0;pieces=[]
        for a,b in spans:pieces.append(src[at:a]);at=b
        outside=''.join(pieces)+src[at:]
    out={}
    for binding in bindings:
        if binding['kind']!='tex_pool':continue
        name=binding['msl_name'];indices=[]
        try:
            if 'spvDescriptorSet0.'+name in outside:raise Unknown('pool referenced outside fragment entries')
            for block in blocks:
                if re.search(r'\bspvDescriptorSet0\b(?!\s*\.)', block.src):raise Unknown('argument buffer escaped')
                if 'spvDescriptorSet0.'+name not in block.src:continue
                for node in block.pool_indices(name):
                    if node not in indices:indices.append(node)
            if not indices:raise Unknown('no pool access')
            out[name]={'indices':indices}
        except Unknown as e:out[name]={'unknown':str(e)}
    return out


# Version 1 intentionally supports only this small subset. Adding an operation
# needs matching host semantics and tests, rather than silently accepting it.
OPS_V1 = {'const': 1, 'cb': 3, 'sb': 3, 'neg_i': 1, 'and': 2,
          'ne': 2, 'not': 1, 'add_i': 2, 'add_u': 2, 'select': 3,
          'shl_i': 2, 'shr_u': 2, 'min_u': 2}
MAX_NODES, MAX_ROOTS = 256, 128


def reflection(src, bindings):
    """Return a complete versioned proof, or None. No shader names/game rules.

    Each pool binding is accounted for, even when it shares an expression with
    another pool. Nodes form a bounded DAG; operands precede their users. Missing
    or unsupported facts do not make a shader fail to build.
    """
    pools = [b for b in bindings if b['kind'] == 'tex_pool']
    if not pools:
        return None
    src = re.sub(r'/\*.*?\*/|//[^\n]*', '', src, flags=re.S)
    if not re.search(r'\bfragment\b', src):
        return None
    # A helper's non-const reference parameter can change a local without an
    # assignment at its call site. Reject references globally; const entry ABI
    # references are safe. Output-argument builtins have no local declaration.
    refs = re.findall(r'\b((?:(?:const|thread|device|constant|threadgroup)\s+)+\w+|(?:bool|uint|int|float|packed_uint[234]))\s*&\s*\w+', src)
    if any('const' not in r.split() and 'constant' not in r.split() for r in refs):
        return None
    if re.search(r'\b(?:modf|frexp|lgamma_r|remquo|sincos|fract)\s*\(', src):
        return None
    # Resolve only the translator's canonical float4 uniform and uint storage
    # arrays. A different structure/layout must never acquire these byte offsets.
    resources = []
    for b in bindings:
        if b['kind'] in ('cbuf', 'sbuf'):
            ty = b.get('msl_type', '')
            m = re.fullmatch(r'(?:constant|const device)\s+(\w+)\s*\*', ty)
            elem = 'float4' if b['kind'] == 'cbuf' else 'uint'
            if not m or not re.search(r'\bstruct\s+' + re.escape(m[1]) +
                    r'\s*\{\s*' + elem + r'\s+_m0\[\d+\];\s*\}', src):
                continue
            if type(b.get('reg')) is not int or not 0 <= b['reg'] < 16:
                continue
        resources.append(b)
    try:
        proofs = prove_pools(src, resources)
        nodes, memo, outputs = [], {}, []

        def emit(n):
            if n in memo:
                return memo[n]
            op, args = n[0], n[1:]
            if op not in OPS_V1 or len(args) != OPS_V1[op]:
                raise Unknown('unsupported proof operation')
            values = [emit(a) if isinstance(a, tuple) else a for a in args]
            if len(nodes) >= MAX_NODES:
                raise Unknown('proof too large')
            idx = len(nodes)
            memo[n] = idx
            nodes.append([op, *values])
            return idx

        for b in pools:
            roots = [emit(n) for n in proofs.get(b['msl_name'], {}).get('indices', [])]
            if not roots or type(b.get('arg_id')) is not int:
                return None
            outputs.append({'binding': b['arg_id'], 'roots': roots})
        if sum(len(p['roots']) for p in outputs) > MAX_ROOTS:
            return None
        return {'version': 1, 'nodes': nodes, 'pools': outputs}
    except (Unknown, RecursionError):
        return None
