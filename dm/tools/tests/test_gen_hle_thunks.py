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
    """The nxhost sources of dm/host/CMakeLists.txt: hle.c loader.c thread.c front_sdl.c and the hle_*.c glob without hle_bigstack.c"""
    cm = (HOST / 'CMakeLists.txt').read_text()
    m = re.search(r'set\(NXHOST_SRC ([^)]*)\)', cm)
    assert m, 'NXHOST_SRC not found in dm/host/CMakeLists.txt'
    assert 'hle_bigstack' in cm
    return [HOST / n for n in m.group(1).split()] + sorted(p for p in HOST.glob('hle_*.c') if p.name != 'hle_bigstack.c')


class Generator(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.out = self.tmp / 'gen/hle_thunks.c'

    def run_gen(self, *sources):
        return subprocess.run([sys.executable, str(GEN), '-o', str(self.out), *map(str, sources)], capture_output=True, text=True, check=False)

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

    def test_real_host_sources(self):
        srcs = host_sources()
        self.assertGreater(len(srcs), 30)
        r = self.run_gen(*srcs)
        self.assertEqual(r.returncode, 0, r.stderr)
        m = re.search(r'gen_hle_thunks: (\d+) entries', r.stdout)
        assert m, r.stdout
        n = int(m.group(1))
        print(f'\n{r.stdout.strip()}')
        self.assertGreater(n, 700)
        self.assertIn('0 unknown types', r.stdout)
        self.assertIn('0 duplicate C names', r.stdout)
        text = self.out.read_text()
        self.assertEqual(text.count('\nstatic void hle_thunk_'), n)
        self.assertIn(f'const size_t hle_thunks_count = {n};', text)
        names = re.findall(r'^    \{(".*"), "(\w+)", hle_thunk_\2, ', text, re.M)
        self.assertEqual(len(names), n)
        mangled = [ast.literal_eval(lit) for lit, _ in names]   # adjacent literals (a wrapper macro's # operands) join as in C
        self.assertEqual(mangled, sorted(mangled, key=lambda s: s.encode('latin-1')), 'sorted by mangled name, strcmp order')


if __name__ == '__main__':
    unittest.main()
