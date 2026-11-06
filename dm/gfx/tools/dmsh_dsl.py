"""data_sized_loop: the loops of a translated shader whose trip count comes from loaded data, for the build's automatic loop guard (dmsh_loopdiag.py auto, DMSH_LOOPGUARD=auto).

Vendored from sw2_tools/xcheck/dsl.py (the static.py rule `data_sized_loop`, 2026-11-05) with the MSL helpers it takes from static.py, so the build does not depend on
the checker; the analysis is the same (sw2_tools/xcheck/work/loopguard/vendor_check.py compares the two). Additions: each loop's guard line (site_line: where loop_guard
put its `++dm_lg < LOOP_CAP` test, the header of a for / while loop, the closing `} while` of a do loop), the bound's full text (bound_full), the loop kind (loop), and
data_loops(..., cbuf_unbounded=True) also returns loops whose bound comes only from constant buffers but that the clamps leave unbounded (why = 'cbuf').

What the checker's docstring says:
data_sized_loop (static.py rule, 2026-11-05): loops whose trip count derives from loaded data (device / pool-table / image / threadgroup reads)
rather than constants, uniforms (constant buffers) or the thread / group size, with a worst-case estimate from the clamps around the bound.

The pattern that locked up PRAGMATA's GPU at the tower ring: the software triangle rasterizer 26166d37ed56d6bb loops
`for y in [max(0, floor(ymin)), min(ceil(ymax), c3[23].y))`, `for x in [... min(ceil(xmax), c3[23].x))` with an image atomic per pixel: both
bounds come from vertex data, clamped only to the viewport, so a screen-sized triangle costs one thread W x H iterations. And OoT's agl task
shaders (99280b7b / 47f9b384): a count read from a pool lookup that returns 0 on a miss underflows to ~2^27 iterations.

Analysis, per MSL function (SPIRV-Cross output: one statement a line, SSA-like names with phi copies):
  1. loops (static.msl_loops) and their exits (the continue condition and the conditions of the loop's own breaks);
  2. the loop's counters (static._induction_vars: a copy / +-constant cycle of the loop's assignments) and the compares between a counter and a
     loop-invariant side (the bound). Step > 0: the bound is the compare's other side; step < 0: the counter's initial value;
  3. the sources of the bound and of the counter's start, through every assignment of the function: data (device buffer, raw global memory
     through a pool-table helper, image read, threadgroup array, a device atomic's return), uniform (constant buffer), thread / group ids,
     constants. A loop is reported when data reaches its bound or start. A loop with no counter whose exit depends on data (a pointer chase,
     a poll) is reported as kind 'exit' with an unbounded estimate;
  4. an upper bound of the bound expression: min / max / clamp / casts / + * >> & %, a constant-buffer word = UNIFORM_EST (a screen-sized
     value: viewports, target sizes), an image's get_width / get_height = IMGDIM_EST, a thread / group id = TID_EST, data = unbounded.
     The nest estimate multiplies the loop's estimate by those of the enclosing data-sized loops.
Ranking: nest estimate, with an unbounded loop counted as UNBOUNDED_RANK (below a clamped screen-sized 2D nest: an unclamped count read from
memory is what every list walk does; the 2D nest clamped only to the viewport is the runaway shape), then atomics in the body.
"""
import re, struct, time, bisect
from typing import Any, cast

NO_DEADLINE = float('inf')

UNIFORM_EST = 2048      # a bound clamped to a constant-buffer value (assumed screen-sized: viewport, render target)
IMGDIM_EST = 4096       # clamped to an image's width / height / depth
TID_EST = 1024          # a thread / group id or size
INF = float('inf')
U32 = float(1 << 32)
UNBOUNDED_RANK = 2.0 ** 20
MASK_EST = 2.0 ** 24     # above this, an estimate from bit masks / shifts of data ranks as unbounded     # ranking weight of one unbounded data loop (est = inf): ~ a 1024 x 1024 clamped nest
DATA = frozenset(('dev', 'gmem', 'img', 'shared', 'atomic'))

_DEVRD = re.compile(r'\(\*(?:spvDescriptorSet\d+\.)?cp_s\d+\)\._m0\[|\bcp_s\d+\._m0\[|\bcp_s\d+->_m0\[|\bcp_s\d+\)\._m0\[')
_CBUF = re.compile(r'\(\*(?:spvDescriptorSet\d+\.)?(cp_c\d+)\)\._m0\[(\d+)\]((?:\.[xyzw]+)?)|\b(cp_c\d+)(?:\.|->)_m0\[(\d+)\]((?:\.[xyzw]+)?)')
_IMGRD = re.compile(r'\.(?:read|sample\w*|gather\w*)\(')
_IMGAT = re.compile(r'\.atomic_\w+\(')
_DEVAT = re.compile(r'\batomic_\w+_explicit\(\(device ')
_IMGDIM = re.compile(r'\.get_(?:width|height|depth|array_size|num_mip_levels|num_samples)\(')
_ID = re.compile(r'\b(_\w+|dm_\w+)\b(?!\s*\()')
_TID = re.compile(r'\bgl_\w+')
_CALL = re.compile(r'\b(_\w+)\(')
_NORM = re.compile(r'\b(as_type|static_cast|reinterpret_cast)<[^<>()]*>')


def pretty_cbuf(t):
    m = _CBUF.search(t)
    if not m: return t
    b = m.group(1) or m.group(4); i = m.group(2) or m.group(5); sw = m.group(3) or m.group(6) or ''
    return f'{b[3:]}[{i}]{sw}'


