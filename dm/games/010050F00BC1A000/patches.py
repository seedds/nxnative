# Resident Evil HD Remaster 1.0.0
# Restore the previously tested 120 FPS timing patch.
# The host frame-lock writes the selected 30/60/120 target to the game's
# primary MT Framework frame-limit field at runtime.

CUTS = {
    '6bec9b23b09df46c315aa6f4bfacf91c': [
        (
            0x799900,
            'c5010054',
            '0e000014',
            'restore: bypass sRender::end timed wait for high-FPS pacing',
        ),
        (
            0x775ac4,
            'e8050034',
            '2f000014',
            'restore: bypass sMain::move timed-wait branch',
        ),
        (
            0xce5dc,
            '083ea8d2',
            '085ea8d2',
            'initialize primary MT Framework timing value to 120',
        ),
        (
            0xce5e8,
            '083ee8f2',
            '085ee8f2',
            'initialize secondary MT Framework timing value to 120',
        ),
    ],
}

BINDING = [
    {
        'what': 'sRender::end and sMain::move timing waits',
        'now': 'build',
        'goal': 'unreviewed',
        'purpose': 'restore previously tested 120 FPS path',
        'why': 'The two wait bypasses and 120.0 constructor constants are restored; lower-rate modes are handled separately by the host frame-lock.',
    },
]
