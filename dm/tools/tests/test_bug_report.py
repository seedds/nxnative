"""scripts/bug_report.py: only the allow-listed text files go in, the home folder is written as ~, key-like lines are dropped, the size cap holds, a missing log is a note, the exit codes."""
import importlib.util
import os
import plistlib
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('bug_report', REPO / 'scripts/bug_report.py')
assert spec and spec.loader
br = importlib.util.module_from_spec(spec); spec.loader.exec_module(br)

TID = '0100AAAA0000B000'
KEY = '0123456789abcdef0123456789abcdef'


class BugReport(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.home = self.root / 'home'
        (self.home / 'Library/Logs/nxnative').mkdir(parents=True)
        (self.home / 'Library/Logs/DiagnosticReports').mkdir(parents=True)
        (self.home / f'Library/Application Support/nxnative/{TID}').mkdir(parents=True)
        (self.home / f'Library/Application Support/nxnative/{TID}/env.txt').write_text('DM_FPS_TARGET=60\n')
        (self.home / f'Library/Logs/nxnative/{TID}.log').write_text(f'start {self.home}/Games/x\ntitle_key = {KEY}\nok\n')
        self.app = self.root / 'Game.app'; (self.app / 'Contents').mkdir(parents=True)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'DMTitleID': TID, 'CFBundleDisplayName': 'Fake Game', 'CFBundleShortVersionString': '1.2.3',
                                                                      'CFBundleExecutable': 'FakeGame', 'DMCommit': 'abc1234'}))
        self.nsp = self.root / 'nsp'; b = self.nsp / f'{TID}_build'
        (b / 'logs').mkdir(parents=True); (b / 'extract').mkdir(); (b / 'shader/b-deadbeef0001').mkdir(parents=True); (b / 'run').mkdir()
        (b / 'logs/dmsh_build.log').write_text('translated 10\n')
        (b / 'logs/blob.log').write_bytes(b'\0\1\2')
        (b / 'logs/sub').mkdir(); (b / 'logs/sub/inner.log').write_text('nested\n')
        (b / 'extract/main.nso').write_bytes(b'NSO0decoy'); (b / 'run/save.bin').write_bytes(b'save')
        (b / 'shader/b-deadbeef0001/lib.metallib').write_bytes(b'metallib')
        (self.nsp / 'game.nsp').write_bytes(b'nsp')
        self.patches = [mock.patch.object(br, 'read_prefs', return_value={'launcher.DM_FPS_TARGET': '30', 'dataFolder': f'{self.home}/data'}),
                        mock.patch.object(br, 'device', return_value={'Mac': 'Mac99,1', 'Chip': 'Apple M9', 'macOS': '27.0', 'Xcode': 'Xcode 27.1', 'Metal compiler': 'metal 1'})]
        for p in self.patches: p.start(); self.addCleanup(p.stop)

    def run_main(self, *args, out=None):
        out = out or self.root / 'out.zip'
        with mock.patch.object(Path, 'home', return_value=self.home), mock.patch.dict(os.environ, {'HOME': str(self.home)}):
            with mock.patch('sys.stdout'), mock.patch('sys.stderr'):
                try: code = br.main([*args, '--out', str(out)])
                except SystemExit as e: code = e.code
        return code, out

    def names(self, out): return set(zipfile.ZipFile(out).namelist())

    def test_only_allowed_files(self):
        (self.home / f'Library/Logs/nxnative/{TID}.prev.log').write_text('prev\n')
        code, out = self.run_main(str(self.nsp))
        self.assertEqual(code, 0)
        n = self.names(out)
        self.assertEqual(n, {'report.md', f'logs/mac-app/{TID}.log', f'logs/mac-app/{TID}.prev.log', 'logs/build/dmsh_build.log'})
        z = zipfile.ZipFile(out)
        blob = b''.join(z.read(i) for i in n)
        for decoy in (b'NSO0decoy', b'save', b'metallib', b'nested'): self.assertNotIn(decoy, blob)

    def test_app_report(self):
        code, out = self.run_main(str(self.app))
        self.assertEqual(code, 0)
        r = zipfile.ZipFile(out).read('report.md').decode()
        for s in ('Fake Game', TID, '1.2.3', 'abc1234', 'Apple M9', 'DM_FPS_TARGET=60', 'launcher.DM_FPS_TARGET = 30', 'Mac app'): self.assertIn(s, r)
        self.assertIn(br.chain_version(), r)

    def test_home_redacted(self):
        code, out = self.run_main(str(self.app))
        z = zipfile.ZipFile(out)
        for i in z.namelist():
            self.assertNotIn(str(self.home), z.read(i).decode(), i)
        self.assertIn('~/Games/x', z.read(f'logs/mac-app/{TID}.log').decode())
        self.assertIn('~/data', z.read('report.md').decode())

    def test_key_lines_dropped(self):
        code, out = self.run_main(TID)
        z = zipfile.ZipFile(out)
        t = z.read(f'logs/mac-app/{TID}.log').decode()
        self.assertNotIn(KEY, t); self.assertIn('ok', t); self.assertIn('1 line(s)', t)
        for line, bad in (('master_key_00 = ' + KEY, True), (f'"titlekey": "{KEY.upper()}"', True), ('aes_kek_generation_source = ' + KEY, True),
                          ('build ID main=' + KEY, False), ('key pressed, short abcd', False), ('keyboard 1234', False)):
            self.assertEqual(br.redact(line + '\n', '/nope')[1], 1 if bad else 0, line)

    def test_size_cap(self):
        (self.home / f'Library/Logs/nxnative/{TID}.log').write_text('HEAD\n' + 'x' * 100 + '\n' * 1 + ('line\n' * 5000) + 'TAIL\n')
        b, _ = br.collect(br.resolve(TID), None, home=str(self.home), file_cap=2000)
        t = b.files[f'logs/mac-app/{TID}.log']
        self.assertLess(len(t), 2500); self.assertIn('HEAD', t); self.assertIn('TAIL', t); self.assertIn('bytes cut here', t)
        b, _ = br.collect(br.resolve(str(self.nsp)), None, home=str(self.home), total_cap=300)
        self.assertLessEqual(b.used, 300); self.assertTrue(any('cap' in n for n in b.notes))

    def test_big_file_read_both_ends(self):
        p = self.root / 'big.log'; p.write_text('HEAD\n' + 'a' * (br.FILE_CAP * 2) + '\nTAIL\n')
        t = br.read_text(p)
        self.assertLess(len(t), br.FILE_CAP + 200); self.assertIn('HEAD', t); self.assertIn('TAIL', t)

    def test_missing_log_is_a_note(self):
        (self.home / f'Library/Logs/nxnative/{TID}.log').unlink()
        code, out = self.run_main(TID)
        self.assertEqual(code, 0)
        self.assertIn('not found', zipfile.ZipFile(out).read('report.md').decode())

    def test_ios_log_and_binary(self):
        ios = self.root / 'log.txt'; ios.write_text('ios line\n')
        code, out = self.run_main(TID, '--ios-log', str(ios))
        self.assertIn('logs/ios/log.txt', self.names(out))
        ios.write_bytes(b'\0\0binary')
        code, out = self.run_main(TID, '--ios-log', str(ios))
        self.assertEqual(code, 0); self.assertNotIn('logs/ios/log.txt', self.names(out))

    def test_crash_reports(self):
        d = self.home / 'Library/Logs/DiagnosticReports'
        for i in range(5):
            (d / f'dmhost-{i}.ips').write_text(f'crash {i}\n')
            os.utime(d / f'dmhost-{i}.ips', (time.time() - i * 60, time.time() - i * 60))
        (d / 'dmhost-old.ips').write_text('old\n'); os.utime(d / 'dmhost-old.ips', (time.time() - 30 * 86400, time.time() - 30 * 86400))
        (d / 'Safari-1.ips').write_text('other\n')
        code, out = self.run_main(TID)
        n = {i for i in self.names(out) if i.startswith('crash/')}
        self.assertEqual(n, {'crash/dmhost-0.ips', 'crash/dmhost-1.ips', 'crash/dmhost-2.ips'})

    def test_exit_codes(self):
        self.assertEqual(self.run_main('/nonexistent/thing')[0], 2)
        self.assertEqual(self.run_main(str(self.root))[0], 2)
        (self.nsp / 'AAAAAAAAAAAAAAAA_build').mkdir()
        self.assertEqual(self.run_main(str(self.nsp))[0], 2)   # two build folders: ambiguous
        self.assertEqual(self.run_main(TID, out=self.root / 'no/such/dir/o.zip')[0], 1)
        self.assertEqual(self.run_main()[0], 2)


if __name__ == '__main__':
    unittest.main()
