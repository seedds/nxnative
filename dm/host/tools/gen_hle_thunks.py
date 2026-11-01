#!/usr/bin/env python3
"""Generate the host-call thunks of the x86-64 execution engine from the host's HLE entries.

Recompiled guest code (docs/design/aot-recompiler.md, section 2.4) calls a host function through its slot in the host-function page (dm/host/aot_hostfn.c);
the dispatcher maps the slot to a thunk, which reads the arguments from the guest state block (dm_guest_state, dm/tools/aot/include/nxaot/guest_state.h) as
AAPCS64 passes them, calls the C function on the host thread's own stack and writes its result back to the block. This script reads the host sources given on
its command line, finds every HLE(, HLE_FORCE( and HLE_FORCE_BIG( entry (hle.h), also through a file's own wrapper macros (hle_net.c's SKF) and aliases
(hle_fs.c's #define HLE_FORCE HLE_FORCE_BIG), the NVN interface layer's NVN( and NVN_NOPROTO( entries (dm/gfx/nvn/hle_nvn.c: NVN(name, ret, params...), the
entry named by the NVN function's name, its C function h_<name>), the guest Vulkan layer's VK( and VKB( entries of the same form (dm/gfx/hostvk/guest_vk.c: named
by the Vulkan function's name, registered in the HLE table) and the guest OpenGL layer's GL(, GLB( and EGL( entries, also of that form (dm/gfx/hostvk/guest_gl.c: named
by the GL or EGL function's name, in the HLE table; their GL types are the layer's own typedefs), the C library passthrough's PASS( entries of the same form
(dm/host/libc_pass.c: named by the C library function the guest imports, C function libc_<name>), and every function defined with HLE_LINKAGE outside a macro (a host
function the guest reaches only through a pointer the host hands it: a vtable slot, a thread entry; no import-table entry, its C name stands for it), and writes one thunk
per entry, the table hle_thunks[] sorted by mangled name (an NVN, Vulkan, GL, EGL or C library entry's name, or an HLE_LINKAGE function's C name, stands for it), and
an extern declaration of each entry's C function (external on non-native builds: HLE_LINKAGE in hle.h).
The generator runs for builds without native execution only (DM_NATIVE_EXEC 0, plat.h), so it knows #if DM_NATIVE_EXEC (not compiled) and #if !DM_NATIVE_EXEC (compiled).

Argument rules (AAPCS64): integer and pointer parameters take the next x register (x0..x7), float and double the next v register (v0..v7, its low 32 or 64
bits); a 32-bit parameter is the low half of its register, as a w register reads it. A parameter past the eighth of its class is read from the guest stack
(the 8-byte slot at st->sp + 8 * n, in parameter order): the thunk reads guest memory there, it never runs on the guest stack.
Returns: integers and pointers in x0, a 32-bit or smaller value zero-extended (AAPCS64 leaves the upper bits unspecified, so guest code cannot depend on them;
zero is the deterministic choice), long as its value (sign-extended), float and double in v0 with the rest of q0 cleared, void nothing. A function pointer (a typedef
of a pointer to a function: PFN_vkVoidFunction, PFNNVNGENERICFUNCPTRPROC, EGL's __eglMustCastToProperFunctionPointerType) is a host function handed to the guest
(the get-proc-address entries): x0 gets its guest-visible address, dm_guest_fnptr_named(result, name) (dm/host/aot_arena.c), with name the entry's const char *
parameter when it has one (a lookup by name: a result with no host-call thunk becomes the stub of that name, as an unimplemented import's). As a parameter it is a
guest function's address, passed on as it is.
Parameter types come from the C spelling, the pointers, and the typedefs of the source and of the headers it includes with quotes (found beside the including
file, then in the -I directories, as the compiler finds them); a Khronos Vulkan header (<vulkan/...>, in angle brackets or quotes) found in the -I directories
is read by rules of its own (learn_vulkan_header: its include guard and platform conditionals would hide every typedef from the C reader). A struct whose one
member is a 64-bit integer (NVN's separate texture and sampler handles) is
passed as that integer: AAPCS64 gives an 8-byte composite one x register, and the host ABIs (System V, Windows x64) one integer register. A type the script
does not know (any other struct or union by value, a sub-32-bit parameter, ..., an unknown typedef), an entry inside a preprocessor conditional other than
#if 0 / #if 1 (and #ifdef / #ifndef __cplusplus, which is never defined in these C sources), or a C name used by two entries stops the run with file:line.

The arena guard (DM_AOT_ARENA_CHECK=1; dm/host/aot.h, "Host memory the guest sees"): every thunk of an entry with pointer parameters or a pointer result checks them,
before and after the call, against the arena below 2^39 (DM_AOT_ARENA_BASE and DM_AOT_ARENA_SIZE, read from dm/host/aot.h), and logs a [RED] line once per entry
and position for an address outside it; opaque handle types (OPAQUE: the Vulkan handles, EGL's) and function pointer results are left out. The check is self-contained
in the generated file, so a table links without the engine.

  gen_hle_thunks.py -o <out.c> [-I <dir>]... <source.c>...
Prints a summary (entries, parameter and return kinds); exit status 1 on any error, and nothing is written then.
"""
import argparse
import os
import re
import sys

MACROS = {"HLE": 3, "HLE_FORCE": 4, "HLE_FORCE_BIG": 4, "NVN": 2, "NVN_NOPROTO": 2, "VK": 2, "VKB": 2, "GL": 2, "GLB": 2, "EGL": 2, "PASS": 2}   # macro -> index of the first parameter argument (mangled, [why,] ret, cname, params...; name, ret, params...)
NAMED = {"NVN", "NVN_NOPROTO", "VK", "VKB", "GL", "GLB", "EGL", "PASS"}   # entries named by an identifier, the API function's name, whose C function is <prefix><name> (dm/gfx/nvn/hle_nvn.c, dm/gfx/hostvk/guest_vk.c, guest_gl.c, dm/host/libc_pass.c)
PREFIX = {"PASS": "libc_"}   # a named entry's C function prefix, h_ when not listed
TABLES = {"HLE": "hle", "HLE_FORCE": "force", "HLE_FORCE_BIG": "force", "NVN": "nvn", "NVN_NOPROTO": "nvn", "VK": "hle", "VKB": "hle", "GL": "hle", "GLB": "hle", "EGL": "hle",
          "PASS": "hle", "HLE_LINKAGE": "hostfn"}   # the host table each entry registers in (a name is unique within one; HLE_LINKAGE functions are in none)
