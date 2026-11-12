# EA SPORTS FC 27 (0100C49025D3E000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO/NRO build ID;
# CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py

# The Frostbite renderer NRO (build d924da0d0673a87b550159a839f5f807, 1.8d.6523 and 1.8d.f570) builds its NVN swapchain in fb::SwapChainNvn from the size its settings pick (a 1280x720 default
# when it picks none) and reads neither display call. The constructor (0x7a080) copies the SwapChainDesc into the object, then calls createSwapChainResources (0x7a3b0) and createNvnWindow (0x7aa00);
# both read the width at +0xc and the height at +0x10. Its four instructions at 0x7a310 give the defaults when a dimension is zero; they now store the chosen size. resize (0x7ad00) takes the
# requested width in x1 and height in x2, returns early on a zero one, and does nothing when both equal the object's; it now takes the chosen size as the request, so an equal size still ends
# there and any other recreates the swapchain at the chosen size.
# The chosen size is data, not code: the cuts load it with PC-relative ldr literals from two words of the NRO's key page (KEY_PAGE_OFF 0xfc000; words 16 and 17, offsets +0x80 and +0x88: the rewriter's own keys are words 0 to 3), which the host fills before the page is made read-only (dm/host/hle_nro.c nro_key_words): the size Native resolution picks (hle_display.c), the Switch's 1920x1080 when it is off.
# Code cannot be written when it runs from the signed app on iOS, so the same rows serve every platform.
swap = 'swapchain size from the key page'
CUTS = {
  'd924da0d0673a87b550159a839f5f807': [
    (0x7a310, '08040034', '88eb4018', swap + ': constructor, width: cbz w8 (the 1280 default) → ldr w8, [key page +0x80]'),
    (0x7a314, '681240b9', '680e00b9', swap + ': constructor: ldr w8,[x19,#0x10] → str w8,[x19,#0xc]'),
    (0x7a318, '48040034', '88eb4018', swap + ': constructor, height: cbz w8 (the 720 default) → ldr w8, [key page +0x88]'),
    (0x7a31c, 'e00313aa', '681200b9', swap + ': constructor: mov x0,x19 → str w8,[x19,#0x10] (x0 still holds the object)'),
    (0x7ad00, 'c10900b4', '019c4058', swap + ': resize, width: cbz x1 → ldr x1, [key page +0x80]'),
    (0x7ad18, 'f40302aa', '829b4058', swap + ': resize, height: mov x20,x2 → ldr x2, [key page +0x88]'),
    (0x7ad1c, '620800b4', 'f40302aa', swap + ': resize: cbz x2 → mov x20,x2'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': "the renderer NRO's swapchain size rows follow the display size (key page words)", 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "Frostbite's SwapChainNvn code, which its other games ship; the rows are this NRO's own instructions",
     'blocked': 'one Frostbite game here, and no function-matched rule for the constructor yet'},
]