# ---- a small C-expression parser (enough of SPIRV-Cross's MSL)
_TOKRE = re.compile(r'\s*(?:(?P<num>0x[0-9a-fA-F]+u?|\d+\.\d*(?:[eE][-+]?\d+)?f?|\d*\.\d+(?:[eE][-+]?\d+)?f?|\d+[eE][-+]?\d+f?|\d+u?)'
                    r'|(?P<id>[A-Za-z_]\w*(?:::\w+)*)|(?P<op><<|>>|<=|>=|==|!=|&&|\|\||->|[-+*/%&|^~!<>?:(),\[\].]))')


def tokens(s):
    s = _NORM.sub(r'\1', s); out = []; p = 0
    while p < len(s):
        m = _TOKRE.match(s, p)
        if not m or m.end() == p:
            if s[p:].strip() == '': break
            raise ValueError('token at ' + s[p:p + 20])
        p = m.end()
        if m.group('num') is not None:
            t = m.group('num').rstrip('uf')
            out.append(('num', float(int(t, 16)) if t.startswith('0x') else float(t)))
        elif m.group('id') is not None: out.append(('id', m.group('id')))
        else: out.append(('op', m.group('op')))
    return out


_BIN = [('||',), ('&&',), ('|',), ('^',), ('&',), ('==', '!='), ('<', '<=', '>', '>='), ('<<', '>>'), ('+', '-'), ('*', '/', '%')]


class _P:
    def __init__(self, toks): self.t = toks; self.i = 0

    def peek(self): return self.t[self.i] if self.i < len(self.t) else ('end', None)

    def eat(self, v=None):
        t = self.peek()
        if v is not None and t != ('op', v): raise ValueError(f'expected {v}, got {t}')
        self.i += 1; return t

    def expr(self):
        c = self.binary(0)
        if self.peek() == ('op', '?'):
            self.eat(); a = self.expr(); self.eat(':'); b = self.expr(); return ('tern', c, a, b)
        return c

    def binary(self, lv):
        if lv == len(_BIN): return self.unary()
        a = self.binary(lv + 1)
        while self.peek()[0] == 'op' and self.peek()[1] in _BIN[lv]:
            op = self.eat()[1]; b = self.binary(lv + 1); a = ('bin', op, a, b)
        return a

    def unary(self):
        t = self.peek()
        if t[0] == 'op' and t[1] in ('-', '!', '~', '*', '&', '+'):
            self.eat(); return ('un', t[1], self.unary())
        return self.postfix(self.primary())

    def primary(self):
        t = self.eat()
        if t[0] == 'num': return ('num', t[1])
        if t[0] == 'id': return ('id', t[1])
        if t == ('op', '('):
            e = self.expr(); self.eat(')'); return e
        raise ValueError(f'unexpected {t}')

    def postfix(self, e):
        while True:
            t = self.peek()
            if t == ('op', '('):
                self.eat(); args = []
                if self.peek() != ('op', ')'):
                    args.append(self.expr())
                    while self.peek() == ('op', ','): self.eat(); args.append(self.expr())
                self.eat(')'); e = ('call', e, args)
            elif t == ('op', '['):
                self.eat(); ix = self.expr(); self.eat(']'); e = ('idx', e, ix)
            elif t in (('op', '.'), ('op', '->')):
                self.eat(); f = self.eat(); e = ('mem', e, f[1])
            else: return e


import functools


@functools.lru_cache(maxsize=65536)
def parse(s):
    p = _P(tokens(s)); e = p.expr()
    if p.i != len(p.t): raise ValueError('trailing tokens')
    return e


def unparse(e):
    k = e[0]
    if k == 'num': return ('%g' % e[1])
    if k == 'id': return e[1]
    if k == 'bin': return f'({unparse(e[2])} {e[1]} {unparse(e[3])})'
    if k == 'un': return e[1] + unparse(e[2])
    if k == 'tern': return f'({unparse(e[1])} ? {unparse(e[2])} : {unparse(e[3])})'
    if k == 'call': return unparse(e[1]) + '(' + ', '.join(unparse(a) for a in e[2]) + ')'
    if k == 'idx': return f'{unparse(e[1])}[{unparse(e[2])}]'
    if k == 'mem': return f'{unparse(e[1])}.{e[2]}'
    return '?'


def _fname(e):
    """the plain name of a call's callee (min, precise::min, int, ...), or the method name of x.get_width()"""
    if e[0] == 'id': return e[1].split('::')[-1]
    if e[0] == 'mem': return '.' + e[2]
    return None


_CASTS = {'int', 'uint', 'float', 'half', 'short', 'ushort', 'char', 'uchar', 'long', 'ulong', 'as_type', 'static_cast', 'trunc', 'floor', 'ceil',
          'round', 'rint', 'saturate'}


