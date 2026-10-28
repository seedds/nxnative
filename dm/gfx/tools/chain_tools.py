#!/usr/bin/env python3
# dmsh_chain.py's tool fingerprints on any OS (Windows, Linux, the Mac): tools_now() and check_tools() run dmsh_chain.py's own functions, with one difference: a tool that cannot be
# started here (xcrun and the Metal compilers off a Mac, the translator's macOS launcher on Windows and Linux) reads as 'missing' instead of raising. Where every tool starts, as on a
# Mac, the values are dmsh_chain.py's byte for byte (the same code runs the same commands). dmsh_chain.py hashes its own bytes into the chain version (FILES), so this behaviour lives
# beside it and is not part of the version (docs/design/vulkan-backend.md, step M0). There is no --update-lock here: off a Mac it would record the Metal compilers as missing.
# Usage: chain_tools.py --check-tools   as dmsh_chain.py --check-tools: [RED] lines for every tool that differs from dmsh_chain.lock (a missing one too), exit 1 when any does
import contextlib, os, subprocess, sys, types
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); import dmsh_chain

def _run(cmd, **kw):   # subprocess.run, except that a program that cannot be started prints 'missing' (dmsh_chain.py passes text=True and reads stdout only)
    try: return subprocess.run(cmd, **kw)
    except OSError as e: return subprocess.CompletedProcess(cmd, 127, 'missing', str(e))

@contextlib.contextmanager
def _portable():   # dmsh_chain.py runs its tools through its module's 'subprocess' name (tools_now only)
    real = dmsh_chain.subprocess
    dmsh_chain.subprocess = types.SimpleNamespace(run=_run)
    try: yield
    finally: dmsh_chain.subprocess = real

def tools_now():
    with _portable(): return dmsh_chain.tools_now()

def check_tools():
    with _portable(): return dmsh_chain.check_tools()

if __name__ == '__main__':
    if sys.argv[1:] != ['--check-tools']: sys.exit('usage: chain_tools.py --check-tools')
    bad = check_tools()
    for b in bad: print('[RED] translation chain tools do not match the lock file: ' + b)
    sys.exit(1 if bad else 0)