API = {"NVN": "NVN", "NVN_NOPROTO": "NVN", "VK": "Vulkan", "VKB": "Vulkan", "GL": "GL", "GLB": "GL", "EGL": "EGL", "PASS": "C library"}   # a named entry's API, for its messages
NATIVE = "DM_NATIVE_EXEC"   # plat.h: 1 where the guest's code runs natively (Apple arm64), 0 for every build the generator serves
# The arena guard (DM_AOT_ARENA_CHECK=1, docs/env_switches.tsv; dm/host/aot.h, "Host memory the guest sees"): each thunk checks its entry's pointer parameters and
# pointer result against the arena below 2^39 (its bounds read from aot.h) and logs a [RED] line once per entry and position for an address outside it. Parameters and
# results of an opaque handle type are left out: the guest never reads through them, and a driver's own handles live where the driver puts them (the Vulkan handles
# learn_vulkan_header finds, and EGL's, which dm/gfx/hostvk/guest_gl.c declares as void *)
OPAQUE = {"EGLDisplay", "EGLConfig", "EGLContext", "EGLSurface"}
AOT_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "aot.h")

# Scalar types by C spelling (qualifiers removed, words joined by one space): (class, canonical C type).
# Classes: x32 / x64 integer in an x register; f32 / f64 in a v register; small: a sub-32-bit integer (accepted as a return only)
SCALARS = {
    "int": ("x32", "int32_t"), "signed": ("x32", "int32_t"), "signed int": ("x32", "int32_t"), "int32_t": ("x32", "int32_t"),
    "unsigned": ("x32", "uint32_t"), "unsigned int": ("x32", "uint32_t"), "uint32_t": ("x32", "uint32_t"), "Result": ("x32", "uint32_t"),
    "int64_t": ("x64", "int64_t"), "uint64_t": ("x64", "uint64_t"), "long long": ("x64", "int64_t"), "long long int": ("x64", "int64_t"),
    "signed long long": ("x64", "int64_t"), "unsigned long long": ("x64", "uint64_t"), "unsigned long long int": ("x64", "uint64_t"),
    "size_t": ("x64", "uint64_t"), "ssize_t": ("x64", "int64_t"), "intptr_t": ("x64", "int64_t"), "uintptr_t": ("x64", "uint64_t"),
    "ptrdiff_t": ("x64", "int64_t"), "off_t": ("x64", "int64_t"),
    "float": ("f32", "float"), "double": ("f64", "double"),
    "bool": ("small", "uint8_t"), "_Bool": ("small", "uint8_t"), "char": ("small", "uint8_t"), "signed char": ("small", "int8_t"),
    "unsigned char": ("small", "uint8_t"), "int8_t": ("small", "int8_t"), "uint8_t": ("small", "uint8_t"), "short": ("small", "int16_t"),
    "short int": ("small", "int16_t"), "signed short": ("small", "int16_t"), "unsigned short": ("small", "uint16_t"),
    "unsigned short int": ("small", "uint16_t"), "int16_t": ("small", "int16_t"), "uint16_t": ("small", "uint16_t"),
}
# long: 64 bits on the guest and on LP64 hosts, 32 bits on Windows (LLP64). The thunk declares and calls the function with its own long, so it is exact on
# Linux; on Windows such an entry sees the low 32 bits of a 64-bit guest argument. Counted in the summary: those entries should say int64_t / uint64_t / size_t
LONGS = {"long": ("xl", "long"), "long int": ("xl", "long"), "signed long": ("xl", "long"), "signed long int": ("xl", "long"),
         "unsigned long": ("xul", "unsigned long"), "unsigned long int": ("xul", "unsigned long")}
QUALIFIERS = {"const", "volatile", "restrict", "__restrict", "__restrict__", "register"}
TYPE_WORDS = {"signed", "unsigned", "int", "long", "short", "char", "float", "double", "void", "_Bool", "bool"}

TOKEN = re.compile(r"""
    (?P<ws>[ \t\r\f\v]+) | (?P<nl>\n) | (?P<lc>//[^\n]*) | (?P<bc>/\*.*?\*/) |
    (?P<str>(?:u8|[uUL])?"(?:[^"\\\n]|\\.)*") | (?P<chr>(?:[uUL])?'(?:[^'\\\n]|\\.)*') |
    (?P<id>[A-Za-z_]\w*) | (?P<num>\.?\d(?:[eEpP][+-]|[\w.])*) | (?P<ell>\.\.\.) | (?P<p>\#\#|->|.)
""", re.S | re.X)


class GenError(Exception):
    pass


def tokens(text, path):
    """(kind, value, line) for the C tokens of text, comments removed. A preprocessor directive is one ('pp', text, line) token"""
    out, line, pos, bol = [], 1, 0, True   # bol: only whitespace since the last newline
    while pos < len(text):
        if bol and text[pos] == "#":
            start_line, parts = line, []
            while pos < len(text):   # up to the newline that no backslash continues; comments removed
                m = TOKEN.match(text, pos)
                if m.lastgroup == "nl":
                    if parts and parts[-1].endswith("\\"):
                        parts[-1] = parts[-1][:-1]; line += 1; pos = m.end(); continue
                    break
                if m.lastgroup == "bc":
                    line += m.group().count("\n"); parts.append(" ")
                elif m.lastgroup != "lc":
                    parts.append(m.group())
                pos = m.end()
            out.append(("pp", "".join(parts).strip(), start_line))
            continue
        m = TOKEN.match(text, pos)
        if not m:
            raise GenError(f"{path}:{line}: cannot tokenize")
        kind, val = m.lastgroup, m.group()
        pos = m.end()
        if kind == "nl":
            line += 1; bol = True; continue
        if kind == "ws":
            continue
        if kind in ("lc", "bc"):
            line += val.count("\n")
            continue
        bol = False
        out.append((kind, val, line))
    return out


def unescape(lit):
    """The characters of a C string literal (mangled names are ASCII)"""
    body = lit[lit.index('"') + 1:-1]
    out, i = [], 0
    simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?"}
    while i < len(body):
        c = body[i]
        if c == "\\":
            n = body[i + 1]
            if n == "x":
                m = re.match(r"[0-9a-fA-F]+", body[i + 2:]); out.append(chr(int(m.group(), 16))); i += 2 + len(m.group()); continue
            if n in "01234567":
                m = re.match(r"[0-7]{1,3}", body[i + 1:]); out.append(chr(int(m.group(), 8))); i += 1 + len(m.group()); continue
            out.append(simple.get(n, n)); i += 2; continue
        out.append(c); i += 1
    return "".join(out)


