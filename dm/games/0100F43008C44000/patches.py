# Pokémon Legends: Z-A (0100F43008C44000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Pokémon Legends: Z-A 2.0.2 (v393216) main: nninitStartup (main+0x1b2e650) initializes its StandardAllocator with virtual address memory over 12 GB of address space (main+0x1b3e3e4), which
  # the Switch's 64 GB alias region holds and an iPhone's single 6 GB reservation cannot. The allocator commits pages as it goes and the pool (3.2 GB) bounds what it maps (profile mem_map_limit);
  # its largest block is nninitStartup's own probe of pool − used (3.15 GB, allocated and freed at once), so 4 GB of address space holds it
  'b1f12fd919eae86ab8a978317677e64b': [
    (0x1b3e3e4, '6200c0d2', '2200c0d2', 'StandardAllocator::Initialize(nullptr, size): address space mov x2,#0x300000000 → mov x2,#0x100000000 (12 GB → 4 GB)'),
  ],
}

# No timing behavior changes by default. Both callbacks return without touching
# guest registers unless their diagnostics / experiment is explicitly enabled.
# hle_za_timing.c checks the same build ID again at runtime.
OBS = {
  'b1f12fd919eae86ab8a978317677e64b': [
    # Clock channel 0 returns {total_ns, delta_ns}. The next instructions convert
    # delta_ns / 1e9 to float and hand it to engine updates (main+0x1b29314/334).
    (0x1b292dc, 26, 'fd400400', 'Trinity base-clock delta: orig ldr d0,[x0,#8]; observe game seconds versus monotonic time'),
    # After the configured interval and native adaptive candidate were selected,
    # immediately after bl main+0x113488. The original mov forwards w20 to the
    # interval setter. An opt-in mutable callback can reproduce Static60 without
    # changing an instruction in memory; off preserves w20 and w22 exactly.
    (0x1b296f0, 27, '2a1403e1', 'Trinity interval: orig mov w1,w20; optional saved-register experiment with timing diagnostics'),
    # Generic layout visibility, filtered by the exact WazaLayoutView vtable in
    # the host. Diagnostics only: no input remapping or register changes.
    (0x425c70, 28, 'f9405408', 'Layout visibility: orig ldr x8,[x0,#0xa8]; observe the actual battle move layout, not ZL alone'),
    (0x2b7bfbc, 29, 'a9be7bfd', 'Waza layout destructor: orig stp x29,x30,[sp,#-0x20]!; expire the observed skill context'),
  ],
}
