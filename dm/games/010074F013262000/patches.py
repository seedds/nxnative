# Xenoblade Chronicles 3: changes to its own modules, checked by NSO build ID.
# Nintendo SDK module changes belong to dm/tools/sdk_patches.py.
CUTS = {
    # 2.2.0 / 2.2.1 share this main build ID (issue #847; see README.md).
    # nninitStartup reserves 60 GB for Monolith's allocator. Its first
    # arena reserves 4 GB + 128 KB, maps the first 128 KB for bookkeeping, and
    # takes additional pages on demand. iOS cannot reserve that address space
    # without the extended virtual addressing entitlement.
    # The first arena commits 203 MB at the title, 225 MB in the opening battle
    # and at most 243 MB through the first chapter's cutscenes (README.md). The
    # windows after it take 3.92 GB, and a 64 MB block sits at the top of the
    # reservation: with a 512 MB arena, 4.25 GB does not fit in 4 GB, and the
    # cutscene after the first chapter's battles crashed on a null allocation.
    # Reserve 288 MB for the first arena and 4 GB for the container, changing
    # the bump increment, subtraction and stored capacity together. This is
    # an address-space adaptation; physical limits still follow the Switch
    # pool. Even 288 MB is too much for some scenes, which then skip character
    # models: ALIAS_CUTS below lets a host with room keep Monolith's own layout.
    '82d187fe9ef9be921b8e1b1cddf7bd28': [
        (0x128e58c, 'e001c0d2', '2000c0d2', 'malloc address region: 60 GB to 4 GB'),
        (0x1241870, 'e98340b2', 'e9ff9fd2', 'first arena bump size, low part: mov x9,#0xffff'),
        (0x1241880, '4900a0f2', '4940a2f2', 'first arena bump size: 288 MB + 128 KB + 64 KB - 1'),
        (0x12418d8, 'e97b5fb2', 'e98361b2', 'first arena subtraction, sign extension'),
        (0x12418dc, 'c9ffbff2', 'c9bfbdf2', 'first arena subtraction: -(288 MB + 128 KB)'),
        (0x12418e8, '2800c0f2', '4840a2f2', 'first arena stored capacity: 288 MB + 128 KB'),
    ],
}

# All six only make room in an iPhone's 4.375 GB alias region (mem_alias): the Mac, whose alias region holds the 60.25 GB of mem_alias_wide, undoes them at load, and a
# DM_WIDE_ALIAS build for an iPhone with the extended virtual addressing entitlement leaves them out (patch_nso.py ALIAS_CUTS; README.md, Profile and memory)
ALIAS_CUTS = {
    '82d187fe9ef9be921b8e1b1cddf7bd28': [0x128e58c, 0x1241870, 0x1241880, 0x12418d8, 0x12418dc, 0x12418e8],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'malloc region 60 GB → 4 GB and its first arena', 'now': 'build', 'goal': 'game', 'purpose': 'address space',
     'why': "Monolith's own allocator; its constants span two functions (a multi-function rule). The cause, an iPhone's address space, is a platform limit"},
]
