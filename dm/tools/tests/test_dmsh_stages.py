"""Intermediate shader caching must preserve outputs and reject stale or incomplete stages."""
import ast
import concurrent.futures as cf
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
from typing import Any
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[2] / 'gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_stages  # noqa: E402
import dmsh_cross  # noqa: E402


class StageFixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.cache = dmsh_stages.Cache(self.tmp.name)


class Stages(StageFixture):
    def test_verified_compressed_roundtrip_and_corruption(self):
        key = dmsh_stages.digest('compiler', {'debug': True}, b'code')
        parts = {'spv': b'\x03\x02\x23\x07' * 10000, 'resources': b'{"key":"a"}'}
        self.cache.put('xlate', key, parts)
        path = Path(self.cache.path('xlate', key))
        self.assertLess(path.stat().st_size, sum(map(len, parts.values())) // 4)
        self.assertEqual(self.cache.get('xlate', key), parts)
        valid = path.read_bytes()
        for bad in (valid[:20], valid[:-1], valid[:20] + bytes([valid[20] ^ 1]) + valid[21:]):
            path.write_bytes(bad)
            self.assertIsNone(self.cache.get('xlate', key))

    def test_concurrent_publication_never_exposes_partial_entries(self):
        key = dmsh_stages.digest('compiler', {})
        parts = {'source': b'hello shader' * 1000}
        def publish(_):
            self.cache.put('msl', key, parts)
            self.assertEqual(self.cache.get('msl', key), parts)
        with cf.ThreadPoolExecutor(8) as pool: list(pool.map(publish, range(16)))
        self.assertFalse(list(self.root.rglob('*.tmp-*')))

    def test_compilers_options_and_binary_participate_in_key(self):
        original = dmsh_stages.digest('compiler', {'attr': {'1': 'float'}}, b'code')
        for identity, inputs, binary in [('new compiler', {'attr': {'1': 'float'}}, b'code'), ('compiler', {'attr': {'1': 'uint'}}, b'code'), ('compiler', {'attr': {'1': 'float'}}, b'other code')]:
            self.assertNotEqual(dmsh_stages.digest(identity, inputs, binary), original)
        self.assertEqual(dmsh_stages.digest('compiler', {'b': 2, 'a': 1}), dmsh_stages.digest('compiler', {'a': 1, 'b': 2}))

    def test_invalid_paths_are_rejected(self):
        with self.assertRaises(ValueError): self.cache.put('../shader', 'a' * 64, {})
        with self.assertRaises(ValueError): self.cache.put('msl', '../shader', {})


class BuilderStages(StageFixture):
    def setUp(self):
        super().setUp()
        tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
        names = {'xlate_cache_key', 'xlate_files', 'xlate_restore', 'xlate_store', 'cross_msl'}
        nodes: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
        self.ns: dict[str, Any] = dict(os=os, subprocess=subprocess, dmsh_stages=dmsh_stages, STAGES=self.cache,
                       XLATE_STAGE_ID='translator', MSL_STAGE_ID='cross', MSL_STAGE_RULE='atomic rule',
                       XC=['--msl-version', '20300'], SPIRV_CROSS='spirv-cross', CROSS=types.SimpleNamespace(compile=lambda binary, args: None), dmsh_cross=dmsh_cross,
                       dmsh_atomic=types.SimpleNamespace(cross_args=lambda args, spv: args))
        exec(compile(ast.Module(body=nodes, type_ignores=[]), '<stages>', 'exec'), self.ns)
        self.code = self.root / 'shader.bin'; self.code.write_bytes(b'code')
        self.job = {'key': 'abc', 'stage': 'vs', 'attr': {}, 'code': str(self.code), 'c1': '00'}

    def test_translation_paths_do_not_change_key_but_facts_do(self):
        key = self.ns['xlate_cache_key'](self.job)
        other = self.root / 'other.bin'; other.write_bytes(self.code.read_bytes())
        self.assertEqual(self.ns['xlate_cache_key']({**self.job, 'code': str(other)}), key)
        for prop, value in [('attr', {'0': 'uint'}), ('c1', '01'), ('debug', False), ('geometry_topology', 3), ('stage', 'ps')]:
            with self.subTest(input=prop): self.assertNotEqual(self.ns['xlate_cache_key']({**self.job, prop: value}), key)
        self.ns['XLATE_STAGE_ID'] = 'new translator'
        self.assertNotEqual(self.ns['xlate_cache_key'](self.job), key)

    def test_geometry_translation_restores_reader_and_unmodified_spirv(self):
        job = {**self.job, 'geometry_topology': 3}
        self.ns['xlate_store'](job, self.tmp.name)
        self.assertEqual(self.cache.stats['xlate', 'stored'], 0)  # an incomplete/failed stage is not cached
        paths = self.ns['xlate_files'](job, self.tmp.name)
        originals = {'spv': b'original spirv', 'resources': json.dumps({'key': 'abc'}).encode(), 'reader': b'reader spirv'}
        for name, path in paths.items(): Path(path).write_bytes(originals[name])
        self.ns['xlate_store'](job, self.tmp.name)
        for path in paths.values(): Path(path).write_bytes(b'consumer modified this file')
        self.assertTrue(self.ns['xlate_restore'](job, self.tmp.name))
        for name, path in paths.items(): self.assertEqual(Path(path).read_bytes(), originals[name])
        self.ns['STAGES'] = None
        self.assertFalse(self.ns['xlate_restore'](job, self.tmp.name))

    def test_cross_cache_avoids_process_and_instruction_walk_on_hit(self):
        spv = self.root / 'shader.spv'; spv.write_bytes(b'original spirv')
        output = self.root / 'original.metal'; restored = self.root / 'restored.metal'
        def compile(args, **kwargs):
            if '--output' in args: Path(args[-1]).write_text('metal source')
            return subprocess.CompletedProcess(args, 0, '' if '--output' in args else 'metal source', '')
        with mock.patch.object(subprocess, 'run', side_effect=compile) as process:
            self.assertEqual(self.ns['cross_msl'](str(spv), str(output)).returncode, 0)
            with mock.patch.object(self.ns['dmsh_atomic'], 'cross_args', side_effect=AssertionError('cache hit scanned instructions')):
                self.ns['cross_msl'](str(spv), str(restored))
                self.assertEqual(self.ns['cross_msl'](str(spv)).stdout, 'metal source')
            self.assertEqual(output.read_bytes(), restored.read_bytes())
            self.assertEqual(process.call_count, 1)
            for name in ('MSL_STAGE_ID', 'MSL_STAGE_RULE'):
                self.ns[name] += 'changed'; self.ns['cross_msl'](str(spv))
            self.ns['XC'] = ['--msl-version', '30100']; self.ns['cross_msl'](str(spv))
            spv.write_bytes(b'changed spirv'); self.ns['cross_msl'](str(spv))
            self.assertEqual(process.call_count, 5)

    def test_failed_cross_is_not_cached(self):
        spv = self.root / 'shader.spv'; spv.write_bytes(b'spirv')
        with mock.patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'failure')) as process:
            for _ in range(2): self.assertEqual(self.ns['cross_msl'](str(spv)).stderr, 'failure')
            self.assertEqual(process.call_count, 2)
        self.assertEqual(self.cache.stats['msl', 'stored'], 0)

    def test_native_error_preserves_cli_failure_diagnostic(self):
        spv = self.root / 'shader.spv'; spv.write_bytes(b'spirv')
        self.ns['CROSS'] = types.SimpleNamespace(compile=mock.Mock(side_effect=dmsh_cross.Error('native error')))
        with mock.patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'CLI diagnostic')) as process:
            result = self.ns['cross_msl'](str(spv))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'CLI diagnostic')
        self.assertEqual(process.call_count, 1)
        self.assertEqual(self.cache.stats['msl', 'stored'], 0)


if __name__ == '__main__': unittest.main()
