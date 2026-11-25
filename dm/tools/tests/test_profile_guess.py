"""Provisional memory layouts must leave room for the game's initial heap."""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import profile_guess as pg


class DepthSignal(unittest.TestCase):
    ZERO, GL = '[dm] [nvn] depth mode near=0 (as D3D / Metal)\n', '[dm] [nvn] depth mode near=-w (GL)\n'
    RESET = '[dm] [nvn-mtl] depth mode: near=-w (GL), vertex shaders use the main0 entry\n'   # the host's own reset at device init, also printed for a game that never calls the setter

    def profile(self, **keys):
        return type('Profile', (), {'get': staticmethod(lambda k, d='': keys.get(k, d))})

    def seen(self, log, **keys):
        return pg.depth_signal(log, self.profile(**keys)).get('depth_mode', (None,))[0]

    def test_the_game_sets_near_zero(self):
        self.assertEqual(self.seen(self.ZERO), 'zero')

    def test_no_call_is_unknown_not_gl(self):
        self.assertIsNone(self.seen('>>> nnMain\n'))
        self.assertIsNone(self.seen('>>> nnMain\n', depth_mode='zero'))   # a boot that stopped before the call says nothing against the key

    def test_the_host_reset_is_not_the_game_calling(self):
        self.assertIsNone(self.seen(self.RESET, depth_mode='zero'))
        self.assertEqual(self.seen(self.ZERO + self.RESET), 'zero')   # a second device's reset does not undo the game's call

    def test_a_profile_that_agrees_gets_no_change(self):
        self.assertIsNone(self.seen(self.ZERO, depth_mode='zero'))
        self.assertIsNone(self.seen(self.GL))

    def test_the_last_call_counts(self):
        self.assertEqual(self.seen(self.GL + self.ZERO), 'zero')
        self.assertEqual(self.seen(self.ZERO + self.GL, depth_mode='zero'), 'gl')

    def test_the_reason_is_one_comment_line(self):
        value, why = pg.depth_signal(self.ZERO, self.profile())['depth_mode']
        self.assertEqual(value, 'zero'); self.assertIn('near = 0', why); self.assertNotIn('\n', why)


class HeapSignals(unittest.TestCase):
    """The heap check's keys must give the heap region the whole request with the alias region the profile keeps (hle_svc.c mem_init: the heap region is mem_reserve less 256 MB, mem_stack and mem_alias)"""
    STACK = 0x10000000

    def profile(self, **keys):
        keys = {'mem_stack': f'{self.STACK:#x}', 'app_pool': '0x240000000', 'sys_resource': '0x3000000', **keys}
        return type('Profile', (), {'get': staticmethod(lambda k, d='': keys.get(k, d))})

    def layout(self, req, reserve, alias):
        """(heap region, alias region, keys set) after the heap check of a run that asked for req"""
        sig = pg.heap_signals(f'[dm] SetHeapSize OOM: req {req:#x} > cap 0x8000000\n', self.profile(mem_reserve=f'{reserve:#x}', mem_alias=f'{alias:#x}'))
        reserve, alias = int(sig.get('mem_reserve', (f'{reserve:#x}',))[0], 0), int(sig.get('mem_alias', (f'{alias:#x}',))[0], 0)
        return reserve - 0x10000000 - alias - self.STACK, alias, set(sig)

    def test_a_large_guessed_alias_region_is_kept_out_of_the_heap_region(self):
        """Ocarina of Time (Switch 2, SDK 22.2.8): the 4.375 GB guess and a 5 GB block, then a 6.20 GB request; the heap region was 0xb8000000 (issue #1361)"""
        heap, alias, keys = self.layout(0x18ce00000, 0x140000000, 0x118000000)
        self.assertEqual(keys, {'mem_reserve'})
        self.assertEqual(alias, 0x118000000)
        self.assertGreaterEqual(heap, 0x18ce00000)
        self.assertEqual(heap, 0x190000000)   # the request rounded up to 128 MB, nothing more

    def test_a_small_alias_region_is_still_given_1_gb_of_room(self):
        heap, alias, keys = self.layout(0x18ce00000, 0x140000000, 0x10000000)
        self.assertEqual(keys, {'mem_reserve'})
        self.assertEqual(heap + alias, 0x40000000 + 0x190000000)   # as before: the block sized for a 1 GB alias region

    def test_a_block_with_room_shrinks_the_alias_region(self):
        """Tears of the Kingdom asks for 0x86000000 of a 5 GB block: the alias region takes the rest, 0x98000000"""
        heap, alias, keys = self.layout(0x86000000, 0x140000000, 0xb8000000)
        self.assertEqual(keys, {'mem_alias'})
        self.assertEqual(alias, 0x98000000)
        self.assertGreaterEqual(heap, 0x86000000)


