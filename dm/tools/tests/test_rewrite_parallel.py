"""Worker configuration and cancellation of the rewriter's actual fork pool, without game files."""
import ast
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[3]
SOURCE = REPO / 'dm/tools/patch_nso.py'


def alive(pid):
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False


def wait_until(test):
    until = time.monotonic() + 5
    while time.monotonic() < until:
        if test(): return True
        time.sleep(0.01)
    return False


class RewriteWorkers(unittest.TestCase):
    def test_invalid_worker_count_stops_before_loading_the_module(self):
        for value in ('0', '-1', 'many'):
            with self.subTest(value=value):
                r = subprocess.run([sys.executable, str(SOURCE), 'not-a-module', 'unused', 'main'],
                                   env=dict(os.environ, DM_REWRITE_J=value), capture_output=True, text=True)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn('[RED] DM_REWRITE_J must be a positive integer', r.stderr)
                self.assertNotIn('Traceback', r.stderr)

    def test_sigterm_reaps_the_active_pool(self):
        # patch_nso is a command-line program: extract its real pool helper and process signal handler, as the spin tests extract its analysis.
        tree = ast.parse(SOURCE.read_text())
        body: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'in_parallel'
                or isinstance(n, ast.Expr) and isinstance(n.value, ast.Call) and isinstance(n.value.func, ast.Attribute)
                and isinstance(n.value.func.value, ast.Name) and n.value.func.value.id == 'signal' and n.value.func.attr == 'signal']
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script = root / 'pool.py'
            script.write_text('import multiprocessing, os, signal, sys, time\nfrom pathlib import Path\nWORKERS = 2\n'
                              + ast.unparse(ast.Module(body=body, type_ignores=[]))
                              + f'\ndef task(n):\n    Path({tmp!r}, str(os.getpid())).touch()\n    while True: time.sleep(1)\n'
                              + '\nin_parallel(task, [(0,), (1,)])\n')
            p = subprocess.Popen([sys.executable, str(script)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            pids = []
            try:
                self.assertTrue(wait_until(lambda: len(list(root.glob('[0-9]*'))) == 2), 'pool did not start both jobs')
                pids = [int(f.name) for f in root.glob('[0-9]*')]
                p.terminate(); p.communicate(timeout=5)
                self.assertTrue(wait_until(lambda: all(not alive(pid) for pid in pids)), 'workers still alive after the rewriter stopped')
            finally:
                try: os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                p.communicate(timeout=5)


if __name__ == '__main__': unittest.main()
