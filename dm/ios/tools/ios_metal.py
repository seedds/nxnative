# The single compile standard for the iOS shader packaging chain: MSL from any source (PC chain / constant patches / in future, the Switch original translated via Ryujinx) → iphoneos metallib.
# Compile parameters are defined only once, here; the chain.metal record in the reflection json is generated from the same parameters, record == actual.
import functools, hashlib, re, subprocess
from pathlib import Path
# -fmetal-math-mode=relaxed: keep fast math's reassociation / contraction, but forbid assuming there are no NaN / Inf. The translated shaders reproduce Maxwell's NaN semantics with isnan selects (8337 / 12809 use them),
# and the default fast mode lets each GPU's backend compiler treat them as always false; on the A20 Pro the opening course's odd frames had 727k NaN pixels in the main image (a few dozen with the same AIR on the M4 Pro), which became the black band / black-edged glow
# -fpreserve-invariance: [[invariant]] position outputs only agree across shaders with this flag (as dmsh_build.py's MAC_METAL: a depth pre-pass followed by a less-or-equal G-buffer)
# O3 without CVP avoids Apple's pathological control-flow lowering; retain its AIR passes and add late vector combining / scalar cleanup.
METAL = ['xcrun', '-sdk', 'iphoneos', 'metal', '-c', '-O3', '-mios-version-min=18.0', '-fmetal-math-mode=relaxed', '-fpreserve-invariance', '-Xclang', '-disable-llvm-passes']
METALLIB = ['xcrun', '-sdk', 'iphoneos', 'metallib']
OPT_RECORD = '; metal-opt default<O3> -CVP +vector-combine,instcombine,early-cse ' + hashlib.sha256(Path(__file__).read_bytes()).hexdigest()[:12]
RECORD = ' '.join(METAL) + '; ' + ' '.join(METALLIB) + OPT_RECORD                  # Written into json chain.metal

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
        subprocess.run([*metal, '-fmodules-cache-path=' + str(Path(air).resolve().parent / 'metal-modules'), msl, '-o', raw], check=True)
        subprocess.run([*optimizer(metal), raw, '-o', air], check=True)
        subprocess.run([*metal[:3], 'metallib', air, '-o', lib], check=True)
    finally:
        raw.unlink(missing_ok=True)
