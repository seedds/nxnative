# The Elder Scrolls V: Skyrim (`01000A10041EA000`)

Experimental bring-up of base 1.0.12.517175 (SDK 3.4.0), Creation Engine on NVN. Mac validation passes the cart intro, character creation and on-foot movement during the Helgen dragon attack. Sound, camera input, checkpoint loading, quicksave creation and quicksave reload work. An initial eight-minute run and subsequent opening-gameplay test use no debugger probes or preemption overrides. The user confirmed gameplay and the Helgen Keep lighting correction on an iPhone 17 with iOS 27.2. That phone build uses the original conversion host with the same build-gated game cuts; the rebased host has Mac runtime and iOS compile validation. Longer sessions and later regions remain unvalidated.

## Builds and resources

Main `b3f6e5da5c501857d754e3e186e04dc2`, SDK `d6afd2cb180ca36e58ec21a12cf27eec`. The modules are `main sdk`; the RomFS is 14.28 GiB in 32 files. The archive set includes English voices and Dawnguard, HearthFires, Dragonborn and Hyrule content.

## Address-space adaptations (experimental)

The game reserves 2 GiB for its Default Heap, 2 GiB for the GPU Heap, 600 MiB for file data, 128 MiB for UI, 32 MiB for audio, a 1 GiB common small-block pool and 64 MiB per-thread ScrapHeaps. These virtual reservations do not fit the host's bounded alias region, independently of physical-memory usage. A failed region leaves a heap uninitialized and later unchecked allocations crash.

The profile uses the supported 6 GiB guest reservation / 5.375 GiB alias range. This SDK build takes 64 MiB address-region node space and 2 MiB alignment through build-gated SDK cuts.

Three build-gated main cuts deliberately reduce virtual growth ceilings: Default Heap and GPU Heap each 2 GiB -> 1.5 GiB, common small-block pool 1 GiB -> 256 MiB. Their initial commits remain 85 MiB, 1 GiB and 4 MiB respectively; the 600 MiB file heap and original 64 MiB per-thread limits are retained. Startup now loads assets and presents its first frame without the allocator crashes. Actual high-water usage and gameplay capacity remain unvalidated.

A 32 MiB common pool exhausted its capacity during startup. Reducing ScrapHeaps to 16 MiB still exhausted virtual address space and constrained individual allocation admission, so that experiment was removed in favor of reducing the two large virtual ceilings.

## Shaders

`Data/Skyrim - Shaders.bsa` (BSA 105) holds `shadersfx/shaders011.fxp`. Its NVN records use a 16-byte offset/length header, 0x848-byte controls and code areas; some compute stages have no SPH header. The archive yields 23,080 unique keys. Main embeds another 53 records, including unaligned 0x7d1-byte control sections. Total: 23,133 unique keys (3,468 vertex / 19,645 fragment / 20 compute).

All 23,133 records compile for Mac and iOS. Single-stage covering pairs provide vertex-output linkage. The streamed BSA reader skips texture/audio payloads and supports 103/104 zlib and 105 LZ4 frame compression, compression-toggle flags and embedded names.

## Graphics completion

After the first frame the render thread waits in `main+0xb5e808` for a timestamp report's counter field to become zero. The host wrote the timestamp into both fields unless conditional rendering was enabled. This leaves the game polling a permanently nonzero counter. The general completion path now writes zero to the counter field and time to the timestamp field, independent of the conditional-rendering profile key. Earlier runs stalled during loading or the cart scene after all submitted GPU work completed. A global game mutex at `main+0x2aea1c0` was free while a worker still waited on its adjacent LightEvent. A native replay reproduces the cause: the plain unlock STR can remain buffered while SignalLightEvent takes its already-signalled fast path, losing the wake. All 517 matching inlined unlocks are now corrected by build-gated release-store cuts. The corrected, unprobed eight-minute run reached character creation and over 14,100 frames, about 30 fps, with `DM_PREEMPT=0`.

## Indoor stencil initialization

