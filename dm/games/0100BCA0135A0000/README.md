# Taiko no Tatsujin: Rhythm Festival, Japanese release (`0100BCA0135A0000`)

Update 7.0.0 (`v2097152`, the base game plus the update), `main` build ID `d907a35c2f7f0ed627c1a930cbe0803e`, SDK 13. Unity 2020.3.26f1 (IL2CPP) with CRIWARE ADX2 sound and `.usm` movies, NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. RomFS: `Data/data.unity3d` (the scenes and shaders, 557 MB) and `StreamingAssets` (`.acb`, `.usm`, the song bundles).

**Current:** plays on the Mac. The profile's one job beyond the provisional guesses is the screen shape: with `DM_SCREEN_ASPECT` the game renders a taller screen (a 16:10 MacBook's 1920x1200) in its own shape, with the whole 16:9 width and more above and below, and every 2D screen fills it. Screens checked at 16:10 against 16:9: hub, Taiko Mode menu, Controller Settings, song select (tab bar, footer), difficulty select, Filter Songs, results, My Room, Memories, Game Settings, Control Type, Store, story scenes, gameplay (top band, stage). With `DM_SCREEN_ASPECT=0` the game looks as it did without the patches (gameplay and the menus compared). The US release (`01006B0016AD6000`, 6.2.0) has its own [profile](../01006B0016AD6000/README.md) on the same code.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `depth_mode zero` | [`game.tsv`](game.tsv) | The game sets `nvnDeviceSetDepthMode` near = 0 in the check boot; the pipeline bases made before it ran were made for the other mode |
| `screen_aspect on` | `game.tsv` | Marks the profile as one with screen-shape stubs, as for the other two games that have them |
| `FRAME_STEP` for main `d907a35c…` | [`patches.py`](patches.py) | The game has no frame step: the key only gives `main` the state page the screen-shape stubs read (+0x48 the output's height, 0 = 16:9, +0x54 the sites mask), and the stubs declare the `SCREEN_ASPECT` capability, so the launcher offers *Picture shape* (`1.6` MacBook on the Mac) |
| `zip_store .acb:.dat:.usm:.usme:.metallib` | `game.tsv` | Sound and movies are read by offset |
| An offline-account answer | engine, [`hle_account.c`](../../host/hle_account.c) | `nn::account::EnsureNetworkServiceAccountAvailable` must report that no network service account is available (what an offline Switch does), or the boot stays on "Transmitting". The engine answers it for every game; the profile needs nothing |

## Screen shape (`DM_SCREEN_ASPECT`, current)

The picture keeps its width (1920) and takes the screen's height (a multiple of 48: 1200 at 1.6). `k = max(1, (16/9) / aspect)` is how much taller the world is, `d = (H - 1080) / 2` the extra half-height at each edge in canvas units. Everything below is for JP 7.0.0 (the US 6.2.0 build uses the same code with its own addresses, see `BUILDS` in `patches.py`); a 16:9 screen or `DM_SCREEN_ASPECT=0` leaves the state page zero and every stub runs the original instruction.

**The size.** Unity takes its screen size from the operation mode (1280x720 handheld, 1920x1080 docked) and clamps `Screen.SetResolution` to 1920x1080. Six `movz` of a height (back buffer, swapchain colour and depth, the two docked and the handheld sizes), the SetResolution clamp (the height follows the width in the screen's shape) and its two "standard size" compares load the height from the state page.

**Ortho widening.** All 2D scenes are orthographic cameras of size 540, which show size x aspect to each side, so a taller screen would lose its sides. The four `SetOrtho` callers load the size through a stub that returns `size x k` (a 16:9 or wider output, or a texture, is unchanged).

**The helper bar camera.** The Back / Confirm bar is a Screen Space - Camera canvas drawn by its own OperationCamera (culling mask `0x800000`, layer 23 alone), laid out in screen pixels. That camera is not widened: its view moves up by `size x (1 - 1/k)` so the bar sits where it did above the bottom edge, and a second stub adds the shift to bottom and top only for the three callers that render, not for the camera's own projection (the canvas is placed through that one, and moving it moved the canvas along).

**The edge-anchored UI (`top_lift.c`).** Headers placed at the 16:9 top (y 540) now sit below the real top. One position-independent C function (its words are in `patches.py`, written from the C file by [`gen_native.py`](gen_native.py): it compiles with the command it holds, checks with `otool -r` that there is no relocation, and `--check` compares; the rule tables are `_SCENE_RULES`, `_FOOTER_RULES`, `_POPUP_RULES`, `_STORY_RULES`, `_BAND_RULES`) walks a rule table through the game's IL2CPP methods (`get_transform`, `GetChild`, `get_name`, `get/set_localPosition`, `get/set_localScale`; offsets from Il2CppDumper in `BUILDS`' `fns`). A rule is `(path, move, guard, g)`:

| Move | Effect |
|---|---|
| an integer m | moved by m x d in y (positive up) |
| `SCALE` | position and scale x 1/k about the helper bar camera's centre (the song select footer) |
| `COVER` | scale x k, once (the story scenes' dimming sprite) |
| `GROW` | position and scale x k (gameplay's stage pictures) |
| `BAND` | stretched in y to the screen's edge with the inner edge on the lane (gameplay's top band; direction from the controller's y, so double play's second band goes down) |
| `FIT` | back to the 16:9 size and height above the bar (the results' Improvement Support bubble) |

The guard (`GE`, `LE`, `NEAR` = within 16) tests the original local y, so a name used lower on another screen is left, and a controller that initializes twice is moved once. The path is an optional `Controller:` then `/`-separated names from a direct child down, `*` any name. It must not hold a relocation: after every compile `otool -r` must show none (helpers `always_inline`, no recursion, no calls out).

**Hooks and the sites mask.** Each hook is a stub that tests its bit of the sites mask (state +0x54, default all ones; `DM_SCREEN_ASPECT_SITES=<mask>`), calls `top_lift` with its table, then runs the original instruction.

| Bit | Gates |
|---|---|
| 1, 2, 3 | the helper bar camera's shift in the second, third and fourth `SetOrtho` callers (the first has none) |
| 4 | the scene rules at `SceneControllerBase.OnInitialize` (`0x1e42b1c`) and `TransitionSceneControllerBase.OnInitialize` (`0x1f11e24`, the loading screens); gameplay's band and stage at `EnsoPartsBase.OnInitialize` (`0x1d3bfc0`) |
| 5 | the song select footer `Root` at `FooterController.OnInitialize` (`0x1f2fb54`) and the results bubble; the popups' footer at `PresenterWindowControllerBase.Initialized` (`0x21231ec`); the story dim sprite at `LetterBoxCamera.Start` (`0x1b55ee0`) |
| 6 | `bg_cover` (next) |

`LetterBoxCamera.RefreshCamera` (`0x1b5605c`) reads its padding as 0 so the story scenes' viewport is the whole screen; it is not masked by a bit.

**Backgrounds (`bg_cover.c`).** Nearly every 2D scene's background, fade and mask is a sprite of exactly one 16:9 screen, which now leaves the extra rows showing what lies behind. Unity's two sprite extraction functions (`0x8630f4`, `0x8635d8`) call a function right after the shared render node fill: a sprite whose world size is at least 1900 wide and 1060..1190 high, unrotated and vertically centred has its node matrix and world bounds scaled by k about its centre, nothing stretched, the transform itself untouched. A sprite already taller (scaled by a rule or an asset edit) is skipped, so the two never double up. Its words (`_BG_COVER`) are written by `gen_native.py` the same way.

## Mac routes

- Build the Mac app with `scripts/build_ipa_from_nsp.py <NSP folder> --platform mac-app -k <keys> -o <app>` ([Port another game](../../../docs/development.md#port-another-game)).
- Compare `DM_SCREEN_ASPECT=1.6` with `DM_SCREEN_ASPECT=0` (the 16:9 reference); at 16:10 the items that touch the top and bottom edges at 16:9 touch them again and the middle stays centred. `DM_SCREEN_ASPECT_SITES=<mask>` switches the hooks off one by one (`0` none, `0xffffffff` all). The launch environment wins over `env.txt`.
- The boot route depends on the save (Taiko Mode menu, difficulty select or a song): look at the first screenshot before sending more keys.
- Live values: `DM_PROBE=main:<off>@xN+<off>` prints registers and memory at an instruction ([debugging](../../../docs/agents/05-debugging.md)); the log is in the Mac app's log folder ([Playing](../../../docs/playing.md)).
- `gen_native.py --check` compiles `top_lift.c` and `bg_cover.c` again and compares them with the words in `patches.py`.

## Found on this game

- The offline-account answer (see the table above): without it the boot stays on "Transmitting".
- Unity's sprite bounds functions (`0x866760`, `0x866834`) never run for these sprites (they need draw mode `+0x204 == 1`), so `bg_cover` scales the node's copy of the world bounds instead.

## Open

- Not checked: loading screens 109, 116 and 117 (the online transition's title and rule rules are written, not seen), double play (the second player's band direction), free training (149 has no `DonBGController`, its band stays).
- Genre loading's Joy-Con notice (`Up_Pos`'s children) is moved by a rule; the state after the notice has played (it ends 60 lower than at 16:9) was not seen.
- Difficulty select's Options panel is left as is (centre content); song select's tab bar keeps the 16:9 gap above it.
- The US 6.2.0 build (`7150d78c…`) has its own profile, [`01006B0016AD6000`](../01006B0016AD6000/README.md), which uses this folder's `patches.py` (`BUILDS`, one address table per build) and the shared `top_lift.c` / `bg_cover.c`.
