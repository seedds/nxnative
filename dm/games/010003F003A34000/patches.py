"""Select the one-tick period in the game's Game Freak framework."""
import struct

from loader_census import adrp, got_values, ldr64
from nso_match import MatchError


def check_period_table(module, start):
    # The constructor reads its period through ADRP/LDR and a relative GOT
    # relocation. The fingerprint ignores the GOT's position across builds.
    page_word, load_word = struct.unpack_from('<II', module.image, start + 0xc0)
    page = adrp(page_word, start + 0xc0)
    load = ldr64(load_word)
    if page is None or load is None or page[0] != load[1]:
        raise MatchError('the frame-period table load does not match')
    table = got_values(module.image).get(page[1] + load[2])
    periods = struct.pack('<QQ', 16_666_667, 33_333_334)
    if table is None or table < module.text_end or module.image[table:table + len(periods)] != periods:
        raise MatchError('the first two frame periods are not 16,666,667 and 33,333,334 ns')


# The constructor initializes both frame periods at this site (main+0x38ba8 on 1.0.2).
# The renderer divides the nominal period by one display tick to set its NVN interval.
# The game's application override (main+0x7af0) writes period table entry 1 into
# the simulation slot on every update. It must use entry 0 too to keep game time.
MATCH_CUTS = {'main': [
    {'name': 'one-tick render and simulation periods', 'functions': [
        {'size': 0x224,
         'fingerprints': ['2ceb7798508227178c3b40a4a1cab7d56793ccedf9727f84185b533edc596b94'],
         'check': check_period_table,
         'cuts': [(0xc8, '080540f9', '080140f9', 'initialize both frame periods from the one-tick entry')]},
        {'size': 0x18,
         'fingerprints': ['81673279c08d7094e975f22e4d974b45e6b994714cb3257a3ff11ccb06bba883'],
         'cuts': [(0x10, 'e2030032', '02008052', 'select the one-tick simulation period on every update')]},
    ]},
]}

BINDING = [
    {'what': 'one-tick render and simulation periods', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the profile selects a faster default while the game's application override sets the simulation period; both functions and the relocated period table must match",
     'checked': 'Pikachu 1.0.0 and 1.0.2 and Eevee 1.0.2 match both functions and the period table offline'},
]
