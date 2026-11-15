# The single compile standard for the iOS shader packaging chain: MSL from any source (PC chain / constant patches / in future, the Switch original translated via Ryujinx) → iphoneos metallib.
# Compile parameters are defined only once, here; the chain.metal record in the reflection json is generated from the same parameters, record == actual.
import os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../gfx/tools')); import dmsh_pch   # the precompiled header, which changes no output (so RECORD does not name it)
# -fmetal-math-mode=relaxed: keep fast math's reassociation / contraction, but forbid assuming there are no NaN / Inf. The translated shaders reproduce Maxwell's NaN semantics with isnan selects (8337 / 12809 use them),
# and the default fast mode lets each GPU's backend compiler treat them as always false; on the A20 Pro the opening course's odd frames had 727k NaN pixels in the main image (a few dozen with the same AIR on the M4 Pro), which became the black band / black-edged glow
# -fpreserve-invariance: [[invariant]] position outputs only agree across shaders with this flag (as dmsh_chain.py's MAC_METAL: a depth pre-pass followed by a less-or-equal G-buffer)
# -O1: -O2's switch folding leaves a control-flow graph Apple's GPU backend can only lower slowly into very large code (as dmsh_chain.py's MAC_METAL)
# IOS_FLAGS: the iPhone shader libraries' deployment target and math mode, which vk_shaders.py and gl_shaders.py pass too (one place for the version)
IOS_MIN_FLAG = '-mios-version-min=18.0'
IOS_FLAGS = [IOS_MIN_FLAG, '-fmetal-math-mode=relaxed']
METAL = ['xcrun', '-sdk', 'iphoneos', 'metal', '-c', '-O1', *IOS_FLAGS, '-fpreserve-invariance', '-fwrapv']   # -fwrapv: see dmsh_chain.py MAC_METAL
METALLIB = ['xcrun', '-sdk', 'iphoneos', 'metallib']
# A source that uses MSL 4.0's vertex_value (a pixel shader reading an attribute per vertex, dmsh_rewrite.per_vertex) compiles as MSL 4.0, which only iOS 26 loads; every other source keeps
# the deployment target's language, so its library loads on every iPhone it did before. The Mac needs no such flag: its deployment target (dm/mac/min_os.mk) is already MSL 4.0
METAL4, METAL4_FLAG = b'vertex_value<', '-std=metal4.0'
RECORD = ' '.join(METAL) + f' ({METAL4_FLAG} for a source using vertex_value); ' + ' '.join(METALLIB)   # Written into json chain.metal
DEPTH = ['-fbracket-depth=1024']   # more nested brackets than the front end's 256 (as dmsh_build.py's METAL_DEPTH): no output changes with it, so RECORD does not name it

def compile(msl, air, lib):
    with open(msl, 'rb') as f: v4 = METAL4 in f.read()
    dmsh_pch.compile(METAL + ([METAL4_FLAG] if v4 else []) + DEPTH, msl, air)
    subprocess.run([*METALLIB, air, '-o', lib], check=True)
