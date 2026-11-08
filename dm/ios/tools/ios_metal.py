# The single compile standard for the iOS shader packaging chain: MSL from any source (PC chain / constant patches / in future, the Switch original translated via Ryujinx) → iphoneos metallib.
# Compile parameters are defined only once, here; the chain.metal record in the reflection json is generated from the same parameters, record == actual.
import os, subprocess, sys, functools, threading
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../gfx/tools')); import dmsh_pch   # the precompiled header, which changes no output (so RECORD does not name it)
# -fmetal-math-mode=relaxed: keep fast math's reassociation / contraction, but forbid assuming there are no NaN / Inf. The translated shaders reproduce Maxwell's NaN semantics with isnan selects (8337 / 12809 use them),
# and the default fast mode lets each GPU's backend compiler treat them as always false; on the A20 Pro the opening course's odd frames had 727k NaN pixels in the main image (a few dozen with the same AIR on the M4 Pro), which became the black band / black-edged glow
# -fpreserve-invariance: [[invariant]] position outputs only agree across shaders with this flag (as dmsh_build.py's MAC_METAL: a depth pre-pass followed by a less-or-equal G-buffer)
# -O1: -O2's switch folding leaves a control-flow graph Apple's GPU backend can only lower slowly into very large code (as dmsh_build.py's MAC_METAL)
METAL = ['xcrun', '-sdk', 'iphoneos', 'metal', '-c', '-O1', '-mios-version-min=18.0', '-fmetal-math-mode=relaxed', '-fpreserve-invariance']
METALLIB = ['xcrun', '-sdk', 'iphoneos', 'metallib']
RECORD = ' '.join(METAL) + '; ' + ' '.join(METALLIB)                                # Written into json chain.metal
DEPTH = ['-fbracket-depth=1024']   # more nested brackets than the front end's 256 (as dmsh_build.py's METAL_DEPTH): no output changes with it, so RECORD does not name it

@functools.cache
def _lookup(sdk, name):
    out = lambda *a: subprocess.run(['xcrun', '-sdk', sdk, *a], check=True, capture_output=True, text=True).stdout.strip()
    return out('-f', name), dict(os.environ, SDKROOT=out('--show-sdk-path'))

_LOCK = threading.Lock()
def _tool(sdk, name):   # the tool xcrun runs for an SDK, and the SDKROOT it sets: looked up once (the build's workers start together). xcrun's lookup on every call cost about a fifth of a shader's compile (250 → 200 ms a library)
    with _LOCK: return _lookup(sdk, name)

def resolve(cmd):   # an 'xcrun -sdk <sdk> <tool> ...' command (METAL, METALLIB, dmsh_build.py's MAC_METAL) as the tool's own command line and the environment xcrun runs it with; the libraries are the same bytes
    assert cmd[:2] == ['xcrun', '-sdk'], cmd
    path, env = _tool(cmd[2], cmd[3])
    return [path, *cmd[4:]], env

def run(cmd):
    argv, env = resolve(cmd)
    subprocess.run(argv, check=True, env=env)

def metal(cmd, msl, air):   # a metal command up to its source (METAL, MAC_METAL) on msl → air, through the precompiled header (dmsh_pch.py)
    argv, env = resolve(cmd)
    dmsh_pch.compile(argv, msl, air, env)

def compile(msl, air, lib):
    metal(METAL + DEPTH, msl, air)
    run([*METALLIB, air, '-o', lib])
