# Cozy Land (`0100777022394000`)

Base game 1.0.0 (v0), SDK 17.5.8. Unity 2023.2.20f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (the movie module: the RedDeer.Games splash and the title's background videos, H.264 with AAC, demuxed by the native `movie::Extractor` from Unity's own stream reader) and `sdk`.

On the Mac: the RedDeer.Games splash movie, the title menu over its background video, New Creation and the builder (the item lists of each category, placing houses, shops and decorations, zooming and reshaping the plot), at 60 fps, no red line; 2.2–2.6 GB resident and 1.2–1.5 GB of Metal allocations over 7 minutes of building. On an iPhone (iPhone19,4, A20 Pro, iOS 27.3 beta): played by hand; a short launch check logged 60 fps with a 0.96 GB footprint and no red line.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`: the SDK-wide rule (`MATCH_CUTS`) of the SDK 15.3.2 to 18.3.5 constructor, which the `sdk` build `71b1a025…` has at Ace Attorney Investigations Collection's SDK 17.5.3 offsets (`sdk+0x139458`, the movs at `+0x139468` and `+0x1394fc`) | `StandardAllocator::Initialize` aborted in `nninitStartup`; see [Memory](#memory) |
| H.264 pictures handed out as soon as they are next in presentation order | host, `hle_movie_avc.c` / `hle_movie.c` (every game) | the splash movie stopped on its last frame; see [Movies](#movies) |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | `game.tsv` | Monster Train 2's and Ace Attorney's layout; the NSP script's heap check set the 1 GB pool |
| `pad_layout full` | `game.tsv` | the D-pad zooms and reshapes the plot; see [Input](#input) |
| `shader_groups /Data/=data` | `game.tsv` | everything is under `Data/`: the player's serialized files and the Addressables bundles in `StreamingAssets/aa` |

## Memory

- The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true: `nninitStartup` (`main+0x907af0`) hands application pool − used memory (2 MB aligned) to StandardAllocator as virtual address memory and never calls `SetMemoryHeapSize`.
- **Resolved: an `nn::diag` abort at `sdk+0x12b1a0` from `StandardAllocator::Initialize` (`main+0x907b5c`) before `nnMain`**; the NSP script's heap check reported "the host stopped before nnMain (exit 134)". The `sdk` build has Ace Attorney Investigations Collection's AddressRegionManager constructor at the same offsets and takes the same node area cut ([The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area)). The game was brought up with a `CUTS` row of that build carrying the two movs; the SDK-wide rule for that implementation gives the same cut, and also the 2 MB alignment of regions of 4 MB and more, which the bring-up did not have (not yet run with the rules alone). With the cut the heap check found the hardware pool's region taking the alias region to +4.26 GB and set a 1 GB pool.
- Measured on the Mac over 7 minutes of building: 2.0 GB mapped (more than the 1 GB pool, which the host does not enforce, as for Ace Attorney), the alias region's high-water mark +3.75 GB of 4.375 GB, 2.2–2.6 GB resident.
- With the SDK-wide `sdk` rules alone (no `CUTS` row for this build) the rewrite matched both: the node area at `sdk+0x139468` / `+0x1394fc` (implementation 4 of 8) and the 2 MB alignment at `+0x139654` / `+0x13965c` / `+0x139660` (4 of 4).

## Movies

- The splash is a 4.3 s RedDeer.Games logo (High profile, I B B P, 29.97 fps, AAC) stored inside `Data/sharedassets0.resource`; the title menu plays background videos as well.
- **Resolved: the game stayed on a plain red screen (the splash's last frame) forever**, with frames still presented and no red line. `DM_MOVIE_TRACE=1`: the video decoder had its last four pictures queued and handed out none of them; the game had acquired four more input indices and sent nothing, not even an end-of-stream packet. Unity 2023.2's feeder (`main+0xf12020`) sends end of stream only when `GetSampleTime` still succeeds after the extractor's last sample, and the native `NuMediaExtractor::getSampleTime` returns `ERROR_END_OF_STREAM` (0x2000) there. The host's H.264 queue held pictures until more than four were queued (`depth`, the H.264 default reorder depth for a stream whose VUI has no bitstream restriction), so the last four never came, Unity's frame queue (`main+0xf0f9f4`, count at +0x2a0) never emptied, and its clock and the movie sound stopped. Tried: depth 0 (decode order) stalled too, the pictures out of order; depth 1 reached the title. The host now hands a picture out as soon as its time is one frame interval after the last one it handed out ([H.264 pictures leave as soon as they are next in presentation order](../../../docs/reference/movies.md#h264-pictures-leave-as-soon-as-they-are-next-in-presentation-order)).

## Input

- Controller: the left stick moves the cursor, A places, B goes back, Y opens the category's item list, L / R switch the category, the D-pad zooms (up / down) and changes the plot's shape (left / right) (the HUD also shows X for delete and ZL / ZR for turning, not tried). It stays docked at 1920x1080 with the 3D pad layout, which has a D-pad. Whether it reads touch has not been tried.

## Files and shaders

- 7,554 shaders in the player's serialized files and the 4 Addressables bundles under `Data/`, and the player's own GLSLC outputs in `main`. Unity 2023.2's `Shader` objects needed no layout written out.
- One compute shader does not build: `MSL 2.3 image atomics require IGN with a discarded result` (an image atomic whose result it uses); nothing on the route above dispatches it.

## Mac routes

- From a fresh save, the title menu comes up about 15 s after launch (after the splash); A on New Creation opens the builder. Then `R` / `Y` / `A` pick a category and an item and place it, `LLeft` / `LUp` move the cursor.

## Found on this game

General changes first needed here; each applies to every game:

- **H.264 pictures handed out when they are next in presentation order** (`mavc_ready`, `md_pump`): [H.264 pictures leave as soon as they are next in presentation order](../../../docs/reference/movies.md#h264-pictures-leave-as-soon-as-they-are-next-in-presentation-order).

## Open

- The compute shader with a used image-atomic result does not build.
- The prewarm list (`pso_manifest.tsv`) is empty.
- The other games with H.264 movies (Super Mario Odyssey, Monster Train 2, Hollow Knight: Silksong, Absolum, Monster Hunter Rise) were not run again after the movie change.