class HeapCheckAddressSpace(unittest.TestCase):
    """No heap and the alias region nearly full within seconds: StandardAllocator's region (pool − used) leaves Unity's own regions no room. The heap check shrinks that region with
    stdalloc_cap and keeps the hardware pool, which the game reads as its memory (Pokémon Friends picks its Low graphics at 2048 MB or less, issue #2830)"""
    FULL = '>>> nnMain\n[dm] [mem] alias region mapped up to +4.26 GB (alias region 4.38 GB)\n'

    def signals(self, log=FULL, **keys):
        keys = {'mem_reserve': '0x140000000', 'mem_alias': '0x118000000', 'mem_stack': '0x10000000', 'app_pool': '0xCD500000', 'sys_resource': '0x3000000', **keys}
        return pg.heap_signals(log, type('Profile', (), {'get': staticmethod(lambda k, d='': keys.get(k, d))}))

    def test_a_full_alias_region_caps_standardallocator_and_keeps_the_pool(self):   # AI: THE SOMNIUM FILES - nirvanA Initiative: +4.26 GB of 4.38 in 5 s with the hardware pool
        sig = self.signals()
        self.assertEqual(set(sig), {'stdalloc_cap'})   # no app_pool: the game reads the hardware's 3.2 GB, above Pokémon Friends' 2048 MB
        self.assertEqual(int(sig['stdalloc_cap'][0], 0), 0x40000000)
        self.assertNotIn('\n', sig['stdalloc_cap'][1])

    def test_room_left_sets_nothing(self):
        self.assertEqual(self.signals('>>> nnMain\n[dm] [mem] alias region mapped up to +3.50 GB (alias region 4.38 GB)\n'), {})

    def test_a_heap_or_no_system_resource_sets_nothing(self):
        self.assertEqual(self.signals(self.FULL + '[dm] [mem] heap 0.12 GB\n'), {})
        self.assertEqual(self.signals(sys_resource='0x0'), {})

    def test_a_profile_that_already_limits_standardallocator_is_kept(self):
        self.assertEqual(self.signals(stdalloc_cap='0x60000000'), {})   # Sonic Superstars' hand-set 1.5 GB
        self.assertEqual(self.signals(app_pool='0x40000000'), {})       # a profile from the heap check's earlier rule, which set a 1 GB pool


