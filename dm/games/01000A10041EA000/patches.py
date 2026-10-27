# Experimental adaptation for the host's bounded guest alias address space.
# The original Default/GPU heaps reserve 2 GiB each, plus a 1 GiB common
# small-block pool, 600 MiB file heap and 64 MiB per-thread ScrapHeaps.
# These reservations cannot coexist in the supported 5.375 GiB alias region.
# Keep the initial commits (85 MiB / 1 GiB / 4 MiB) and original ScrapHeap
# limits; reduce only the virtual growth ceilings of the three largest pools.
# Capacity high-water validation remains open; this is not a release profile.
CUTS = {
    'b3f6e5da5c501857d754e3e186e04dc2': [
        (0x551d24, 'e1030132', '0100ac52',
         'Default Heap reserve 2 GiB -> 1.5 GiB; initial 85 MiB commit unchanged'),
        (0x551d5c, 'e1030132', '0100ac52',
         'GPU Heap reserve 2 GiB -> 1.5 GiB; initial 1 GiB commit unchanged'),
        (0x57b2a0, 'e1030232', '0100a252',
         'common small-block pool reserve 1 GiB -> 256 MiB; initial 4 MiB commit unchanged'),
        # Mode 7 clears depth, then initializes stencil through a raw-buffer
        # compute dispatch over the Tegra D24S8 allocation. Metal's stencil
        # texture does not alias that buffer. Also clear stencil through NVN:
        # w21 is 1 for mode 7 and 0 for mode 1 (ordinary depth-only clears),
        # so NEG supplies the respective masks 0xffffffff and 0. The original
        # dispatch remains; its lower-five-bit pattern is not consumed by the
        # eight stencil-fetch shader variants. See the game's graphics note.
        (0xb55250, 'e3031f2a', 'e303154b',
         'mode-7 scene initialization: clear native stencil before raw Tegra D24S8 compute writes'),
    ],
}

# All inlined copies of the same plain-store unlock need release ordering
# before SignalLightEvent's acquire-only fast path. The build-gated table is
# regenerated and checked by dm/tools/check_skyrim_unlocks.py from the user's
# original main; it verifies every byte, address equivalence and register
# liveness. Three x0 writeback forms use a tiny wrapper in zero text padding.
from pathlib import Path
for line in Path(__file__).with_name('unlock_release.tsv').read_text().splitlines():
    if not line or line.startswith('#'):
        continue
    at, old, new, reason = line.split('\t')
    CUTS['b3f6e5da5c501857d754e3e186e04dc2'].append((int(at, 16), old, new, reason))
