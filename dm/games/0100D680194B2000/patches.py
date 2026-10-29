# Pikmin 2's own code, keyed by the NSO build ID. SDK cuts live in
# dm/tools/sdk_patches.py.
CUTS = {
    # 1.0.0 (v0): _ZN5movie destructor unconditionally calls nn::diag::detail::AbortImpl
    # at main+0x2ba5b4 during video teardown. NOP out the abort call.
    'b23a43495191e7ad33d1d0effcbb1d84': [
        (0x2ba5b4, 'c9010094', '1f2003d5', '_ZN5movie destructor: abort call → nop'),
    ],
}

GENERALIZE = "No: game-specific video cleanup abort call"
