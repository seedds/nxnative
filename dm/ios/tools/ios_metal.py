# The single compile standard for the iOS shader packaging chain: MSL from any source (PC chain / constant patches / in future, the Switch original translated via Ryujinx) → iphoneos metallib.
# Compile parameters are defined only once, here; the chain.metal record in the reflection json is generated from the same parameters, record == actual.
import functools, hashlib, os, re, subprocess, sys
from pathlib import Path
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../gfx/tools')); import dmsh_pch   # the precompiled header, which changes no output (so RECORD does not name it)
# -fmetal-math-mode=relaxed: keep fast math's reassociation / contraction, but forbid assuming there are no NaN / Inf. The translated shaders reproduce Maxwell's NaN semantics with isnan selects (8337 / 12809 use them),
# and the default fast mode lets each GPU's backend compiler treat them as always false; on the A20 Pro the opening course's odd frames had 727k NaN pixels in the main image (a few dozen with the same AIR on the M4 Pro), which became the black band / black-edged glow
# -fpreserve-invariance: [[invariant]] position outputs only agree across shaders with this flag (as dmsh_build.py's MAC_METAL: a depth pre-pass followed by a less-or-equal G-buffer)
# PR #556: O3 without correlated value propagation avoids the pathological switch lowering, while retaining AIR passes and late vector combining.
METAL = ['xcrun', '-sdk', 'iphoneos', 'metal', '-c', '-O3', '-mios-version-min=18.0', '-fmetal-math-mode=relaxed', '-fpreserve-invariance', '-Xclang', '-disable-llvm-passes']
METALLIB = ['xcrun', '-sdk', 'iphoneos', 'metallib']
OPT_RECORD = '; metal-opt default<O3> -CVP +vector-combine,instcombine,early-cse ' + hashlib.sha256(Path(__file__).read_bytes()).hexdigest()[:12]
RECORD = ' '.join(METAL) + '; ' + ' '.join(METALLIB) + OPT_RECORD                   # Written into json chain.metal
DEPTH = ['-fbracket-depth=1024']   # more nested brackets than the front end's 256 (as dmsh_build.py's METAL_DEPTH): no output changes with it, so RECORD does not name it

@functools.lru_cache(None)
def optimizer(metal):
    tool = Path(subprocess.check_output([*metal[:3], '-f', 'metal'], text=True).strip()).parent / 'metal-opt'
    driver = subprocess.check_output([*metal, '-###', '-x', 'metal', '/dev/null', '-o', '/dev/null'], stderr=subprocess.STDOUT, text=True)
    target = re.search(r'"-triple" "([^"]+)"', driver)
    if target is None: raise ValueError('Metal did not report its target triple')
    options = ['-mtriple=' + target[1], '-vectorize-loops', '-vectorize-slp', '-extra-vectorizer-passes']
    pipeline = subprocess.check_output([tool, *options, '-passes=default<O3>', '-print-pipeline-passes', '-disable-output'], input='', text=True).strip()
    names = {
        'AIRFunctionPredicateResolverPass': 'air-function-predicate-resolver',
        'AIRDisabledEliminationPass': 'air-disabled-elimination',
        'AIRPostTessellationOptPass': 'air-post-tessellation-opt',
        'AIRAliasScopePropagationPass': 'air-alias-scope-propagation',
        'AIRConditionalAddressSpacePass': 'air-conditional-address-space',
        'AIRIntersectionQueryDCEPass': 'air-intersection-query-dce',
        'GlobalCtorOptPass': 'global-ctors-opt',
        'AIRVertexAmplifierPass': 'air-vertex-amplifier',
        'AIRUnusedArgumentsPass': 'air-unused-args',
        'AIRFixupMetadataPass': 'air-fixup-metadata',
        'AIRArraysOfResourcesEmulationPass': 'air-arrays-of-resources-emulation',
        'AIRMetadataPrunerPass': 'air-metadata-pruner',
        'AIRCallGraphCheckerPass': 'air-callgraph-checker',
        'AIRVerifierPass': 'air-verify',
    }
    pipeline = re.sub(r'air::(\w+)', lambda m: names[m[1]], pipeline)
    if pipeline.count('correlated-propagation,') != 2 or pipeline.count('global-ctors-opt') != 1:
        raise ValueError('unexpected Metal O3 pipeline')
    pipeline = pipeline.replace('correlated-propagation,', '').replace('global-ctors-opt', 'function(vector-combine,instcombine,early-cse<>),global-ctors-opt')
    if 'correlated-propagation' in pipeline: raise ValueError('unfiltered Metal CVP pass')
    return [tool, *options, '-passes=' + pipeline]


def compile(msl, air, lib, metal=None):
    metal = tuple(METAL if metal is None else metal)
    raw = Path(str(air) + '.raw')
    try:
        # Keep the existing PCH equivalence check and stdin source naming: a
        # file name otherwise changes static initializer names and cache bytes.
        dmsh_pch.compile(list(metal) + DEPTH, msl, raw)
        subprocess.run([*optimizer(metal), raw, '-o', air], check=True)
        subprocess.run([*metal[:3], 'metallib', air, '-o', lib], check=True)
    finally:
        raw.unlink(missing_ok=True)