class GlslSources(unittest.TestCase):
    def test_fixed_function_detection_preserves_shader_and_api_evidence(self):
        egl = {'eglCreateWindowSurface'}
        for image, imports in ((b'glVertexPointer\0', egl), (b'', egl | {'glMatrixMode'}),
                               (('glEnableClientState' + '\0').encode('utf-16-le'), egl)):
            self.assertTrue(pg.gl_fixed_calls(image, imports))
        self.assertFalse(pg.gl_fixed_calls(b'glVertexPointerSomething\0', egl))
        shaderless = pg.shader_sources(egl, 0, [], 0, 0, gl_fixed_calls=True)
        self.assertEqual(shaderless['graphics'][0], 'gl')
        self.assertTrue(shaderless['gl_fixed'][0])
        self.assertFalse(shaderless['gl_capture'][0])
        for evidence in ('gl_names', 'gl_src', 'gl_files', 'nro_imports'):
            with self.subTest(evidence=evidence):
                with_shaders = pg.shader_sources(
                    egl, 0, [], 0, 0, gl_fixed_calls=True,
                    gl_names=int(evidence == 'gl_names'), gl_src=int(evidence == 'gl_src'), gl_files=int(evidence == 'gl_files'),
                    nro_imports=frozenset({'glShaderSource'}) if evidence == 'nro_imports' else frozenset(),
                )
                self.assertEqual(with_shaders['graphics'][0], 'gl')
                self.assertFalse(with_shaders['gl_fixed'][0])
        for imports, api in ((egl | {'vkCreateInstance'}, 'vulkan'), (egl | {'nvnBootstrapLoader'}, 'nvn')):
            other = pg.shader_sources(imports, 0, [], 0, 0, gl_fixed_calls=True)
            self.assertEqual(other['graphics'][0], api)
            self.assertFalse(other['gl_fixed'][0])

    def test_survey_detects_a_managed_gl_loader(self):
        managed = 'glShaderSource'.encode('utf-16-le')
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'main').write_bytes(b'NSO0')
            manifest = root / 'romfs_manifest.tsv'
            manifest.write_text('# Empty RomFS\n')
            imports = [('eglCreateWindowSurface', False, 1)]
            with mock.patch.object(pg, 'nso', return_value=(managed, '', [0, 0, 0], 0)), mock.patch.object(pg.nso_syms, 'dynsyms', return_value=imports):
                facts = pg.facts_of(root, manifest)
            self.assertEqual(facts['gl_names'], 1)
            self.assertEqual(pg.gl_name_count(b'\0glShaderSource\0' + managed, {'eglCreateWindowSurface'}), 2)
            self.assertEqual(pg.gl_name_count(managed, set()), 0)

    def test_language_preamble_is_not_a_shader(self):
        self.assertEqual(pg.glsl_sources(b'#version 450 core\n#define float4 vec4\n\0void main() {}\0'), 0)

    def test_gl_prefixes_are_not_sources(self):   # DOOM 3's main: the prefix its renderer puts before the GLSL it makes at run time, so the game gets a run-time capture
        image = b'\0#version 150\n#define PC\nfloat saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n\0#version 330\nvoid main() {}\0'
        self.assertEqual(pg.gl_sources(image), 1)
        self.assertEqual(pg.gl_sources(image[:image.index(b'#version 330') - 1]), 0)

    def test_complete_embedded_sources(self):
        image = b'#version 450\nvoid main() {}\0#version 450\nvoid main (void) {}\0'
        self.assertEqual(pg.glsl_sources(image), 2)

    def test_only_text_files_count_as_glsl_sources(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); (root / 'shaders').mkdir()
            (root / 'shaders/sprite.vert').write_bytes(b'#version 450\n\tvoid main() {}\r\n')
            (root / 'shaders/sprite.FRAG').write_bytes('// café\nvoid main() {}\n'.encode())
            (root / 'shaders/sprite.txt').write_bytes(b'void main() {}\n')   # not a shader file by its name
            (root / 'shaders/cipher.vert').write_bytes(hashlib.sha256(b'ciphertext').digest() * 128)   # Skyward Sword HD's Dummy.vert: encrypted, read only on a cache miss
            (root / 'shaders/empty.frag').write_bytes(b'')
            self.assertEqual(pg.romfs_glsl_files(root), 2)


    def test_glsl_files_are_gl_sources(self):   # Mega Man Legacy Collection's shaders/nx/*.glsl, which gl_shaders.py reads: no run-time capture
        self.assertIn('.glsl', pg.GL_FILES)
        s = pg.shader_sources({'eglCreateWindowSurface'}, 0, [], 0, 0, gl_names=1, gl_src=2, gl_files=6)
        self.assertEqual(s['graphics'][0], 'gl'); self.assertFalse(s['gl_capture'][0])

    def test_embedded_main_glsl_alone_still_enables_gl_capture(self):   # embedded movie/utility shaders in main must not suppress capture when RomFS has no standalone shaders
        s = pg.shader_sources({'eglCreateWindowSurface'}, 0, [], 0, 0, gl_names=1, gl_src=3, gl_files=0)
        self.assertEqual(s['graphics'][0], 'gl'); self.assertTrue(s['gl_capture'][0])


class Npdm(unittest.TestCase):
    def npdm(self, flags):
        d = bytearray(0x80); d[:4] = b'META'; d[0xc] = flags
        p = Path(self.tmp.name) / 'main.npdm'; p.write_bytes(d); return p

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)

    def test_64bit_flag(self):
        self.assertTrue(pg.aarch64(self.npdm(0x3f)))   # 64-bit, 39-bit address space

    def test_32bit_program(self):
        self.assertFalse(pg.aarch64(self.npdm(0x04)))   # Pikmin 3 Deluxe: 32-bit, no reserved region

    def test_no_npdm(self):
        self.assertTrue(pg.aarch64(Path(self.tmp.name) / 'missing.npdm'))


class UnrealMemory(unittest.TestCase):
    def test_initial_heap_fits_beside_the_engine_region(self):
        facts = {
            'mods': {'main': {'imports': {'nvnBootstrapLoader', pg.HEAP_API}, 'bss': 0, 'far': False}},
            'romfs': {'root': set(), 'exts': {'.pak': 1}},
            'sysres': 40 * pg.MiB,
            'main_markers': {'Unreal'},
        }
        keys = pg.guess(facts, hooks=set())
        reserve, alias, stack = (int(keys[k][0], 0) for k in ('mem_reserve', 'mem_alias', 'mem_stack'))
        # An iPhone reserves the profile's block as it is: 5 GB is the total proven on a phone (issue #2278: a 64 GB block stopped Dispatch at launch)
        self.assertLessEqual(reserve, 5 * 1024**3)
        self.assertGreaterEqual(alias, 4 * 1024**3)
        # mem_init puts the modules before the alias region, then stacks and the heap.
        heap = reserve - 0x10000000 - alias - stack
        self.assertGreaterEqual(heap, 128 * pg.MiB)


