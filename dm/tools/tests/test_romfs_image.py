"""Packer/cache round trips and the index validator shared by host mounts and iOS startup; synthetic files only."""
import importlib.util
import os
from pathlib import Path
import shutil
import shlex
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('romfs_image', REPO / 'dm/tools/romfs_image.py')
assert spec is not None and spec.loader is not None
image = importlib.util.module_from_spec(spec); spec.loader.exec_module(image)


class Images(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tools.cleanup)
        root = Path(cls.tools.name)
        source = root / 'validate.c'
        source.write_text('#include "romfs_image.h"\nint main(int argc, char **argv) {\n'
                          'if (argc != 2) return 2; romfs_img *m = romfs_image_open(argv[1]);\n'
                          'if (!m) return 1; romfs_image_close(m); return 0; }\n')
        cls.validator = root / 'validate'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/host'),
                        str(source), '-o', str(cls.validator)], check=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / 'RomFS'; self.src.mkdir()
        self.dst = self.root / 'romfs.img'

    def valid(self):
        return subprocess.run([str(self.validator), str(self.dst)]).returncode == 0

    def content(self, name):
        isdir, offset, size = image.read_index(self.dst)[name]
        self.assertFalse(isdir)
        with self.dst.open('rb') as f:
            f.seek(offset); return f.read(size)

    def test_empty_romfs_and_packed_files_use_the_host_validator(self):
        image.pack(self.src, self.dst)
        self.assertTrue(self.valid()); self.assertEqual(self.dst.stat().st_size, 56)
        (self.src / 'data/empty').mkdir(parents=True)
        (self.src / 'data/a').write_bytes(b'hello')
        (self.src / 'data/z').write_bytes(b'')
        image.pack(self.src, self.dst)
        self.assertTrue(self.valid()); self.assertEqual(self.content('data/a'), b'hello')
        self.assertEqual(self.content('data/z'), b'')
        self.assertTrue(image.read_index(self.dst)['data/empty'][0])

    def test_format_mismatches_name_zs_files_that_are_not_zstd(self):
        tex = self.src / 'Tex'; tex.mkdir()
        (tex / 'ok.bntx.zs').write_bytes(b'\x28\xb5\x2f\xfd' + b'frame')
        (tex / 'skip.bntx.zs').write_bytes(b'\x53\x2a\x4d\x18' + b'\0' * 8)   # a skippable frame (any of 0x184d2a50..5f)
        (tex / 'UPPER.ZS').write_bytes(b'\x28\xb5\x2f\xfd')
        (tex / 'zeros.bntx.zs').write_bytes(b'\0' * 12288 + b'\x28\xb5\x2f\xfd')   # as on a damaged NUP of Ocarina of Time
        (tex / 'short.zs').write_bytes(b'\x28\xb5')
        (tex / 'empty.zs').write_bytes(b'')   # no bytes, no promise broken: left out
        (tex / 'plain.bntx').write_bytes(b'\0' * 16)   # no format promised by the name
        image.pack(self.src, self.dst)
        seen = []
        bad = image.format_mismatches(self.dst, lambda done, total: seen.append((done, total)))
        self.assertEqual(sorted((p, size, head) for p, size, head in bad), [('Tex/short.zs', 2, b'\x28\xb5'), ('Tex/zeros.bntx.zs', 12292, b'\0\0\0\0')])
        self.assertEqual(seen, [(0, 5)])

    def test_reuse_rebuilds_changed_added_removed_and_renamed_files(self):
        (self.src / 'a').write_bytes(b'old')
        image.pack(self.src, self.dst, reuse=True)
        identity = image.file_identity(self.dst)
        image.pack(self.src, self.dst, reuse=True)
        self.assertEqual(image.file_identity(self.dst), identity)
        # Same length and a restored mtime: ctime still invalidates the cached image.
        times = (self.src / 'a').stat()
        (self.src / 'a').write_bytes(b'new')
        os.utime(self.src / 'a', ns=(times.st_atime_ns, times.st_mtime_ns))
        image.pack(self.src, self.dst, reuse=True)
        self.assertEqual(self.content('a'), b'new')
        (self.src / 'added').write_bytes(b'added')
        image.pack(self.src, self.dst, reuse=True); self.assertEqual(self.content('added'), b'added')
        (self.src / 'added').rename(self.src / 'renamed'); (self.src / 'a').unlink()
        image.pack(self.src, self.dst, reuse=True)
        self.assertEqual(set(image.read_index(self.dst)), {'renamed'})

    def test_changed_image_and_forced_repack_are_not_reused(self):
        image.pack(self.src, self.dst, reuse=True)
        self.dst.write_bytes(image.HEAD.pack(image.MAGIC, 32, 0, 0))
        image.pack(self.src, self.dst, reuse=True); self.assertTrue(self.valid())
        identity = image.file_identity(self.dst)
        image.pack(self.src, self.dst)
        self.assertNotEqual(image.file_identity(self.dst), identity)

    def test_reuse_survives_a_new_device_number(self):   # macOS numbers a volume at each mount: the Data volume went from 16777234 to 16777232 across a reboot, with the same inode, size and times
        (self.src / 'a').write_bytes(b'data')
        image.pack(self.src, self.dst, reuse=True)
        digest, identity = image.source_digest(self.src, image.entries(self.src)), image.file_identity(self.dst)
        real = os.stat
        def remounted(path, *args, **kw):
            s = real(path, *args, **kw)
            return SimpleNamespace(st_dev=s.st_dev + 2, st_ino=s.st_ino, st_size=s.st_size, st_mtime_ns=s.st_mtime_ns, st_ctime_ns=s.st_ctime_ns)
        with patch.object(image.os, 'stat', remounted):
            self.assertEqual(image.file_identity(self.dst), identity)
            self.assertEqual(image.source_digest(self.src, image.entries(self.src)), digest)
            self.assertTrue(image.reusable(str(self.dst), digest))

    def test_changed_source_during_pack_preserves_previous_image(self):
        (self.src / 'a').write_bytes(b'old'); image.pack(self.src, self.dst)
        original = self.dst.read_bytes()
        scan = image.entries
        calls = 0
        def changed(src):
            nonlocal calls
            calls += 1
            if calls == 2: (self.src / 'a').write_bytes(b'changed during packing')
            return scan(src)
        with patch.object(image, 'entries', side_effect=changed), self.assertRaisesRegex(RuntimeError, 'changed while packing'):
            image.pack(self.src, self.dst)
        self.assertEqual(self.dst.read_bytes(), original)
        self.assertFalse(Path(str(self.dst) + '.part').exists())

    def test_push_helper_refreshes_cache_and_propagates_copy_failure(self):
        resources = self.root / 'resources.env'
        resources.write_text('DM_RESOURCE_ROMFS=' + shlex.quote(str(self.src)) + '\n')
        bin_dir = self.root / 'bin'; bin_dir.mkdir()
        xcrun = bin_dir / 'xcrun'
        xcrun.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> "$ROMFS_PUSH_ARGS"\nexit "${ROMFS_PUSH_RC:-0}"\n')
        xcrun.chmod(0o755)
        args_file = self.root / 'copy-args'
        env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'], ROMFS_PUSH_ARGS=str(args_file), ROMFS_PUSH_VIA='devicectl')
        cmd = ['bash', str(REPO / 'scripts/push_romfs.sh'), 'fixture-device', 'fixture.bundle', str(resources)]
        (self.src / 'a').write_bytes(b'old')
        def push(*args): return subprocess.run(cmd + list(args), env=env, capture_output=True, text=True)
        self.assertEqual(push().returncode, 0)
        self.dst = Path(str(self.src) + '.img')
        self.assertEqual(self.content('a'), b'old')
        (self.src / 'a').write_bytes(b'new')
        self.assertEqual(push().returncode, 0); self.assertEqual(self.content('a'), b'new')
        identity = image.file_identity(self.dst)
        result = push('--repack'); self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(image.file_identity(self.dst), identity)
        self.assertIn('Documents/romfs.img', args_file.read_text())
        env['ROMFS_PUSH_RC'] = '7'; result = push()
        self.assertEqual(result.returncode, 7); self.assertNotIn('game files on the phone', result.stdout)

    def test_case_collision_guest_names(self):
        if not shutil.which('xattr'): self.skipTest('macOS xattr tool required')
        (self.src / 'a').write_bytes(b'lower')
        alias = self.src / '.dm-case-1'; alias.write_bytes(b'upper')
        subprocess.run(['xattr', '-w', image.GUEST_NAME, 'A', str(alias)], check=True)
        image.pack(self.src, self.dst)
        self.assertTrue(self.valid()); self.assertEqual(self.content('A'), b'upper')
        self.assertEqual(self.content('a'), b'lower')

    def test_size_consistent_invalid_index_is_rejected(self):
        for raw in (image.HEAD.pack(image.MAGIC, 32, 0, 0),
                    image.HEAD.pack(image.MAGIC, 32, 24, 1) + image.ENT.pack(1, 0, 0, 0, 1, 0)):
            with self.subTest(raw=raw):
                self.dst.write_bytes(raw); self.assertFalse(self.valid())
        (self.src / 'a').write_bytes(b'content'); (self.src / 'b').write_bytes(b'other')
        image.pack(self.src, self.dst); complete = self.dst.read_bytes()
        _, index, _, _ = image.HEAD.unpack_from(complete)
        mutations = {
            'truncated': complete[:-1], 'trailing bytes': complete + b'x',
            'root cycle': (index, '<Q', 0), 'missing child': (index + 8, '<Q', 1),
            'bad name offset': (index + 24 + 16, '<I', 0xffffffff),
            'file in header': (index + 24, '<Q', 0), 'file into index': (index + 24 + 8, '<Q', index),
            'duplicate children': (index + 48 + 16, '<I', 0),
            'unsorted children': complete[:-2] + b'ba', 'path separator': complete[:-2] + b'a/',
        }
        for name, change in mutations.items():
            with self.subTest(name=name):
                raw = bytearray(complete)
                if isinstance(change, tuple):
                    off, fmt, value = change; struct.pack_into(fmt, raw, off, value)
                else: raw = change
                self.dst.write_bytes(raw); self.assertFalse(self.valid())


