# EA SPORTS FC 27 (0100C49025D3E000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # 1.8d.6523 (v458752) Engine.Render.Core2.PlatformNvn.nro: the Frostbite NVN swapchain. fb::SwapChainNvn::SwapChainNvn (nro0+0x7a080) copies the SwapChainDesc into the object, then
  # calls createSwapChainResources (0x7a3b0) and createNvnWindow (0x7aa00). Both read the width at +0xc and the height at +0x10. This four-instruction block only supplied the
  # 1280x720 defaults when the descriptor left a dimension zero; replacing it forces 2560x1440 for the object, so the NVN window textures, crop and present are native 1440p.
  # The layer size comes from vi::ListDisplays (DM_DISPLAY_SIZE); use DM_DISPLAY_SIZE=2560x1440 with this patch.
  'd924da0d0673a87b550159a839f5f807': [
    (0x7a310, '08040034', '08408152', 'swapchain width 2560: cbz w8 → mov w8, #0xa00'),
    (0x7a314, '681240b9', '680e00b9', 'swapchain width store: ldr w8,[x19,#0x10] → str w8,[x19,#0xc]'),
    (0x7a318, '48040034', '08b48052', 'swapchain height 1440: cbz w8 → mov w8, #0x5a0'),
    (0x7a31c, 'e00313aa', '681200b9', 'swapchain height store: mov x0,x19 → str w8,[x19,#0x10]'),
    (0x7ad00, 'c10900b4', '1f2003d5', 'resize: ignore a zero width, the object is always forced to 2560x1440'),
    (0x7ad18, 'f40302aa', '14b480d2', 'resize: height 1440 instead of x2'),
    (0x7ad1c, '620800b4', '1f2003d5', 'resize: ignore a zero height'),
    (0x7ad24, 'f50301aa', '154081d2', 'resize: width 2560 instead of x1'),
    (0x7ad3c, '60070054', '1f2003d5', 'resize: recreate even when the requested size equals the current one'),
  ],
}

BINDING = [
  {'what': 'native 1440p swapchain: the NRO swapchain object forced to 2560x1440', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
   'why': 'the swapchain size is a per-build constant in the Frostbite NVN renderer; a future build needs the same instructions verified'},
]