class Func:
    """one function: its assignments (name -> rhs list), threadgroup arrays, the helpers that read memory"""
    def __init__(self, body, tg, data_helpers, deadline=NO_DEADLINE):
        self.body = body; self.tg = tg; self.dh = data_helpers
        self.asg = _assigns(body)
        self.defs = {}
        self.at = {}; self.pos = [p for l, r, p in self.asg]   # by name: (rhs, position) in source order; positions (sorted) for slicing a loop's assignments
        for l, r, p in self.asg: self.at.setdefault(l, []).append((r, p))
        for l, r, p in self.asg: self.defs.setdefault(l, []).append(r)
        self.src_memo = {}; self.ub_memo = {}; self.z_memo = {}; self.deadline = deadline
        self.types = {n: t for t, n in re.findall(r'\b(int|uint|float|bool|half)\s+(_\w+)\s*=', body)}
        self.tg_re = re.compile(r'\b(?:' + '|'.join(map(re.escape, sorted(tg))) + r')\[') if tg else None

    def kinds_of_text(self, r):
        k = set()
        if _DEVRD.search(r): k.add('dev')
        if _DEVAT.search(r): k.add('atomic')
        if _IMGRD.search(r) or _IMGAT.search(r): k.add('img')
        if self.tg_re is not None and self.tg_re.search(r): k.add('shared')
        if 'cp_c' in r: k.add('cbuf')
        if _IMGDIM.search(r): k.add('imgdim')
        if _TID.search(r): k.add('tid')
        for m in _CALL.finditer(r):
            if m.group(1) in self.dh: k.add('gmem' if self.dh[m.group(1)] == 'gmem' else 'dev')
        return k

    def sources(self, text, limit=6000):
        """source kinds of an expression through the function's assignments (BFS; memo by expression text)"""
        if text in self.src_memo: return set(self.src_memo[text])
        r = self._sources(text, limit); self.src_memo[text] = r
        return set(r)

    def _sources(self, text, limit):
        out = self.kinds_of_text(text); seen = set(); todo = list(set(_ID.findall(text)))
        while todo and len(seen) < limit:
            if len(seen) & 255 == 0: self.tick()
            x = todo.pop()
            if x in seen: continue
            seen.add(x)
            for r in self.defs.get(x, ()):
                out |= self.kinds_of_text(r)
                for y in _ID.findall(r):
                    if y not in seen: todo.append(y)
        if todo: out.add('deep')
        return out

    # ---- the value with every loaded word 0 (a pool-table miss): the zero-input trip count (OoT 99280b7b: (min(n, 256) - 1) >> 5 = 2^27 - 1)
    def tick(self):
        """the analysis budget (data_loops budget=): over it, TimeoutError (34 of Orbitals' compute kernels took over 30 s each in the checker)"""
        if self.deadline != NO_DEADLINE and time.monotonic() > self.deadline: raise TimeoutError('data_sized_loop: over its budget')

    def zval_var(self, name, depth, stack):
        if name not in self.z_memo: self.tick()
        if name in self.z_memo: return self.z_memo[name]
        if name in stack or depth > 64: return None
        rs = self.defs.get(name)
        if not rs: return None
        stack.add(name); vals = []
        for r in rs:          # phi copies: known only when every definition gives the same value (which one reaches the loop is control flow:
            try: vals.append(self.zval(parse(r), depth + 1, stack))   # Orbitals 09c51649's entry value 0x3f000000u is replaced in a branch)
            except (ValueError, RecursionError, ZeroDivisionError, OverflowError): vals.append(None)
        v = vals[0] if vals and all(x is not None and x == vals[0] for x in vals) else None
        stack.discard(name)
        t = self.types.get(name)
        if v is not None and t in ('int', 'uint'): v = _wrap(v, t)
        self.z_memo[name] = v
        return v

    def zval(self, e, depth=0, stack=None):
        """concrete value (Python int or float) with data = 0, ids = 0; None when unknown (constant buffers, parameters)"""
        if stack is None: stack = set()
        k = e[0]
        if k == 'num': return int(e[1]) if float(e[1]).is_integer() and e[1] >= 0 and not isinstance(e[1], bool) else e[1]
        if k == 'id':
            n = e[1]
            if n.startswith(('gl_', 'dm_')): return 0
            if n == 'true': return 1
            if n == 'false': return 0
            return self.zval_var(n, depth, stack)
        if k in ('idx', 'mem'):
            t = unparse(e)
            if 'cp_c' in t: return None      # a uniform: unknown (a made-up value bit-cast to an int gave PRAGMATA a320142c a 1.1G 'trip')
            if k == 'mem' and re.fullmatch(r'[xyzw]{1,4}|[rgba]{1,4}', e[2]):
                return self.zval(e[1], depth + 1, stack) if e[1][0] == 'id' else 0
            return 0          # a load: 0
        if k == 'tern':
            c = self.zval(e[1], depth + 1, stack)
            if c is None: return None
            return self.zval(e[2] if c else e[3], depth + 1, stack)
        if k == 'un':
            a = self.zval(e[2], depth + 1, stack)
            if a is None: return None
            if e[1] == '-': return -a
            if e[1] == '!': return int(not a)
            if e[1] == '~': return ~int(a)
            return a
        if k == 'bin':
            a = self.zval(e[2], depth + 1, stack)
            if a is None: return None
            if e[1] == '&&' and not a: return 0
            if e[1] == '||' and a: return 1
            b = self.zval(e[3], depth + 1, stack)
            if b is None: return None
            op = e[1]
            if op in ('&&', '||'): return int(bool(b))
            if op == '+': return a + b
            if op == '-': return a - b
            if op == '*': return a * b
            if op == '/': return (a / b if isinstance(a, float) or isinstance(b, float) else int(a / b)) if b else 0
            if op == '%': return (a % b) if b else 0
            if op in ('<<', '>>', '&', '|', '^'):
                a, b = int(a), int(b)
                if op == '<<': return (a << (b & 31)) & 0xffffffff
                if op == '>>': return a >> (b & 31)
                return a & b if op == '&' else a | b if op == '|' else a ^ b
            return int({'<': a < b, '<=': a <= b, '>': a > b, '>=': a >= b, '==': a == b, '!=': a != b}[op])
        if k == 'call':
            f = _fname(e[1])
            if f in self.dh: return 0      # a pool-table / memory helper: a miss reads 0
            args = [self.zval(x, depth + 1, stack) for x in e[2]]
            if f and f.startswith('.'):
                return IMGDIM_EST if f.startswith('.get_') else 0      # an image read: 0
            if any(x is None for x in args): return None
            if f in ('min', 'fmin'): return min(args)
            if f in ('max', 'fmax'): return max(args)
            if f == 'clamp' and len(args) == 3: return min(max(args[0], args[1]), args[2])
            if f in ('uint', 'int', 'ushort', 'short', 'uchar', 'char') and len(args) == 1:
                x = args[0]
                return _wrap(int(x) if not isinstance(x, float) else (max(0, int(x)) if f.startswith('u') else int(x)), 'uint' if f.startswith('u') else 'int')
            if f in ('float', 'half') and len(args) == 1: return float(args[0])
            if f == 'as_type' and len(args) == 1:
                x = args[0]
                if isinstance(x, float) and x != 0.0: return None     # a computed float's bit pattern as a count (Orbitals 09c51649: 0.5f = 1.06e9): no claim
                return x if not isinstance(x, float) else struct.unpack('<I', struct.pack('<f', x))[0] if abs(x) < 3e38 else None
            if f in ('trunc', 'floor', 'ceil', 'round', 'rint', 'abs', 'saturate') and len(args) == 1:
                import math
                x = float(args[0])
                return {'trunc': math.trunc, 'floor': math.floor, 'ceil': math.ceil, 'round': round, 'rint': round, 'abs': abs,
                        'saturate': lambda v: min(1.0, max(0.0, v))}[f](x) * 1.0
            return None
        return None

    # ---- an upper bound of a value: (bound, label); label names the clamp that set it ('' = a constant)
    def ub_var(self, name, depth, stack):
        if name not in self.ub_memo: self.tick()
        if name in self.ub_memo: return self.ub_memo[name]
        if name in stack or depth > 48: return INF, 'loop-carried' if name in stack else 'deep'
        rs = self.defs.get(name)
        if not rs: return INF, 'param'
        stack.add(name); best = (-INF, '')
        for r in rs:
            try: v = self.ub(parse(r), depth + 1, stack)
            except (ValueError, RecursionError): v = (INF, 'unparsed')
            if v[0] > best[0]: best = v
            if best[0] == INF: break
        stack.discard(name)
        self.ub_memo[name] = best
        return best

    def ub(self, e, depth=0, stack=None):
        if stack is None: stack = set()
        k = e[0]
        if k == 'num': return e[1], ''
        if k == 'id':
            n = e[1]
            if n.startswith('gl_'): return TID_EST, n
            if n in ('true', 'false'): return 1, ''
            return self.ub_var(n, depth, stack)
        if k in ('idx', 'mem'):
            t = unparse(e)
            if 'cp_c' in t: return UNIFORM_EST, pretty_cbuf(_cbuf_text(e))
            if k == 'mem' and re.fullmatch(r'[xyzw]{1,4}|[rgba]{1,4}', e[2]): return self.ub(e[1], depth + 1, stack)
            return INF, 'data' if (_DEVRD.search(t) or (self.tg_re and self.tg_re.search(t))) else 'mem'
        if k == 'tern':
            a = self.ub(e[2], depth + 1, stack); b = self.ub(e[3], depth + 1, stack)
            return a if a[0] >= b[0] else b
        if k == 'un':
            if e[1] in ('!',): return 1, ''
            if e[1] in ('*', '+', '&'): return self.ub(e[2], depth + 1, stack)
            if e[1] == '-' and e[2][0] == 'num': return -e[2][1], ''
            return U32, 'neg'           # -x as a uint (wraps): up to 2^32
        if k == 'bin':
            op = e[1]
            if op in ('||', '&&', '==', '!=', '<', '<=', '>', '>='): return 1, ''
            a = self.ub(e[2], depth + 1, stack)
            if op == '-': return a
            k_ = _const(e[3])
            if op in ('>>', '/') and k_ is not None and k_ >= 1:
                return (a[0] / (2 ** k_ if op == '>>' else k_)), a[1] or op
            b = self.ub(e[3], depth + 1, stack)
            if op == '&': return a if a[0] <= b[0] else b
            if op == '%': return b
            if op == '+': return min(a[0] + b[0], U32), a[1] or b[1]      # uint wraps: x - 1 is written x + 4294967295u (0 - 1 = 2^32 - 1)
            if op == '*': return (a[0] * b[0] if a[0] >= 0 and b[0] >= 0 else INF), a[1] or b[1]
            if op == '<<': return (a[0] * 2 ** b[0] if b[0] < 64 else INF), a[1]
            if op in ('|', '^'): return (a[0] + b[0]), a[1] or b[1]
            return INF, op
        if k == 'call':
            f = _fname(e[1]); args = e[2]
            if f in ('min', 'fmin'):
                vs = [self.ub(a, depth + 1, stack) for a in args]; return min(vs, key=lambda v: v[0])
            if f in ('max', 'fmax', 'select'):
                vs = [self.ub(a, depth + 1, stack) for a in args[:2]]; return max(vs, key=lambda v: v[0])
            if f == 'clamp' and len(args) == 3:
                a = self.ub(args[0], depth + 1, stack); b = self.ub(args[2], depth + 1, stack); return a if a[0] <= b[0] else b
            if f in _CASTS or (f and re.fullmatch(r'u?(?:int|short|char)[234]?|float[234]?|half[234]?', f)):
                vs = [self.ub(a, depth + 1, stack) for a in args] or [(INF, 'cast')]; return max(vs, key=lambda v: v[0])
            if f and f.startswith('.get_'): return IMGDIM_EST, unparse(e[1]).split('.')[-1] + '()'
            if f == 'abs' and args and args[0][0] == 'num': return abs(args[0][1]), ''
            return INF, f'call {f}'
        return INF, '?'


