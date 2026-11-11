# Minecraft Switch 2 Edition (`04008A90274EC000`)

Version 1.26.50 (standalone update v262144), SDK 20.5.14 (Ounce). The supplied NUP marks the update as a complete game and contains no base package. Mac gameplay is confirmed by the user; iOS has not been built or tested.

## Mac gameplay (current)

On November 11, 2026, following the direct-relay rewrite, the user confirmed that the world loads and the game runs well on the M3 Pro Mac, reporting **56–60 FPS**. This is the user's gameplay report; scene, settings and test duration were not recorded. Sound was also confirmed by the user. The session log contains ongoing rendering and audio callbacks and ends with host exit code 0. No game inputs were sent by the agent during this validation.

The user also confirmed local character creation: after creating a character in the dressing room, Steve renders correctly and is marked as the current character (November 11 screenshot). The earlier spinners in empty character slots did not establish a local character-loading defect. Online account/content access remains outside the host's implementation.

## Profile

`game.tsv` registers main build `71e679a4d77a93032dbecf11823656c0`, SDK build `dfb937c3d3e764f8393c5ce38dd276bf` and the excluded NintendoSDK_Vulkan module. Only main and sdk load: the host supplies Vulkan. The NPDM declares 16 MB of system resource. The memory partition is the static provisional guess, not measured capacity validation.

## Files and shaders

The merged RomFS has 13,232 files. RenderDragon stores length-prefixed SPIR-V inside 154 `renderer/materials/*.material.bin` containers. All 14,293 shader blocks pass instruction-boundary checks; they contain 1,817 different modules. The Mac Vulkan library builds all 1,817 material modules, including 44 compute shaders, plus 11 additional built-in modules captured during initialization (1,828 total). `vk_capture.tsv` supplements the static material containers through the existing capture step. The NVN tables are intentionally empty.

Main resolves `vkGetInstanceProcAddr` through `nn::ro::LookupSymbol`, rather than importing Vulkan functions directly. NROs: `MediaDecoders.NN.nro` and `libPlayFabMultiplayer.nro`; collection registers both and the host maps them for loading on demand.

## Mac bring-up

- Main text ends at `0x10541bf0` (261.3 MiB). Its required x18 rewrites extend into the gap between the two external patch areas. The rewriter first tries two direct branches through verified, unreferenced zero alignment between unwind functions; only edges without an available relay use a marked BRK, whose host handler preserves registers (BL updates LR). Main and both NROs rewrite successfully. Known host exports are also available through dynamic module lookup.
- Boot reaches `nnMain`, creates three cache storages and mounts RomFS. `GetCacheStorageMax` must use host backing-volume capacity rather than the SDK's absent fsp object.
- Server environment selection is local process state; entering nsd:a/nsd:u IPC here aborted before renderer initialization.
- Dynamic Vulkan lookup requires known host HLE exports. The next observed missing entry points were the promoted Vulkan 1.1 format queries; wrappers delegate their base properties to the existing format implementations.
- Resolved world-loading stall: samples of the previous app showed heavy far-branch signal traffic in world workers (100 of 103 samples under `_sigtramp`), with BRK at `main+0x163155fc` returning to `main+0xd9bfd28`. The new rewrite uses direct alignment relays; the user subsequently confirmed in-game operation above.

## Technical verification

- The complete Mac app packages successfully; signing is ad hoc for local use.
- All 1,828 shaders compile, including the 44 compute shaders. The extra capture table contains only the 11 modules absent from the material containers.
- Native Vulkan GPU tests: 35 checks pass with Metal API validation, covering dynamic rendering, blend changes, scaled/mirrored blits, multisample resolve, synchronization2 submits, query copies, direct/indirect compute, separate compute descriptor/push state and integer vertex fetch with signed/unsigned 8/16/32-bit source formats.
- The SDK 20.5 event overload of `OpenOutputStream` now uses the host's AudioOut backend; the default microphone query follows its existing no-input-device behavior. All main's `nn::audio` imports are covered. Native stream tests verify interleaved samples, capacity, release events, purge and both event overloads. Test frames consume 100 ms through the real Mac AudioUnit and 101 ms through the silent sink. On November 11 the user confirmed sound in the packaged game; its log reports a real two-channel 48 kHz output and no underruns in the first 8,000 callbacks.
- Rewriter boundary tests, material-parser tests and related Python suites pass (217 tests including documentation and integer-input translation). Another 75 rewriter/matcher tests pass; four optional checks are skipped. Native B/BL trap tests, filesystem tests (148 checks) and NRO lifecycle tests pass.
- Main now resolves all 39,338 formerly trapped edges through 39,295 direct relay words, with no remaining `FAR_BRANCH` entries. Every emitted source-to-relay and relay-to-target instruction was decoded and checked against its recorded endpoints, including BL placement; each relay also passes the original-image padding/reference checks. The sampled hot edge is now `0x163155fc → 0xf4d4dfc → 0xd9bfd28`. Metadata remains consumable by the iOS embedding generator (tested with both a fixture and the actual rewritten modules); no IPA was built.
- LLVM 23.1.2 clang-tidy on the changed host/Vulkan sources has no finding above the existing baseline. Targeted pyright on the relay/embedding changes has no finding above baseline; the new helper/tests pass ruff. The earlier whole Python lint run still reports an unchanged unused `dmsh_fetch` import in `dmsh_entry.py`; it was not repeated for this focused change.
- Cross-game comparison runs were not performed: no other local game builds were configured for this port. iOS was not built, signed, installed or tested.

## Open

- The observed integer-input pipeline rejection (`18cf018eba675096` / `9f92700c0a7ee2da`) is addressed by the general shader/host fetch bridge, with zero/sign extension verified by GPU readback. Exact equivalence of the mismatched input on Switch hardware remains unverified; detailed visual comparison has not been performed.
- Independently validate memory capacity, world save/reload persistence and extended play. Gameplay and sound have the user confirmation above; the remaining validation is outside that report. iOS and cross-game comparison runs remain untested.

## Found on this game

- [Required sites between two external patch areas](../../../docs/reference/rewriter.md#required-sites-between-two-external-patch-areas).
- [RenderDragon SPIR-V containers](../../../docs/reference/shaders.md#renderdragon-spir-v-absent-from-the-shader-scan) and [multi-target dual-source variants](../../../docs/reference/shaders.md#a-dual-source-variant-made-from-a-multi-target-shader).
- [Dynamic Vulkan lookup](../../../docs/reference/services.md#dynamic-vulkan-entry-point-resolves-to-null), [cache capacity](../../../docs/reference/services.md#cache-capacity-query-enters-an-absent-fsp-object), and [server environment selection](../../../docs/reference/services.md#server-environment-selection-enters-nsd-ipc).

- [Modern Vulkan commands](../../../docs/reference/rendering.md#modern-vulkan-commands-resolved-to-null) and [ordered query copies](../../../docs/reference/rendering.md#query-results-copied-before-their-cpu-snapshot-was-available).
- [Integer vertex fetch](../../../docs/reference/rendering.md#integer-vertex-input-rejected-for-format-signedness) and [SDK 20.5 output-stream overload](../../../docs/reference/audio.md#outputstream-event-overload-without-flags-sdk-205).