The first Helgen Keep interior sharply alternated between bright and dark while standing still. This is [raw depth/stencil storage written as a buffer](../../../docs/reference/rendering.md#raw-depthstencil-storage-written-as-a-buffer), rather than torch animation or exposure adaptation.

Scene-init mode 7 in `main+0xb551c0` clears depth, then dispatches `a76fe13a8630d619` over the same allocation: storage buffer 0 is exactly the D24S8 texture's address and storage size (1600x900, 6,553,600 bytes in the tested scene). The shader writes a repeated stencil pattern whose upper three bits are zero. Those buffer writes do not update the separate Metal stencil plane. Scaleform clears that plane to 255 at the end of a frame; next frame the shadow-mask shader `f83614ab6954bec4` reads shadow class `255 >> 5 = 7`, selecting unused secondary-shadow data.

A build-gated cut at `main+0xb55250` changes `mov w3,wzr` to `neg w3,w21`. Mode 7 has `w21=1`, so its existing NVN depth clear also initializes stencil to zero; mode 1 has `w21=0`, preserving ordinary depth-only clears. This is a deliberate platform adaptation at the scene initializer, not general coherence between raw buffers and Metal depth textures. The original compute dispatch, UI rendering and shadow shaders remain intact.

All eight translated variants fetching this stencil slot extract the upper three bits. The original lower-five-bit patterned initialization still does not transfer to the Metal texture; reproducing the Tegra raw layout and broader raw depth/stencil aliasing remain open. In an isolated copy of the user's interior checkpoint, the native stencil clear removed the large frame-to-frame brightness jumps across 16 adjacent frames (mean luminance 45.0 to 43.4, changing smoothly). The subsequent full 180-second route reached frame 5,273 at 29.5 fps (p50 33.33 ms, p99 42.25 ms), with graceful shutdown and the known second-audio-renderer warning. Current-chain static checks pass all 23,133 Skyrim and 12,851 Arceus shaders with unchanged MSL and interfaces. After installing the same scene-initializer cut in the original phone conversion, the user confirmed that the lighting correction works on the iPhone. This confirms the game cut on-device, not a runtime test of the rebased native host.

## Audio and remaining validation

This SDK's renderer Update locks `nn::os::MutexType` at +0x1f8 and exports no `SdkMutexType::Lock`. Resolve required lock symbols after selecting the actual layout. The implementation's system member is also recovered from its own first mode call: +0x50 on this SDK, instead of the previous hardcoded +0x60. The legacy first command has both size and ID20, which falsely passed the modern header walk. Validating its enabled boolean selects the older header correctly, and the existing SDK4 DeviceSink layout captures nonzero stereo PCM. Concurrent renderer instances still produce a warning and need further validation. The renderer also reports `stage 1 cb11 has no uniform buffer bound`; its significance outside the tested Helgen scene remains open.

## Validation

Final source qualification compares upstream `c8ab816` with candidate `7cf9742`, on chain `157e1b96c8da`. All 23,133 Skyrim shaders compile for Mac and iOS; the harness compares Skyrim and 12,851 Arceus shaders with no new failures, changed MSL or interface errors. The 62-file Python suite passed before the final upstream refresh, followed by 67 focused cases covering its changed tools and docs. Host unit tests, host/memory ThreadSanitizer, the full Metal graphics suite and GPU completion ThreadSanitizer pass. The iOS source check compiles 178 files with zero failures; pinned Python lint has no new findings. Breath of the Wild, Tears of the Kingdom, Scarlet and Hades are reached by the impact scan but unavailable for local checks; Arceus has no runtime route here.

The final 180-second interior checkpoint run reaches 5,292 frames at 29.9 fps (p50 33.43 ms, p99 45.31 ms), peaks at 2,179 MB and exits gracefully on request; its screenshot was reviewed. This run removes `DM_WINDOW` at the harness's process-launch boundary and uses `DM_PRESENT_LINK=0`, avoiding Mac window activation. It validates offscreen rendering, not windowed presentation or iPhone frame times. The unsupported upstream baseline stops at the SDK's Break during heap initialization (exit 42), before a frame. The full report is marked REGRESSION solely for the known second-audio-renderer warning first reached by the candidate; that limitation remains open.

Ten synthetic BSA/FXP tests pass, including an embedded record with unaligned code, bounded file dispatch for large archives, and a BSA member inside a packed RomFS image. NVN tests pass 181 checks, including short controls ending at a protected page, copies surviving caller unmapping, completed TIMESTAMP / TIMESTAMP_TOP reports, and control-memory grant accounting. The old-SDK audio lock regression passes. The native event-order regression measured 793 lost wakes in 20 million original STR iterations, and zero in 20 million each with STLR and STLUR. All 537 unlock cuts were checked in the rewritten module and embedded iPhone Mach-O; the final phone update also passed all 541 main-profile byte checks, including the three heap adaptations and stencil initializer. An eight-minute Mac run reached character creation with sound and camera input, without a crash or stall; its graceful timeout is expected. A second unprobed 670-second test loaded the checkpoint, finished character naming, reached the dragon attack, walked and turned with controller inputs, opened the quest journal, wrote a new quicksave and reloaded it successfully. Over 19,200 frames presented; the test exited gracefully on request. Mac opening gameplay passes, and the user reports working iPhone gameplay with the indoor lighting correction. Later regions and longer sessions remain unvalidated.

## Mac test controls

Use A holds of about 0.5 seconds for title-menu confirmation; brief presses can fall between frames while loading. Character creation uses Y to finish, then A to confirm. Naming opens the normal native keyboard dialog and deliberately pauses the calling game thread. For unattended local tests, the existing `DM_KBD=Prisoner` override supplies a name; keep that override out of packaged app settings. A progress watchdog can time out while a human-input dialog is waiting, which is distinct from the earlier guest lock stall.

## iPhone touch controls

The profile selects `pad_layout full` (3D): dragging an empty area in the right third of the screen controls the invisible right stick and turns the camera. The layout button at the top center switches between 2D and 3D, and the app remembers the choice. If a previous install saved 2D, switch to 3D to enable camera dragging; changing the profile default respects existing preferences.

## Found on this game

- [BSA archives and FXP shader records](../../../docs/reference/shaders.md#bsa-archives-and-fxp-shader-records).
- [Diagnostic snapshots of short shader controls](../../../docs/reference/shaders.md#diagnostic-snapshots-of-short-shader-controls).
- [The renderer's update lock](../../../docs/reference/audio.md#the-renderers-update-lock-update_lock).
- [Timestamp report completion counter](../../../docs/reference/rendering.md#timestamp-report-completion-counter).
- [Raw depth/stencil storage written as a buffer](../../../docs/reference/rendering.md#raw-depthstencil-storage-written-as-a-buffer).
- [Audio renderer system offset](../../../docs/reference/audio.md#audio-renderer-system-offset-inside-its-implementation).
- [Control-memory grant queries](../../../docs/reference/nvn-objects.md#control-memory-grant-queries).
- [Plain mutex unlock before an auto-clear event fast path](../../../docs/reference/threads.md#plain-mutex-unlock-before-an-auto-clear-event-fast-path).

## Unlock-order cut coverage

`unlock_release.tsv` has 537 byte-checked cuts for 517 inlined unlocks. Zero-offset stores use STLR. Small nonzero offsets and four proven event-minus-four addresses use STLUR (FEAT_LRCPC2, present on the tested M1 Max and target A19). Sixteen post-indexed forms use a separate event-address calculation only after CFG analysis proves the updated base dead until restoration. Three post-indexed x0 forms call a 12-byte tail wrapper in original zero text padding at 0x112f7c0; the loader and iOS embedding include that executable page. Original post-call DMB instructions remain. No preemption setting is needed by this correction.

Regenerate/check with `dm/tools/check_skyrim_unlocks.py <original main> [--write dm/games/01000A10041EA000/unlock_release.tsv]`. The tool refuses other build IDs. Native litmus tests reproduce the original lost wake and check both release-store forms. A first single-lock correction advanced to frame5,444 before hitting the same defect in a second inlined copy; the table covers the engine-wide pattern.