ZERO_HAZARD = 1 << 16   # a zero-input trip count at least this: the loop runs away when its data reads 0


def _wrap(v, t):
    v = int(v) & 0xffffffff
    return v - (1 << 32) if t == 'int' and v >= 1 << 31 else v


def _const(e):
    """the value of a literal, or of a cast of one (uint(5)), else None"""
    while e[0] == 'call' and _fname(e[1]) in _CASTS and len(e[2]) == 1: e = e[2][0]
    return e[1] if e[0] == 'num' else None


def _cbuf_text(e):
    """the MSL text of a constant-buffer access node, for pretty_cbuf"""
    t = unparse(e)
    # unparse gives (*spvDescriptorSet0.cp_c3)._m0[23].y as *spvDescriptorSet0.cp_c3._m0[23].y; rebuild the form pretty_cbuf reads
    m = re.search(r'(cp_c\d+)\._m0\[(\d+)\]((?:\.[xyzw]+)?)', t)
    return f'{m.group(1)}._m0[{m.group(2)}]{m.group(3)}' if m else t


def _if_blocks(body):
    """[(cond, open, close, is_else, only_returns)] of every if block and else block"""
    out = []
    for m in re.finditer(r'\bif\s*\(', body):
        p = m.end() - 1; e = _bal(body, p); cond = body[p + 1:e - 1]
        t = re.match(r'\s*\{', body[e:])
        if not t: continue
        o = e + t.end() - 1; c = _bal(body, o, '{', '}'); inner = body[o + 1:c - 1].strip()
        out.append((cond, o, c, False, bool(re.fullmatch(r'(?:return[^;]*;\s*)+', inner))))
        el = re.match(r'\s*else\s*\{', body[c:])
        if el:
            o2 = c + el.end() - 1; c2 = _bal(body, o2, '{', '}'); out.append((cond, o2, c2, True, False))
    return out


