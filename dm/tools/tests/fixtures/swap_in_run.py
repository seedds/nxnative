"""One swap_in call of the NSP script in a process of its own, with a fault injected, for test_nsp_swap_in.py:

  swap_in_run.py SCRIPT ROOT FAULT

SCRIPT: the NSP script to load (the repository's, or another revision's for a comparison). ROOT holds the staged output new.partial and the destination out.
FAULT: none; rename-fail or rename-int (the rename of new.partial onto out raises OSError or KeyboardInterrupt); rename-kill (that rename SIGKILLs the process);
unlink-int:N or unlink-fail:N (the Nth unlink from then on raises KeyboardInterrupt or PermissionError), counted from the call's start.
Prints "ok" or the exception's class name; a SIGKILL prints nothing."""
import importlib.util, os, signal, sys
from pathlib import Path

script, root, fault = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
repo = script.resolve().parents[1]
sys.path[:0] = [str(repo / 'dm/games'), str(repo / 'dm/tools'), str(repo / 'dm/gfx/tools')]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', script)
assert spec is not None and spec.loader is not None
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
tmp, dst = root / 'new.partial', root / 'out'

kind, _, n = fault.partition(':')
rename, unlink, count = os.rename, os.unlink, [0]


def fake_rename(a, b, *args, **kw):
    if Path(a) == tmp and Path(b) == dst:
        if kind == 'rename-fail': raise OSError(5, 'injected rename failure')
        if kind == 'rename-int': raise KeyboardInterrupt
        if kind == 'rename-kill': os.kill(os.getpid(), signal.SIGKILL)
    return rename(a, b, *args, **kw)


def fake_unlink(*args, **kw):
    count[0] += 1
    if count[0] == int(n or 0):
        if kind == 'unlink-int': raise KeyboardInterrupt
        if kind == 'unlink-fail': raise PermissionError(13, 'injected unlink failure')
    return unlink(*args, **kw)


os.rename, os.unlink = fake_rename, fake_unlink
try:
    nsp.swap_in(tmp, dst); print('ok')
except BaseException as e:   # noqa: BLE001 - the outcome is the output
    print(type(e).__name__)
