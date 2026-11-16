"""UnityFS bundles inside CRI CPK archives (Ace Angler: Fishing Spirits keeps its Addressables bundles in 22 CPKs) go to the Unity reader, entry by entry, as loose bundles do;
other entries are still scanned as blobs, and a range of entries that needs a borrowed type tree is deferred as that range. Synthetic bytes only."""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from dm.tools.tests.test_cpk_offsets import cpk_file
from dm.tools.tests.test_split_gzip import REPO, ss

sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import unityfs  # noqa: E402

BUNDLE = b'UnityFS\0' + bytes(range(64))
FILES = {'a.bundle': BUNDLE, 'b.dat': b'not a bundle ' * 40, 'c.bundle': BUNDLE + b'second'}


class CpkUnity(unittest.TestCase):
    def scan(self, unity):
        """scan_file over the one job cpk_jobs makes of the archive, with the Unity reader and the blob scan replaced → (result, labels given to each, the job)"""
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()):
            p = Path(t) / 'res.cpk'; p.write_bytes(cpk_file(FILES, toc_at_end=False))
            jobs, taken = ss.cpk_jobs([(str(p), 'aa/res.cpk')])
            self.assertEqual(taken, {str(p)})
            job = (jobs[0][0], jobs[0][1], [e for j in jobs for e in j[2]])
            blobs = []
            with mock.patch.object(ss, 'unity_shaders', side_effect=unity) as u, \
                 mock.patch.object(ss, 'scan_blob', side_effect=lambda d, label, out, *a: blobs.append(label)):
                result = ss.scan_file(job)
            return result, [c.args[1] for c in u.call_args_list], blobs, job

    def test_bundles_go_to_the_unity_reader(self):
        (rel, keys, pairs, extra), bundles, blobs, _ = self.scan(lambda d, label, out, trees: {('v' + label[-8], 'p' + label[-8])})
        self.assertEqual(bundles, ['/aa/res.cpk/d/a.bundle', '/aa/res.cpk/d/c.bundle'])
        self.assertEqual(blobs, ['/aa/res.cpk/d/b.dat'])
        self.assertEqual(rel, 'aa/res.cpk'); self.assertEqual(extra, {})
        self.assertEqual(pairs, {('va', 'pa', 'prog'), ('vc', 'pc', 'prog')})   # legacy pass pairs, tagged as a loose bundle's are

    def test_a_range_without_a_type_tree_is_deferred(self):
        def no_tree(d, label, out, trees): raise unityfs.NoTree(label)
        (rel, keys, pairs, extra), _, _, job = self.scan(no_tree)
        self.assertEqual((rel, keys, pairs, extra), ('aa/res.cpk', {}, set(), 'defer'))
        self.assertEqual(len(job), 3)   # the second pass scans the job again, entries and all, not the archive as one loose file


if __name__ == '__main__':
    unittest.main()