def _reached_zero(F, ifs2, at):
    """False when, with every load reading 0, the code at `at` cannot run: an enclosing if (else) whose condition is false (true), or an
    if-block before it that only returns and whose condition is true. Unknown conditions do not decide"""
    for cond, o, c, is_else, only_ret in ifs2:
        if o < at < c:
            try: v = F.zval(parse(cond))
            except (ValueError, RecursionError, ZeroDivisionError, OverflowError, TypeError): v = None
            if v is not None and bool(v) == is_else: return False
        elif only_ret and c <= at:
            try: v = F.zval(parse(cond))
            except (ValueError, RecursionError, ZeroDivisionError, OverflowError, TypeError): v = None
            if v is not None and v: return False
    return True


def _own(loops, o, c):
    inner = [(o2, c2) for k2, s2, o2, c2, _ in loops if o < o2 and c2 <= c]
    return inner


def data_loops(src, gmem_bufs=(), cbuf_unbounded=False, budget=None):
    """[dict(fn, line, kind, est, nest, bound, start, sources, atomic, depth, site_line, bound_full: the bound's expression, the start's for a count down, '' for an exit loop, loop, why)] of the data-sized loops of an MSL source. gmem_bufs: the MSL names
    of the pool-table (gmem_map) bindings, from the json: helpers that read them are 'gmem' sources. cbuf_unbounded: also the loops bounded by constant-buffer words only
    whose estimate is 2^31 or more (`n - 1` of a uniform n that may be 0 wraps). budget: seconds, then TimeoutError (None: no limit)"""
    deadline = time.monotonic() + budget if budget is not None else NO_DEADLINE
    fns = functions_fast(src)
    if not fns: return []
    tg = set(re.findall(r'^\s*threadgroup\s+[\w:<>, ]+?\s+(_\w+)(?:\[[^\]]*\])?\s*;', src, re.M))
    tg |= set(re.findall(r'\bthreadgroup\s+[\w:<>, ]+?&\s*(_\w+)', src))      # threadgroup arrays passed to a helper (dm_stage_body)
    # helpers that return memory reads: the pool-table ones (gmem / reinterpret_cast<device) and any other with a device / image read
    dh = {}
    for n, b in fns:
        if not n.startswith('_') or not re.search(r'\breturn\b\s*[^;\s]', b): continue
        if 'gmem' in b or 'reinterpret_cast<device' in b or any(re.search(r'\b' + g + r'\b', b) for g in gmem_bufs): dh[n] = 'gmem'
        elif _DEVRD.search(b) or _IMGRD.search(b): dh[n] = 'dev'
    out = []
    for name, body in fns:
        if 'do' not in body and 'for' not in body and 'while' not in body: continue
        loops = msl_loops(body)
        if not loops: continue
        F = Func(body, tg, dh, deadline)
        base = src[:src.find(body)].count('\n')
        ifs = _if_spans(body); sw = _switch_spans(body)
        ifs2 = _if_blocks(body)
        res = []
        for li, (k, s, o, c, cond) in enumerate(loops):
            F.tick()
            lb = body[o:c]
            la = F.asg[bisect.bisect_left(F.pos, o):bisect.bisect_left(F.pos, c)]   # = [x for x in F.asg if o <= x.p < c] (F.asg is in source order)
            defs = {}
            for l, r, p in la: defs.setdefault(l, []).append(r)
            inner = _own(loops, o, c)
            own = {l for l, r, p in la if not any(o2 <= p < c2 for o2, c2 in inner)}
            ivs = _induction_vars(defs) & own
            # exit expressions: the continue condition; the conditions of this loop's own breaks
            exits = [cond] if cond else []
            for m in re.finditer(r'\bbreak\b\s*;', lb):
                p = o + m.start()
                if any(o2 < p < c2 for o2, c2 in inner + sw): continue
                for cnd, a, z in ifs:
                    if o < a < p < z: exits.append(cnd)
            if not exits: continue
            dep = {l: set(_ID.findall(' '.join(rs))) for l, rs in defs.items()}

            def closure(ids, dep=dep):
                seen = set(ids); todo = list(ids)
                while todo:
                    x = todo.pop()
                    for y in dep.get(x, ()):
                        if y not in seen: seen.add(y); todo.append(y)
                return seen
            # compares between a counter and a loop-invariant side, in the exits' slice through the loop's assignments
            slice_ = closure(set().union(*[set(_ID.findall(x)) for x in exits]))
            texts = list(exits) + [r for l in slice_ for r in defs.get(l, ())]
            cmps = []
            for t in texts:
                try: e = parse(t)
                except (ValueError, RecursionError): continue
                stack: list[Any] = [e]
                while stack:
                    x = stack.pop()
                    if not isinstance(x, tuple): continue
                    if x[0] == 'bin' and x[1] in ('<', '<=', '>', '>=', '!=', '=='):
                        L, R = unparse(x[2]), unparse(x[3])
                        li_ = closure(set(_ID.findall(L))) & ivs; ri_ = closure(set(_ID.findall(R))) & ivs
                        if li_ and not ri_: cmps.append((sorted(li_)[0], R, x[1], 'L'))
                        elif ri_ and not li_: cmps.append((sorted(ri_)[0], L, x[1], 'R'))
                    for y in x[1:]:
                        if isinstance(y, tuple): stack.append(y)
                        elif isinstance(y, list): stack += y
            loc = base + body[:s].count('\n') + 1
            atom = 'img atomic' if _IMGAT.search(lb) else 'atomic' if _DEVAT.search(lb) else ''
            if cmps:
                best = None
                for iv, bound, op, side in cmps:
                    F.tick()
                    # the counter's step sign and its start (its definitions outside the loop)
                    step = 0
                    for r in defs.get(iv, ()) + [r2 for l2 in closure({iv}) for r2 in defs.get(l2, ())]:
                        m = re.search(r'([+-])\s*\(?(-?\d+)u?\)?\s*\)*$', r.strip())
                        if m and _ID.search(r):
                            v = int(m.group(2)); v = v - (1 << 32) if v >= 1 << 31 else v
                            v *= 1 if m.group(1) == '+' else -1
                            if v: step = v; break
                    starts = [r for l2 in closure({iv}) for r, p in F.at.get(l2, ()) if not (o <= p < c)]
                    start = ' | '.join(sorted(set(starts)))[:160]
                    bsrc = F.sources(bound); ssrc = F.sources(start) if start else set()
                    if step < 0:
                        try: est = max((F.ub(parse(x)) for x in starts), key=lambda v: v[0]) if starts else (INF, 'start')
                        except (ValueError, RecursionError): est = (INF, 'unparsed')
                    else:
                        try: est = F.ub(parse(bound))
                        except (ValueError, RecursionError): est = (INF, 'unparsed')
                    # the trip count when every load reads 0: bound - start (step > 0), start - bound (step < 0), over the step
                    zt = None
                    try:
                        zb = F.zval(parse(bound)); zs = F.zval(parse(starts[0])) if starts else 0
                        if zb is not None and zs is not None:
                            zt = (zb - zs if step >= 0 else zs - zb) / max(1, abs(step) if step else 1)
                            if op in ('<=', '>=') and step >= 0 and side == 'L' and op == '<=' or (op == '>' and side == 'L'): zt += 1
                            zt = max(0, zt)
                            # reached with zero loads at all? An earlier `if (c) { return; }` whose c holds, or an enclosing if / else whose
                            # condition fails, keeps the loop from running (PRAGMATA 37c23a54: `if (!(flags & 2)) return;` before n - 2)
                            if zt and not _reached_zero(F, ifs2, s): zt = 0
                    except (ValueError, RecursionError, ZeroDivisionError, OverflowError, TypeError): zt = None
                    cand = dict(kind='count', bound=bound.strip()[:160], start=start, op=op, step=step, est=est[0], clamp=est[1], zero_trip=zt,
                                value_expr=bound.strip() if step >= 0 else starts[0].strip() if len(set(starts)) == 1 else '',
                                sources=sorted((bsrc | ssrc) - {'deep'}), data=bool((bsrc | ssrc) & DATA))
                    if best is None or (cand['data'], cand['est']) > (best['data'], best['est']): best = cand
                # several exits: the loop ends at the first one reached, but which compare decides an exit through flag words (OoT 99280b7b:
                # bit 0 of `(i*32 + lane >= n) | (i + 1 > m) << 1`, masked & 2) is not tracked: the largest estimate is taken (worst case)
                r = cast(dict[str, Any], best)   # set: cmps is not empty
            else:
                # a loop-carried value halved each iteration (x = x >> k, x / 2^k) that the exit tests: a binary search (the translator's pool-table
                # lookup), at most 32 iterations
                if any(re.search(r'>>\s*(?:uint\()?[1-9]\d*u?\)?\s*\)*$', r2.strip()) for l2 in slice_ for r2 in defs.get(l2, ())): continue
                # a CAS-retry (lock-free update: the exit is the compare-exchange's own success) retries only on contention
                if any('atomic_compare_exchange' in x for x in exits) or any('atomic_compare_exchange' in r2 for l2 in slice_ for r2 in defs.get(l2, ())):
                    continue
                srcs = set()
                for x in exits: srcs |= F.sources(x)
                r = cast(dict[str, Any], dict(kind='exit', bound=' || '.join(x.strip() for x in exits)[:160], start='', op='', step=0, est=INF, clamp='exit on data', zero_trip=None,
                         sources=sorted(srcs - {'deep'}), data=bool(srcs & DATA)))
            r.update(fn=name, line=loc, atomic=atom, span=(o, c), li=li, loop=k, site_line=base + body[:c].count('\n') + 1 if k == 'do' else loc,
                     bound_full=r.pop('value_expr', ''))
            r['why'] = 'data' if r['data'] else 'cbuf' if cbuf_unbounded and 'cbuf' in r['sources'] and r['est'] >= U32 / 2 else ''
            res.append(r)
        # nesting: the product with the enclosing data-sized loops
        for r in res:
            if not r['why']: continue
            n = r['est']; d = 0
            for q in res:
                if q is r or not q['data']: continue
                if q['span'][0] < r['span'][0] and r['span'][1] <= q['span'][1]: n *= q['est']; d += 1
            r['nest'] = n; r['depth'] = d
            out.append(r)
    for r in out: r.pop('span', None); r.pop('li', None)
    return out


