"""The host-call thunk generator (dm/host/tools/gen_hle_thunks.py, docs/design/aot-recompiler.md 2.4): every entry shape it cannot pass as AAPCS64 does (a struct by
value, a sub-32-bit parameter, a variadic entry) and a C name two entries share stop it with the entry's file and line, and nothing is written; over the real host
sources (the list dm/host/CMakeLists.txt gives nxhost) it succeeds with every entry. Temporary files only; no game files, keys or compiler needed."""
import ast
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
GEN = REPO / 'dm/host/tools/gen_hle_thunks.py'
HOST = REPO / 'dm/host'


def host_sources():
    """The nxhost sources of dm/host/CMakeLists.txt: hle.c loader.c thread.c front_sdl.c, the hle_*.c glob without hle_bigstack.c, and the NVN frontend (NVN_FRONTEND_SRC)"""
    cm = (HOST / 'CMakeLists.txt').read_text()
    m = re.search(r'set\(NXHOST_SRC ([^)]*)\)', cm)
    assert m, 'NXHOST_SRC not found in dm/host/CMakeLists.txt'
    assert 'hle_bigstack' in cm
    f = re.search(r'set\(NVN_FRONTEND_SRC([^)]*)\)', cm)
    assert f, 'NVN_FRONTEND_SRC not found in dm/host/CMakeLists.txt'
    assert 'list(APPEND NXHOST_SRC ${NVN_FRONTEND_SRC}' in cm, 'the NVN frontend is not part of nxhost'
    nvn = [Path(n.replace('${REPO}', str(REPO))) for n in f.group(1).split()]
    return [HOST / n for n in m.group(1).split()] + sorted(p for p in HOST.glob('hle_*.c') if p.name != 'hle_bigstack.c') + nvn


NVN_INCLUDE = REPO / 'dm/gfx/nvn/include'   # the -I the CMake rule gives the generator (GEN_HLE_THUNKS_INC)


