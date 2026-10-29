#!/usr/bin/env python3
# SPIR-V passes of the Vulkan shader chain (docs/design/vulkan-backend.md, 2.2), and the reads of a module that dmsh_build_vk.py makes its reflection and census from. The Metal
# chain makes these changes in the MSL text (dmsh_build.py); on Vulkan the translator's SPIR-V is the output, so they are made on the SPIR-V binary here, in plain Python: no stock
# spirv-opt pass does any of them. The one external tool is spirv-link (SPIRV-Tools), which merges the integer-output translations into one module (merge_entries).
#   precise         GLSL.std.450 FMin / FMax / FClamp → NMin / NMax / NClamp: the F forms leave the result of a NaN operand undefined, Maxwell's FMNMX returns the other operand and
#                   its saturation gives 0, as the N forms do (dmsh_build.py precise; shaders.md "min, max and clamp with a NaN operand")
#   invariant       the Invariant decoration on every Position output (the translator adds it only with relaxed precision): identical position math gives identical depth across
#                   shaders and pipelines (dmsh_build.py -fpreserve-invariance; shaders.md "Position invariance across shaders")
#   vs_init         a vertex shader stores (0, 0, 0, 1) into every generic output at entry, what an unwritten component reads on the hardware (dmsh_build.py vs_init)
#   vs_pad          a vertex shader declares the generic outputs its paired pixel shaders read but it does not write, written with (0, 0, 0, 1) (dmsh_build.py vs_pad)
#   float_controls  the SignedZeroInfNanPreserve execution mode for 32-bit floats, the counterpart of the Metal compile's NaN-preserving math (ios_metal.py). The build does not apply
#                   it: a device without shaderSignedZeroInfNanPreserveFloat32 refuses such a module, so the Vulkan backend patches it in per device when it creates the module
#   shadow_inputs   the <key>_bv variant: every integer vertex input also declared as a float4 and as the integer of the other sign at locations 16-30, ORed into the input the body
#                   reads (dmsh_vin.py, the MSL form; Vulkan, like Metal, fetches a format only into an input of its own numeric type [A: check with the validation layers])
#   merge_entries   main, main_u and main_m (dmsh_build.py fs_uint's entry points; on Vulkan made by translating again with fout facts) as entry points of one module (spirv-link)
# Usage: dmsh_spv.py passes <in.spv> <out.spv> [--stage vs|ps|...] [--float-controls]   the passes the build applies (and float_controls when asked); prints what each did
#        dmsh_spv.py resources <spv>   the descriptor bindings as dmsh_build_vk.py reads them;   dmsh_spv.py census <spv>   the float-declared image reads and how their bits are used
import json, os, struct, subprocess, sys, tempfile

