"""Keep local character selection independent of the unavailable EOS lobby."""

# 1.0.9 main+0xa33dc0 updates local state and calls two EOS lobby helpers.
# Both helpers enter GetLobbyInterface with a null platform wrapper after
# EOS_Initialize fails, faulting at main+0x6d1b4c. The host runs offline.
# Match the caller and the bodies of its direct callees before omitting only
# the two lobby calls. If either call encoding differs, the whole rule skips.
MATCH_CUTS = {'main': [
    {'name': 'character selection without EOS lobby updates',
     'size': 0xfc,
     'fingerprints': ['c8b98d1dcc4f80c626bd0b656e6b603b5643193b57410b1bbbbef876df4bbb61'],
     'cuts': [
         (0x94, 'e7e9ff97', '1f2003d5', 'character select offline: skip lobby flags update'),
         (0xec, '79eaff97', '1f2003d5', 'character select offline: skip lobby state update'),
     ]},
]}

BINDING = [
    {'what': 'character selection without EOS lobby updates',
     'now': 'game', 'goal': 'game', 'purpose': 'runtime',
     'why': "the game's own character-select path asks for an EOS lobby even after EOS initialization fails",
     'checked': '1.0.9: both null dereferences traced to the two calls in the same function; caller and callees fingerprinted'},
]