class ModulesLeftOut(unittest.TestCase):
    """main_modules and nro_modules: which modules a list leaves out are needed, read from each module's symbols (a symbol: (name, defined, bind))"""
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.dir = Path(td.name)
        self.syms = {}
        for cm in (mock.patch.object(pg, 'nso', lambda p: (self.syms[Path(p).name][0], '', [], 0)), mock.patch.object(pg.nso_syms, 'dynsyms', lambda img: next((s for i, s in self.syms.values() if i == img), []))):
            cm.start(); self.addCleanup(cm.stop)

    def module(self, name, imports=(), exports=(), comp=b''):
        self.syms[name] = (name.encode() + b'\0' + comp, [(s, False, 1) for s in imports] + [(s, True, 1) for s in exports])
        (self.dir / name).write_bytes(b'NSO0')

    def test_a_module_main_imports_from_is_added(self):   # Absolum 1.2.1: the movie module moved to subsdk0
        self.module('main', ['_ZN5movie9ExtractorC1Ev', 'nvnBootstrapLoader'])
        self.module('sdk', exports=['nvnBootstrapLoader'])
        self.module('subsdk0', exports=['_ZN5movie9ExtractorC1Ev'], comp=b'NintendoSDK_movie-20_5_29')
        self.assertEqual(pg.main_modules(self.dir, ['main', 'sdk'], hooks=set()), {'subsdk0': 'NintendoSDK_movie'})
        self.assertEqual(pg.main_modules(self.dir, ['main', 'subsdk0', 'sdk'], hooks=set()), {})   # listed already

    def test_what_is_supplied_adds_nothing(self):
        self.module('main', ['f', 'g'])
        self.module('sdk', exports=['f'])
        self.module('subsdk0', exports=['f', 'g'], comp=b'NintendoSDK_movie-20_5_29')
        self.assertEqual(pg.main_modules(self.dir, ['main', 'sdk'], hooks={'g'}), {})   # sdk has f, the host g (GLSLC and nn::pcm under Super Mario Galaxy 2)

    def test_gpu_drivers_and_rtld_stay_out(self):
        self.module('main', ['eglGetDisplay', 'rtld_f'])
        self.module('rtld', exports=['rtld_f'])
        self.module('sdk')
        self.module('subsdk1', exports=['eglGetDisplay'], comp=b'NintendoSDK_OpenGL-20_5_29')
        self.assertEqual(pg.main_modules(self.dir, ['main', 'sdk'], hooks=set()), {})

    def test_no_other_module_reads_nothing(self):   # the common case: a profile that lists every module, read no further than the first bytes
        (self.dir / 'main').write_bytes(b'NSO0'); (self.dir / 'main.npdm').write_bytes(b'META')
        self.assertEqual(pg.main_modules(self.dir, ['main'], hooks=set()), {})

    def test_nro_modules_still_adds_what_an_nro_imports(self):   # Portal's video_nx.nro and the movie module; a symbol another NRO defines needs nothing
        self.module('main'); self.module('sdk')
        self.module('subsdk2', exports=['m'], comp=b'NintendoSDK_movie-20_5_29')
        self.module('subsdk3', exports=['n'], comp=b'NintendoSDK_movie-20_5_29')
        for name, syms in (('a.nro', [('m', False, 1), ('n', False, 1)]), ('b.nro', [('n', True, 1)])):
            self.syms[name] = (name.encode(), syms); (self.dir / name).write_bytes(name.encode())
        nros = {n: self.dir / n for n in ('a.nro', 'b.nro')}
        self.assertEqual(pg.nro_modules(self.dir, nros, ['main', 'sdk'], hooks=set()), {'subsdk2': 'NintendoSDK_movie'})


class Switch2(unittest.TestCase):
    FACTS = {'mods': {'main': {'imports': {pg.HEAP_API}, 'bss': 0, 'far': False}}, 'romfs': {'root': set(), 'exts': {}}, 'sysres': 0}

    def test_pool(self):   # Donkey Kong Bananza's profile: a 9 GB pool, and a heap region that holds it
        keys = pg.guess(dict(self.FACTS, ounce=True), hooks=set())
        self.assertEqual((keys['app_pool'][0], keys['mem_reserve'][0]), ('0x240000000', '0x270000000'))
        self.assertEqual(pg.guess(self.FACTS, hooks=set())['app_pool'][0], '0xCD500000')

    def test_sdk_variant(self):   # nnSdk-Generic_22_2_0 (Fire Emblem Fortune's Weave)
        self.assertEqual(pg.SDK_VERSION.findall(b'NintendoSdk_nnSdk-Generic_22_2_0\0NintendoSDK_nnSdk-16_2_0\0'), [b'22_2_0', b'16_2_0'])



