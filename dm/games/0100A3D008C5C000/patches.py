# Pokémon Scarlet's own code, keyed by the NSO build ID. SDK cuts live in
# dm/tools/sdk_patches.py; see README.md for the measured startup failure.
CUTS = {
    # 1.0.0: nninitStartup calls this helper at main+0x12ded48. The allocator
    # receives (nullptr, 0x300000000, true), confirmed by an entry probe on
    # sdk+0x324aa8. This virtual reservation exceeds Apple's compact guest block.
    # Platform workaround: reserve 4 GB, as for Trinity in Legends: Z-A; the
    # application pool bounds physical commits independently of this address range.
    'ca44e7dd62e61a2a4e8889b3319ae173': [
        (0x12df074, '6200c0d2', '2200c0d2', 'StandardAllocator::Initialize(nullptr, size): virtual address space 12 GB → 4 GB'),
    ],
    # 4.0.0: the same request, (nullptr, 0x300000000, false) from the call at
    # main+0x167d554; the abort's backtrace passes main+0x167d558.
    '421c5411b487eb4d049dd065fec95477': [
        (0x167d54c, '6200c0d2', '2200c0d2', 'StandardAllocator::Initialize(nullptr, size): virtual address space 12 GB → 4 GB'),
    ],
}

# Whether a general rule could replace these patches (dm/tools/docs_patches.py lists it in docs/agents/game-patches.md)
GENERALIZE = "No: one constant of Trinity's own allocator call; the cause, an iPhone's address space, cannot be fixed in the host"
