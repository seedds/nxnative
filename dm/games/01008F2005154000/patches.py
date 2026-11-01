# South Park: The Fractured But Whole 1.05: platform-limit cuts for Snowdrop's three-pool page allocator, the same routine as Mario + Rabbids Kingdom
# Battle's (010067300059A000) built from an older source. Its initializer (+0x17fb29c onwards) reserves one 12 GB address range (+0x17fb2b0,
# AllocateAddressRegion through +0x185ff04) and splits it at fixed offsets: large allocations [base, base+4 GB) with a 65,536-page limit (w5 at
# +0x17fb2dc for the large allocator's init +0x17fef14), 512 KB pages [align(base+4 GB, 512 KB), base+8 GB) (+0x17fb300..+0x17fb30c) and 64 KB pages
# [align(base+8 GB, 64 KB), base+12 GB) (+0x17fb388 and x22 from +0x17fb2b8). 12 GB cannot fit the 4.375 GB alias region an iPhone has (the NSP
# script's region check measured alias +13.16 GB in nnMain's first 15 s).
# These cuts keep the layout and shrink it to 4 GB, sized from the original layout's use on the Mac (64 GB alias region, the pools' bump pointers and
# the large allocator's page counts read with DM_CTL memdump, 2026-10-31): after 90 s (title screen, then the opening logos) 64 KB pages had reached
# 848 MB (735 MB at 30 s), 512 KB pages 236 MB, large allocations 16,384 pages (1 GB). So: 1.5 GB large (24,576 pages), 512 MB of 512 KB pages,
# 2 GB of 64 KB pages. A first cut of 2 GB / 512 MB / 512 MB ran the 64 KB pool out in 2 s: its bump path (+0x17fb600) then asks for another 4 GB
# at the range's end (+0x17fb640), a fixed-address reservation the SDK aborts on (Break at sdk+0xa9dec, through +0x8ce0c). With 4 GB, the other
# regions (64 MB node area, about 2 MB of small ones before it, 104 MB after it) leave about 146 MB of the alias region.
# free tells the classes apart by the stored range starts (+0xdf0c8, +0xdf0f8; +0x17fce98, +0x17fceac), never by fixed offsets, and the slab paths
# take their range from the same fields (+0x17fc3e8, +0x17fc830). The SDK's 1 GB node area and 1 GB region alignment are cut in
# dm/tools/sdk_patches.py. Pool use over sustained play is open (README.md, Memory).
CUTS = {
    'df15edaaf603e00cd7d8a01bd05a15c2': [
        (0x17fb2b0, 'e00760b2', 'e00360b2', 'Snowdrop allocator: reserve 4 GB instead of 12 GB (mov x0,#0x100000000)'),
        (0x17fb2b8, 'f60760b2', 'f60360b2', 'Snowdrop allocator: combined range end 12 GB -> 4 GB (mov x22,#0x100000000), the 64 KB pool end'),
        (0x17fb2dc, 'e5031032', '05008c52', 'Snowdrop large allocator: maximum 64 KB page count 65536 -> 24576, matching its 1.5 GB range (mov w5,#0x6000)'),
        (0x17fb300, '2a00c0d2', 'ea00acd2', 'Snowdrop 512 KB pool start: base + 4 GB + 0x7ffff -> base + 1.5 GB + 0x7ffff (mov x10,#0x60070000; +0x17fb308 sets the low half)'),
        (0x17fb304, 'ea00a0f2', 'eaff9ff2', 'Snowdrop 512 KB pool start: movk x10,#7,lsl #16 is folded into the mov above; this slot repeats +0x17fb308 (movk x10,#0xffff)'),
        (0x17fb30c, 'eb035fb2', '0b00b0d2', 'Snowdrop 512 KB pool end: base + 8 GB -> base + 2 GB (mov x11,#0x80000000)'),
        (0x17fb388, 'ea031fb2', '0a00b0d2', 'Snowdrop 64 KB pool start: base + 8 GB + 0xffff -> base + 2 GB + 0xffff (mov x10,#0x80000000; +0x17fb38c sets the low half)'),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'Snowdrop three-pool allocator: 12 GB of ranges → 4 GB', 'now': 'build', 'goal': 'game', 'purpose': 'address space',
     'checked': 'disassembly of the initializer and of the free / slab paths that read the range fields; pool use measured on the Mac, 2026-10-31',
     'why': "Kingdom Battle's routine from an older source: the pool bounds are separate immediates (two of them split across a mov and movk), not one size field, so "
            "Kingdom Battle's cuts do not carry over; a function-matched rule could follow the game's updates. The cause, an iPhone's address space, is a platform limit"},
]