class AddressRegions(unittest.TestCase):
    """region_findings: the host's region table (DM_REGIONS) of a run with a Switch-sized alias region against the profile's alias region"""
    SYMS = {'_ZN2nn3mem17StandardAllocator10InitializeEPvmb': (0x1000, 0x100), '_ZN2nn2os21AllocateAddressRegionEPmm': (0x2000, 0x40)}
    HEAD = '# alias region 0x1000000000; offset\tsize\twho\tmapped\tfirst opened\tend\tvia\n'

    def test_demangle(self):
        self.assertEqual(pg.demangle('_ZN2nn3mem17StandardAllocator10InitializeEPvmb'), 'nn::mem::StandardAllocator::Initialize')
        self.assertEqual(pg.demangle('memcpy'), 'memcpy')

    def test_fits(self):
        t = self.HEAD + '0x4000000\t0x100000000\tmain+0x10\t0x60000000\t0\t0x100000000\t0x1010\n0x104000000\t0x1000000\tmain+0x20\t0\t-\t-\t0x2010\n'
        f = pg.region_findings(t, 0x118000000, self.SYMS)
        self.assertEqual(len(f), 2, f)   # the 16 MB region is under the 256 MB listed, but counts for the reach
        self.assertIn('through nn::mem::StandardAllocator::Initialize', f[0][1])
        self.assertEqual(f[1], (False, "in its first 15 s in nnMain the game's regions reach alias +4.08 GB, within the profile's 4.38 GB alias region (what it takes later in play is not measured here)"))
        self.assertIn('returning to main+0x10', f[0][1])

    def test_standard_allocator_regions_the_host_caps(self):
        # alone: the host's cap makes it fit (not red); with 1.5 GB of regions made after it (Absolum's .NET garbage collector): red, with the stdalloc_cap value that leaves them room
        alone = self.HEAD + '0x4000000\t0x300000000\tmain+0x10\t0x60000000\t0\t0x300000000\t0x1010\n'
        f = pg.region_findings(alone, 0x110000000, self.SYMS)
        self.assertFalse(any(red for red, _ in f), f)
        self.assertIn('on the iPhone layout the host gives it 3.94 GB', f[0][1])
        self.assertIn('only by StandardAllocator regions, which the host makes fit', f[-1][1])
        later = alone + '0x304000000\t0x60000000\tmain+0x40\t0x40000000\t0\t0x60000000\t0x2010\n'
        red, text = pg.region_findings(later, 0x118000000, self.SYMS)[-1]
        self.assertTrue(red)
        # the value leaves them room and the host's own 256 MB for what the run did not see: 4.38 - 0.06 - 1.5 - 0.25 GB (Absolum: 2.56 GB, between the 2.5 and 2.72 GB that both play)
        self.assertIn('the regions made after the one returning to main+0x10 take 1.50 GB, more than the 0.25 GB the host leaves them: stdalloc_cap 0xa4000000 (2.56 GB)', text)
        self.assertNotIn('main+0x40 takes', text)   # it fits once the region before it is capped
        self.assertEqual(pg.stdalloc_cap_needed(pg.region_rows(later, self.SYMS), 0x118000000), 0xa4000000)
        self.assertIsNone(pg.stdalloc_cap_needed(pg.region_rows(alone, self.SYMS), 0x110000000))   # the host's own cap does

    def test_the_profiles_stdalloc_cap(self):
        later = (self.HEAD + '0x4000000\t0xa00000000\tmain+0x10\t0x60000000\t0\t0xa00000000\t0x1010\n'
                 '0xa04000000\t0x60000000\tmain+0x40\t0x40000000\t0\t0x60000000\t0x2010\n')
        f = pg.region_findings(later, 0x118000000, self.SYMS, cap=0xa0000000)   # Absolum's 2.5 GB
        self.assertFalse(any(red for red, _ in f), f)
        self.assertIn("the profile's stdalloc_cap gives it 2.50 GB, which leaves the 1.50 GB of regions made after it room", f[0][1])
        red, text = pg.region_findings(later, 0x118000000, self.SYMS, cap=0xc0000000)[-1]
        self.assertTrue(red); self.assertIn("the profile's stdalloc_cap 0xc0000000 leaves the 1.50 GB of regions made after the one returning to main+0x10 too little room: stdalloc_cap 0xa4000000", text)

    def test_no_room_at_all(self):
        t = (self.HEAD + '0x4000000\t0x300000000\tmain+0x10\t0x60000000\t0\t0x300000000\t0x1010\n'
             '0x304000000\t0xe0000000\tmain+0x40\t0\t-\t-\t0x2010\n')
        red, text = pg.region_findings(t, 0x118000000, self.SYMS)[-1]
        self.assertTrue(red); self.assertIn('take 3.50 GB, which leaves it no room', text)
        self.assertIsNone(pg.stdalloc_cap_needed(pg.region_rows(t, self.SYMS), 0x118000000))

    def test_a_region_too_big_by_itself(self):
        # the game's own allocator through AllocateAddressRegion (Smash's engine): red, and it does not count as room the StandardAllocator region must leave
        t = (self.HEAD + '0x4000000\t0x300000000\tmain+0x10\t0x60000000\t0\t0x300000000\t0x1010\n'
             '0x304000000\t0xf80000000\tmain+0x30\t0x70000000\t0\t0xf7f210000\t0x2010\n')
        red, text = pg.region_findings(t, 0x118000000, self.SYMS)[-1]
        self.assertTrue(red and text.startswith('in its first 15 s in nnMain'), text)
        self.assertIn('the request returning to main+0x30 takes 62.00 GB through nn::os::AllocateAddressRegion: a smaller one works only if its allocator places nothing by fixed offsets', text)
        self.assertNotIn('stdalloc_cap', text)

    def test_together_too_big(self):
        t = self.HEAD + '0x4000000\t0xc0000000\tmain+0x30\t0\t-\t-\t0x2010\n0xc4000000\t0x80000000\tmain+0x50\t0\t-\t-\t0x2010\n'
        red, text = pg.region_findings(t, 0x118000000, self.SYMS)[-1]
        self.assertTrue(red); self.assertIn('no single region is too big, but together they need more room', text)

    def test_no_table(self):
        self.assertEqual(pg.region_findings('', 0x118000000), [])
        self.assertEqual(pg.region_verdict('', 0x20000000, mac_only=True), ([], False))

    def test_a_mac_only_build_of_regions_that_fit_the_mac(self):   # issue #2860: Minecraft Dungeons' row, past the 0.50 GB iPhone layout and inside the Mac's 64 GB
        row = f'{0x7000000:#x}\t{37 << 30:#x}\tmain+0x185a8e0\t{0x14800000:#x}\t0\t{0x920000000:#x}\t0x2010\n'
        t = self.HEAD + row
        ios, ios_mac = pg.region_verdict(t, 0x20000000, self.SYMS)
        self.assertTrue(ios[-1][0]); self.assertFalse(ios_mac)   # for an iPhone it stays red
        mac, mac_stamp = pg.region_verdict(t, 0x20000000, self.SYMS, mac_only=True)
        self.assertTrue(mac_stamp); self.assertFalse(any(red for red, _ in mac), mac)
        self.assertEqual(mac, pg.region_findings(t, 0x20000000, self.SYMS, mac_only=True))
        self.assertEqual(mac[:-1], ios[:-1])   # per-region detail (size, API, caller) stays intact
        self.assertIn("within this Mac-only build's 64.00 GB alias region", mac[-1][1])
        self.assertIn("past the profile's 0.50 GB iPhone layout", mac[-1][1])
        self.assertIn('a build for an iPhone checks again', mac[-1][1])
        self.assertNotIn('fixed offsets', mac[-1][1])   # direct Mac-only verdict, not a rewritten iPhone error
        sw2, sw2_stamp = pg.region_verdict(t.replace('0x1000000000', '0x2000000000'), 0x20000000, self.SYMS, mac_only=True)
        self.assertTrue(sw2_stamp); self.assertFalse(any(red for red, _ in sw2), sw2)
        self.assertIn("within this Mac-only build's 128.00 GB alias region", sw2[-1][1])   # the size the run had: a Switch 2 game's

    def test_a_mac_only_build_reports_early_stop_and_over_limit(self):
        row = f'{0x7000000:#x}\t{37 << 30:#x}\tmain+0x185a8e0\t{0x14800000:#x}\t0\t{0x920000000:#x}\t0x2010\n'
        t = self.HEAD + row
        early, mac_stamp = pg.region_verdict(t, 0x20000000, self.SYMS, mac_only=True, stopped='the host stopped before the end of the check window (exit 1)')
        self.assertTrue(mac_stamp); self.assertTrue(early[-1][0], early)
        self.assertIn('the host stopped before the end of the check window (exit 1)', early[-1][1])
        self.assertIn('a refused request leaves no row in the table', early[-1][1])
        self.assertNotIn('in its first 15 s in nnMain', early[-1][1])
        self.assertNotIn('fixed offsets', early[-1][1])

        over = self.HEAD + f'{0x7000000:#x}\t{64 << 30:#x}\tmain+0x10\t0\t-\t-\t0x2010\n'   # reaches past the 64 GB the run had
        mac_over, mac_stamp = pg.region_verdict(over, 0x20000000, self.SYMS, mac_only=True)
        self.assertTrue(mac_stamp); self.assertTrue(mac_over[-1][0], mac_over)
        self.assertIn("past this Mac-only build's 64.00 GB alias region", mac_over[-1][1])
        mac_over_early, _ = pg.region_verdict(over, 0x20000000, self.SYMS, mac_only=True, stopped='the host stopped before nnMain (exit 1)')
        self.assertTrue(mac_over_early[-1][0]); self.assertIn('the host stopped before nnMain (exit 1)', mac_over_early[-1][1])

        fits = self.HEAD + '0x4000000\t0x100000000\tmain+0x10\t0x60000000\t0\t0x100000000\t0x1010\n'
        clean, mac_stamp = pg.region_verdict(fits, 0x118000000, self.SYMS, mac_only=True)
        self.assertFalse(mac_stamp); self.assertFalse(any(red for red, _ in clean))   # fits the iPhone too: shared verdict and stamp

        unnamed = '# offset\tsize\twho\tmapped\tfirst opened\tend\tvia\n' + row
        unk, mac_stamp = pg.region_verdict(unnamed, 0x20000000, self.SYMS, mac_only=True)
        self.assertTrue(mac_stamp); self.assertTrue(unk[-1][0], unk)
        self.assertIn("did not name this Mac-only build's alias region", unk[-1][1])