MAGIC = 0x07230203
# Opcodes
NAME, MEMBER_NAME, EXTENSION, EXT_INST_IMPORT, EXT_INST, ENTRY_POINT, EXECUTION_MODE, CAPABILITY = 5, 6, 10, 11, 12, 15, 16, 17
T_BOOL, T_INT, T_FLOAT, T_VECTOR, T_IMAGE, T_SAMPLER, T_SAMPLED_IMAGE, T_ARRAY, T_RUNTIME_ARRAY, T_STRUCT, T_POINTER = 20, 21, 22, 23, 25, 26, 27, 28, 29, 30, 32
CONSTANT, CONSTANT_COMPOSITE = 43, 44
FUNCTION, FUNCTION_END, FUNCTION_CALL, VARIABLE, IMAGE_TEXEL_POINTER, LOAD, STORE, COPY_MEMORY, ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN, PTR_ACCESS_CHAIN = 54, 56, 57, 59, 60, 61, 62, 63, 65, 66, 67
DECORATE, MEMBER_DECORATE = 71, 72
VECTOR_SHUFFLE, COMPOSITE_CONSTRUCT, COMPOSITE_EXTRACT, COPY_OBJECT = 79, 80, 81, 83
SAMPLED_IMAGE, IMAGE_WRITE, IMAGE, IMAGE_QUERY_SIZE_LOD, IMAGE_QUERY_SIZE = 86, 99, 100, 103, 104
BITCAST, SELECT, BITWISE_OR, PHI, LABEL, LINE, NO_LINE = 124, 169, 197, 245, 248, 8, 317
ANNOTATIONS = {DECORATE, MEMBER_DECORATE, 73, 74, 75, 332, 5632, 5633}
# Decorations, built-ins, storage classes, execution models
D_BUFFER_BLOCK, D_BUILTIN, D_NO_PERSPECTIVE, D_FLAT, D_CENTROID, D_SAMPLE, D_INVARIANT, D_NON_WRITABLE = 3, 11, 13, 14, 16, 17, 18, 24
D_LOCATION, D_COMPONENT, D_BINDING, D_DESCRIPTOR_SET = 30, 31, 33, 34
BUILTIN_POSITION = 0
SC_UNIFORM_CONSTANT, SC_INPUT, SC_UNIFORM, SC_OUTPUT, SC_PRIVATE, SC_FUNCTION, SC_STORAGE_BUFFER = 0, 1, 2, 3, 6, 7, 12
MODELS = {0: 'vs', 1: 'hs', 2: 'ds', 3: 'gs', 4: 'ps', 5: 'cs'}
GLSL_N = {37: 79, 40: 80, 43: 81}   # FMin → NMin, FMax → NMax, FClamp → NClamp
EM_SIGNED_ZERO_INF_NAN_PRESERVE, CAP_SIGNED_ZERO_INF_NAN_PRESERVE = 4461, 4466
DIM_BUFFER = 5
DIMS = {0: '1d', 1: '2d', 2: '3d', 3: 'cube', 4: 'rect', 5: 'buffer', 6: 'subpass'}
# Opcodes whose result id is word 1 (types, labels, imports, strings), and those with no result
RESULT_ONLY = set(range(19, 40)) | {7, EXT_INST_IMPORT, 73, LABEL, 322}
NO_RESULT = {0, 2, 3, 4, NAME, MEMBER_NAME, LINE, EXTENSION, 14, ENTRY_POINT, EXECUTION_MODE, CAPABILITY, FUNCTION_END, STORE, COPY_MEMORY, 64, DECORATE, MEMBER_DECORATE,
             74, 75, IMAGE_WRITE, 218, 219, 220, 221, 224, 225, 228, 246, 247, 249, 250, 251, 252, 253, 254, 255, 256, 257, NO_LINE, 330, 331, 332, 4416, 5632, 5633}
# Image reads: opcode → kind (the image operand is word 3); depth comparisons are float by definition and are not reads of the image's bits
READS = {87: 'sample', 88: 'sample', 91: 'sample', 92: 'sample', 95: 'fetch', 96: 'gather', 98: 'read', 305: 'sample', 306: 'sample', 309: 'sample', 310: 'sample', 311: 'fetch',
         312: 'gather', 320: 'read'}


def string_words(s):
    b = s.encode() + b'\0'; b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


def read_string(words):
    """(the literal string at the start of words, the number of words it takes)"""
    b = b''
    for n, w in enumerate(words):
        b += struct.pack('<I', w)
        if b'\0' in b[-4:]: return b[:b.index(b'\0')].decode(errors='replace'), n + 1
    raise ValueError('unterminated SPIR-V string')


