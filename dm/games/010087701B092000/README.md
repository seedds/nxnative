# Persona 5 Tactica (`010087701B092000`)

Update 1.1.0 (`v262144`) over the Asian base release. Unity 2021.3.23f1 IL2CPP with NVN, CRIWARE and FMOD. Loads `main`, `subsdk0` (NintendoSDK_movie) and `sdk` (16.2.0, the same builds of both Nintendo modules as [Sonic Superstars](../01008F701C074000/README.md)).

## Memory

The NPDM declares 16 MiB of system resource. The game uses virtual address memory without setting a process heap. The existing SDK-wide node-area and alignment rules apply to this SDK.

With the Mac's 64 GiB alias region, `nn::mem::StandardAllocator::Initialize` (returning to `main+0xe2dc60`) reserves 3.12 GiB and maps 0.41 GiB during the first 15 seconds of `nnMain`. Unity's 188 separate regions reserve another 1.43 GiB, extending the layout to +5.06 GiB. With `DM_ALIAS_SIZE=0x118000000` (the iPhone profile's 4.375 GiB limit), Unity runs out of address space at startup. Its allocator returns a null base for a 16 MiB block, then the store at `main+0xa28db0` writes the block's header at base + 16 MiB − 0x20 and faults at `0xffffe0`.

The profile gives StandardAllocator 1.5 GiB through the existing SDK-symbol gate and keeps the hardware application pool. This accommodates Unity's reservations within the iPhone layout. On the Mac with the same alias limit, the capped startup probe reached +3.46 GiB and the live regions at the end of the prologue run reached +3.77 GiB. See [the allocator cap and application pool](../../../docs/reference/memory.md#the-heap-checks-1-gb-pool-picked-a-games-low-graphics).

## Files and shaders

- The selected update contains 7,591 UnityFS bundles, plus serialized Unity assets and CRI audio and movie data. The four DLC NSPs supply indices 1, 2, 3 and 5. Index 1 has a RomFS and the other three carry licenses.
- The shader split finds 44,277 keys (19,608 vertex, 19,612 pixel, 5,056 compute and one geometry). Unity assets supply 44,266 keys and the player's `main` supplies 11 GLSLC keys. The game sets depth mode to zero.
- All 44,277 shaders built for macOS and iOS with Xcode 27.2's Metal compiler (`32023.922`). The prewarm list covers startup, the title screen and the opening story scenes.
- The rewriter leaves Unity's bucket allocator alone. Its `ALLOC_BUCKET` string is copied into an object instead of passed in the register expected by the existing matcher. The startup failure above was resolved with the StandardAllocator cap.

## Frame rate

The game starts at a present interval of two vblanks (30 fps). `fps_target 60` uses the [shared Unity frame-rate support](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table). The rewriter finds `Application.targetFrameRate` and `QualitySettings.vSyncCount` from Unity's internal calls. The host changes the active quality level's count from two to one and applies the corresponding 60 fps present cap. The only title-specific setting is the target in [`game.tsv`](game.tsv).

On the M6 Mac mini with the iPhone alias limit, the same prologue route measured 33.34 ms p50 / 33.60 ms p95 at the original rate and 16.67 ms p50 / 16.91 ms p95 with the profile default. The opening subtitles reached the same cues at 12, 18 and 24 seconds after confirming the name, and both runs reached the office scene by 36 seconds. These checkpoints cover the prologue before the first tactical battle.

The iOS launcher's Frame rate setting offers 30 and 60 fps. On the Mac, `DM_FPS_TARGET=0` uses the game's original timing for comparison.

## Mac validation

At the original 30 fps, the game ran for six minutes on an Apple M6 Mac mini with the iPhone's alias limit. It rendered the intro movie, title, main and difficulty menus, software keyboard name entry, and the opening dialogue through the Café Leblanc scene. All four DLC indices mounted and the host opened a six-channel audio output. The last 300-frame dialogue window measured 33.34 ms p50 and 33.57 ms p95. A second boot prewarmed all 346 recorded entries with no missing shader keys.

## Running

Point [`scripts/build_ipa_from_nsp.py`](../../../scripts/build_ipa_from_nsp.py) at the NSP folder and the local keys file. Use `--platform mac` for the Mac host and its run command. Add `DM_METAL_HUD=1` to that command to show the performance HUD. The default iOS output is the unsigned `Persona5Tactica.ipa`.

The 1.1.0 IPA is 7.16 GiB with the base and update RomFS, the iOS shader image and all four DLCs. Its executable targets arm64 and requires iOS 26.0 or later.

On a Mac keyboard, use J for A, the arrow keys to select a name field, and Return for Plus to confirm the names. The name fields use the host's keyboard dialog. Controllers use the standard host mapping.

## Open

- Verify the first tactical battle, later stages and battle save/load on the Mac.
- Verify the unsigned IPA on an iPhone after sideloading.