def rank_key(r):
    """(class, weight, depth). Class 2: the zero-input trip count is >= ZERO_HAZARD (the loop runs away when its loads read 0: OoT's agl task
    shaders); 1: a finite estimate from clamps (26166d37: the viewport); 0: unbounded (a count read from memory, unclamped: what every list walk
    does, bounded by the game's data). Within a class: the nest estimate, x2 with an atomic in the body"""
    at = 2 if 'atomic' in r['atomic'] else 1
    zt = r.get('zero_trip')
    if zt is not None and zt >= ZERO_HAZARD: return (2, zt * at, r['depth'])
    n = r['nest']
    # an estimate above 2^24 that no uniform or image size set (a bit field of a loaded word: & 0x3ffffff, >> 4) is no clamp either
    if r['est'] > MASK_EST and not re.match(r'c\d+\[|get_', r.get('clamp') or ''): n = U32
    if n >= U32 / 2 or r['est'] >= U32 / 2: return (0, (r['nest'] / r['est'] if r['est'] not in (0, INF) and r['nest'] != INF else 1) * at, r['depth'])
    return (1, n * at, r['depth'])


def fmt_est(v):
    if v == INF: return 'unbounded'
    if v >= U32: return '2^32'
    if v >= U32 / 2: return '>=2^31'
    if v >= 1e6: return f'{v / 1e6:.1f}M'
    if v >= 1e4: return f'{v / 1e3:.0f}k'
    return f'{v:.0f}'


