# Bridge Constructor Portal (`010016E004796000`)

Update 1.1 (v131072) over the base game, with its licence-only DLC `010016E004797001` (no content; the title shows Portal Proficiency). SDK 8.3.2, NVN API 53.313. Unity 2019.1.14f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays by touch alone at 30 fps, as on the Switch: on the Mac from the title through the first level into the second, on an iPhone through the third. A double-clickable Mac app is packed.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `op_mode handheld` | [`game.tsv`](game.tsv) | the game plays by touch, which it reads only in handheld mode: see [Touch](#touch) |
| `pad_layout touch` | `game.tsv` | the iPhone's virtual gamepad starts in its Tap layout, every touch to the game |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NSP script's layout for a game with a system resource; the pool is the Switch's |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (the NSP script's heap check) |

No code of the game is cut, and no frame-rate key is needed: see [Frame rate](#frame-rate).

## Touch

- The game has its own touch controls and tutorial, which it shows once a touch has come in (the Joy-Con's hints are greyed beside them): draw a part from an orange anchor with a finger, tap a part to turn it into road and back, tap a part or joint twice to remove it; Test, Build, Drive and the materials are buttons on screen, as are the menus and dialogs.
- The game draws a dragged part's end about a tenth of the screen's height above the finger, so the finger does not hide it.
- The game reads the touch screen once a frame, at 30 fps. A `DM_CTL` `drag` of 0.5 s ended its part short of the point it aimed at (the last read came before the end); 2 s drags landed on it.
- On the Mac the mouse is the touch screen ([Input](../../../docs/development.md)), in handheld mode only, which `op_mode handheld` makes the default.

## Memory

- `nninitStartup` calls `StandardAllocator::Initialize(nullptr, 3.14 GB)` (returning to `main+0x1752830`, the pool less what is used), which takes alias +0.06 to +3.20 GB; Unity's own regions come after it.
- **Current:** with the iPhone's alias region (`DM_ALIAS_SIZE=0x118000000` on the Mac) the high-water mark stays at +3.21 GB of 4.375 GB from the title through the first level into the second, with 0.51 GB mapped; about 1.34 GB resident on the Mac.

## Files and shaders

- RomFS: 36 files, 0.19 GB, everything under `Data/` (4 UnityFS files, 0.06 GB).
- 5,137 shaders: 2,704 vertex, 2,412 pixel, 20 compute and one geometry shader. All built for the Mac with no failure, and no run met a shader missing from the map.
- The prewarm list (`pso_manifest.tsv`, 309 keys: 161 shaders and 148 render pipelines) was recorded from a run through the title, the office and the first level into the second (`DM_PSO_MANIFEST=off DM_PSO_RECORD`).

## Frame rate

- **Current:** 30 fps on an M5 Max (frame time p50 33.35 ms, p95 34.1 to 34.8 ms, 300-frame windows over about 6 minutes), GPU p50 1.9 to 2.8 ms, CPU about 32 to 48% of one core.
- The game asks for present interval 2 itself (`present interval set by the game 2`), as on the Switch. The host finds Unity's `vSyncCount` through the internal-call table ([Unity's vSyncCount and targetFrameRate](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table)), with quality levels of 0xa0 bytes: `[main+0x4058e58] +0x30 + [+0x4c] * 0xa0 + 0x68`; without `fps_target` it leaves the count as the game sets it. 60 fps was not tried.

## Audio

- One `AudioOut` (opened from `main+0x1752670`). Over 70,000 callbacks of 240 frames each: 0 underruns, 4 blocks queued at each callback.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app`.
- From a fresh save, by touch through `DM_CTL` (normalized picture points): Play on the title (`T0.21x0.47`), the office dialog's answers and OK buttons at the bottom (`T0.5x0.948`), GLaDOS's OK (`T0.6625x0.737`) and the tutorial's OK at the top (`T0.5x0.189`). In the first level: double taps remove the old parts, the first material button (`T0.029x0.448`) selects it, `drag` from the anchors and joints (2 s, the end a tenth of the height below the point) draws the road and the three supports, taps on the four road parts turn them into road, and Drive (`T0.554x0.948`) sends the vehicle across.

## Testing

- 2026-11-14, Mac (M5 Max, macOS 27.2.1), main `4258c22a` with this profile, the packed app (`Bridge Constructor Portal.app`, 0.7 GB, ad-hoc signed) from a fresh save in its default handheld mode, with the iPhone's alias region: the title with Portal Proficiency, the office dialog, the first level built and driven by touch alone ("Well Done!") and the second level's start, about 6 minutes; no red line from the game.
- 2026-11-14, Mac, main `b06f4e0e` with the provisional profile and the same two input keys: the same route, the options menu by touch, 30 fps, no red line.
- 2026-11-14, iPhone 17 Pro, iOS 27.2: played by hand by touch through the third level, from main `b06f4e0e` with the same keys and tables (no prewarm list yet).
- Not tested: levels past the third, Portal Proficiency, Convoy, controllers, 60 fps, two-finger gestures on their own, and a long session's memory.