class Module:
    """A SPIR-V module: its header and its instructions, each the list of its words (the first is word count << 16 | opcode)"""
    def __init__(self, data):
        n = len(data) // 4
        w = list(struct.unpack('<%dI' % n, data[:n * 4]))
        if n < 5 or w[0] != MAGIC: raise ValueError('not a SPIR-V module')
        self.header, self.ins, i = w[:5], [], 5
        while i < n:
            c = w[i] >> 16
            if c == 0 or i + c > n: raise ValueError(f'malformed SPIR-V instruction at word {i}')
            self.ins.append(w[i:i + c]); i += c

    @classmethod
    def read(cls, path):
        with open(path, 'rb') as f: return cls(f.read())

    def data(self):
        flat = self.header + [x for i in self.ins for x in i]
        return struct.pack('<%dI' % len(flat), *flat)

    def write(self, path):
        tmp = path + '.tmp-' + str(os.getpid())
        with open(tmp, 'wb') as f: f.write(self.data())
        os.replace(tmp, path)

    def new_id(self):
        self.header[3] += 1
        return self.header[3] - 1

    # Reading
    def of(self, opcode): return [i for i in self.ins if i[0] & 0xffff == opcode]

    @staticmethod
    def result(i):
        o = i[0] & 0xffff
        if o in NO_RESULT: return None
        if o in RESULT_ONLY: return i[1] if len(i) > 1 else None
        return i[2] if len(i) > 2 else None

    def defs(self):
        out = {}
        for i in self.ins:
            r = self.result(i)
            if r is not None: out[r] = i
        return out

    def names(self): return {i[1]: read_string(i[2:])[0] for i in self.of(NAME)}

    def decorations(self):
        """{id: {decoration: [literals]}} (OpDecorate)"""
        out = {}
        for i in self.of(DECORATE): out.setdefault(i[1], {})[i[2]] = i[3:]
        return out

    def member_decorations(self):
        out = {}
        for i in self.of(MEMBER_DECORATE): out.setdefault((i[1], i[2]), {})[i[3]] = i[4:]
        return out

    def entry_points(self):
        """[(stage, name, function id, interface ids)]"""
        out = []
        for i in self.of(ENTRY_POINT):
            name, n = read_string(i[3:])
            out.append((MODELS.get(i[1], str(i[1])), name, i[2], i[3 + n:]))
        return out

    def glsl_set(self):
        return next((i[1] for i in self.of(EXT_INST_IMPORT) if read_string(i[2:])[0] == 'GLSL.std.450'), None)

    def variables(self, storage=None):
        """The global variables: [(id, pointer type, storage class)]"""
        out, depth = [], 0
        for i in self.ins:
            o = i[0] & 0xffff
            if o == FUNCTION: depth += 1
            elif o == FUNCTION_END: depth -= 1
            elif o == VARIABLE and not depth and (storage is None or i[3] == storage): out.append((i[2], i[1], i[3]))
        return out

    def type_shape(self, t, defs=None):
        """(base, component count, bit width) of a type: base 'float', 'uint', 'sint' or 'bool' for a scalar or a vector, else the kind of type"""
        defs = defs or self.defs(); i = defs.get(t)
        if not i: return None, 0, 0
        o = i[0] & 0xffff
        if o == T_VECTOR:
            base, _, width = self.type_shape(i[2], defs)
            return base, i[3], width
        if o == T_FLOAT: return 'float', 1, i[2]
        if o == T_INT: return ('sint' if i[3] else 'uint'), 1, i[2]
        if o == T_BOOL: return 'bool', 1, 1
        return {T_STRUCT: 'struct', T_ARRAY: 'array', T_RUNTIME_ARRAY: 'array', T_IMAGE: 'image', T_SAMPLED_IMAGE: 'sampled_image', T_SAMPLER: 'sampler'}.get(o, str(o)), 0, 0

    # Writing: an instruction goes at the end of its section of the module's logical layout
    def _after_last(self, opcodes, before=None):
        last = max((k for k, i in enumerate(self.ins) if i[0] & 0xffff in opcodes), default=None)
        if last is not None: return last + 1
        if before: return next(k for k, i in enumerate(self.ins) if i[0] & 0xffff in before)
        return self._global_end()

    def _global_end(self):   # before the first function: new types, constants and global variables
        return next((k for k, i in enumerate(self.ins) if i[0] & 0xffff == FUNCTION), len(self.ins))

    def add_capability(self, cap):
        if not any(i[1] == cap for i in self.of(CAPABILITY)): self.ins.insert(self._after_last({CAPABILITY}), [2 << 16 | CAPABILITY, cap])

    def add_extension(self, name):
        if any(read_string(i[1:])[0] == name for i in self.of(EXTENSION)): return
        w = string_words(name); self.ins.insert(self._after_last({EXTENSION, CAPABILITY}), [(1 + len(w)) << 16 | EXTENSION] + w)

    def add_name(self, target, name):
        w = string_words(name); self.ins.insert(self._after_last({NAME, MEMBER_NAME}, before=ANNOTATIONS | set(range(19, 40))), [(2 + len(w)) << 16 | NAME, target] + w)

    def decorate(self, target, dec, *literals):
        if dec in self.decorations().get(target, {}): return False
        self.ins.insert(self._after_last(ANNOTATIONS, before=set(range(19, 40))), [(3 + len(literals)) << 16 | DECORATE, target, dec, *literals])
        return True

    def member_decorate(self, struct_id, member, dec, *literals):
        if dec in self.member_decorations().get((struct_id, member), {}): return False
        self.ins.insert(self._after_last(ANNOTATIONS, before=set(range(19, 40))), [(4 + len(literals)) << 16 | MEMBER_DECORATE, struct_id, member, dec, *literals])
        return True

    def type_id(self, opcode, *operands):
        """The id of a type: found, or added before the first function (the validator refuses two declarations of one non-aggregate type)"""
        for i in self.of(opcode):
            if i[2:] == list(operands): return i[1]
        r = self.new_id(); self.ins.insert(self._global_end(), [(2 + len(operands)) << 16 | opcode, r, *operands])
        return r

    def float_type(self): return self.type_id(T_FLOAT, 32)
    def int_type(self, signed): return self.type_id(T_INT, 32, 1 if signed else 0)
    def vector_type(self, comp, n): return self.type_id(T_VECTOR, comp, n)
    def pointer_type(self, storage, t): return self.type_id(T_POINTER, storage, t)

    def constant(self, t, *words):
        for i in self.of(CONSTANT):
            if i[1] == t and i[3:] == list(words): return i[2]
        r = self.new_id(); self.ins.insert(self._global_end(), [(3 + len(words)) << 16 | CONSTANT, t, r, *words])
        return r

    def composite(self, t, parts):
        for i in self.of(CONSTANT_COMPOSITE):
            if i[1] == t and i[3:] == list(parts): return i[2]
        r = self.new_id(); self.ins.insert(self._global_end(), [(3 + len(parts)) << 16 | CONSTANT_COMPOSITE, t, r, *parts])
        return r

    def variable(self, ptr_type, storage):
        r = self.new_id(); self.ins.insert(self._global_end(), [4 << 16 | VARIABLE, ptr_type, r, storage])
        return r

    def add_interface(self, var, stages=None):
        """var in the interface list of every entry point (of the given stages)"""
        for k, i in enumerate(self.ins):
            if i[0] & 0xffff != ENTRY_POINT or (stages and MODELS.get(i[1]) not in stages): continue
            if var not in i[3 + read_string(i[3:])[1]:]: self.ins[k] = [(len(i) + 1) << 16 | ENTRY_POINT] + i[1:] + [var]

    def entry_insert_point(self, fn):
        """The index after the OpLabel and OpVariable instructions that start function fn's first block"""
        k = next(k for k, i in enumerate(self.ins) if i[0] & 0xffff == FUNCTION and i[2] == fn)
        while self.ins[k][0] & 0xffff != LABEL: k += 1
        k += 1
        while k < len(self.ins) and self.ins[k][0] & 0xffff in (VARIABLE, LINE, NO_LINE): k += 1
        return k

    def rename_entry(self, old, new):
        for k, i in enumerate(self.ins):
            if i[0] & 0xffff == ENTRY_POINT:
                name, n = read_string(i[3:])
                if name == old:
                    w = string_words(new); rest = i[3 + n:]
                    self.ins[k] = [(3 + len(w) + len(rest)) << 16 | ENTRY_POINT, i[1], i[2]] + w + rest
                    return True
        return False