def split_args(toks, i, path):
    """Arguments of the invocation whose '(' is toks[i]: (list of token lists, index after ')')"""
    depth, args, cur = 0, [], []
    for j in range(i, len(toks)):
        kind, val, _ = toks[j]
        if kind == "pp":
            raise GenError(f"{path}:{toks[j][2]}: preprocessor directive inside a macro invocation's arguments")
        if kind == "p" and val in "([{":
            depth += 1
            if depth == 1:
                continue
        elif kind == "p" and val in ")]}":
            depth -= 1
            if depth == 0:
                args.append(cur)
                return args, j + 1
        elif kind == "p" and val == "," and depth == 1:
            args.append(cur); cur = []; continue
        cur.append(toks[j])
    raise GenError(f"{path}:{toks[i][2]}: unterminated macro invocation")


def define(pp, path, line):
    """A #define directive as (name, macro): ('alias', target), ('obj',) or ('fn', params, variadic, body tokens); None for a malformed one"""
    m = re.match(r"#\s*define\s+([A-Za-z_]\w*)(\()?", pp)
    if not m:
        return None
    name, rest = m.group(1), pp[m.end():]
    if not m.group(2):
        body = tokens(rest, path)
        return name, ("alias", body[0][1]) if len(body) == 1 and body[0][0] == "id" else ("obj",)
    close = rest.index(")")
    params = [x.strip() for x in rest[:close].split(",") if x.strip()]
    variadic = bool(params) and params[-1] == "..."
    if variadic:
        params = params[:-1]
    return name, ("fn", params, variadic, [(k, v, line) for k, v, _ in tokens(rest[close + 1:], path)])


class Scope:
    """The preprocessor and typedef state of one source file: macros (entry macro aliases, wrapper macros) and typedef names with their class"""
    def __init__(self):
        self.macros = {}
        self.typedefs = {}

    def entry_macro(self, name, seen=()):
        """The base entry macro that name invokes, through aliases; None when it invokes none"""
        m = self.macros.get(name)
        if m is None:
            return name if name in MACROS else None
        if m[0] == "alias" and name not in seen:
            return self.entry_macro(m[1], seen + (name,))
        return None

    def wrapper(self, name, seen=()):
        """True for a function-like macro of the file whose body invokes an entry macro or another wrapper"""
        m = self.macros.get(name)
        if not m or m[0] != "fn" or name in seen:
            return False
        return any(k == "id" and (self.entry_macro(v) or self.wrapper(v, seen + (name,))) for k, v, _ in m[3])


def expand(scope, name, args, line, path):
    """The tokens of one wrapper macro invocation: argument substitution, __VA_ARGS__, # (stringizing) and ## (pasting; hle_hid.c's TOUCH)"""
    _, params, variadic, body = scope.macros[name]
    if len(args) == 1 and not args[0] and not params:
        args = []
    if len(args) < len(params) or (len(args) > len(params) and not variadic):
        raise GenError(f"{path}:{line}: macro {name}: {len(args)} arguments for {len(params)} parameters")
    sub = dict(zip(params, args))
    va = []
    for k, a in enumerate(args[len(params):]):
        if k:
            va.append(("p", ",", line))
        va.extend(a)
    if variadic:
        sub["__VA_ARGS__"] = va
    out, paste, j = [], False, 0
    while j < len(body):
        k, v, _ = body[j]
        if v == "#" and j + 1 < len(body) and body[j + 1][1] in sub:   # stringize the argument's spelling
            text = " ".join(t[1] for t in sub[body[j + 1][1]])
            piece = [("str", '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"', line)]
            j += 2
        elif v == "##":
            if not out or j + 1 == len(body):
                raise GenError(f"{path}:{line}: macro {name}: ## at the edge of its body")
            paste = True; j += 1; continue
        else:
            piece = [(kk, vv, line) for kk, vv, _ in sub[v]] if k == "id" and v in sub else [(k, v, line)]
            j += 1
        if paste and piece:   # glue the last token so far and the first of this piece into one token
            glued = tokens(out[-1][1] + piece[0][1], path)
            if len(glued) != 1:
                raise GenError(f"{path}:{line}: macro {name}: ## does not form one token from {out[-1][1]!r} and {piece[0][1]!r}")
            out[-1] = (glued[0][0], glued[0][1], line)
            piece = piece[1:]
        paste = False
        out.extend(piece)
    return out


def resolve_words(words, typedefs):
    """(class, canonical type) of a type spelled by words without qualifiers, pointers or a name; None when unknown"""
    key = " ".join(words)
    if key in SCALARS:
        return SCALARS[key]
    if key in LONGS:
        return LONGS[key]
    if len(words) == 1 and words[0] in typedefs:
        return typedefs[words[0]]
    return None


def learn_typedef(scope, decl):
    """Record the names 'typedef <decl>;' declares with their class: ptr, fnptr (a pointer to a function), a scalar's (class, type), ('struct', tag) or ('unknown', spelling)"""
    vals = [v for _, v, _ in decl]
    for k in range(len(decl) - 3):   # a function pointer: typedef ret (*name)(...)
        if vals[k] == "(" and vals[k + 1] == "*" and decl[k + 2][0] == "id" and vals[k + 3] == ")":
            scope.typedefs[vals[k + 2]] = ("fnptr", "void *")
            return
    depth, parts, cur = 0, [], []   # the declarators, split at top-level commas
    for t in decl:
        if t[1] in "({[":
            depth += 1
        elif t[1] in ")}]":
            depth -= 1
        if t[1] == "," and depth == 0:
            parts.append(cur); cur = []; continue
        cur.append(t)
    parts.append(cur)

    def name_at(p):
        d, last = 0, None
        for k, (kind, v, _) in enumerate(p):
            if v in "({[":
                d += 1
            elif v in ")}]":
                d -= 1
            elif kind == "id" and d == 0 and v not in QUALIFIERS:
                last = k
        return last
    n0 = name_at(parts[0])
    if n0 is None:
        return
    base = parts[0][:n0]
    while base and base[-1][1] == "*":
        base = base[:-1]
    bwords = [v for _, v, _ in base if v not in QUALIFIERS]
    aggregate = bool(bwords) and bwords[0] in ("struct", "union")
    single = one_member_x64(base, scope.typedefs) if aggregate and bwords[0] == "struct" else None
    for idx, p in enumerate(parts):
        n = name_at(p)
        if n is None:
            continue
        name = p[n][1]
        head = p[len(base):n] if idx == 0 else p[:n]
        if any(v == "*" for _, v, _ in head) or any(v == "[" for _, v, _ in p[n + 1:]):
            scope.typedefs[name] = ("ptr", "void *")
        elif single:
            scope.typedefs[name] = single
        elif aggregate:
            scope.typedefs[name] = ("struct", " ".join(bwords[:2]))
        elif bwords and bwords[0] == "enum":
            scope.typedefs[name] = ("x32", "uint32_t")   # an enum is int-sized on every host ABI when its values fit, as the Switch passes it in a w register
        else:
            scope.typedefs[name] = resolve_words(bwords, scope.typedefs) or ("unknown", " ".join(bwords))