def rule(src, gmem_bufs=()):
    ls = data_loops(src, gmem_bufs)
    if not ls: return []
    ls.sort(key=rank_key, reverse=True)
    def one(r):
        cl = f' clamp {r["clamp"]}' if r['clamp'] else ''
        if r.get('zero_trip') is not None and r['zero_trip'] >= ZERO_HAZARD: cl += f' ZERO-INPUT trip {fmt_est(r["zero_trip"])}'
        return (f'{r["fn"]}:{r["line"]} {r["kind"]} est {fmt_est(r["est"])}{cl} nest {fmt_est(r["nest"])}'
                + (f' {r["atomic"]}' if r['atomic'] else '') + f' [{",".join(s for s in r["sources"] if s in DATA)}]')
    return [('data_sized_loop', f'{len(ls)} loop(s): ' + '; '.join(one(r) for r in ls[:3]) + (f' (+{len(ls) - 3})' if len(ls) > 3 else ''))]


# ---- MSL structure helpers (from sw2_tools/xcheck/static.py)
_ASSIGN = re.compile(r'^\s*(?:(?:const\s+)?(?:threadgroup\s+)?[A-Za-z_][\w:<>]*(?:\s*\*)?\s+)?(_\w+|dm_\w+)((?:\.\w+|\[[^\]]*\])*)\s*([+\-*/|&^]?=)(?!=)\s*(.*);\s*$')
_SIMPLE = re.compile(r'^(?:(?:int|uint|as_type<\w+>)\()*\s*(_\w+)\s*\)*(?:\s*([+-])\s*\(?(-?\d+)u?\)?)?\s*\)*$')


_BRACKETS = {}