# The passes

def precise(m):
    """FMin / FMax / FClamp → NMin / NMax / NClamp (see the file header); → the number of instructions changed"""
    g, n = m.glsl_set(), 0
    if g is None: return 0
    for i in m.of(EXT_INST):
        if i[3] == g and i[4] in GLSL_N: i[4] = GLSL_N[i[4]]; n += 1
    return n


def invariant(m):
    """The Invariant decoration on every Position output, a variable or a block member (see the file header); → the number of Position outputs"""
    defs, n = m.defs(), 0
    for v, ptr, _ in m.variables(SC_OUTPUT):
        if m.decorations().get(v, {}).get(D_BUILTIN) == [BUILTIN_POSITION]: m.decorate(v, D_INVARIANT); n += 1; continue
        t = defs[ptr][3]
        while defs[t][0] & 0xffff in (T_ARRAY, T_RUNTIME_ARRAY): t = defs[t][2]   # per-vertex output arrays (tessellation control)
        if defs[t][0] & 0xffff == T_STRUCT:
            for idx in range(len(defs[t]) - 2):
                if m.member_decorations().get((t, idx), {}).get(D_BUILTIN) == [BUILTIN_POSITION]: m.member_decorate(t, idx, D_INVARIANT); n += 1
    return n


def interface(m):
    """The generic inputs and outputs of the module's entry points: [{'id', 'storage': 'in' | 'out', 'location', 'component', 'type', 'base', 'n', 'width', 'interp',
    'centroid', 'sample'}]; built-ins and blocks are left out"""
    dec, defs, out = m.decorations(), m.defs(), []
    for v, ptr, sc in m.variables():
        d = dec.get(v, {})
        if sc not in (SC_INPUT, SC_OUTPUT) or D_BUILTIN in d or D_LOCATION not in d: continue
        t = defs[ptr][3]; base, n, width = m.type_shape(t, defs)
        out.append({'id': v, 'storage': 'in' if sc == SC_INPUT else 'out', 'location': d[D_LOCATION][0], 'component': d.get(D_COMPONENT, [0])[0], 'type': t,
                    'base': base, 'n': n, 'width': width, 'interp': 'flat' if D_FLAT in d else 'noperspective' if D_NO_PERSPECTIVE in d else 'smooth',
                    'centroid': D_CENTROID in d, 'sample': D_SAMPLE in d})
    return out


