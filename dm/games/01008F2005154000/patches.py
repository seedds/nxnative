# South Park: The Fractured But Whole, update 1.05 (v327680), Snowdrop.
# Experimental phone-sized virtual address reservation; only exact original words
# of this main's NSO build ID may be replaced. No Mac/iPhone gameplay validation yet.
#
# The allocator previously reserves 12 GiB as three adjacent 4 GiB address ranges
# (large blocks, 512 KiB slabs, 64 KiB slabs). In a 5 GiB guest block that
# reservation fails; Snowdrop ignores the error and writes through the fabricated
# 8 GiB slab pointer at main+0x17fb4b0 (fault 0x20000ffff).
#
# A first compact run with 3 GiB total, 2 GiB large/512 MiB 512 KiB slabs/
# 512 MiB 64 KiB slabs exhausted all 8192 slots of the 64 KiB class just
# after frame 300. Its unmodified 4 GiB fallback (main+0x17fb640) cannot fit
# in the 4.375 GiB alias alongside the original 3 GiB reservation, retries
# indefinitely and exhausts the SDK's 62 MiB address-region node area. SDK
# sdk+0x8dcac then asserts via Break SVC. Raising the node area only delays
# the assertion, and reducing the fallback request alone is unsafe: Snowdrop
# extends the class end without using the new reservation's returned address.
#
# Experimental correction: reserve 4 GiB up front, still within the 4.375 GiB
# guest alias after the SDK node area. Give 1.5 GiB to large allocations,
# 512 MiB to 512 KiB slabs, and 2 GiB to 64 KiB slabs. The observed compact
# run used <=705 MiB large-address high-water and <=170 MiB 512 KiB slab bump
# space, while consuming the full prior 512 MiB 64 KiB class. This quadruples
# its size while retaining headroom in the other two classes. Later high-water
# and whether the 4 GiB reservation fits every initialization path are untested.
# The large child allocator's independent 64 KiB page count MUST shrink from
# 65536 to 24576; otherwise it can hand out allocations beyond its 1.5 GiB
# class boundary.
# The 512 KiB class boundary is constructed with THREE instructions:
#   mov x10,#0x60000000; movk x10,#0x6007,lsl#16; movk x10,#0xffff
# It then aligns (x8 + 0x6007ffff) down to a 512 KiB boundary, yielding
# x8 + 0x60000000 when x8 is aligned. Changing only the first instruction
# fails: the original second movk (#7,lsl#16) clears the 1.5 GiB high bits, setting the
# boundary to the very beginning of the reserved range instead. The result
# overlaps the large allocator and
# corrupts the per-block owner pointers (crash at main+0x17fd774).
# The metadata reserve (main+0x17fb28c) already covers the reduced page count.
# SDK 3.5.1 needs two node-area cuts and one alignment cut in sdk_patches.py.
# The original 4 GiB slab growth fallbacks remain unsafe when any class runs
# out: this change only gives the 64 KiB class more up-front space.
CUTS = {
    'df15edaaf603e00cd7d8a01bd05a15c2': [
        (0x17fb2b0, 'e00760b2', '2000c0d2', 'Snowdrop: reserve 4 GiB instead of 12 GiB (mov x0,#0x100000000)'),
        (0x17fb2b8, 'f60760b2', '3600c0d2', 'Snowdrop: combined region end 4 GiB instead of 12 GiB (mov x22,#0x100000000)'),
        (0x17fb2dc, 'e5031032', '05008c52', 'Snowdrop large allocator: cap 64 KiB pages at 24576 (1.5 GiB) instead of 65536'),
        (0x17fb300, '2a00c0d2', '0a00acd2', 'Snowdrop: initialize large / 512 KiB slabs boundary to 1.5 GiB (paired with following movk)'),
        (0x17fb304, 'ea00a0f2', 'ea00acf2', 'Snowdrop: preserve 1.5 GiB when completing first boundary with +0x7ffff before 512 KiB alignment'),
        (0x17fb30c, 'eb035fb2', '0b00b0d2', 'Snowdrop: second class boundary 512 KiB / 64 KiB slabs at 2 GiB, formerly 8 GiB'),
        (0x17fb388, 'ea031fb2', '0a00b0d2', 'Snowdrop: 64 KiB slab range starts at 2 GiB; following movk supplies 0xffff alignment mask'),
    ],
}

# Binding: see dm/tools/docs_patches.py and docs/agents/game-patches.md.
BINDING = [
    {'what': 'Snowdrop three-pool allocator: 12 GiB reservation → 4 GiB with rebalanced classes',
     'now': 'build', 'goal': 'game', 'purpose': 'address space',
     'why': 'the seven cuts describe this game build\'s own allocator layout and its unchecked reservation failure; '
            'a function-matched rule could carry the layout across verified updates',
     'blocked': 'other South Park builds and sustained allocation-class high-water have not been checked',
     'checked': 'exact v1.05 main words and compact Mac progress through character customization and difficulty, 2026-10-27'},
]
