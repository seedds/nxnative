# Mario + Rabbids Sparks of Hope 1.6.2225577. Snowdrop's three-pool allocator is cut for every build by dm/tools/snowdrop_pools.py, with the sizes in game.tsv.
# The memory system's startup (+0x94260) also builds a pool of 4,194,304 272-byte
# slots (+0x94364 -> +0x123484), 1.06 GB of address space, which no code in this
# build allocates from: only that startup and its teardown (+0x94214) name the
# pool or the default-pool slot it fills (0x7dd1260), and the opening quest,
# with the overalls found, commits none of it. With it, the regions reach the end of the
# 4.375 GB alias region in the opening quest; each UI node context
# (Moria::Services::UI::Shared::Nodes::Context, +0x2c4a228) reserves 576 KB and
# 256 KB more, unchecked, and the first entry pushed onto a context without its
# range writes to NULL + 0x78 (+0x2c534a8; shaking the overalls bush, GitHub
# issue #483). 65,536 slots keep the pool as built, in 17 MB.
CUTS = {
    '5b76a43b231e0640521ac66203f7c178': [
        (0x94354, '0108a052', '2100a052', 'Unused 272-byte slot pool: 4,194,304 -> 65,536 slots, 1.06 GB -> 17 MB of address space (mov w1,#0x10000)'),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'unused 272-byte slot pool: 4,194,304 → 65,536 slots', 'now': 'build', 'goal': 'game', 'purpose': 'address space',
     'why': "Sparks of Hope's own init (Kingdom Battle has none)"},
]