def _init_value(m, base, n, component):
    """(0, 0, 0, 1) restricted to n components from component on, of the base type (an integer output takes the bits of 1.0, as a Maxwell register holds them)"""
    vals = [0, 0, 0, 0x3f800000][component:component + n]
    t = m.float_type() if base == 'float' else m.int_type(base == 'sint')
    parts = [m.constant(t, v) for v in vals]
    return parts[0] if n == 1 else m.composite(m.vector_type(t, n), parts)


def _store_at_entry(m, stores):
    """OpStore of each (variable, value) at the start of every vertex entry point"""
    for stage, _, fn, _ in m.entry_points():
        if stage != 'vs' or not stores: continue
        k = m.entry_insert_point(fn)
        m.ins[k:k] = [[3 << 16 | STORE, var, value] for var, value in stores]


def vs_init(m):
    """(0, 0, 0, 1) into every generic output of the vertex entry points at entry (see the file header); → the number of outputs initialized"""
    outs = [x for x in interface(m) if x['storage'] == 'out' and x['base'] in ('float', 'uint', 'sint') and x['width'] == 32 and x['component'] + x['n'] <= 4]
    _store_at_entry(m, [(x['id'], _init_value(m, x['base'], x['n'], x['component'])) for x in outs])
    return len(outs)


def pixel_inputs(m):
    """{location: (base, n, component)} of a pixel shader's generic inputs, what vs_pad gives a vertex shader that does not write them"""
    out = {}
    for x in interface(m):
        if x['storage'] == 'in' and x['base'] in ('float', 'uint', 'sint') and x['width'] == 32 and x['component'] + x['n'] <= 4:
            out.setdefault(x['location'], (x['base'], x['n'], x['component']))
    return out


def vs_pad(m, want):
    """Declare the generic outputs at the locations of want ({location: (base, n, component)}, pixel_inputs of the paired pixel shaders) that the vertex shader does not write,
    written with (0, 0, 0, 1) at entry (see the file header); → the sorted locations padded"""
    have = {x['location'] for x in interface(m) if x['storage'] == 'out'}
    pad, stores = sorted(set(want) - have), []
    for loc in pad:
        base, n, comp = want[loc]
        t = m.float_type() if base == 'float' else m.int_type(base == 'sint')
        v = m.variable(m.pointer_type(SC_OUTPUT, t if n == 1 else m.vector_type(t, n)), SC_OUTPUT)
        m.add_name(v, f'dm_pad{loc}'); m.decorate(v, D_LOCATION, loc)
        if comp: m.decorate(v, D_COMPONENT, comp)
        m.add_interface(v, ('vs',))
        stores.append((v, _init_value(m, base, n, comp)))
    _store_at_entry(m, stores)
    return pad


def float_controls(m):
    """SignedZeroInfNanPreserve for 32-bit floats on every entry point (see the file header: the backend applies it per device); → the number of entry points"""
    m.add_capability(CAP_SIGNED_ZERO_INF_NAN_PRESERVE)
    if m.header[1] < 0x10400: m.add_extension('SPV_KHR_float_controls')
    have = {(i[1], i[2]) for i in m.of(EXECUTION_MODE)}
    eps = m.entry_points()
    for _, _, fn, _ in eps:
        if (fn, EM_SIGNED_ZERO_INF_NAN_PRESERVE) not in have:
            m.ins.insert(m._after_last({EXECUTION_MODE, 331, ENTRY_POINT}), [4 << 16 | EXECUTION_MODE, fn, EM_SIGNED_ZERO_INF_NAN_PRESERVE, 32])
    return len(eps)


def apply(m, stage, pad=None, fp_controls=False):
    """The passes the build applies to a module of the given stage (see the file header); → {pass: what it did}"""
    done = {'precise': precise(m), 'invariant': invariant(m) if stage in ('vs', 'ds', 'gs') else 0}
    if stage == 'vs':
        done['vs_init'] = vs_init(m)
        done['vs_pad'] = vs_pad(m, pad) if pad else []
    if fp_controls: done['float_controls'] = float_controls(m)
    return done


SHADOW0, SHADOW_END = 16, 31   # vertex input locations 16..30 (NVN attributes are 0-15), as dmsh_vin.py