def one_member_x64(base, typedefs):
    """("x64", canonical type) for a struct whose body is one member of a 64-bit integer type (struct { uint64_t value; }), else None"""
    vals = [v for _, v, _ in base]
    if "{" not in vals or vals.count("{") != 1 or "}" not in vals:
        return None
    body = base[vals.index("{") + 1:vals.index("}")]
    decls = [d for d in " ".join(v for _, v, _ in body).split(";") if d.strip()]
    if len(decls) != 1 or any(c in decls[0] for c in "*[],:(") or [v for _, v, _ in body][-1:] != [";"]:
        return None
    words = [w for w in decls[0].split() if w not in QUALIFIERS]
    if len(words) < 2:
        return None
    r = resolve_words(words[:-1], typedefs)   # the member's type, without its name
    return r if r and r[0] == "x64" else None


# The forms of a Khronos Vulkan header (generated from the API registry, one declaration per line): handles, enums, structs and unions, function pointer types, and
# typedefs of integer types and of other Vulkan types (the flags, VkBool32, VkDeviceSize, the KHR aliases of promoted names)
VK_HANDLE = re.compile(r"^\s*VK_DEFINE(?:_NON_DISPATCHABLE)?_HANDLE\((\w+)\)")
VK_TAGGED = re.compile(r"^\s*typedef\s+(enum|struct|union)\s+(\w+)\s*\{")
VK_PFN = re.compile(r"^\s*typedef\s+[\w\s*]+?\(\s*VKAPI_PTR\s*\*\s*(\w+)\s*\)")
VK_TYPEDEF = re.compile(r"^\s*typedef\s+([\w\s*]+?)\s*\b(\w+)\s*;")


def learn_vulkan_header(path, scope, seen):
    """The typedef names a Khronos Vulkan header declares, with their class: a handle a pointer (VK_DEFINE_HANDLE, and VK_DEFINE_NON_DISPATCHABLE_HANDLE on 64-bit hosts:
    a pointer there, and an 8-byte integer in one x register on the guest either way), an enum 32 bits, a function pointer type fnptr, a struct or union by value
    unsupported (entries take them by pointer), any other typedef its type's class. Read line by line, not as C: the header's include guard and platform conditionals
    would hide every typedef from parse_file. Its own include (vk_platform.h) declares no Vulkan type"""
    real = os.path.realpath(path)
    if real in seen:
        return
    seen.add(real)
    with open(path, encoding="utf-8") as f:
        text = re.sub(r"/\*.*?\*/", " ", f.read(), flags=re.S)
    for line in text.split("\n"):
        line = line.split("//", 1)[0]
        m = VK_HANDLE.match(line)
        if m:
            scope.typedefs[m.group(1)] = ("ptr", "void *")
            OPAQUE.add(m.group(1))   # the arena guard leaves handles out
            continue
        m = VK_TAGGED.match(line)
        if m:
            scope.typedefs[m.group(2)] = ("x32", "uint32_t") if m.group(1) == "enum" else ("struct", f"{m.group(1)} {m.group(2)}")
            continue
        m = VK_PFN.match(line)
        if m:
            scope.typedefs[m.group(1)] = ("fnptr", "void *")
            continue
        m = VK_TYPEDEF.match(line)
        if not m or "(" in line:
            continue
        spelled = m.group(1)
        words = [w for w in spelled.replace("*", " ").split() if w not in QUALIFIERS]
        if "*" in spelled:
            scope.typedefs[m.group(2)] = ("ptr", "void *")
        elif words and words[0] in ("struct", "union"):
            scope.typedefs[m.group(2)] = ("struct", " ".join(words[:2]))
        elif words and words[0] == "enum":
            scope.typedefs[m.group(2)] = ("x32", "uint32_t")
        else:
            scope.typedefs[m.group(2)] = resolve_words(words, scope.typedefs) or ("unknown", " ".join(words))


