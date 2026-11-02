"""Privatize shared arrays whose every access belongs to the calling invocation.

Only the straight-line scalar address expressions emitted by SPIRV-Cross are
accepted. Unknown expressions, mutations, escaping addresses and communication
between invocations leave the source unchanged. No shader keys are involved.
"""
import ast
import collections
import re


def privatize(source, local):
    """Fold one-dimensional, disjoint per-invocation uint slices to local arrays."""
    if not local or len(local) != 3 or local[0] <= 1 or list(local[1:]) != [1, 1]:
        return source
    kernel = re.search(r'kernel void \w+\([^\n]*\)\s*\{', source)
    if not kernel or 'uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]]' not in kernel[0]:
        return source
    head, body = source[:kernel.end()], source[kernel.end():]
    n = local[0]
    definitions = list(re.finditer(r'\b(int|uint|float) (\w+) = ([^;\n]+);', body))
    writes = collections.Counter(re.findall(r'\b(\w+)\s*(?:=(?!=)|\+=|-=|\*=|\+\+|--)', body))
    values = {}
    dependencies = {}
    safe_spans = []

    def bounds(value):
        a, b, _ = value
        return min(b, a * (n - 1) + b), max(b, a * (n - 1) + b)

    def evaluate(node, used):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return (0, node.value, 'int')
        if isinstance(node, ast.Name) and node.id in values:
            used.add(node.id)
            return values[node.id]
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and node.value.id == 'gl_LocalInvocationID':
            if node.attr == 'x':
                return (1, 0, 'int')
            if node.attr in ('y', 'z'):
                return (0, 0, 'int')
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and len(node.args) == 1:
            value = evaluate(node.args[0], used)
            kind = node.func.id
            if kind in ('bits_float', 'bits_int', 'bits_uint'):
                return (*value[:2], 'bits' if kind == 'bits_float' else 'int')
            if kind in ('uint', 'int') and value[2] == 'int':
                lo, hi = bounds(value)
                if 0 <= lo <= hi <= (0x7fffffff if kind == 'int' else 0xffffffff):
                    return value
        if isinstance(node, ast.BinOp):
            left, right = evaluate(node.left, used), evaluate(node.right, used)
            if left[2] != 'int' or right[2] != 'int':
                raise ValueError
            a, b, _ = left
            c, d, _ = right
            if isinstance(node.op, ast.Add):
                result = (a + c, b + d, 'int')
            elif isinstance(node.op, ast.Mult) and (not a or not c):
                result = (a * d + b * c, b * d, 'int')
            elif isinstance(node.op, ast.BitAnd) and not c and d >= 0 and d & (d + 1) == 0 and 0 <= bounds(left)[0] <= bounds(left)[1] <= d:
                result = left
            elif isinstance(node.op, ast.RShift) and not c and 0 <= d < 32 and a % (1 << d) == 0 and bounds(left)[0] >= 0:
                result = (a >> d, b >> d, 'int')
            else:
                raise ValueError
            if 0 <= bounds(result)[0] <= bounds(result)[1] <= 0x7fffffff:
                return result
        raise ValueError

    def expression(text):
        text = re.sub(r'as_type<(float|int|uint)>', r'bits_\1', text)
        return ast.parse(text, mode='eval').body

    for definition in definitions:
        ty, name, rhs = definition.groups()
        if writes[name] != 1:
            continue
        used = set()
        try:
            value = evaluate(expression(rhs), used)
        except (ValueError, SyntaxError, RecursionError):
            continue
        if (ty == 'float') != (value[2] == 'bits'):
            continue
        values[name] = value
        dependencies[name] = used
        safe_spans.append(definition.span())

    replacements = []
    declarations = re.finditer(r'threadgroup spvUnsafeArray<uint, (\d+)> (\w+);', body)
    for declaration in declarations:
        count, name = int(declaration[1]), declaration[2]
        if count % n:
            continue
        stride = count // n
        uses = list(re.finditer(r'\b' + re.escape(name) + r'\b', body))
        accesses = list(re.finditer(r'\b' + re.escape(name) + r'\[([^\]\n]+)\]', body))
        if len(uses) != len(accesses) + 1 or not accesses:
            continue
        pending, used = [], set()
        try:
            for access in accesses:
                line = body[body.rfind('\n', 0, access.start()) + 1:body.find('\n', access.end())].strip()
                if len(re.findall(r'\b' + re.escape(name) + r'\b', line)) != 1:
                    raise ValueError
                load = r'(?:float|int|uint) \w+ = (?:as_type<(?:float|int|uint)>\()?' + re.escape(access[0]) + r'\)?;'
                store = re.escape(access[0]) + r' = [^;]+;'
                if not re.fullmatch(load, line) and not re.fullmatch(store, line):
                    raise ValueError
                a, b, kind = evaluate(expression(access[1]), used)
                if kind != 'int' or a != stride or not 0 <= b < stride:
                    raise ValueError
                pending.append((access.start(), access.end(), f'{name}[{b}]'))
        except (ValueError, SyntaxError, RecursionError):
            continue
        # Dependencies may occur only in recognized scalar definitions or array
        # subscripts. This excludes passing one to an unknown helper by reference.
        todo = list(used)
        while todo:
            for dep in dependencies.get(todo.pop(), set()):
                if dep not in used:
                    used.add(dep)
                    todo.append(dep)
        spans = safe_spans + [a.span() for a in accesses]
        if any(not any(start <= match.start() < end for start, end in spans)
               for var in used for match in re.finditer(r'\b' + re.escape(var) + r'\b', body)):
            continue
        # Taking an element's address, including a threadgroup atomic cast,
        # would escape this direct load/store analysis.
        if any(re.search(r'&\s*$', body[max(0, a.start() - 100):a.start()]) for a in accesses):
            continue
        pending.append((declaration.start(), declaration.end(), f'spvUnsafeArray<uint, {stride}> {name};'))
        replacements.extend(pending)
    for start, end, text in sorted(replacements, reverse=True):
        body = body[:start] + text + body[end:]
    return head + body
