# The game's update callback accumulates elapsed seconds and schedules the next
# camera/render update at 1/30 s. Its two animation deltas also multiply their
# time-scale factors by 1/30 s. Change all three accumulator constants to
# 1/60 s, and use the elapsed time already in s8 for the deltas, bounded by
# 1/60 s and the game's existing 1/15 s catch-up limit.
#
# s5–s7 are free after the last direct call. Keep the new threshold and the
# existing accumulator cap (4/30 s). Half that cap gives the animation
# bound of 1/15 s. The low 16 bits of the cap and threshold are both 0x8889.
# The complete callback has the same normalized body in v0 and 1.0.4
# (main+0x80c20 and +0x81910). Their direct callees give different fingerprints.
MATCH_CUTS = {'main': [
    {'name': 'Reverie frame scheduler and elapsed step', 'size': 904, 'fingerprints': [
        'a7eb9c7976e223b1c59130fd39fc500e59d632e3ac06e66a7ef65fd6c30cf893',
        '0ce4518cace7a74a90c5b79a747d551952b5cde467616f98e627fd1a8f3840cf',
    ], 'cuts': [
        (0x198, '0201271e2811915208a1a7722178221e0201271e', '0601271e07102c1e0891a7722178261e0501271e',
         'keep the accumulator cap in s6, its half multiplier in s7, and the new 1/60 s threshold in s5'),
        (0x1b0, '2020221e', '2020251e', 'update eligibility uses the 1/60 s threshold'),
        (0x1d4, '09a1a772', '0991a772', 'camera update threshold: 1/60 s'),
        (0x1dc, '0aa1b772', '0a91b772', 'subtract 1/60 s for each camera update'),
        (0x240, '2b1191520ba1a772824c221e6401271e', 'c608271e0769251e824c221ee478261e',
         'animation deltas use the elapsed frame time clamped to [1/60 s, 1/15 s]'),
    ]},
]}

BINDING = [
    {'what': 'render and camera scheduler at 60 Hz with elapsed animation deltas', 'now': 'game', 'goal': 'game',
     'purpose': 'frame rate', 'checked': 'complete timing callback verified in v0 and 1.0.4',
     'why': "the game's own accumulator and time-scale code; NVN already presents at interval 1, and neither Final Fantasy XII main matches"},
]