def parse_file(path, scope=None, entries=None, seen=None, incs=()):
    """The entries of a source file. The typedefs and macros of headers it includes with quotes (beside it) are learned; headers hold no entries"""
    scope = scope or Scope()
    entries = [] if entries is None else entries
    seen = set() if seen is None else seen
    real = os.path.realpath(path)
    if real in seen:
        return entries
    seen.add(real)
    with open(path, encoding="utf-8") as f:
        toks = tokens(f.read(), path)
    conds = []   # [directive, line, state, taken]: state 1 = compiled, 0 = not compiled, None = unknown to the generator; taken: an earlier branch was compiled (None: unknown)

    def active():
        return all(c[2] for c in conds)

    def state(dname, arg):   # what the generator knows of a condition: #if 0 / #if 1, __cplusplus, which a C source never has, and DM_NATIVE_EXEC, 0 for every build it serves
        a = re.sub(r"\s+", "", arg)
        if dname in ("if", "elif") and a in ("0", "1"):
            return int(a)
        if dname in ("ifdef", "ifndef") and a == "__cplusplus":
            return 0 if dname == "ifdef" else 1
        if dname in ("if", "elif") and (a == NATIVE or a.startswith(NATIVE + "&&")):
            return 0
        if dname in ("if", "elif") and a == "!" + NATIVE:
            return 1
        return None

    i, brace = 0, 0
    while i < len(toks):
        kind, val, line = toks[i]
        if kind == "pp":
            d = re.match(r"#\s*(\w+)\s*(.*)", val, re.S)
            dname, arg = (d.group(1), d.group(2).strip()) if d else ("", "")
            if dname in ("if", "ifdef", "ifndef"):
                st = state(dname, arg)
                conds.append([val, line, st, None if st is None else st == 1])
            elif dname in ("elif", "elifdef", "elifndef", "else"):
                if not conds:
                    raise GenError(f"{path}:{line}: #{dname} without #if")
                taken = conds[-1][3]
                if dname == "else":
                    conds[-1] = [conds[-1][0] + " ... #else", line, None if taken is None else int(not taken), True if taken is not None else None]
                else:   # compiled only when no earlier branch was and its own condition holds
                    st = 0 if taken else state(dname, arg) if taken is not None else None
                    conds[-1] = [val, line, st, True if taken or st == 1 else None if st is None else False]
            elif dname == "endif":
                if not conds:
                    raise GenError(f"{path}:{line}: #endif without #if")
                conds.pop()
            elif dname == "define" and active():
                dm = define(val, path, line)
                if dm and dm[0] in MACROS and dm[1][0] == "fn":
                    scope.macros.pop(dm[0], None)   # hle.h's own definition: the entry macro itself
                elif dm:
                    scope.macros[dm[0]] = dm[1]
            elif dname == "undef" and active() and arg:
                scope.macros[arg.split()[0]] = ("obj",)   # no longer an entry macro or a wrapper
            elif dname == "include" and active():
                m = re.match(r'"([^"]+)"', arg)
                v = re.match(r'[<"](vulkan/[^>"]+)[>"]', arg)
                if v:   # a Khronos Vulkan header: found in the -I directories, read by its own rules
                    for d in incs:
                        inc = os.path.join(d, v.group(1))
                        if os.path.isfile(inc):
                            learn_vulkan_header(inc, scope, seen)
                            break
                elif m:
                    for d in (os.path.dirname(path),) + tuple(incs):   # beside the file, then the -I directories
                        inc = os.path.join(d, m.group(1))
                        if os.path.isfile(inc):
                            if not inc.endswith(".c"):
                                parse_file(inc, scope, [], seen, incs)
                            break
            i += 1
            continue
        if any(c[2] == 0 for c in conds):   # not compiled: its braces do not count (an extern "C" { of a __cplusplus block)
            i += 1
            continue
        if kind == "p" and val == "{":
            brace += 1
        elif kind == "p" and val == "}":
            brace -= 1
        if kind == "id" and val == "typedef" and brace == 0:
            j, depth = i + 1, 0
            while j < len(toks) and not (toks[j][1] == ";" and depth == 0):
                if toks[j][1] in "({[":
                    depth += 1
                elif toks[j][1] in ")}]":
                    depth -= 1
                j += 1
            if active():
                learn_typedef(scope, toks[i + 1:j])
            i = j + 1
            continue
        if kind == "id" and val == "HLE_LINKAGE" and brace == 0:   # a host function the guest calls through a pointer the host hands it: ret cname(params) {
            i = take_linkage(scope, toks, i, conds, path, line, entries)
            continue
        base = scope.entry_macro(val) if kind == "id" else None
        wrap = kind == "id" and not base and scope.wrapper(val)
        if (base or wrap) and i + 1 < len(toks) and toks[i + 1][1] == "(" and not (i > 0 and toks[i - 1][1] in (".", "->")):
            args, nxt = split_args(toks, i + 1, path)
            if any(c[2] == 0 for c in conds):   # in an #if 0 region: not compiled
                i = nxt; continue
            unknown = next((c for c in conds if c[2] is None), None)
            if unknown:
                raise GenError(f"{path}:{line}: {val} entry inside '{unknown[0]}' (line {unknown[1]}): the generator cannot tell whether it is compiled; "
                               "move it out of the conditional or teach the generator the condition")
            if wrap:
                if sum(1 for t in toks if t[0] == "id") > 10 ** 6:
                    raise GenError(f"{path}:{line}: macro {val}: expansion does not end")
                toks[i:nxt] = expand(scope, val, args, line, path)   # scanned again: the expansion holds the entry (or another wrapper)
                continue
            take_entry(scope, base, val, args, path, line, entries)
            i = nxt
            continue
        i += 1
    if conds:
        raise GenError(f"{path}:{conds[-1][1]}: '{conds[-1][0]}' without #endif")
    return entries


def take_entry(scope, base, name, args, path, line, entries):
    first = MACROS[base]
    if len(args) < first + 1 or not args[first - 1]:
        raise GenError(f"{path}:{line}: {name}( with {len(args)} arguments")
    if base in NAMED:   # NVN(name, ret, params...), VK(name, ret, params...), GL / EGL(name, ret, params...), PASS(...): the entry is the API function, its C function h_<name> (libc_<name>)
        nm = args[0]
        if len(nm) != 1 or nm[0][0] != "id":
            raise GenError(f"{path}:{line}: {name}: the {API[base]} function's name is not one identifier")
        entries.append({
            "macro": base, "path": path, "line": line, "mangled_lit": f'"{nm[0][1]}"', "mangled": nm[0][1], "cname": PREFIX.get(base, "h_") + nm[0][1],
            "typedefs": dict(scope.typedefs), "ret": [(k, v) for k, v, _ in args[1]], "params": [[(k, v) for k, v, _ in p] for p in args[first:]],
        })
        return
    mangled = args[0]
    if not mangled or any(k != "str" for k, _, _ in mangled):
        raise GenError(f"{path}:{line}: {name}: the mangled name is not a string literal")
    ret, cname = args[first - 2], args[first - 1]
    if len(cname) != 1 or cname[0][0] != "id":
        raise GenError(f"{path}:{line}: {name}: the C name is not one identifier")
    entries.append({
        "macro": base, "path": path, "line": line, "mangled_lit": " ".join(v for _, v, _ in mangled),
        "mangled": "".join(unescape(v) for _, v, _ in mangled), "cname": cname[0][1], "typedefs": dict(scope.typedefs),
        "ret": [(k, v) for k, v, _ in ret], "params": [[(k, v) for k, v, _ in p] for p in args[first:]],
    })


