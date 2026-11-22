# Taiko no Tatsujin: Rhythm Festival, US release (`01006B0016AD6000`)

Update 6.2.0 (`v2031616`, the base game plus the update), `main` build ID `7150d78c454e7c55b3deba8b86386c8e`, SDK 13. The same game as the [Japanese release](../0100BCA0135A0000/README.md) (Unity 2020.3.26f1 IL2CPP, CRIWARE ADX2, NVN; modules `main`, `subsdk0`, `sdk`), an older content version. Its `sdk` and `subsdk0` build IDs equal the JP profile's.

**Current:** plays on the Mac with the screen shape (`DM_SCREEN_ASPECT`, `1.6` for a 16:10 MacBook, unchanged at 16:9 or with `DM_SCREEN_ASPECT=0`). Checked at 16:10 against 16:9 on a fresh save: title and first-run dialogs, story scenes, hub, Taiko Mode menu, Controller Settings popup, song select, difficulty select, a genre loading screen (POP), gameplay (two stages), the results popups.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `FRAME_STEP` for main `7150d78c…` | [`patches.py`](patches.py) | Registers this build with the screen-shape code of the JP profile's [`patches.py`](../0100BCA0135A0000/patches.py): one module, `BUILDS` holds an address table per build. This file only loads that module and keys its `screen_shape` on the US build |
| `depth_mode zero`, `screen_aspect on`, `zip_store` | [`game.tsv`](game.tsv) | As for JP |
| Shader tables | `shader_*.tsv` | Written by the shader split from this build's `data.unity3d` (the JP tables differ: 697 of the map's keys are not JP's) |

The two builds share Unity's native code at a constant offset (US = JP - `0x7BF0`: the screen and swapchain sizes, `SetResolution`, the four `SetOrtho` callers and the two sprite extraction functions of `bg_cover`), so those sites are the JP ones plus `d`. The game's IL2CPP code is laid out differently (no constant shift), so `BUILDS` lists its addresses per build: the eight methods of `top_lift`'s table, the hooks (SceneControllerBase / TransitionSceneControllerBase `OnInitialize`, FooterController, PresenterWindowControllerBase, LetterBoxCamera `Start` and `RefreshCamera`, EnsoPartsBase `OnInitialize`). They were found by matching function bodies against JP; every site checks its original word on the build (all 25 do), so a wrong address stops the rewrite. `top_lift.c` and `bg_cover.c` are position independent and shared (the JP folder holds them). The rule tables apply unchanged: the scenes' paths and positions that they use are the same in US 6.2.0 (320 of 322 shared files identical; the two that differ hold JP-only 7.0.0 content).

Everything else in the [JP note](../0100BCA0135A0000/README.md#screen-shape-dm_screen_aspect-current) (moves, the sites mask, the helper bar camera) holds for this build.

## Mac routes

Build with a folder that holds the US base NSP and the 6.2.0 update NSP, as for the [JP release](../0100BCA0135A0000/README.md#mac-routes). A fresh save starts with the language / EULA / privacy dialogs, the story intro, a tutorial song, then many guide popups (`Y` closes a guide, `Plus` skips a story scene); the network usage dialog appears at every boot with the default answer "hide".

## Found on this game

- The offline account answer needs nothing per game: the engine's [`hle_account.c`](../../host/hle_account.c) answers it, and the boot reaches the title without the "Transmitting" hang.
- [A HORI Taiko Drum gets 8BitDo's button map](../../../docs/reference/services.md#a-hori-taiko-drum-gets-8bitdos-button-map): the drum's own map is chosen by its USB ID.

## Open

- Not seen on screen: story replays, results with Improvement Support, the song select footer `Root` (the song select shot shows the bar at the bottom edge), gameplay's top band on a DonBG stage (gameplay's stage backgrounds were seen), double play, the online and 4-player screens. EnsoPartsBase.OnInitialize (`0x1ce7e40`, a bare `ret`) was matched by its neighbours only; gameplay's stages at 16:10 looked right, which uses its stub.