def shadow_inputs(m):
    """The <key>_bv variant of a vertex shader with integer inputs (see the file header). Changes m; → [{'location', 'float': location, 'sint' | 'uint': location}], or the reason
    there is none (a string). The body reads a private copy of each integer input, made at entry from the input ORed with the bits of its shadows (an input cannot be written)"""
    ins = sorted((x for x in interface(m) if x['storage'] == 'in'), key=lambda x: x['location'])
    if not ins: return 'no stage input'
    if any(x['location'] >= SHADOW0 for x in ins): return 'an input at attribute 16 or above'
    ints = [x for x in ins if x['base'] in ('uint', 'sint') and x['n'] == 4 and x['width'] == 32 and x['component'] == 0]
    if not ints: return 'no integer input'
    ids = {x['id'] for x in ints}
    for i in m.ins:   # the body may only load an integer input or index into it: those uses read the private copy instead
        o = i[0] & 0xffff
        if o in (LOAD, ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN) or o in (ENTRY_POINT, DECORATE, NAME, MEMBER_NAME): continue
        if o in (STORE, COPY_MEMORY, FUNCTION_CALL, IMAGE_TEXEL_POINTER, PTR_ACCESS_CHAIN, COPY_OBJECT) and ids & set(i[1:]): return 'an integer input used other than by loads and access chains'
    v4f = m.vector_type(m.float_type(), 4)
    loc, shadows, made = SHADOW0, [], []
    for x in ints:   # a float4 shadow for every integer input first (half and normalized formats are the common case), then the other sign while locations last (as dmsh_vin.py)
        if loc >= SHADOW_END: break
        sv = m.variable(m.pointer_type(SC_INPUT, v4f), SC_INPUT); m.add_name(sv, f'dm_in{x["location"]}_vf'); m.decorate(sv, D_LOCATION, loc); m.add_interface(sv, ('vs',))
        shadows.append({'location': x['location'], 'float': loc}); made.append([x, sv, None]); loc += 1
    for e, rec in zip(shadows, made):
        if loc >= SHADOW_END: break
        x = rec[0]; other = 'sint' if x['base'] == 'uint' else 'uint'
        ot = m.vector_type(m.int_type(other == 'sint'), 4)
        ov = m.variable(m.pointer_type(SC_INPUT, ot), SC_INPUT); m.add_name(ov, f'dm_in{x["location"]}_vo'); m.decorate(ov, D_LOCATION, loc); m.add_interface(ov, ('vs',))
        e[other] = loc; rec[2] = (ov, ot); loc += 1
    priv = {x['id']: m.variable(m.pointer_type(SC_PRIVATE, x['type']), SC_PRIVATE) for x, _, _ in made}
    defs = m.defs()
    for i in m.ins:
        o = i[0] & 0xffff
        if o == LOAD and i[3] in priv: i[3] = priv[i[3]]
        elif o in (ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN) and i[3] in priv: i[1] = m.pointer_type(SC_PRIVATE, defs[i[1]][3]); i[3] = priv[i[3]]
    code = []
    for x, sv, ov in made:
        a, f, fb, r = m.new_id(), m.new_id(), m.new_id(), m.new_id()
        code += [[4 << 16 | LOAD, x['type'], a, x['id']], [4 << 16 | LOAD, v4f, f, sv], [4 << 16 | BITCAST, x['type'], fb, f], [5 << 16 | BITWISE_OR, x['type'], r, a, fb]]
        if ov:
            o_, ob, r2 = m.new_id(), m.new_id(), m.new_id()
            code += [[4 << 16 | LOAD, ov[1], o_, ov[0]], [4 << 16 | BITCAST, x['type'], ob, o_], [5 << 16 | BITWISE_OR, x['type'], r2, r, ob]]; r = r2
        code.append([3 << 16 | STORE, priv[x['id']], r])
    for stage, _, fn, _ in m.entry_points():
        if stage == 'vs': k = m.entry_insert_point(fn); m.ins[k:k] = [list(c) for c in code]
    return shadows


def merge_entries(parts, out, spirv_link='spirv-link'):
    """One module holding the entry points of several (parts: [(module path, entry name)]; each module's entry point main takes that name) through spirv-link;
    → (True, '') or (False, the reason)"""
    with tempfile.TemporaryDirectory(prefix='dmsh_link_') as d:
        paths = []
        for k, (p, name) in enumerate(parts):
            m = Module.read(p)
            if name != 'main' and not m.rename_entry('main', name): return False, f'{p}: no entry point main'
            q = os.path.join(d, f'{k}.spv'); m.write(q); paths.append(q)
        r = subprocess.run([spirv_link, '--target-env', 'vulkan1.3', *paths, '-o', out], capture_output=True, text=True)
        if r.returncode: return False, (r.stderr or r.stdout).strip()[:300]
    return True, ''