def take_linkage(scope, toks, i, conds, path, line, entries):
    """HLE_LINKAGE <ret> <cname>(<params>) at toks[i] outside a macro: static on native builds, external elsewhere (hle.h). A definition is an entry of its own, a host
    function the guest reaches only through a pointer the host hands it (dm_guest_fnptr: a vtable slot, a thread entry); a declaration (;) is not. Returns the index after
    the parameter list"""
    j = i + 1
    while j < len(toks) and toks[j][1] not in ("(", ";", "{", "}"):
        j += 1
    if j >= len(toks) or toks[j][1] != "(" or j - i < 3 or toks[j - 1][0] != "id":
        raise GenError(f"{path}:{line}: HLE_LINKAGE not followed by '<return type> <name>(<parameters>)'")
    args, nxt = split_args(toks, j, path)
    if any(c[2] == 0 for c in conds):   # not compiled
        return nxt
    if nxt >= len(toks) or toks[nxt][1] not in ("{", ";"):
        raise GenError(f"{path}:{line}: HLE_LINKAGE {toks[j - 1][1]}(...) is followed by '{toks[nxt][1] if nxt < len(toks) else 'the end of the file'}', not a body or ';'")
    if toks[nxt][1] == ";":   # a declaration: the definition is the entry
        return nxt
    unknown = next((c for c in conds if c[2] is None), None)
    if unknown:
        raise GenError(f"{path}:{line}: HLE_LINKAGE function {toks[j - 1][1]} inside '{unknown[0]}' (line {unknown[1]}): the generator cannot tell whether it is compiled; "
                       "move it out of the conditional or teach the generator the condition")
    cname = toks[j - 1][1]
    entries.append({
        "macro": "HLE_LINKAGE", "path": path, "line": line, "mangled_lit": f'"{cname}"', "mangled": cname, "cname": cname, "typedefs": dict(scope.typedefs),
        "ret": [(k, v) for k, v, _ in toks[i + 1:j - 1]], "params": [[(k, v) for k, v, _ in p] for p in args],
    })
    return nxt


def classify(toks, typedefs, path, line, what):
    """(class, canonical C type) of a parameter or return type given as tokens (the parameter's name may be among them); GenError when unknown"""
    vals = [v for _, v in toks]
    text = " ".join(vals)
    if "..." in vals:
        raise GenError(f"{path}:{line}: {what} '{text}': variadic parameters are not supported (hle.h: the Switch passes variadic arguments in registers)")
    if "*" in vals or "[" in vals:
        return ("ptr", "void *")
    words = [v for v in vals if v not in QUALIFIERS]
    if words and words[0] in ("struct", "union"):
        raise GenError(f"{path}:{line}: {what} '{text}': a struct or union by value is not supported")
    if words and words[0] == "enum":
        return ("x32", "uint32_t")
    if len(words) >= 2 and words[-1] not in TYPE_WORDS and words[-1] not in SCALARS and words[-1] not in typedefs:
        words = words[:-1]   # the parameter's name
    if words == ["void"]:
        return ("void", "void")
    r = resolve_words(words, typedefs)
    if r is None or r[0] == "unknown":
        raise GenError(f"{path}:{line}: {what} '{text}': unknown type '{' '.join(words)}' (known: the fixed-width integers, int, unsigned, long, size_t, "
                       "float, double, pointers, and typedefs of them in the source or its quoted headers)")
    if r[0] == "struct":
        raise GenError(f"{path}:{line}: {what} '{text}': '{' '.join(words)}' is a {r[1]} by value, which is not supported")
    return r


def opaque(toks, typedefs):
    """Whether a pointer-class type given as tokens (the parameter's name may be among them) is an opaque handle type (OPAQUE): one typedef name, no * of its own"""
    words = [v for _, v in toks if v not in QUALIFIERS]
    if len(words) == 2 and words[0] in typedefs:
        words = words[:1]   # the parameter's name
    return len(words) == 1 and words[0] in OPAQUE


def analyse(e):
    p, ln, td = e["path"], e["line"], e["typedefs"]
    e["rcls"], e["rtype"] = classify(e["ret"], td, p, ln, "return type")
    e["check_ret"] = e["rcls"] == "ptr" and not opaque(e["ret"], td)   # the arena guard: a pointer result
    e["checks"] = []   # the arena guard: (1-based position, register) of each pointer parameter
    params = e["params"]
    if len(params) == 1 and [v for _, v in params[0]] == ["void"]:
        params = []
    args, nx, nv, nsaa = [], 0, 0, 0
    e["name_arg"] = None   # a function pointer return's lookup name: the first parameter spelled const char *
    for k, toks in enumerate(params):
        if not toks:
            raise GenError(f"{p}:{ln}: parameter {k + 1} is empty")
        cls, ctype = classify(toks, td, p, ln, f"parameter {k + 1}")
        if cls == "void":
            raise GenError(f"{p}:{ln}: parameter {k + 1}: void")
        if cls == "fnptr":   # a guest function's address the guest passes: as any pointer
            cls = "ptr"
        check = cls == "ptr" and not opaque(toks, td)
        words = [v for _, v in toks]
        if e["name_arg"] is None and words[:3] in (["const", "char", "*"], ["char", "const", "*"]) and len(words) <= 4:
            e["name_arg"] = len(args)
        if cls == "small":
            raise GenError(f"{p}:{ln}: parameter {k + 1} '{' '.join(v for _, v in toks)}': a sub-32-bit parameter (the Switch caller does not extend it; "
                           "hle.h: declare it as int or unsigned)")
        if cls in ("f32", "f64") and nv < 8:
            args.append((cls, ctype, f"v{nv}")); nv += 1
        elif cls not in ("f32", "f64") and nx < 8:
            args.append((cls, ctype, f"x{nx}")); nx += 1
        else:   # AAPCS64: the next 8-byte slot of the stacked arguments (every accepted type is at most 8 bytes)
            args.append((cls, ctype, f"s{nsaa}")); nsaa += 8
        if check:
            e["checks"].append((k + 1, args[-1][2]))
    e["args"] = args


def load(reg, ctype):
    """C expression reading an argument from the state block (or the guest stack) as ctype"""
    n = int(reg[1:])
    if reg[0] == "x":
        return f"(void *)(uintptr_t)st->x[{n}]" if ctype == "void *" else f"({ctype})st->x[{n}]"
    if reg[0] == "v":
        return f"ld_{'f' if ctype == 'float' else 'd'}(st->v[{n}])"
    if ctype in ("float", "double"):
        return f"ld_{'f' if ctype == 'float' else 'd'}((const void *)(uintptr_t)(st->sp + {n}))"
    v = f"ld_stk(st, {n})"
    return f"(void *)(uintptr_t){v}" if ctype == "void *" else f"({ctype}){v}"


