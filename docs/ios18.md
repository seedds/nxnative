# Experimental iOS 18 / Metal 3 support

This experimental build adds an opt-in NVN replay backend for A17 Pro and newer devices with native BC texture support. Device testing used the original implementation based on `228ddb4089aceb02f41a6e0fe1c5b1abd63b3f60`; the upstream proposal is rebased on `e0cf2ac7a7b468a70f10243587b960d300bb4f5e` and requires fresh device validation with that revision's shader chain. It does not change the default supported platform baseline.

## Build for iOS 18

Use the normal dependencies and game-folder setup in [the README](../README.md#how-to-play), with Python 3.12 or newer. Xcode 26.0 was used for the original device build; Xcode 27.2 remains the default toolchain. The alternative compiler requires explicit opt-in:

```sh
DM_METAL3=1 MACOS_MIN=15.0 python3 scripts/build_ipa_from_nsp.py \
  /path/to/game-folder -k /path/to/prod.keys \
  --allow-other-metal --no-prebake --romfs separate
```

Add `--prepare-xcode` to prepare an Xcode project instead. Keep the generated resources and profile together; this does not bypass shader-chain or input-stamp checks. The output is an unsigned IPA. JIT activation is not required; see the normal signing instructions for increased-memory-limit and extended-virtual-addressing entitlements.

## Backend selection and upstream compatibility

`DM_METAL3=1` selects `dm/ios/project-metal3.yml`, which includes the original project and changes the deployment target to 18.0 and enables the compile-time macro. The host Makefile also passes the macro and builds its embedded helper shaders for iOS 18. Without the variable, the original project, iOS 26 deployment target and Metal 4 path remain selected. This is a build-time choice, not runtime automatic fallback.

The adapter in `dm/gfx/nvn/nvn_metal3_compat.inc` implements the queue, command-buffer, encoder and argument-table operations using classic Metal. GPU addresses resolve to registered buffers and offsets; native BC textures remain compressed. Argument-buffer resources are declared to classic Metal, buffers/textures/heaps use tracked hazards, and resource snapshots keep indirect allocations alive until command-buffer completion. Presentation is scheduled asynchronously after the producing command buffer is scheduled; the active path does not wait for GPU completion on the CPU.

Classic MetalFX spatial and temporal scalers replace their Metal 4 equivalents. Optional Metal 4 compiler/archive and timestamp paths are disabled in the experimental build. Placement sparse mapping uses the existing whole-texture fallback; GPU-written sparse aliases remain unverified. The shader translator and chain lock are unchanged. Xcode 26 is an explicitly permitted alternative shader compiler, not the upstream pinned toolchain.

Resource-ID reads use a shared inline helper; registry work inside it is compiled only for Metal 3. SDK guards keep newer motion and compile APIs available when compiling with their original newer SDK. No original project configuration was deleted. The Metal 4 runtime has not been regression-tested on this macOS 15 development host.

## External shader libraries

`--romfs separate` externalizes RomFS, but the builder still bundles shader libraries. For the diagnostic App (the normal build here, without `DM_PLAYER_BUILD=1`), shaders can also be externalized before signing:

1. Build the App normally and retain the complete `DmIOS.app/msl` directory.
2. Copy that directory into the App's `Documents/msl` on the device, preserving all filenames and subdirectories.
3. Add `DM_MSL_DIR=msl` to `Documents/env.txt`, preserving any other settings. The App changes its working directory to Documents before starting the host.
4. Remove `msl` from the App bundle before signing and packaging the smaller IPA. Do not modify an already signed bundle without signing it again.

The device layout is:

```text
Documents/
  romfs.img
  env.txt
  msl/
    b-<shader-chain>.img
```

A build using unpacked shader files has a directory under `msl` instead of the image; copy the whole original layout. Host chain and profile inputs must match the shader resources. Rebuilding in Xcode stages bundled shaders again, so repeat the packaging step if external shaders are desired. This is an optional packaging procedure, not a changed builder default.

File Sharing is enabled. Finder, Files, or `xcrun devicectl device copy to --domain-type appDataContainer` can transfer files after installation. Do not request removal of existing container contents: saves live there too. Replace resources only while the App is stopped and transfers are complete. Updating game executable code or host code still requires rebuilding and signing.

## Device findings

The initial iPad mini 7 / iPadOS 18.6.2 run failed at `CAMetalLayer.residencySet`. The selector is unavailable on that OS. A no-op classic queue method did not help because Objective-C evaluates its arguments first. The complete layer residency registration is now compiled only for the Metal 4 backend. Classic command buffers retain their drawable attachments.

After that fix, LLDB located another startup exception inside Apple's `libMTLHud`: `CAMetalDisplayLinkUpdate_drawable` called `Overlay::Overlay(CAMetalLayer*)`, which sent `device` to a `CAMetalDisplayLinkUpdate` object. Launch environment variables disabling HUD did not remove the interception. Disable **Settings → Developer → Show Graphics HUD** on the device. A diagnostic launch with `DM_PRESENT_LINK=0` avoided the failing combination and reached frame 900 in 20 seconds. Its final SIGALRM was the requested watchdog. No permanent display-pacing override was applied; this is a known platform diagnostic limitation, not evidence that normal pacing is slower or incorrect.

The tester reported disabling the system HUD and resumed testing. A new controlled run with default pacing, sustained frame-time measurements, and full gameplay coverage remain outstanding.

## Validation record

- Xcode 26.0 / macOS 15.7.9: all 263 iOS C/Objective-C translation units compiled. The compile checker suppresses warnings and cannot establish runtime API availability.
- Full synthetic iOS App built and linked, including Swift and staged resources. Synthetic guest modules are not playable games.
- Kirby's Return to Dream Land Deluxe 1.0.0 (`01006B601380E000`): all 47,099 shader records and generated variants compiled; the real App built with deployment target 18.0. Its profile is provisional and generated outside the repository; this fork does not add the game to the supported-games table.
- Physical iPad mini 7 / iPadOS 18.6.2: signed installation, external RomFS/shader transfer, and the bounded 900-frame diagnostic run described above. No JIT activation was required.
- Real Mac GPU adapter test: compute → blit → compute preserves pipeline, argument table and threadgroup memory; final result 42, intermediate copy 41. Creating a diagnostic queue leaves the execution queue unchanged.
- Present crop / MetalFX test: 117 checks passed with Metal validation on M1 Pro. It uses a null front-end layer, so the device present path requires its own integration check.
- Full NVN synthetic suite: 323 of 325 checks passed. Two failures remain in unchanged CPU command recording (compact state size and compact draw record bytes); no runnable Metal 4 baseline was available on this host. Do not interpret this as a fully green regression suite.
- Five-game regression coverage is unavailable locally: only the user's Kirby dump was supplied. No claim is made about other upstream games, Vulkan/OpenGL games on iOS 18, or Switch 2 games on iOS 18.

Remaining risks include conservative resource declarations and their performance cost, sparse aliases, sustained device memory pressure, and this provisional game's font-engine table limit. The default Metal 4 build still needs testing with upstream's toolchain and runtime.

## Publishing checks

The documentation checks (6 tests) and iOS packaging/workflow checks (52 tests) pass with Python 3.12. The real GPU transition test also passes with `MTL_DEBUG_LAYER=1`. The first packaging run used an older system Python and failed six fixture variants; rerunning with the documented Python version passed.

The multi-game regression harness could not run: no `REGRESS_ROOT` / `REGRESS_GAMES` harness and baseline are configured, and only one game dump is available. C lint database generation was attempted; clang-tidy is unavailable on this host, so C lint is not a pass. Python lint reports zero new findings (318 current versus 321 baseline); three pre-existing missing-import findings disappear because this build venv includes backports.zstd, so the unrelated baseline was left unchanged. These limits are separate from successful compilation and the device startup test.

The standalone adapter test can be run on an Apple silicon Mac with Xcode 26:

```sh
clang -fobjc-arc -mmacosx-version-min=15.0 \
  -framework Foundation -framework Metal -framework QuartzCore \
  dm/gfx/nvn/tests/t_metal3_transition.m -o /tmp/t_metal3_transition
MTL_DEBUG_LAYER=1 /tmp/t_metal3_transition
```

## Adapter provenance

The adapter began with [Llllll07/nxnative-ios18-check](https://github.com/Llllll07/nxnative-ios18-check), branch `bayonetta-metal3-performance`, commit `44e7da0` (MIT). This fork adapts that work to the current-main replay interfaces and fixes encoder transitions, queue stability, resource registration, barriers and asynchronous presentation. The older game-specific [d2r-mobile-ios17](https://github.com/4ppp44/d2r-mobile-ios17) and [ot2-mobile](https://github.com/leeashine/ot2-mobile) were useful compatibility references; their frozen host/backend trees were not substituted for current nxnative.

## Kirby save-slot transition

On iPad mini 7 (A17 Pro), iPadOS 18.6.2, Kirby v1.0.0 entered the main mode's first save slot and aborted in the Metal 3 adapter. The log reports `Metal3 unresolved GPU buffer address`, and a live debugger reproduced it on `dm-encode` in the vertex stage of a four-vertex draw. This was not a filesystem abort or a jetsam termination. See [the retired argument-table binding fix](reference/rendering.md#metal-3-resolves-retired-argument-table-slots) for the general mechanism.

After the fix, the user confirmed passing the same transition and continued playing, reporting 60 fps gameplay and normal audio. The supplied title-screen screenshot shows 60 fps; the world-map screenshot shows 22 fps, so these observations do not establish a sustained 60 fps minimum. A subsequent log reached frame 1800 with no repeated unresolved-buffer error or crash; recent 300-frame windows reported no dropped frames, p99 16.67 ms, and about 3389 MiB footprint. This is a short observation, not a controlled performance comparison or full-game validation. The save directory was backed up before updating the app; no save reset or shader rebuild was needed.

The binding regression test runs with the same compiler flags as `t_metal3_transition.m`, substituting `t_metal3_bindings.m`. Normal execution must pass GPU validation and readback. Running with `--used-missing` must abort with `Metal3 unresolved GPU buffer address`: required resources must never silently become null bindings. Five-game regression coverage and clang-tidy remain unavailable as described above.

## Upstream proposal validation

On the proposal rebased onto `e0cf2ac7`, all 263 iOS C/Objective-C/assembly sources compile with Xcode 26 and `DM_METAL3=1`; the 52 iOS workflow tests, 6 documentation tests, and both standalone adapter GPU tests pass. Python lint has zero new findings. C compilation database generation succeeds (309 sources), but clang-tidy is not installed. The default `dm/ios/project.yml`, shader-chain header and lock are unchanged against that base. This host cannot run the required Xcode 27.2 / Metal 4 validation. Device evidence above belongs to the tested fork revision, not a rebuilt game on the proposal's newer shader chain.
