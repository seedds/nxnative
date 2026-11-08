# Hollow Knight: Silksong (`010013C00E930000`)

Update 1.0.30000 (v655360) over the base game, SDK 20.5.8. Unity 6000.0.50f1 IL2CPP with FMOD (one AudioOut) on NVN. Modules `main`, `subsdk0` (the SDK's movie library, whose `movie::Extractor` demuxes the 27 MP4 cinematics, H.264 and AAC; `Intro_Cinematic.mp4` after a new file) and `sdk`. Plays on the iPhone; on a screen taller than 16:9 (an iPhone Duo) in 16:10 with *Picture shape* ([Screen shape](#screen-shape-dm_screen_aspect-current)). On the Mac: the logos, the title, a new file, the intro poem, the opening cinematic with its sound, and Hornet under control in the first two rooms of Moss Grotto (bind, jump, attack), with music and sound, at 60 fps as on the Switch (30 while a cinematic plays, as the game asks); memory flat in play. The base game alone (1.0.27776: another `main`, the same `sdk` and movie module) packs from a derived profile with its own 2,244 shaders and plays the same opening on the Mac with no red line.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `e01950c5…` (the constructor at `sdk+0x145bec`; the movs at `+0x145bfc` and `+0x145c68`) | Unity 6's address regions start after the node area: +3.00 GB of the alias region in the first room without it, +2.26 GB with it; see [Memory](#memory) |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout with the hardware pool: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the block of the other Unity games with virtual address memory (256 MB in front + 4.375 GB alias region + 256 MB stack region + 128 MB heap region); the pool is the hardware's (3.2 GB): Unity only reads it through `QueryMemoryInfo`, and nothing maps against it |
| `nn::hid::GetDebugPadStates` | host, `hle_hid.c` (every game) | an `nn::diag` abort in the first frames; see [Services](#services) |
| `shader_groups /Data/=data` | `game.tsv` | everything is under `Data/`: the player's serialized files and the `StreamingAssets` bundles |
| No `pad_layout` (the 2D default) | `game.tsv` | A binds, B jumps, Y attacks; see [Input](#input) and [Open](#open) |
| `screen_aspect on` and Unity's screen apply and size getter stubbed (1.0.30000 `main`) | `game.tsv`, [`patches.py`](patches.py) | *Picture shape* (`DM_SCREEN_ASPECT`): a 16:10 picture on a screen taller than 16:9, such as the iPhone Duo; see [Screen shape](#screen-shape-dm_screen_aspect-current) |

## Frame rate

- 60 fps, as on the Switch. While a cinematic plays the game draws 1280x720 with a window crop and asks for present interval 2 (30 fps).

## Screen shape (`DM_SCREEN_ASPECT`, current)

With *Picture shape* taller than 16:9 (`DM_SCREEN_ASPECT=1.43`, the iPhone Duo's; `auto` on a Duo resolves to it) the game renders 16:10: 1728x1080 docked, 1152x720 handheld, the same height with more of the scene above and below, the HUD at its own corners and the menus laid out in that shape. On a 1.43:1 screen that leaves thin bars above and below in Fit (about 5 % each instead of about 10 % at 16:9), and Fill crops much less of the sides. It also draws about a tenth fewer pixels than 16:9.

- Unity's Switch player renders into its 1920x1080 window textures and crops them to the size it applies: the size getter `main+0x124e224` (1280x720 handheld, 1920x1080 docked by `nn::oe::GetOperationMode`) gives the start-up size, and the apply `main+0x129074c` (this, width, height) takes the game's own `Screen.SetResolution(1920, 1080)` from its settings (`main+0x253cad0`), clamped to the window textures. Both store at +0xb4 / +0xb8 of the screen manager, which the renderer's window crop (`main+0x1271594`) and `Screen.width` / `height` read. Giving the getter a taller height did nothing (the apply clamps it to the 1080 textures), so the stubs keep the height and take the width from the shape.
- The game's camera (`ForceCameraAspect`, with `AutoScaleViewport` and a main camera height factor) fills any shape from 16:9 to 16:10 with more of the scene, as on a Steam Deck. At 1544x1080 (1.43:1) it letterboxes to 16:10 and draws a 16:9 view inside that, with white, yellow and blue bars between them (Mac). So the stubs ([`patches.py`](patches.py), each running the original code when `DM_SCREEN_ASPECT` is 16:9 or unset) turn a 16:9 size into the screen's width / height, at most 16:10, even: the apply's request and the getter's answer. The host shows a crop of that shape in its own shape ([A crop of the screen's shape is shown in its own shape](../../../docs/reference/rendering.md#a-crop-of-the-screens-shape-is-shown-in-its-own-shape)).
- Checked on the Mac (M1 Pro, 1.0.30000) at 1.43:1 with the route under [Mac routes](#mac-routes): the profile menu, the poem and the opening cinematic (the movie fills the 16:10 frame unstretched) and the first room, docked (1728x1080; 1152x720 while the cinematic plays) and handheld (1152x720); `DM_SCREEN_ASPECT=0` keeps 1920x1080 and 1280x720 as before. Not yet run on a device.
- Only build 1.0.30000's `main` has the stubs; another version packs at 16:9.

## Memory

- 16 MB of system resource (virtual address memory) and no heap: Unity 6's `nninitStartup` (`main+0x118cb70`) only calls `QueryMemoryInfo` and `SetMemoryAllocatorForThreadLocal`, so it sets up no StandardAllocator, and nothing aborts at startup as with 2022.3. Unity's own allocator reserves address regions with `nn::os::AllocateAddressRegion` and commits pages with `AllocateMemoryPages` in the alias region.
- **Resolved: the provisional profile's block was far too small.** `main` imports no `SetMemoryHeapSize`, and the provisional profile took that for a game living in its bss (a 1 GB block, 256 MB of alias region) while Unity maps 1.5 GB in the first room. `profile_guess.py` now gives every game with an NPDM system resource the 5 GB block (Absolum and Pokémon Legends: Z-A, heapless too, came out right with it).
- **Current:** the node area still sits at the start of the alias region, so it takes the cut: without it the high-water mark was +1.00 GB at `nninitStartup` and +3.00 GB in the first room of play; with it +2.26 GB, 1.51 GB mapped.
- Measured on the Mac: resident about 2.1–2.15 GB and 1.25 GB of GPU memory, flat for 100 s of play.
- BALL x PIT ([010086A022444000](../010086A022444000/README.md), Unity 6000.0.62f1, SDK 20.5.19) takes its memory the same way and needed only the same cut on its `sdk` build.

## Services

- **Resolved: an `nn::diag` abort with Result 0x2aca in the first frames of `nnMain`** (`bt: sdk+0x514580 main+0x1296c64 …`). `main+0x1296c60` calls `hid::GetDebugPadStates(states, 16)` every frame, and the SDK's own reads a debug pad resource nothing initialized (module 202, hid). The host had only the single-state `GetDebugPadState`; the history form now answers every requested entry as not connected, newest first with consecutive sampling numbers (`hle_hid.c`).

## Input

- A binds, B jumps, Y attacks (the game's own prompts name A and B). It stays docked at 1920x1080 with the 2D pad layout. Whether it reads touch in handheld mode has not been tried.

## Files and shaders

- Nearly everything is in 2,068 Addressables bundles (`StreamingAssets/aa/Switch`).
- 2,450 stage shaders: 1,189 vertex, 1,193 pixel, 67 compute and one geometry shader (which does not build), 11 of them the player's own GLSLC outputs in `main`.
- The bundles carry their type trees (they report the engine version `0.0.0`, Unity's stripped version string), so `unityfs.py` read Unity 6's `Shader` objects with no layout written out.

## Mac routes

- From a fresh save, `DM_KEYS="f1600:A,f1900:A,f2200:A,f2500:A,f2800:A,f9400:A~1.5,f9700:LLeft~60,f9800:B~0.5*60/50,f9830:Y*60/50"` starts a new file in slot 1, plays the poem and the opening cinematic (control at about frame 9,200), binds, and walks left into the second room (its "B Jump" prompt), jumping and attacking as it goes.

## Found on this game

General host changes first needed here; each applies to every game:

- **`hid::GetDebugPadStates`**, the history form (`hle_hid.c`): [History reads fill every requested entry](../../../docs/reference/services.md#history-reads-fill-every-requested-entry).
- **Unity 6 memory with no StandardAllocator and no heap**, and the NSP script's 5 GB block for every game with an NPDM system resource (`profile_guess.py`): [Unity 6 takes its memory with no StandardAllocator and no heap](../../../docs/reference/memory.md#unity-6-takes-its-memory-with-no-standardallocator-and-no-heap), [dm/games/README.md, Adding a game](../README.md#adding-a-game).
- **Unity 6 `Shader` objects** read from the bundles' own type trees: [Unity 6 Shader objects](../../../docs/reference/shaders.md#unity-6-shader-objects).
- **A crop of the screen's shape kept in its own shape** under *Picture shape* (`nvn_mtl.m` `present_shape`): [A crop of the screen's shape is shown in its own shape](../../../docs/reference/rendering.md#a-crop-of-the-screens-shape-is-shown-in-its-own-shape).

## Open

- The pad layout disagrees: the profile has no `pad_layout` (the default `simple`, 2D) and the notes say 2D, but README.md's touch-controls paragraph does not list this game among those that start in 2D.
- Whether it reads touch in handheld mode has not been tried.
- The one geometry shader does not build.