ZEXT = {"int32_t": "uint32_t", "uint32_t": "uint32_t", "int16_t": "uint16_t", "uint16_t": "uint16_t", "int8_t": "uint8_t", "uint8_t": "uint8_t"}


def arena_bounds():
    """DM_AOT_ARENA_BASE and DM_AOT_ARENA_SIZE from dm/host/aot.h, for the arena guard"""
    try:
        with open(AOT_H, encoding="utf-8") as f:
            text = f.read()
    except OSError as x:
        raise GenError(f"{AOT_H}: {x.strerror} (the arena guard reads the arena's bounds there)")
    got = [re.search(rf"^#define {n} (0x[0-9a-fA-F]+)ull\b", text, re.M) for n in ("DM_AOT_ARENA_BASE", "DM_AOT_ARENA_SIZE")]
    if not all(got):
        raise GenError(f"{AOT_H}: no '#define DM_AOT_ARENA_BASE 0x...ull' and '#define DM_AOT_ARENA_SIZE 0x...ull' (the arena guard's bounds)")
    return int(got[0].group(1), 16), int(got[1].group(1), 16)


def render(entries, sources, bounds=None):
    out = []
    w = out.append
    w("// Generated by dm/host/tools/gen_hle_thunks.py from the HLE entries of the host sources below; do not edit. One thunk per entry: the arguments from the guest state")
    w("// block as AAPCS64 passes them (integers and pointers x0..x7, float and double v0..v7, the rest in the guest stack's 8-byte slots), the result into x0 (32 bits and")
    w("// fewer zero-extended) or v0 (docs/design/aot-recompiler.md 2.4). The C functions are declared with canonical types of the same ABI class as their definitions")
    w("// (a pointer as void *, an integer by its width and sign), since their own parameter types are often local to their files")
    for s in sources:
        w(f"//   {s}")
    w("#include <stdint.h>")
    w("#include <string.h>")
    w('#include "aot_hostfn.h"')
    w("")
    w("static inline float ld_f(const void *p) { float f; memcpy(&f, p, sizeof f); return f; }")
    w("static inline double ld_d(const void *p) { double d; memcpy(&d, p, sizeof d); return d; }")
    w("static inline uint64_t ld_stk(const dm_guest_state *st, uint64_t off) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)(st->sp + off), sizeof v); return v; }")
    w("static inline void st_f(dm_guest_state *st, float f) { memset(st->v[0], 0, sizeof st->v[0]); memcpy(st->v[0], &f, sizeof f); }")
    w("static inline void st_d(dm_guest_state *st, double d) { memset(st->v[0], 0, sizeof st->v[0]); memcpy(st->v[0], &d, sizeof d); }")
    w("")
    if any(e["rcls"] == "fnptr" for e in entries):
        w("// A host function an entry hands the guest (a function pointer return): its guest-visible address (dm/host/aot_arena.c; aot.h)")
        w("void *dm_guest_fnptr_named(const void *fn, const char *name);")
        w("")
    if any(e["checks"] or e["check_ret"] for e in entries):
        base, size = bounds if bounds else arena_bounds()
        w("// The arena guard (DM_AOT_ARENA_CHECK=1, docs/env_switches.tsv; dm/host/aot.h, \"Host memory the guest sees\"): a pointer parameter or result of an entry that lies outside")
        w("// the arena (its bounds read from aot.h by the generator) is a [RED] line, once per entry and position (0: the result). Addresses below 64 KiB are sentinels, not memory;")
        w("// opaque handle types are not checked. Self-contained, so a table links without the engine (t_hle_thunks)")
        w("#include <stdatomic.h>")
        w("#include <stdio.h>")
        w("#include <stdlib.h>")
        w(f"#define HLE_ARENA_BASE {base:#x}ull")
        w(f"#define HLE_ARENA_SIZE {size:#x}ull")
        w("static _Atomic int hle_arena_on = -1;")
        w("static int hle_arena_check(void) {")
        w("    int v = atomic_load_explicit(&hle_arena_on, memory_order_relaxed);")
        w("    if (v < 0) { const char *e = getenv(\"DM_AOT_ARENA_CHECK\"); v = e && *e && strcmp(e, \"0\") ? 1 : 0; atomic_store_explicit(&hle_arena_on, v, memory_order_relaxed); }")
        w("    return v;")
        w("}")
        w("static void hle_arena_bad(_Atomic unsigned *seen, int pos, uint64_t v, const char *cname) {")
        w("    const unsigned bit = 1u << (pos < 31 ? pos : 31);")
        w("    if (atomic_fetch_or(seen, bit) & bit) return;")
        w("    if (pos) fprintf(stderr, \"[dm] [RED] aot: arena check: the guest passes %s an address outside the arena %#llx..%#llx in argument %d: %#llx (host memory it was handed: \"")
        w("                     \"dm/host/aot.h, \\\"Host memory the guest sees\\\")\\n\", cname, HLE_ARENA_BASE, HLE_ARENA_BASE + HLE_ARENA_SIZE, pos, (unsigned long long)v);")
        w("    else fprintf(stderr, \"[dm] [RED] aot: arena check: %s returns the guest an address outside the arena %#llx..%#llx: %#llx (dm/host/aot.h, \\\"Host memory the guest \"")
        w("                 \"sees\\\")\\n\", cname, HLE_ARENA_BASE, HLE_ARENA_BASE + HLE_ARENA_SIZE, (unsigned long long)v);")
        w("}")
        w("#define HLE_ARENA(v, seen, pos, cname)                                                                                   \\")
        w("    do {                                                                                                                 \\")
        w("        const uint64_t v_ = (uint64_t)(v);                                                                               \\")
        w("        if (v_ >= 0x10000 && v_ - HLE_ARENA_BASE >= HLE_ARENA_SIZE && hle_arena_check()) hle_arena_bad(&(seen), (pos), v_, (cname)); \\")
        w("    } while (0)")
        w("")
    for e in entries:
        params = ", ".join(t for _, t, _ in e["args"]) or "void"
        w(f"extern {e['rtype']} {e['cname']}({params});   // {e['path']}:{e['line']}")
    w("")
    for e in entries:
        call = f"{e['cname']}({', '.join(load(r, t) for _, t, r in e['args'])})"
        rc = e["rcls"]
        if rc == "void":
            body = f"{call};"
        elif rc == "ptr":
            body = f"st->x[0] = (uint64_t)(uintptr_t){call};"
        elif rc == "fnptr":
            k = e["name_arg"]
            name = f"(const char *){load(e['args'][k][2], 'void *')}" if k is not None else "0"
            body = f"st->x[0] = (uint64_t)(uintptr_t)dm_guest_fnptr_named((const void *){call}, {name});"
        elif rc in ("x64", "xul"):
            body = f"st->x[0] = (uint64_t){call};"
        elif rc == "xl":
            body = f"st->x[0] = (uint64_t)(int64_t){call};"
        elif rc in ("x32", "small"):
            body = f"st->x[0] = (uint64_t)({ZEXT[e['rtype']]}){call};"
        elif rc == "f32":
            body = f"st_f(st, {call});"
        else:
            body = f"st_d(st, {call});"
        unused = "" if e["args"] or rc != "void" else "(void)st; "
        pre = post = ""
        if e["checks"] or e["check_ret"]:   # the arena guard: the pointer parameters before the call, the pointer result after it
            w(f"static _Atomic unsigned hle_seen_{e['cname']};")
            for pos, reg in e["checks"]:
                pre += f"HLE_ARENA({load(reg, 'uint64_t')}, hle_seen_{e['cname']}, {pos}, \"{e['cname']}\"); "
            if e["check_ret"]:
                post = f" HLE_ARENA(st->x[0], hle_seen_{e['cname']}, 0, \"{e['cname']}\");"
        w(f"static void hle_thunk_{e['cname']}(dm_guest_state *st) {{ {unused}{pre}{body}{post} }}")
    w("")
    w("const hle_thunk_ent hle_thunks[] = {")
    for e in entries:
        w(f"    {{{e['mangled_lit']}, \"{e['cname']}\", hle_thunk_{e['cname']}, (const void *){e['cname']}}},")
    if not entries:
        w("    {0, 0, 0, 0},")
    w("};")
    w(f"const size_t hle_thunks_count = {len(entries)};")
    return "\n".join(out) + "\n"