# Reads for the reflection and the census

def resources(m):
    """The descriptor-backed variables: [{'id', 'name', 'set', 'binding', 'array', 'used', 'class', ...}], used being whether a function refers to it; class 'ubo', 'ssbo'
    (with 'writable'), 'combined' (an image with its sampler),
    'texture' (a sampled image alone), 'texel_buffer', 'storage_image', 'storage_texel_buffer', 'sampler'; an image also has 'dim', 'sampled', 'depth', 'arrayed', 'ms'"""
    dec, defs, names, out = m.decorations(), m.defs(), m.names(), []
    used, depth = set(), 0   # the ids an instruction inside a function refers to: a descriptor nothing refers to is declared but not used
    for i in m.ins:
        o = i[0] & 0xffff
        if o == FUNCTION: depth += 1
        elif o == FUNCTION_END: depth -= 1
        elif depth: used.update(i[1:])
    for v, ptr, sc in m.variables():
        d = dec.get(v, {})
        if D_DESCRIPTOR_SET not in d or D_BINDING not in d: continue
        t, n = defs[ptr][3], 1
        while defs[t][0] & 0xffff in (T_ARRAY, T_RUNTIME_ARRAY):
            c = defs.get(defs[t][3]) if defs[t][0] & 0xffff == T_ARRAY else None
            n = n * c[3] if c and c[0] & 0xffff == CONSTANT else 0
            t = defs[t][2]
        e = {'id': v, 'name': names.get(v, ''), 'set': d[D_DESCRIPTOR_SET][0], 'binding': d[D_BINDING][0], 'array': n, 'used': v in used}
        ti = defs[t]; o = ti[0] & 0xffff
        if sc in (SC_UNIFORM, SC_STORAGE_BUFFER) and o == T_STRUCT:
            ssbo = sc == SC_STORAGE_BUFFER or D_BUFFER_BLOCK in dec.get(t, {})
            e.update({'class': 'ssbo' if ssbo else 'ubo', 'writable': ssbo and D_NON_WRITABLE not in d})
        elif o == T_SAMPLER: e['class'] = 'sampler'
        elif o in (T_SAMPLED_IMAGE, T_IMAGE):
            img = defs[ti[2]] if o == T_SAMPLED_IMAGE else ti
            dim, depth, arrayed, ms, sampled = img[3], img[4], img[5], img[6], img[7]
            e.update(dim=DIMS.get(dim, str(dim)), depth=depth == 1, arrayed=bool(arrayed), ms=bool(ms), sampled=m.type_shape(img[2], defs)[0])
            if o == T_SAMPLED_IMAGE: e['class'] = 'texel_buffer' if dim == DIM_BUFFER else 'combined'
            elif sampled == 2: e['class'] = 'storage_texel_buffer' if dim == DIM_BUFFER else 'storage_image'
            else: e['class'] = 'texel_buffer' if dim == DIM_BUFFER else 'texture'
        else: e['class'] = 'other'
        out.append(e)
    return out


def _sources(m):
    """{value id: the resource variable it comes from}, through loads, access chains, copies, OpSampledImage and OpImage"""
    src = {v: v for v, _, _ in m.variables(SC_UNIFORM_CONSTANT)}
    for i in m.ins:
        o = i[0] & 0xffff
        if o in (LOAD, ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN, COPY_OBJECT, IMAGE, SAMPLED_IMAGE) and len(i) > 3 and i[3] in src: src[i[2]] = src[i[3]]
    return src


def image_uses(m):
    """{resource variable: the set of 'write', 'atomic', 'size', 'read'} from the instructions that use it"""
    src, out = _sources(m), {}
    for i in m.ins:
        o = i[0] & 0xffff
        if o == IMAGE_WRITE and i[1] in src: out.setdefault(src[i[1]], set()).add('write')
        elif o == IMAGE_TEXEL_POINTER and i[3] in src: out.setdefault(src[i[3]], set()).add('atomic')
        elif o in (IMAGE_QUERY_SIZE, IMAGE_QUERY_SIZE_LOD) and i[3] in src: out.setdefault(src[i[3]], set()).add('size')
        elif (o in READS or 89 <= o <= 97) and len(i) > 3 and i[3] in src: out.setdefault(src[i[3]], set()).add('read')
    return out


