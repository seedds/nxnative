# Batman: Arkham Knight (0100ACD0163D0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Batman: Arkham Knight 1.0.5 (v327680) main: Rocksteady's Unreal Engine 3 carries an FMallocBinned3, a static object (main+0x51818c0) built inline in its guarded getter (main+0x192eb30) on the
  # first allocation, during the static constructors. The construction reserves a 1 GB address region (main+0x192eee8) and then 37 GB for its 74 pools of 512 MB (main+0x192fe50), which no alias
  # region of the host's holds (AllocateAddressRegion failed with 0x1803, err::ShowError, then a crash at main+0xb2dae8). The first instruction after the guard is taken jumps past the construction to
  # the guard's release (main+0x192fea4), so the object keeps the vtable the host put in it before any guest code ran: Unreal's allocator on a host malloc zone (dm/host/hle_uemalloc.c, layout UE3)
  'feae83dc9e9fb34c77d0a62f1fa9ae9f': [
    (0x192ec14, '68cb0090', 'a4040014', 'FMalloc getter: adrp x8 (start of the inline FMallocBinned3 construction) → b 0x192fea4 (the guard release); the host allocator\'s vtable is already in the object'),
    # The task queue's ring (push main+0xc5adbc, pop main+0xc5ab30: items at [q], tail at +0x18, head at +0x1c, 16-bit indices) publishes with plain stores: the push stores the item, then
    # the new tail, and a queue made with +0x10 set takes no lock on that side; the pop reads the tail, then the item, with no barrier. The Switch's Cortex-A57 cores let that pass; Apple's
    # cores make stores visible out of order and satisfy loads early, so a worker sometimes saw the new tail but the slot's old null, counted the item as taken (head + 1, slot cleared) and
    # dropped it: the task never ran, its parent's counter stayed at 1 and the rendering thread waited forever (the freezes at the menus, in the opening and in the diner). A store barrier
    # between the two stores of the push, a load barrier between the two loads of the pop
    (0xc5ae24, '4a3d4092', '4b3d4092', 'queue push: and x10, x10, #0xffff → and x11, x10, #0xffff (w10 keeps the tail)'),
    (0xc5ae28, '28792af8', '28792bf8', 'queue push: str x8, [x9, x10, lsl #3] → str x8, [x9, x11, lsl #3] (the item)'),
    (0xc5ae2c, '681a40b9', 'bf3a03d5', 'queue push: ldr w8, [x19, #0x18] (the tail again) → dmb ishst'),
    (0xc5ae30, '08050011', '48050011', 'queue push: add w8, w8, #1 → add w8, w10, #1 (then str w8, [x19, #0x18]: the new tail)'),
    (0xc5ab8c, '680240f9', 'bf3903d5', 'queue pop: ldr x8, [x19] → dmb ishld (after the tail load; w8 = head)'),
    (0xc5ab94, '691e40b9', '093d4092', 'queue pop: ldr w9, [x19, #0x1c] (the head again) → and x9, x8, #0xffff'),
    (0xc5ab98, '293d4092', '680240f9', 'queue pop: and x9, x9, #0xffff → ldr x8, [x19] (the ring; then ldr x8, [x8, x9, lsl #3]: the item)'),
  ],
}
