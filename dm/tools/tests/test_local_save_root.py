"""Exercise the launcher's real folder copy and APFS transaction preflight."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HOST = Path(__file__).resolve().parents[2] / 'host'
TITLE = '0123456789ABCDEF'
SECOND = '0123456789abcdef0123456789abcdef'


@unittest.skipUnless(sys.platform == 'darwin', 'Apple save storage')
class SaveRoot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        lib = Path(tmp.name) / 'root.dylib'
        subprocess.run(['clang', '-Wall', '-Werror', '-fobjc-arc', '-dynamiclib',
                        HOST / 'local_save_root.m', '-framework', 'Foundation', '-o', lib], check=True)
        cls.prepare = ctypes.CDLL(str(lib)).dm_save_prepare_root
        cls.prepare.argtypes = [ctypes.c_char_p] * 4 + [ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        cls.prepare.restype = ctypes.c_int

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name).resolve()
        self.root = self.folder / 'Saved Games'
        self.root.mkdir()
        self.source = self.folder / 'original' / 'save'
        self.source.parent.mkdir()

    def use(self, root=None, title=TITLE, source=None, *, ok=True):
        output, error = ctypes.create_string_buffer(1024), ctypes.create_string_buffer(512)
        result = self.prepare(os.fsencode(root or self.root), title.encode(), os.fsencode(source or self.source),
                              output, len(output), error, len(error))
        self.assertEqual(bool(result), ok, error.value)
        if not ok:
            self.assertTrue(error.value)
            self.assertEqual(output.value, b'')
            return None
        return Path(os.fsdecode(output.value))

    def test_copies_only_committed_stores_and_preserves_guest_names(self):
        for name in ['save', 'save_device', 'save.user-' + SECOND, 'save.work', 'save.user-' + SECOND + '.new', 'save-backups']:
            store = self.source.parent / name
            store.mkdir()
            (store / 'slot').write_text(name)
        subprocess.run(['xattr', '-w', 'org.d2rmobile.guest-name', 'CASE', self.source / 'slot'], check=True)
        chosen = self.use(title=TITLE.lower())
        assert chosen is not None
        self.assertEqual(chosen, self.root / TITLE / 'save')
        root = chosen.parent
        self.assertEqual(sorted(p.name for p in root.iterdir()), sorted(['save', 'save_device', 'save.user-' + SECOND]))
        for name in ['save', 'save_device', 'save.user-' + SECOND]:
            self.assertEqual((root / name / 'slot').read_text(), name)
            self.assertEqual((self.source.parent / name / 'slot').read_text(), name)
        self.assertEqual(subprocess.check_output(['xattr', '-p', 'org.d2rmobile.guest-name', chosen / 'slot']).strip(), b'CASE')
        (chosen / 'slot').write_text('new progress')
        reopened = self.use()
        assert reopened is not None
        self.assertEqual((reopened / 'slot').read_text(), 'new progress')
        self.assertEqual((self.source / 'slot').read_text(), 'save')

    def test_two_titles_and_empty_install(self):
        (self.root / TITLE).mkdir()  # even a previously created empty title directory may be used
        first = self.use()
        assert first is not None
        (first / 'slot').write_text('one')
        second = self.use(title='f' * 16)
        assert second is not None
        self.assertFalse((second / 'slot').exists())
        self.assertEqual((first / 'slot').read_text(), 'one')

    def test_missing_previous_library_can_only_reopen_existing_progress(self):
        existing = self.use()
        assert existing is not None
        (existing / 'slot').write_text('current')
        missing = self.folder / 'disconnected' / 'save'
        self.use(source=missing)
        self.assertEqual((existing / 'slot').read_text(), 'current')
        self.use(source=missing, title='f' * 16, ok=False)
        self.assertFalse((self.root / ('F' * 16)).exists())

    def test_invalid_or_overlapping_destinations_preserve_the_source(self):
        self.source.mkdir()
        (self.source / 'slot').write_text('safe')
        nested = self.source / 'inside'
        nested.mkdir()
        self.use(root=nested, ok=False)
        self.assertEqual(list(nested.iterdir()), [])
        self.use(title='../bad', ok=False)
        (self.root / TITLE).symlink_to(self.source, target_is_directory=True)
        self.use(ok=False)
        self.assertEqual((self.source / 'slot').read_text(), 'safe')

    def test_unsafe_source_aborts_without_publishing_or_leaving_staging_data(self):
        self.source.mkdir()
        (self.source / 'alias').symlink_to(self.folder, target_is_directory=True)
        self.use(ok=False)
        self.assertEqual(list(self.root.iterdir()), [])
        self.assertTrue((self.source / 'alias').is_symlink())