def _flows(m):
    """For bit_use: {value: [(opcode or 'store', result or pointer)]}, the function-local variables and their loads"""
    uses, defs, loads = {}, m.defs(), {}
    fvars = {i[2] for i in m.of(VARIABLE) if i[3] == SC_FUNCTION}
    for i in m.ins:
        o = i[0] & 0xffff
        if o == LOAD and i[3] in fvars: loads.setdefault(i[3], []).append(i[2])
        if o == STORE: uses.setdefault(i[2], []).append(('store', i[1])); continue
        r = m.result(i)
        if r is None or o in RESULT_ONLY: continue
        for w in i[3:]:
            if w in defs: uses.setdefault(w, []).append((o, r))
    return uses, loads, fvars, defs


def bit_use(m, value, flows=None):
    """How the bits of a float value are used: 'bits' when every use reached through extracts, shuffles, copies, selects, phis, composites and function-local variables is a bit
    cast to an integer type, 'float' when none is, 'mixed' when both; 'unused' without a use"""
    uses, loads, fvars, defs = flows or _flows(m)
    seen, todo, kinds = set(), [value], set()
    while todo:
        x = todo.pop()
        if x in seen: continue
        seen.add(x)
        for o, r in uses.get(x, ()):
            if o == 'store':
                if r in fvars: todo += loads.get(r, [])
                else: kinds.add('float')
            elif o in (COMPOSITE_EXTRACT, VECTOR_SHUFFLE, COPY_OBJECT, SELECT, PHI, COMPOSITE_CONSTRUCT): todo.append(r)
            elif o == BITCAST: kinds.add('bits' if m.type_shape(defs[r][1], defs)[0] in ('uint', 'sint') else 'float')
            else: kinds.add('float')
    return 'unused' if not kinds else 'mixed' if len(kinds) > 1 else kinds.pop()


def census(m):
    """The image reads whose declared sampled type is float: [{'var', 'op', 'use'}], use being bit_use of the result ('bits': integer or stencil data read through a float
    declaration, which Apple GPUs pass through and Vulkan does not; docs/design/vulkan-backend.md 2.2 and section 11)"""
    src, flows, rows = _sources(m), None, []
    floats = {r['id'] for r in resources(m) if r.get('sampled') == 'float'}
    for i in m.ins:
        o = i[0] & 0xffff
        if o not in READS or i[3] not in src or src[i[3]] not in floats: continue
        flows = flows or _flows(m)
        rows.append({'var': src[i[3]], 'op': READS[o], 'use': bit_use(m, i[2], flows)})
    return rows


def validate(path, spirv_val='spirv-val'):
    """spirv-val --target-env vulkan1.3; → (True, '') or (False, its message, cut short)"""
    r = subprocess.run([spirv_val, '--target-env', 'vulkan1.3', path], capture_output=True, text=True)
    return r.returncode == 0, (r.stderr or r.stdout).strip()[:400]


def flatten(m):
    """A copy for the MSL analysis rendering (dmsh_build_vk.py): every descriptor in set 0, numbered in (set, binding) order, so that SPIRV-Cross writes one argument buffer
    and the analyses read its members as they do on Metal (spvDescriptorSet0.<name>)"""
    c = Module(m.data()); dec = c.decorations()
    new = {k: n for n, k in enumerate(sorted({(d[D_DESCRIPTOR_SET][0], d[D_BINDING][0]) for d in dec.values() if D_DESCRIPTOR_SET in d and D_BINDING in d}))}
    for i in c.of(DECORATE):
        d = dec.get(i[1], {})
        if D_DESCRIPTOR_SET in d and D_BINDING in d:
            if i[2] == D_DESCRIPTOR_SET: i[3] = 0
            elif i[2] == D_BINDING: i[3] = new[(d[D_DESCRIPTOR_SET][0], d[D_BINDING][0])]
    return c


def uses_float64(m): return any(i[2] == 64 for i in m.of(T_FLOAT))


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == 'passes':
        mod = Module.read(a[1])
        stage = a[a.index('--stage') + 1] if '--stage' in a else next((e[0] for e in mod.entry_points()), 'cs')
        print(json.dumps(apply(mod, stage, fp_controls='--float-controls' in a))); mod.write(a[2])
    elif len(a) == 2 and a[0] == 'resources': print(json.dumps([{k: v for k, v in r.items() if k != 'id'} for r in resources(Module.read(a[1]))], indent=1))
    elif len(a) == 2 and a[0] == 'census':
        mod = Module.read(a[1]); nm = mod.names()
        for row in census(mod): print(nm.get(row['var'], row['var']), row['op'], row['use'], sep='\t')
    else: sys.exit('usage: see the file header')
