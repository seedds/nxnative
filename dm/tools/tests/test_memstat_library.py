"""Real dlopen coverage of memory diagnostics, without a GPU, game or window."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
HOST = REPO / 'dm/host'

# Interpose, rather than stub the memory-stat implementation: even an accidental
# constructor call to Metal is observable, but never creates an actual device.
RUNNER = r'''
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int exists(const char *name) { return access(getenv(name), F_OK) == 0; }
static void wait_started(void) {
    for (int i = 0; i < 300 && !(exists("DM_MEMSTAT") && exists("DM_THRCPU") && exists("TEST_METAL")); i++) usleep(10000);
    assert(exists("DM_MEMSTAT") && exists("DM_THRCPU") && exists("TEST_METAL"));
}
int main(int argc, char **argv) {
    assert(argc == 3);
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    if (!strcmp(argv[2], "standalone")) { wait_started(); return 0; }
    usleep(300000);
    assert(!exists("DM_MEMSTAT") && !exists("DM_THRCPU") && !exists("TEST_METAL"));
    if (!strcmp(argv[2], "view-only")) return 0;
    void (*start)(void) = dlsym(h, "dm_memstat_start"); assert(start);
    // Resolve the settings only at entry, not when dyld loaded the library.
    assert(setenv("DM_MEMSTAT", getenv("TEST_AFTER_MEMSTAT"), 1) == 0);
    assert(setenv("DM_THRCPU", getenv("TEST_AFTER_THRCPU"), 1) == 0);
    start(); start();
    wait_started();
    usleep(100000);
    FILE *f = fopen(getenv("TEST_METAL"), "r"); assert(f);
    char line[32]; assert(fgets(line, sizeof line, f));
    assert(!strcmp(line, "device\n") && !fgets(line, sizeof line, f)); fclose(f);
    return 0;
}
'''

METAL_TRAP = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static void *test_device(void) {
    FILE *f = fopen(getenv("TEST_METAL"), "a"); assert(f);
    fputs("device\n", f); fclose(f);
    return NULL;
}
extern void *MTLCreateSystemDefaultDevice(void);
__attribute__((used)) static struct { void *replacement; void *original; }
interpose __attribute__((section("__DATA,__interpose"))) =
    { (void *)test_device, (void *)MTLCreateSystemDefaultDevice };
'''

STUBS = r'''
#include "dm.h"
#include <time.h>
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
module_t *g_mod;
int g_nmod;
void svc_guest_range(uint8_t **base, size_t *size) { *base = NULL; *size = 0; }
int thr_name_of(pthread_t thread, char *name, size_t size) { return 0; }
'''


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'requires macOS clang')
class MemstatLibraryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='memstat-library-', dir=os.environ.get('NXNATIVE_TEST_ROOT'))
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.build = Path(cls.tmp.name)
        (cls.build / 'game_profile.h').write_text('')
        (cls.build / 'stubs.c').write_text(STUBS)
        (cls.build / 'runner.c').write_text(RUNNER)
        cls.compile(str(cls.build / 'runner.c'), '-o', str(cls.build / 'runner'))
        (cls.build / 'metal_trap.c').write_text(METAL_TRAP)
        cls.compile('-dynamiclib', str(cls.build / 'metal_trap.c'), '-framework', 'Metal',
                    '-o', str(cls.build / 'metal_trap.dylib'))
        for mode in ('library', 'standalone'):
            cls.compile('-dynamiclib', '-fobjc-arc', *(['-DDM_MAC_HOST_LIBRARY'] if mode == 'library' else []),
                        '-I' + str(cls.build), '-I' + str(HOST), str(HOST / 'memstat.m'),
                        str(cls.build / 'stubs.c'), '-framework', 'Foundation', '-framework', 'Metal',
                        '-o', str(cls.build / (mode + '.dylib')))

    @staticmethod
    def compile(*args):
        result = subprocess.run(['clang', *args], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_library(self, library, mode):
        with tempfile.TemporaryDirectory(dir=self.build) as td:
            root = Path(td)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('DM_', 'DYLD_'))}
            paths = {'DM_MEMSTAT': root / 'before-memory.tsv', 'DM_THRCPU': root / 'before-cpu.tsv',
                     'TEST_AFTER_MEMSTAT': root / 'after-memory.tsv', 'TEST_AFTER_THRCPU': root / 'after-cpu.tsv',
                     'TEST_METAL': root / 'metal-calls'}
            env.update({k: str(v) for k, v in paths.items()})
            env['DYLD_INSERT_LIBRARIES'] = str(self.build / 'metal_trap.dylib')
            result = subprocess.run([str(self.build / 'runner'), str(library), mode], env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if mode == 'entry':
                self.assertFalse(paths['DM_MEMSTAT'].exists())
                self.assertFalse(paths['DM_THRCPU'].exists())
                self.assertIn('phys_footprint', paths['TEST_AFTER_MEMSTAT'].read_text())
                self.assertIn('thread ID', paths['TEST_AFTER_THRCPU'].read_text())

    def test_library_has_no_dlopen_side_effects_then_starts_once_at_entry(self):
        self.run_library(self.build / 'library.dylib', 'entry')

    def test_standalone_retains_constructor_defaults(self):
        self.run_library(self.build / 'standalone.dylib', 'standalone')

    @unittest.skipUnless(os.environ.get('NXNATIVE_TEST_HOST_LIBRARY'), 'opt-in full real host library')
    def test_full_host_dlopen_has_no_memory_stat_side_effects(self):
        self.run_library(Path(os.environ['NXNATIVE_TEST_HOST_LIBRARY']).resolve(), 'view-only')

    def test_host_entry_starts_diagnostics_before_metal_tool(self):
        source = (HOST / 'main.c').read_text().split('int main(int argc, char **argv) {', 1)[1]
        self.assertLess(source.index('dm_memstat_start();'), source.index('if (mtl_pso_prebuild_tool())'))


if __name__ == '__main__':
    unittest.main()