class Ue3OodlePackages(unittest.TestCase):
    @staticmethod
    def pkg(blocks, block=0x40, wide=False):
        raw = sum(u for _, u in blocks)
        comp = sum(len(s) for s, _ in blocks)
        head = (pg.UE3_TAG + bytes(4) if wide else pg.UE3_TAG) + struct.pack('<QQQ' if wide else '<III', block, comp, raw)
        table = b''.join(struct.pack('<QQ' if wide else '<II', len(s), u) for s, u in blocks)
        return pg.UE3_TAG + struct.pack('<HH', 864, 0x80e3) + b'summary.' + head + table + b''.join(s for s, _ in blocks)

    def test_detects_narrow_and_wide_oodle_chunks(self):
        for wide in (False, True):
            with self.subTest(wide=wide):
                self.assertTrue(pg.ue3_oodle_package(self.pkg([(b'\x8c\x0a' + b'x' * 10, 0x40), (b'\x8c\x0a' + b'y' * 10, 0x20)], wide=wide)))
                z = zlib.compressobj(wbits=10)
                zblk = z.compress(b'TweakvarMap' * 8) + z.flush()
                self.assertFalse(pg.ue3_oodle_package(self.pkg([(zblk, 88)], block=512, wide=wide)))
                # A stored uncompressed block whose first byte happens to be 0x8c is not Oodle.
                self.assertFalse(pg.ue3_oodle_package(self.pkg([(b'\x8c' + b'raw' * 5, 16)], wide=wide)))

    def test_rejects_non_ue3_and_truncated_headers(self):
        self.assertFalse(pg.ue3_oodle_package(pg.UE3_TAG + b'\0'))
        self.assertFalse(pg.ue3_oodle_package(pg.UE3_TAG + struct.pack('<i', -7)))
        self.assertFalse(pg.ue3_oodle_package(pg.UE3_TAG + struct.pack('<HH', 864, 0) + pg.UE3_TAG + bytes(4) + struct.pack('<QQQ', 0, 0, 0)))
        self.assertFalse(pg.ue3_oodle_package(pg.UE3_TAG + struct.pack('<HH', 864, 0) + pg.UE3_TAG + bytes(4) + struct.pack('<QQQ', 0x40, 0, 0)))
        wide = self.pkg([(b'\x8c\x0a' + b'x' * 10, 0x40)], wide=True)
        self.assertFalse(pg.ue3_oodle_package(wide[:28]))


