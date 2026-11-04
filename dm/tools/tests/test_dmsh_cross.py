"""Pinned C API / CLI parity and independent compiler contexts, without game files."""
import concurrent.futures as cf
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[2] / 'gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_cross  # noqa: E402

FLAGS = ['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native', '--msl-pad-fragment-output']


def compute():
    words = [0x07230203, 0x00010000, 0, 5, 0]
    for opcode, operands in [(17, [1]), (14, [0, 1]), (15, [5, 3, 0x6e69616d, 0]), (16, [3, 17, 1, 1, 1]), (19, [1]), (33, [2, 1]), (54, [1, 3, 0, 2]), (248, [4]), (253, []), (56, [])]:
        words.extend([((1 + len(operands)) << 16) | opcode, *operands])
    return struct.pack('<%dI' % len(words), *words)


class Fallback(unittest.TestCase):
    def test_unknown_flags_and_missing_library_keep_cli_available(self):
        self.assertIsNone(dmsh_cross.options([*FLAGS, '--future-option']))
        self.assertIsNone(dmsh_cross.options(['--msl-version']))
        self.assertIsNone(dmsh_cross.options(['--msl-version', 'invalid']))
        compiler = dmsh_cross.Compiler('/missing/library')
        self.assertIsNone(compiler.compile(compute(), FLAGS))
        self.assertEqual(compiler.record, 'cli')
        with mock.patch.dict(os.environ, {'DMSH_CROSS': 'cli'}): self.assertIsNone(dmsh_cross.Compiler().library)


@unittest.skipUnless(Path(dmsh_cross.LIBRARY).is_file(), 'optional pinned SPIRV-Cross C API not built')
class Native(unittest.TestCase):
    def setUp(self):
        with mock.patch.dict(os.environ, {'DMSH_CROSS': 'auto'}): self.compiler = dmsh_cross.Compiler()

    def test_exact_cli_output_and_concurrent_contexts(self):
        binary = compute()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'shader.spv'; path.write_bytes(binary)
            cli = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), *FLAGS, str(path)], capture_output=True, text=True, check=True).stdout
        with cf.ThreadPoolExecutor(8) as pool:
            results = list(pool.map(lambda _: self.compiler.compile(binary, FLAGS), range(32)))
        self.assertEqual(results, [cli] * 32)

    def test_malformed_module_and_compiler_error_release_context(self):
        for binary in (b'invalid', compute()[:20]):
            with self.assertRaises(dmsh_cross.Error): self.compiler.compile(binary, FLAGS)
        self.assertIn('kernel', self.compiler.compile(compute(), FLAGS) or '')

    def test_shipped_library_contains_both_mac_architectures(self):
        result = subprocess.run(['lipo', '-archs', dmsh_cross.LIBRARY], capture_output=True, text=True, check=True)
        self.assertEqual(sorted(result.stdout.split()), ['arm64', 'x86_64'])

    @unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'requires the pinned .NET translator')
    def test_image_atomic_versions_match_cli(self):
        from dm.tools.tests import test_image_atomic_bounds as fixture
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory); jobs = []
            for dim, image in ((1, 'buf'), (3, '2d'), (4, '2darray'), (5, '3d')):
                key = 'atomic_' + str(dim); code = work / (key + '.bin')
                code.write_bytes(fixture.synthetic_shader(0, 0, dim=dim, discard=False, consume_result=True))
                jobs.append(dict(key=key, stage='cs', code=str(code), img={'0': dict(type=image, fmt='R32Uint')}, local=[1, 1, 1], debug=True))
            job_file = work / 'jobs.json'; job_file.write_text(json.dumps(jobs))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], str(job_file), str(work)], capture_output=True, check=True)
            for job in jobs:
                spv = work / (job['key'] + '.spv'); flags = fixture.atomic.cross_args(FLAGS, str(spv))
                self.assertEqual(flags[2], '30100')
                cli = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), *flags, str(spv)], capture_output=True, text=True, check=True).stdout
                self.assertEqual(self.compiler.compile(spv.read_bytes(), flags), cli)


if __name__ == '__main__': unittest.main()