class Generator(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.out = self.tmp / 'gen/hle_thunks.c'

    def run_gen(self, *sources, incs=()):
        flags = [a for d in incs for a in ('-I', str(d))]
        return subprocess.run([sys.executable, str(GEN), '-o', str(self.out), *flags, *map(str, sources)], capture_output=True, text=True, check=False)

    def source(self, name, text):
        p = self.tmp / name
        p.write_text('#include "hle.h"\n' + text)
        return p

    def fails(self, text, line, *words):
        src = self.source('bad.c', text)
        r = self.run_gen(src)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn(f'{src}:{line}:', r.stderr)
        for w in words:
            self.assertIn(w, r.stderr)
        self.assertFalse(self.out.exists(), 'nothing is written on an error')
        return r.stderr

    def test_struct_parameter(self):
        self.fails('typedef struct { int a, b; } pair;\nHLE_FORCE("f", "why", void, h_f, void *p, pair v) {}\n', 3, 'struct', 'by value')

    def test_struct_keyword_parameter(self):
        self.fails('HLE_FORCE("f", "why", int, h_f, struct timespec t) { return 0; }\n', 2, 'struct or union by value')

    def test_short_parameter(self):
        self.fails('\nHLE_FORCE("f", "why", void, h_f, int a, short s) {}\n', 3, 'sub-32-bit parameter', "'short s'")

    def test_variadic_entry(self):
        self.fails('HLE("printf", int, h_printf, const char *fmt, ...) { return 0; }\n', 2, 'variadic')

    def test_struct_return(self):
        self.fails('typedef struct { int a; } box;\nHLE_FORCE("f", "why", box, h_f, int a) { box b = {a}; return b; }\n', 3, 'return type', 'by value')

    def test_unknown_typedef(self):
        self.fails('HLE_FORCE("f", "why", void, h_f, some_type_t v) {}\n', 2, "unknown type 'some_type_t'")

    def test_unknown_conditional(self):
        self.fails('#if DM_SOMETHING\nHLE_FORCE("f", "why", void, h_f, int v) {}\n#endif\n', 3, "inside '#if DM_SOMETHING'")

    def test_duplicate_c_name(self):
        a = self.source('a.c', '\nHLE_FORCE("f1", "why", void, h_same, int v) {}\n')
        b = self.source('b.c', '\n\nHLE_FORCE("f2", "why", void, h_same, int v) {}\n')
        r = self.run_gen(a, b)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(f'{b}:4: duplicate C name h_same', r.stderr)
        self.assertIn(f'{a}:3', r.stderr)
        self.assertFalse(self.out.exists())

    def test_accepted_shapes(self):
        src = self.source('ok.c', 'typedef uint32_t handle;\n#define W(m, c, ...) HLE_FORCE(m, "why", int, c, __VA_ARGS__)\n'
                          'W("w", h_w, handle h, float f, double d, const void *p)\n{ return 0; }\n#if 0\nHLE_FORCE("x", "why", void, h_x, short s) {}\n#endif\n')
        r = self.run_gen(src)
        self.assertEqual(r.returncode, 0, r.stderr)
        text = self.out.read_text()
        self.assertIn('extern int32_t h_w(uint32_t, float, double, void *);', text)
        self.assertIn('st->x[0] = (uint64_t)(uint32_t)h_w((uint32_t)st->x[0], ld_f(st->v[0]), ld_d(st->v[1]), (void *)(uintptr_t)st->x[1]);', text)
        self.assertNotIn('h_x', text)
        self.assertIn('const size_t hle_thunks_count = 1;', text)

    def test_nvn_entries(self):   # dm/gfx/nvn/hle_nvn.c: NVN(name, ret, params...) and NVN_NOPROTO(...), named by the NVN function, C function h_<name>
        src = self.source('nvn.c', 'typedef struct { uint64_t value; } sep_handle;\ntypedef uint8_t NVNboolean;\n'
                          '#define NVN(name, ret, ...) static ret h_##name(__VA_ARGS__)\n'
                          'NVN(nvnFoo, NVNboolean, void *d, sep_handle h, float f, uint32_t on) { return 1; }\n'
                          'NVN_NOPROTO(nvnBar, sep_handle, const void *d, int id) { sep_handle s = {id}; return s; }\n')
        r = self.run_gen(src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('2 entries (1 NVN, 1 NVN_NOPROTO)', r.stdout)
        text = self.out.read_text()
        self.assertIn('extern uint8_t h_nvnFoo(void *, uint64_t, float, uint32_t);', text)   # the one-member struct as its 64-bit integer
        self.assertIn('st->x[0] = (uint64_t)(uint8_t)h_nvnFoo((void *)(uintptr_t)st->x[0], (uint64_t)st->x[1], ld_f(st->v[0]), (uint32_t)st->x[2]);', text)
        self.assertIn('extern uint64_t h_nvnBar(void *, int32_t);', text)
        self.assertIn('{"nvnBar", "h_nvnBar", hle_thunk_h_nvnBar, (const void *)h_nvnBar},', text)
        self.assertLess(text.index('{"nvnBar"'), text.index('{"nvnFoo"'), 'sorted by name')

    def test_nvn_name_not_identifier(self):
        self.fails('\nNVN("nvnFoo", void, int x) {}\n', 3, "NVN function's name is not one identifier")

    def test_nvn_boolean_parameter(self):   # a sub-32-bit parameter is refused here too (hle_nvn.c takes NVNboolean as uint32_t)
        self.fails('typedef uint8_t NVNboolean;\nNVN_NOPROTO(nvnSet, void, NVNboolean on) {}\n', 3, 'sub-32-bit parameter')

    def test_one_member_struct_only_64_bit(self):   # a two-member or 32-bit one stays a struct by value
        self.fails('typedef struct { uint64_t a; uint64_t b; } two;\nNVN(nvnF, void, two v) {}\n', 3, 'by value')

    def test_include_directory_and_cplusplus_block(self):   # a quoted header found through -I, its typedefs inside an extern "C" { of a __cplusplus block
        inc = self.tmp / 'inc'
        inc.mkdir()
        (inc / 'api.h').write_text('#pragma once\n#ifdef __cplusplus\nextern "C" {\n#endif\ntypedef uint64_t api_addr;\ntypedef enum { A_0, A_1 } api_mode;\n'
                                   '#ifdef __cplusplus\n}\n#endif\n')
        src = self.source('user.c', '#include "api.h"\nHLE_FORCE("f", "why", api_addr, h_f, api_mode m, api_addr a) { return a; }\n')
        self.assertNotEqual(self.run_gen(src).returncode, 0, 'without -I the typedefs are unknown')
        r = self.run_gen(src, incs=[inc])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('extern uint64_t h_f(uint32_t, uint64_t);', self.out.read_text())

    def test_real_host_sources(self):
        srcs = host_sources()
        self.assertGreater(len(srcs), 30)
        r = self.run_gen(*srcs, incs=[NVN_INCLUDE])
        self.assertEqual(r.returncode, 0, r.stderr)
        m = re.search(r'gen_hle_thunks: (\d+) entries', r.stdout)
        assert m, r.stdout
        n = int(m.group(1))
        print(f'\n{r.stdout.strip()}')
        self.assertGreater(n, 700)
        self.assertIn('0 unknown types', r.stdout)
        self.assertIn('0 duplicate C names', r.stdout)
        nvn = re.search(r'(\d+) NVN, (\d+) NVN_NOPROTO', r.stdout)
        assert nvn, r.stdout
        self.assertGreater(int(nvn.group(1)) + int(nvn.group(2)), 400, 'every NVN( entry of dm/gfx/nvn/hle_nvn.c')
        self.assertEqual(int(nvn.group(1)), len(re.findall(r'^NVN\(', (REPO / 'dm/gfx/nvn/hle_nvn.c').read_text(), re.M)))
        text = self.out.read_text()
        self.assertEqual(text.count('\nstatic void hle_thunk_'), n)
        self.assertIn(f'const size_t hle_thunks_count = {n};', text)
        names = re.findall(r'^    \{(".*"), "(\w+)", hle_thunk_\2, ', text, re.M)
        self.assertEqual(len(names), n)
        mangled = [ast.literal_eval(lit) for lit, _ in names]   # adjacent literals (a wrapper macro's # operands) join as in C
        self.assertEqual(mangled, sorted(mangled, key=lambda s: s.encode('latin-1')), 'sorted by mangled name, strcmp order')


if __name__ == '__main__':
    unittest.main()