class ManifestProvenance(unittest.TestCase):
    """Multi-program NUP manifests tag sibling rows in an optional fourth column: combined inventory stays whole, while biggest (romfs_marker) stays in the packed program."""
    def test_sibling_rows_contribute_to_inventory_but_not_biggest(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            exefs = root / 'ExeFS'; exefs.mkdir()
            manifest = root / 'romfs_manifest.tsv'
            manifest.write_text(
                '# path\tbytes\tfirst 32 bytes (hex)\n'
                f'/dummy.bin\t5\t{b"12345".hex()}\n'
                f'/sound/jp.pck\t916455424\t{b"AKPK".hex()}\tprogram:0\n'
            )
            self.assertEqual(list(pg.romfs_files(manifest)), [('/dummy.bin', 5, b'12345'), ('/sound/jp.pck', 916455424, b'AKPK')])
            rom = pg.facts_of(exefs, manifest)['romfs']
            self.assertEqual((rom['n'], rom['bytes']), (2, 916455429))
            self.assertEqual(rom['mags']['Wwise pck'], 1)
            self.assertEqual(rom['dirs']['sound'], 1)
            self.assertEqual(rom['biggest'], (5, 'dummy.bin'))

            manifest.write_text(f'# path\tbytes\tfirst 32 bytes (hex)\n/sound/jp.pck\t916455424\t{b"AKPK".hex()}\tprogram:0\n')
            rom = pg.facts_of(exefs, manifest)['romfs']
            self.assertEqual((rom['n'], rom['bytes'], rom['biggest']), (1, 916455424, (0, '')))


class DecoderSources(unittest.TestCase):
    """decoder_sources / decoder_archives must detect every Oodle/LZ5 archive format identically across directory- and .img-backed roots, and ignore LZO1X Scimitar v27."""

    @staticmethod
    def _forge(ver):
        return b'scimitar\0' + struct.pack('<I', ver) + bytes(16)

    @staticmethod
    def _pkz(names):
        return struct.pack('<4sIIIIIII', b'pkzl', 0, 0, 0, 1, 0x20, len(names), 0) + bytes(0x20) + names

    @staticmethod
    def _gfpak(comp):
        return struct.pack('<8sIIIIQ', b'GFLXPACK', 0, 0, 1, 0, 0x20) + struct.pack('<HH20s', 0, comp, b'')

    @staticmethod
    def _ue_v4_pak():
        return b'\0' + struct.pack('<IIQQ', 0x5A6F12E1, 4, 0, 0) + bytes(20)

    def _roots(self, files):
        sys.path.insert(0, str(REPO / 'dm/tools'))
        import romfs_image
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name) / 'dir'
        root.mkdir()
        for rel, data in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        img = Path(tmp.name) / '1.img'
        romfs_image.pack(root, img)
        return (('dir', root), ('img', img))

    def test_forge_oodle_versions_vs_lzo_and_invalid_across_dir_and_image(self):
        for ver in (25, 28, 31):
            for kind, root in self._roots({'ACB/DataNX_extra.forge': self._forge(ver)}):
                with self.subTest(ver=ver, kind=kind):
                    got = pg.decoder_sources([root])
                    self.assertTrue(got['oodle_packs'][0], got)
                    self.assertIn('Anvil forge(s)', got['oodle_packs'][1])
                    self.assertFalse(got['ue_legacy'][0])
        for label, data in (('v27-lzo', self._forge(27)), ('unknown-ver', self._forge(99)), ('bad-magic', b'notforge' + bytes(16)), ('short', b'scimitar\0')):
            for kind, root in self._roots({'AC3/DataNX.forge': data}):
                with self.subTest(label=label, kind=kind):
                    got = pg.decoder_sources([root])
                    self.assertFalse(got['oodle_packs'][0], got)
                    self.assertFalse(got['ue_legacy'][0])

    def test_other_decoder_formats_across_dir_and_image(self):
        ue3 = Ue3OodlePackages.pkg([(b'\x8c\x0a' + b'x' * 10, 0x40)], wide=False)
        cases = (
            ('trinity', {'arc/data.trpfs': b'ONEPACK\0' + bytes(16)}, 'oodle_packs', 'Trinity pack'),
            ('pkz-oodle', {'data/a.pkz': self._pkz(b'OodleKraken\0')}, 'oodle_packs', 'PlatinumGames PKZ'),
            ('gfpak-oodle', {'data/a.gfpak': self._gfpak(3)}, 'oodle_packs', 'GFPAK'),
            ('ue3-oodle', {'cooked/shaders.upk': ue3}, 'oodle_packs', 'Unreal Engine 3'),
            ('frostbite', {'Data/layout.toc': bytes.fromhex('00d1ce01') + bytes(16)}, 'oodle_packs', 'Frostbite'),
            ('ue-v4-legacy', {'Paks/game.pak': self._ue_v4_pak()}, 'ue_legacy', 'Unreal pak'),
        )
        for label, files, key, snippet in cases:
            for kind, root in self._roots(files):
                with self.subTest(label=label, kind=kind):
                    got = pg.decoder_sources([root])
                    self.assertTrue(got[key][0], got)
                    self.assertIn(snippet, got[key][1])
        for label, files in (('pkz-zstd', {'data/a.pkz': self._pkz(b'ZStandard\0')}), ('gfpak-lz4', {'data/a.gfpak': self._gfpak(2)})):
            for kind, root in self._roots(files):
                with self.subTest(label=label, kind=kind):
                    got = pg.decoder_sources([root])
                    self.assertFalse(got['oodle_packs'][0], got)
                    self.assertFalse(got['ue_legacy'][0])

    def test_multiple_roots_combine_directory_and_image(self):
        (_, empty_dir), (_, empty_img) = self._roots({'placeholder.txt': b'ok'})
        (_, forge_dir), (_, forge_img) = self._roots({'ACB/a.forge': self._forge(25)})
        counts = pg.decoder_archives([empty_dir, empty_img, forge_dir, forge_img])
        self.assertEqual(counts['anvil_forges'], 2)


if __name__ == '__main__':
    unittest.main()