# libimobiledevice's afcclient as push_romfs.sh drives it: one command after the options and "--", files in the app's data container (here a folder),
# and, as the real one does, exit status 0 even when the command fails (the error goes to stderr); a put does not make missing parent folders, rm needs -r for a
# folder that is not empty, and rm -r asks on stdin unless -f. AFC_PUT_SHORT cuts a put off halfway; DLC_PUSH_FAIL fails every put into Documents/aoc;
# AFC_NO_DEVICE finds no device with that UDID (a devicectl identifier, a device name, or one on Wi-Fi only).
FAKE_AFC = '''#!/usr/bin/env python3
import json, os, shutil, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ['AFC_CALLS'], 'a') as f: f.write(' '.join(args) + '\\n')
assert args[0] == '-u' and args[2] == '--container' and args[4] == '--', args
if os.environ.get('AFC_NO_DEVICE'): print(f'ERROR: Device {args[1]} not found!', file=sys.stderr); sys.exit(1)
cmd = args[5]; flags = ''.join(a[1:] for a in args[6:] if a.startswith('-')); rest = [a for a in args[6:] if not a.startswith('-')]
root = Path(os.environ['AFC_DEVICE'])
def at(p): return root / p.lstrip('/')
def fail(msg): print('Error: ' + msg, file=sys.stderr); sys.exit(0)
if cmd == 'put':
    src, dst = Path(rest[0]), at(rest[1])
    if os.environ.get('DLC_PUSH_FAIL') and rest[1].startswith('/Documents/aoc'): fail('Failed to open remote file')
    if not dst.parent.is_dir(): fail('Not found (8)')
    if src.is_dir():
        if 'r' not in flags: fail(f'{src} is a directory')
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        if dst.exists() and 'f' not in flags: fail('Failed to write into existing file without -f option')
        data = src.read_bytes(); dst.write_bytes(data[:len(data) // 2] if os.environ.get('AFC_PUT_SHORT') else data)
elif cmd == 'info':
    p = at(rest[0])
    if not p.exists(): fail('Failed to get file info: Not found (8)')
    print(json.dumps({'st_size': p.stat().st_size, 'st_ifmt': 'S_IFDIR' if p.is_dir() else 'S_IFREG'}, indent=2))
elif cmd == 'mkdir':
    p = at(rest[0])
    if p.exists() or not p.parent.is_dir(): fail('Failed to create directory')
    p.mkdir()
elif cmd == 'rm':
    p = at(rest[0])
    if not p.exists(): fail('Failed to remove: Not found (8)')
    if p.is_dir() and 'r' in flags:
        if 'f' not in flags and sys.stdin.readline().strip() != 'y': fail('cancelled')
        shutil.rmtree(p)
    elif p.is_dir():
        if any(p.iterdir()): fail('Directory not empty (33)')
        p.rmdir()
    else: p.unlink()
elif cmd == 'mv':
    src, dst = at(rest[0]), at(rest[1])
    if not src.exists(): fail('Failed to rename: Not found (8)')
    src.replace(dst)
else: sys.exit(f'fake afcclient: no command {cmd}')
'''