def _bal(src, i, o='(', c=')'):
    """index after the bracket matching the one at i (from bracket to bracket: the checker's character loop took 27 s on OoT's 7,556-line 1f78730053feb036)"""
    pat = _BRACKETS.get(o + c)
    if pat is None: pat = _BRACKETS[o + c] = re.compile(re.escape(o) + '|' + re.escape(c))
    d = 0
    for m in pat.finditer(src, i):
        if m.group() == o: d += 1
        else:
            d -= 1
            if not d: return m.end()
    return len(src)


def msl_loops(body):
    """[(kind, start, open, close, cond)] of the loops of a function body: do { } while (cond); / for (...) { } / while (cond) { }. open / close:
    the body's braces (close = index after '}'); cond: the continue condition with the translator's loop guard removed ('' = none)"""
    out = []
    for m in re.finditer(r'\bdo\s*\{|\bfor\s*\(|\bwhile\s*\(', body):
        k = m.group(0)
        if k.startswith('do'):
            o = m.end() - 1; c = _bal(body, o, '{', '}')
            t = re.match(r'\s*while\s*\(', body[c:])
            if not t: continue
            p = c + t.end() - 1; e = _bal(body, p)
            cond = body[p + 1:e - 1]
            out.append(('do', m.start(), o, c, cond))
        else:
            p = m.end() - 1; e = _bal(body, p)
            if k.startswith('while'):
                # the tail of a do-while is not a loop of its own
                pre = body[:m.start()].rstrip()
                if pre.endswith('}'): continue
            if not re.match(r'\s*\{', body[e:]): continue
            o = body.index('{', e); c = _bal(body, o, '{', '}')
            cond = body[p + 1:e - 1]
            if k.startswith('for'): cond = cond.split(';')[1] if cond.count(';') >= 2 else ''
            out.append((k[:3].strip(), m.start(), o, c, cond))
    for n, (k, s, o, c, cond) in enumerate(out):
        cond = re.sub(r'\s*&&\s*\+\+dm_lg\w*\s*<\s*\d+u?\s*$', '', cond.strip())
        cond = cond.strip()
        while cond.startswith('(') and _bal(cond, 0) == len(cond): cond = cond[1:-1].strip()
        out[n] = (k, s, o, c, '' if cond in ('true', '1') else cond)
    return out


def _if_spans(body):
    """[(cond, start, end)] of every if block and its else block (the else carries the if's condition)"""
    out = []
    for m in re.finditer(r'\bif\s*\(', body):
        p = m.end() - 1; e = _bal(body, p); cond = body[p + 1:e - 1]
        t = re.match(r'\s*\{', body[e:])
        if not t: continue
        o = e + t.end() - 1; c = _bal(body, o, '{', '}'); out.append((cond, o, c))
        el = re.match(r'\s*else\s*\{', body[c:])
        if el:
            o2 = c + el.end() - 1; out.append((cond, o2, _bal(body, o2, '{', '}')))
    return out


def _switch_spans(body):
    out = []
    for m in re.finditer(r'\bswitch\s*\(', body):
        e = _bal(body, m.end() - 1); t = re.match(r'\s*\{', body[e:])
        if t: o = e + t.end() - 1; out.append((o, _bal(body, o, '{', '}')))
    return out


def _assigns(text):
    """[(lhs base, rhs, line start)] of the assignments in text (one statement a line, as SPIRV-Cross writes them)"""
    out = []; pos = 0
    for line in text.split('\n'):
        m = _ASSIGN.match(line)
        if m: out.append((m.group(1), m.group(4) if m.group(3) == '=' else m.group(1) + ' ' + m.group(4), pos))
        else:
            # a threadgroup / local array element store: name[...] = value;
            m2 = re.match(r'^\s*(_\w+)\[[^=]*\]\s*=(?!=)\s*(.*);\s*$', line)
            if m2: out.append((m2.group(1), m2.group(2), pos))
        pos += len(line) + 1
    return out


def _induction_vars(defs):
    """loop counters: variables on a cycle of the loop's assignments made only of copies (casts allowed) and steps by a constant, with at least
    one step: i = i' + 1; i' = i. A phi web through general arithmetic (address offsets of loop-carried values) does not count."""
    simple = {}; step = set()
    for l, rs in defs.items():
        srcs = set()
        for r in rs:
            m = _SIMPLE.match(r.strip())
            if not m: srcs = None; break
            srcs.add(m.group(1))
            if m.group(2) and int(m.group(3)) != 0: step.add(l)
        if srcs: simple[l] = srcs
    out = set()
    for l in step:
        if l not in simple: continue
        seen = set(); todo = list(simple[l])
        while todo:
            x = todo.pop()
            if x == l: out.add(l); break
            if x in seen or x not in simple: continue
            seen.add(x); todo += simple[x]
    return out


def functions_fast(src):
    """(name, body) of each function, by lines: SPIRV-Cross puts a function's signature at column 0 and its '{' alone on the next line
    (functions()'s regex backtracks for seconds on 30k-line kernels)"""
    out = []; pos = 0; lines = src.split('\n'); offs = []
    for l in lines: offs.append(pos); pos += len(l) + 1
    for n in range(len(lines) - 1):
        l = lines[n]
        if not l or l[0] in ' \t#}/{' or lines[n + 1] != '{' or l.startswith(('struct ', 'constant ', 'typedef ', 'template', 'using ', 'enum ')): continue
        m = re.search(r'(\w+)\([^;]*\)\s*$', re.sub(r'__attribute__\(\([^()]*\)\)', '', l))
        if not m: continue
        o = offs[n + 1]; c = _bal(src, o, '{', '}')
        out.append((m.group(1), src[o:c]))
    return out