NAMES = {"ptr": "pointer", "fnptr": "host function pointer", "x64": "64-bit integer", "x32": "32-bit integer", "xl": "long", "xul": "unsigned long",
         "small": "sub-32-bit integer", "f32": "float", "f64": "double", "void": "void"}


def tally(items):
    d = {}
    for x in items:
        d[x] = d.get(x, 0) + 1
    return ", ".join(f"{d[k]} {NAMES[k]}" for k in NAMES if k in d) or "none"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Generate the AOT host-call thunks from the HLE entries of the host sources")
    ap.add_argument("-o", "--out", required=True, help="the C file to write")
    ap.add_argument("-I", dest="incs", action="append", default=[], help="a directory to find quoted headers in after the including file's own (as the compiler's -I)")
    ap.add_argument("sources", nargs="+", help="host sources: exactly the ones compiled into the host")
    a = ap.parse_args(argv)
    errors, entries = [], []
    for src in a.sources:
        try:
            for e in parse_file(src, incs=tuple(a.incs)):
                try:
                    analyse(e)
                    entries.append(e)
                except GenError as x:
                    errors.append(str(x))
        except GenError as x:
            errors.append(str(x))
        except OSError as x:
            errors.append(f"{src}: {x.strerror}")
    by_cname, by_mangled = {}, {}
    for e in entries:
        o = by_cname.setdefault(e["cname"], e)
        if o is not e:
            errors.append(f"{e['path']}:{e['line']}: duplicate C name {e['cname']}: also the entry at {o['path']}:{o['line']} (entries are external outside native builds)")
        o = by_mangled.setdefault((TABLES[e["macro"]], e["mangled"]), e)
        if o is not e:
            errors.append(f"{e['path']}:{e['line']}: duplicate entry {e['mangled']}: also at {o['path']}:{o['line']}")
    if errors:
        for x in errors:
            print(f"gen_hle_thunks: error: {x}", file=sys.stderr)
        print(f"gen_hle_thunks: {len(errors)} error(s); nothing written", file=sys.stderr)
        return 1
    # strcmp order of the mangled names; an HLE and an HLE_FORCE entry may share one (they are two tables), the forced one first
    entries.sort(key=lambda e: (e["mangled"].encode("latin-1"), e["macro"] == "HLE"))
    try:
        bounds = arena_bounds() if any(e["checks"] or e["check_ret"] for e in entries) else None
    except GenError as x:
        print(f"gen_hle_thunks: error: {x}", file=sys.stderr)
        return 1
    text = render(entries, a.sources, bounds)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        f.write(text)
    macros = {}
    for e in entries:
        macros[e["macro"]] = macros.get(e["macro"], 0) + 1
    stacked = sum(1 for e in entries if any(r[0] == "s" for _, _, r in e["args"]))
    longs = sum(1 for e in entries if e["rcls"] in ("xl", "xul") or any(c in ("xl", "xul") for c, _, _ in e["args"]))
    print(f"gen_hle_thunks: {len(entries)} entries ({', '.join(f'{v} {k}' for k, v in sorted(macros.items()))}) from {len(a.sources)} files -> {a.out}")
    print(f"gen_hle_thunks: parameters: {tally(c for e in entries for c, _, _ in e['args'])}; 0 unknown types")
    print(f"gen_hle_thunks: returns: {tally(e['rcls'] for e in entries)}; 0 duplicate C names")
    print(f"gen_hle_thunks: {stacked} entries read arguments from the guest stack; {longs} entries use long (32 bits on Windows)")
    print(f"gen_hle_thunks: arena guard (DM_AOT_ARENA_CHECK): {sum(len(e['checks']) for e in entries)} pointer parameters and {sum(1 for e in entries if e['check_ret'])} "
          f"pointer results checked in {sum(1 for e in entries if e['checks'] or e['check_ret'])} entries")
    fn = [e for e in entries if e["rcls"] == "fnptr"]
    if fn:
        print(f"gen_hle_thunks: {len(fn)} entries return host functions, handed to the guest at their guest-visible addresses: "
              + ", ".join(e["mangled"] + ("" if e["name_arg"] is not None else " (no lookup name)") for e in fn))
    return 0


if __name__ == "__main__":
    sys.exit(main())