class DlcTransfers(unittest.TestCase):
    """The DLC set replaces Documents/aoc over AFC, and nothing else in the app's data container: devicectl's --remove-existing-content empties the whole
    container (CoreDevice 651.32, docs/reference/packing.md), saves included, so the script never passes it."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='dlc push '); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        source = self.root / 'RomFS'; source.mkdir(); (source / 'marker').write_text('base game')
        self.img = self.root / 'RomFS.img'; image.pack(source, self.img)
        self.aoc = self.root / 'add-on content'; self.aoc.mkdir()
        self.device = self.root / 'device'; (self.device / 'Documents').mkdir(parents=True)
        self.scratch = self.root / 'scratch'; self.scratch.mkdir()
        bin_dir = self.root / 'bin'; bin_dir.mkdir()
        (bin_dir / 'afcclient').write_text(FAKE_AFC); (bin_dir / 'afcclient').chmod(0o755)
        self.xcrun_calls = self.root / 'xcrun-calls'
        (bin_dir / 'xcrun').write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$XCRUN_CALLS"\n'); (bin_dir / 'xcrun').chmod(0o755)
        self.env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'], AFC_DEVICE=str(self.device),
                        AFC_CALLS=str(self.root / 'afc-calls'), XCRUN_CALLS=str(self.xcrun_calls), TMPDIR=str(self.scratch))
        self.env.pop('ROMFS_PUSH_VIA', None)

    def push(self, aoc=None, *, image_only=False):
        resources = self.root / 'resources.env'
        resources.write_text('DM_RESOURCE_ROMFS=' + shlex.quote(str(self.img)) + '\n' +
                             ('DM_RESOURCE_AOC=' + shlex.quote(str(aoc)) + '\n' if aoc is not None else ''))
        return subprocess.run(['bash', str(REPO / 'scripts/push_romfs.sh'), 'fixture-device', 'fixture.bundle',
                               str(self.img if image_only else resources)], env=self.env, capture_output=True, text=True)

    def test_smaller_dlc_set_replaces_images_and_legacy_folders(self):
        (self.aoc / '1.img').write_text('old')
        (self.aoc / '2').mkdir(); (self.aoc / '2/marker').write_text('legacy DLC')
        result = self.push(self.aoc); self.assertEqual(result.returncode, 0, result.stderr)
        remote = self.device / 'Documents/aoc'
        self.assertEqual({p.name for p in remote.iterdir()}, {'1.img', '2'})
        self.assertEqual((remote / '2/marker').read_text(), 'legacy DLC')
        (self.aoc / '1.img').write_text('updated'); shutil.rmtree(self.aoc / '2')
        result = self.push(self.aoc); self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual({p.name for p in remote.iterdir()}, {'1.img'})
        self.assertEqual((remote / '1.img').read_text(), 'updated')
        self.assertIn('DLC on the phone', result.stdout)

    def test_empty_dlc_set_clears_previous_content(self):
        for aoc in ('', None, self.aoc):
            with self.subTest(aoc=aoc):
                remote = self.device / 'Documents/aoc'; remote.mkdir(parents=True, exist_ok=True)
                (remote / '1.img').write_text('stale')
                self.env['DM_RESOURCE_AOC'] = str(self.root / 'inherited-stale-path')
                result = self.push(aoc); self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(remote.is_dir()); self.assertEqual(list(remote.iterdir()), [])
                self.assertEqual(list(self.scratch.iterdir()), [])

    def test_dlc_sync_keeps_saves_and_the_rest_of_the_container(self):
        save = self.device / 'Documents/save/slot_00'; save.parent.mkdir(); save.write_text('progress')
        prefs = self.device / 'Library/Preferences/app.plist'; prefs.parent.mkdir(parents=True); prefs.write_text('choices')
        for aoc in ('', self.aoc):
            with self.subTest(aoc=aoc):
                (self.aoc / '1.img').write_text('dlc')
                result = self.push(aoc); self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(save.read_text(), 'progress'); self.assertEqual(prefs.read_text(), 'choices')
        self.assertFalse(self.xcrun_calls.exists())   # neither the image nor the DLC goes through devicectl

    def test_image_only_push_preserves_dlc(self):
        remote = self.device / 'Documents/aoc'; remote.mkdir(parents=True)
        (remote / '1.img').write_text('keep')
        self.env['DM_RESOURCE_AOC'] = str(self.aoc)
        result = self.push(image_only=True); self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((remote / '1.img').read_text(), 'keep')
        self.assertEqual((self.device / 'Documents/romfs.img').read_bytes(), self.img.read_bytes())

    def test_missing_dlc_stops_before_any_transfer(self):
        result = self.push(self.root / 'missing')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Add-on content unavailable', result.stderr)
        self.assertEqual(list((self.device / 'Documents').iterdir()), [])

    def test_dlc_transfer_failure_is_reported(self):
        (self.aoc / '1.img').write_text('dlc')
        self.env['DLC_PUSH_FAIL'] = '1'   # afcclient exits 0 all the same: the script checks the device
        result = self.push(self.aoc)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('1.img', result.stderr)
        self.assertNotIn('DLC on the phone', result.stdout)
        self.assertNotIn('game files on the phone', result.stdout)

    def test_devicectl_never_empties_the_container(self):
        self.env['ROMFS_PUSH_VIA'] = 'devicectl'
        result = self.push(''); self.assertEqual(result.returncode, 0, result.stderr)   # no DLC in the build: Documents/aoc left as it is
        (self.aoc / '1.img').write_text('dlc')
        result = self.push(self.aoc)
        self.assertNotEqual(result.returncode, 0)   # a DLC set needs afcclient
        self.assertIn('afcclient', result.stderr)
        self.assertNotIn('--remove-existing-content', self.xcrun_calls.read_text())


class AfcTransfers(unittest.TestCase):
    """The image goes over AFC (afcclient), which streams it, where devicectl copies the whole file through CoreDeviceService's memory."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='afc push '); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        source = self.root / 'RomFS'; source.mkdir(); (source / 'marker').write_text('base game ' * 100)
        self.img = self.root / 'RomFS.img'; image.pack(source, self.img)
        self.device = self.root / 'device'; (self.device / 'Documents').mkdir(parents=True)
        self.bin = self.root / 'bin'; self.bin.mkdir()
        self.calls = self.root / 'afc-calls'; self.xcrun_calls = self.root / 'xcrun-calls'
        (self.bin / 'xcrun').write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$XCRUN_CALLS"\n'); (self.bin / 'xcrun').chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ['PATH'], AFC_DEVICE=str(self.device),
                        AFC_CALLS=str(self.calls), XCRUN_CALLS=str(self.xcrun_calls))
        self.env.pop('ROMFS_PUSH_VIA', None)

    def afc(self):
        (self.bin / 'afcclient').write_text(FAKE_AFC); (self.bin / 'afcclient').chmod(0o755)

    def push(self):
        return subprocess.run(['bash', str(REPO / 'scripts/push_romfs.sh'), 'fixture-device', 'fixture.bundle', str(self.img)],
                              env=self.env, capture_output=True, text=True)

    def test_image_goes_over_afc_and_is_renamed_once_complete(self):
        self.afc(); (self.device / 'Documents/romfs.img').write_bytes(b'an older image')
        result = self.push(); self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.device / 'Documents/romfs.img').read_bytes(), self.img.read_bytes())
        self.assertFalse((self.device / 'Documents/romfs.img.part').exists())
        calls = self.calls.read_text()
        self.assertIn(f'put -f {self.img} /Documents/romfs.img.part', calls)
        self.assertIn('mv /Documents/romfs.img.part /Documents/romfs.img', calls)
        self.assertFalse(self.xcrun_calls.exists())   # devicectl is not used for the image
        self.assertIn('game files on the phone', result.stdout)

    def test_cut_off_afc_copy_fails_and_never_replaces_the_image(self):
        self.afc(); old = b'an older image'; (self.device / 'Documents/romfs.img').write_bytes(old)
        self.env['AFC_PUT_SHORT'] = '1'
        result = self.push()
        self.assertNotEqual(result.returncode, 0)   # afcclient itself exits 0: the script checks the size on the device
        self.assertIn('incomplete', result.stderr)
        self.assertEqual((self.device / 'Documents/romfs.img').read_bytes(), old)
        self.assertNotIn('game files on the phone', result.stdout)

    def test_unknown_device_is_named_before_any_copy(self):
        self.afc(); self.env['AFC_NO_DEVICE'] = '1'
        result = self.push()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('UDID', result.stderr); self.assertNotIn('incomplete', result.stderr)
        self.assertNotIn(' put ', self.calls.read_text())

    def test_without_afcclient_a_large_image_is_not_copied_through_devicectl(self):
        self.env['PATH'] = str(self.bin) + os.pathsep + '/usr/bin:/bin'   # no afcclient; sysctl is the fake below
        (self.bin / 'sysctl').write_text('#!/bin/sh\necho 64\n'); (self.bin / 'sysctl').chmod(0o755)   # a "Mac" with 64 bytes of memory
        result = self.push()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('afcclient', result.stderr)
        self.assertFalse(self.xcrun_calls.exists())
        self.env['ROMFS_PUSH_VIA'] = 'devicectl'   # chosen explicitly, it goes through devicectl
        result = self.push(); self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Documents/romfs.img', self.xcrun_calls.read_text())


if __name__ == '__main__': unittest.main()
