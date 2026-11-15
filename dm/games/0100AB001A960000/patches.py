CUTS = {
    '73c19a6aaead08e517b6553357a5260b': [
        # The first-start block (gameStarts == 0 at main+0x10d494) unconditionally writes
        # English to GAME/language and GAME/voice. Keep the configured values. The later
        # reads already default to zero (English) and the other first-start defaults still run.
        (0x10d55c, '6acdff97', '1f2003d5', 'Preserve the text language during first-start initialization'),
        (0x10d5d8, '4bcdff97', '1f2003d5', 'Preserve the speech language during first-start initialization'),
    ],
}

BINDING = [
    {'what': 'preserve language and voice on first start', 'now': 'build', 'goal': 'unreviewed',
     'purpose': 'runtime', 'why': 'the game overwrites its INI values at two unconditional call sites during first-start initialization'},
]
