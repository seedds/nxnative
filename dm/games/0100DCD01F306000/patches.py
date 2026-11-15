# Mina the Hollower 1.0.6. These cuts only apply to the measured main build.
MAIN = 'e86ae07bdfec77ee10d50d2163d0ff2e'


def movz_x1(size, shift):
    """Encode MOVZ X1 for a reservation size that fits one immediate."""
    immediate = size >> shift
    if not 0 <= immediate <= 0xffff or immediate << shift != size:
        raise ValueError('reservation does not fit the MOVZ immediate')
    word = 0xd2800001 | ((shift // 16) << 21) | (immediate << 5)
    return word.to_bytes(4, 'little').hex()


MIB = 1 << 20
GIB = 1 << 30
CUTS = {
    MAIN: [
        (0x103e8c, movz_x1(16 * GIB, 32), movz_x1(1536 * MIB, 16),
         'StandardAllocator reservation: 16 GiB to 1.5 GiB'),
        (0x6d8cc, movz_x1(4 * GIB, 32), movz_x1(768 * MIB, 16),
         'Default Mem slab reservation: 4 GiB to 768 MiB'),
        (0x6dc2c, movz_x1(4 * GIB, 32), movz_x1(768 * MIB, 16),
         'Render Mem slab reservation: 4 GiB to 768 MiB'),
    ],
}

BINDING = [
    {'what': 'three startup virtual reservations', 'now': 'build', 'goal': 'game',
     'purpose': 'memory', 'why': 'the Switch engine reserves 24 GiB across three allocators at startup'},
]
