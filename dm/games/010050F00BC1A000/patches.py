# Resident Evil HD Remaster 1.0.0
# Experimental: bypass the timed wait in sRender::end().
# The GPU synchronization path after the wait remains intact.

CUTS = {
    '6bec9b23b09df46c315aa6f4bfacf91c': [
        (
            0x799900,
            'c5010054',
            '0e000014',
            'experimental: bypass sRender::end timed wait; retain subsequent GPU synchronization',
        ),
        (
            0x775ac4,
            'e8050034',
            '2f000014',
            'experimental: bypass sMain::move timed-wait branch',
        ),
        (
            0xce5dc,
            '083ea8d2',
            '085ea8d2',
            'experimental: change first initialized 30.0 to 120.0',
        ),
        (
            0xce5e8,
            '083ee8f2',
            '085ee8f2',
            'experimental: change second initialized 30.0 to 120.0',
        ),
    ],
}

# The timing loop is identified, but its full effect on game pacing
# has not yet been validated. Keep this patch build-bound and experimental.
BINDING = [
    {
        'what': 'sRender::end timed wait bypass',
        'now': 'build',
        'goal': 'unreviewed',
        'purpose': 'experimental frame pacing test',
        'why': 'The loop waits against a timing threshold; whether it is the limiting FPS cap or also protects other timing behaviour remains unverified.',
    },
]
