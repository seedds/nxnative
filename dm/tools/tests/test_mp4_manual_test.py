"""The manual workflow launches exactly one chosen mode. All game/build processes are test doubles."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[3] / 'dm/games/010019A01E2F2000/manual_test.sh'


class ManualWorkflow(unittest.TestCase):
    def test_each_diagnostic_launches_only_one_selected_mode(self):
        for switch, flag in [('SCAN_STATE_PROBE', '--state'), ('SCAN_ACQUIRE_PROBE', '--acquisition')]:
            with self.subTest(switch=switch), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                images = root / 'images'
                run = images / 'Metroid Prime 4 Beyond [NSZ]/010019A01E2F2000_build/run'
                run.mkdir(parents=True)
                launches = root / 'launches.txt'
                fake = run / 'dmhost_TEST_DOUBLE'
                fake.write_text('#!/bin/sh\nprintf "one fake launch\\n" >> ' + shlex.quote(str(launches)) + '\nprintf "test log only\\n"\n')
                fake.chmod(0o755)
                calls = root / 'python-calls.txt'
                python_stub = root / 'python_stub'
                python_stub.write_text(f'''#!{sys.executable}
from pathlib import Path
import sys
with Path({str(calls)!r}).open('a') as f: f.write(' '.join(sys.argv[1:]) + '\\n')
if sys.argv[1].endswith('build_ipa_from_nsp.py'):
    print({('cd ' + shlex.quote(str(run)) + ' && ./dmhost_TEST_DOUBLE | tee host.log')!r})
elif sys.argv[1].endswith('scan_diag.py') and sys.argv[3] == 'env':
    print('DM_PROBE_EVERY=0')
else:
    print('test-double summary/validation')
''')
                python_stub.chmod(0o755)
                env = {k: v for k, v in os.environ.items() if k not in ('PROBE', 'SCALE_PROBE', 'SCAN_PROBE', 'RUMBLE_PROBE', 'VIS_SCALE', 'ANISO', 'SCAN_STATE_PROBE', 'SCAN_ACQUIRE_PROBE')}
                env.update(PY=str(python_stub), MP4_IMAGES=str(images), **{switch: '1'})
                result = subprocess.run(['/bin/bash', str(SCRIPT), '1800p'], env=env, text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(launches.read_text().splitlines(), ['one fake launch'])
                self.assertIn('ONE game launch: 1800p', result.stdout)
                self.assertNotIn('ONE game launch: default', result.stdout)
                self.assertIn(f'{flag} env ', calls.read_text())
                self.assertIn(f'{flag} check ', calls.read_text())
                self.assertIn(f'{flag} summary ', calls.read_text())
                outputs = list(images.glob('*/010019A01E2F2000-work/manual-test/1800p-*'))
                self.assertEqual(len(outputs), 1)
                self.assertIn('DM_INTERNAL_RESOLUTION=1800p', (outputs[0] / 'env.txt').read_text())


if __name__ == '__main__':
    unittest.main()
