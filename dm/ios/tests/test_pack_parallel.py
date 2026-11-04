"""Large ZIP entries remain a single standard Deflate stream when blocks run in parallel."""
import concurrent.futures
import contextlib
import importlib.util
import io
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock
import zipfile

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pack_parallel', REPO / 'dm/ios/tools/pack_ipa.py')
pack = importlib.util.module_from_spec(spec); spec.loader.exec_module(pack)


class ParallelPack(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name); self.source = self.root / 'romfs.img'; self.out = self.root / 'a.ipa'

    def run_pack(self, data, *opts):
        self.source.write_bytes(data)
        args = ['pack_ipa', str(self.out), *opts, f'Payload/Game.app/romfs.img={self.source}']
        with mock.patch.object(sys, 'argv', args), mock.patch.object(pack, 'BIG', 1), mock.patch.object(pack, 'BLOCK', 32768, create=True), \
                contextlib.redirect_stdout(io.StringIO()): pack.main()
        with zipfile.ZipFile(self.out) as z:
            self.assertIsNone(z.testzip()); self.assertEqual(z.read('Payload/Game.app/romfs.img'), data)
            return z.getinfo('Payload/Game.app/romfs.img')

    def test_multiple_compressors_are_active(self):
        # Synchronize real compressors: a serial implementation times out before the second arrives.
        real = pack.zlib.compressobj; barrier = threading.Barrier(2); threads = set(); lock = threading.Lock()
        def start(*args, **kwargs):
            if threading.current_thread() is not threading.main_thread():
                with lock:
                    first = threading.get_ident() not in threads
                    threads.add(threading.get_ident())
                if first: barrier.wait(timeout=5)   # both workers must rendezvous, not the final compressor on the main thread
            return real(*args, **kwargs)
        with mock.patch.object(pack.zlib, 'compressobj', start), mock.patch.object(pack.os, 'cpu_count', return_value=2):
            self.run_pack(b'compress me ' * 20000)
        self.assertGreaterEqual(len(threads), 2)

    def test_boundaries_and_standard_readers(self):
        block = random.Random(12).randbytes(16000) * 2   # the next block starts with a match in previous history
        for n in (0, 1, 32767, 32768, 32769, 65536, 110007):
            with self.subTest(n=n):
                data = (block * 4)[:n]
                self.run_pack(data, '--no-reuse')
                r = subprocess.run(['/usr/bin/unzip', '-t', str(self.out)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_incompressible_falls_back_to_stored(self):
        info = self.run_pack(random.Random(4).randbytes(100003))
        self.assertEqual(info.compress_type, zipfile.ZIP_STORED)

    def test_stored_input_does_not_start_compressors(self):
        with mock.patch.object(pack.zlib, 'compressobj', side_effect=AssertionError('stored input compressed')):
            self.assertEqual(self.run_pack(b'store ' * 20000, '--store', '.img').compress_type, zipfile.ZIP_STORED)

    def test_out_of_order_completion_keeps_input_order(self):
        real = concurrent.futures.ThreadPoolExecutor.submit
        def submit(pool, fn, *args, **kwargs):
            def delayed():
                import time
                if args and isinstance(args[0], bytes) and args[0].startswith(b'first'): time.sleep(0.02)
                return fn(*args, **kwargs)
            return real(pool, delayed)
        data = b'first' + b'a' * 32763 + b'second' + b'b' * 70000
        with mock.patch.object(pack.cf.ThreadPoolExecutor, 'submit', submit): self.run_pack(data)

    def test_worker_failure_preserves_old_archive(self):
        self.run_pack(b'old archive')
        before = self.out.read_bytes(); idx = Path(str(self.out) + '.idx').read_bytes()
        with mock.patch.object(pack.zlib, 'compressobj', side_effect=RuntimeError('compile block failed')):
            with self.assertRaisesRegex(RuntimeError, 'compile block failed'): self.run_pack(b'new ' * 30000)
        self.assertEqual(self.out.read_bytes(), before); self.assertEqual(Path(str(self.out) + '.idx').read_bytes(), idx)
        self.assertFalse(Path(str(self.out) + '.partial').exists())


if __name__ == '__main__': unittest.main()
