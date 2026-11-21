# Exact main build only: retain room for Unity's independent virtual pools.
BINDING = [
    {'what': 'StandardAllocator startup reservation capped at 2 GiB', 'now': 'build', 'goal': 'game', 'purpose': 'address space',
     'why': "the game's startup reservation competes with Unity's separate pools"},
]
CUTS = {
    '5e6f9d0c5161586bcd32dd7ddc5e9db0': [
        (0xeec3c8, 'e20313aa', '0200b0d2',
         'nninitStartup: cap StandardAllocator virtual reservation at 2 GiB'),
    ],
}
