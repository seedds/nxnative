"""Shader scheduling: real worker callbacks, dependency publication and one CPU budget."""
import importlib.util
from pathlib import Path
import threading
import time
import unittest

PATH = Path(__file__).resolve().parents[1] / 'tools/dmsh_schedule.py'


class Schedule(unittest.TestCase):
    def setUp(self):
        self.assertTrue(PATH.exists(), 'the shader pipeline scheduler is not implemented')
        spec = importlib.util.spec_from_file_location('dmsh_schedule', PATH)
        self.scheduler = importlib.util.module_from_spec(spec); spec.loader.exec_module(self.scheduler)

    def test_translation_and_dependency_ready_finishes_overlap(self):
        pixel_finished = threading.Event(); vertex_finished = threading.Event()
        def translate(batch):
            if batch == ['slow']:
                self.assertTrue(pixel_finished.wait(5), 'compilation waited for all translations')
                self.assertTrue(vertex_finished.wait(5), 'vertex waited for unrelated shaders')
        def finish(key):
            if key == 'pixel': pixel_finished.set()
            if key == 'vertex':
                self.assertTrue(pixel_finished.is_set())
                vertex_finished.set()
            return key
        keys = ['slow', 'pixel', 'vertex']
        self.assertEqual(self.scheduler.run(keys, keys, {'vertex': {'pixel'}}, translate, finish, 2, batch_size=1), keys)

    def test_cached_and_failed_dependencies_complete_before_consumers(self):
        published = set()
        def translate(batch): pass   # a failed translation still reaches finish(), which reports its failure
        def finish(key):
            if key == 'vertex': self.assertEqual(published, {'cached', 'failed'})
            else: published.add(key)
            return key, key != 'failed'
        keys = ['vertex', 'cached', 'failed']
        self.assertEqual(self.scheduler.run(keys, ['vertex', 'failed'], {'vertex': {'cached', 'failed', 'absent'}}, translate, finish, 3),
                         [('vertex', True), ('cached', True), ('failed', False)])

    def test_shared_worker_bound_and_single_worker_progress(self):
        for workers in (1, 3):
            lock = threading.Lock(); active = 0; peak = 0
            def work(value):
                nonlocal active, peak
                with lock: active += 1; peak = max(peak, active)
                time.sleep(0.002)
                with lock: active -= 1
                return value
            keys = list(range(40))
            self.assertEqual(self.scheduler.run(keys, keys, {}, work, work, workers, batch_size=2), keys)
            self.assertLessEqual(peak, workers)
            self.assertEqual(peak, workers)

    def test_failure_propagates_and_cycle_stops(self):
        def fail(_): raise OSError('translator could not start')
        with self.assertRaisesRegex(OSError, 'translator could not start'):
            self.scheduler.run(['a'], ['a'], {}, fail, str, 1)
        with self.assertRaisesRegex(ValueError, 'cycle'):
            self.scheduler.run(['a', 'b'], [], {'a': {'b'}, 'b': {'a'}}, fail, str, 2)

    def test_blocked_dependencies_are_not_translated_into_a_waiting_disk_backlog(self):
        fast_vertex = threading.Event(); slow_pixel = threading.Event()
        def translate(batch):
            for key in batch:
                if key.startswith('blocked'):
                    self.assertTrue(slow_pixel.is_set(), 'blocked vertex translated before its pixel finished')
        def finish(key):
            if key == 'slow':
                self.assertTrue(fast_vertex.wait(2), 'ready vertex waited for an unrelated pixel')
                slow_pixel.set()
            if key == 'ready': fast_vertex.set()
            return key
        blocked = [f'blocked{i}' for i in range(100)]
        keys = ['slow', 'fast', *blocked, 'ready']
        pairs = {k: {'slow'} for k in blocked} | {'ready': {'fast'}}
        try:
            self.assertEqual(self.scheduler.run(keys, keys, pairs, translate, finish, 2, batch_size=1), keys)
        finally: fast_vertex.set()

    def test_empty_build(self):
        self.assertEqual(self.scheduler.run([], [], {}, str, str, 1), [])


if __name__ == '__main__': unittest.main()
