# FIFA 23 Legacy Edition (`01001C8016B4E000`)

EA proprietary engine, NVN, SDK 13.3.5. The profile registers base 1.0.0 (v0)
and update 1.0.3 (v196608), packages the update and extracts shaders from v0.
`main` and `sdk` load; no game or SDK rewriter cuts are added.

**Current integration loads a match on the Mac (user-tested).** The real archive
split finds all 386 expected keys and all 386 Mac shader libraries compile.
The reader, structural profile and crash-report API hooks are on current main.
The GPU fixes are limited to the requirements reproduced on current main:
compressed IMAGE copy storage, empty direct grids, nested view offsets and
placed-texture parent residency. The current owner, view cache, heap recycler
and queued residency commits are retained. The upload stress fixture passes
without the PR's separate upload-residency set.
Historical gameplay on the contributor's combined candidate does not validate
this integration.

## What it needs

BIG4 archives contain EASF-encrypted chunkzip archives and NXSHADER programs.
The format reader discovers keys from each version's own ExeFS `main` registry
and verifies them against the SHA-256 of the first 32 decrypted bytes. This is
prefix verification, not whole-file authentication. CommonCrypto supplies AES-CBC;
no additional Python crypto package or title-specific resource path is required.

The imported memory settings reserve 5 GiB with a 1.5 GiB alias region and
256 MiB of stacks, an application pool of `0xCD500000` and system resource
reservation of `0x800000`. The contributor reported a startup heap need of about
2.91 GiB. These settings still need validation on current main and on iOS.

## Crash-report call path (resolved)

SDK build `43211771ed39e7fcef03d88e27091831` was extracted from the update.
`DisableCrashReport` starts at `sdk+0x36ee30`: it calls
`GetApplicationFunctions` at +0xc, then `ldr x8, [x0]` at +0x10 faults when the
returned interface is null. The getter reads the application-functions slot at
+0x48 of the SDK's am context. The called service method at `sdk+0x2f844` masks
the argument to one bit and sends command `0x5a` (90). Hand-translated from this
instruction trace, the wrapper is `r = f->EnableApplicationCrashReport(false)`
and aborts if `r` fails. The host intentionally creates no am proxy when replacing
`oe::Initialize`; completing its HLE API coverage fixes that boundary. A full
applet IPC implementation would not add a crash-report service to macOS.

## Builds and shader inputs

- Main 1.0.0: `da9da20e1649759ab2e3ebfbd6307c11`.
- Main 1.0.3: `4b713267a882da35b4789a5e45823ac4`.
- SDK: `43211771ed39e7fcef03d88e27091831`.
- 386 unique shader keys: 187 vertex, 187 pixel, 12 compute.
- 187 single-stage cover pairs and 12 compute-query rows; all references and
  package indices validate. These tables contain structural metadata only.

## Validation

Current checks cover synthetic archive decryption without PyCryptodome, directory
and RomFS-image reads, base/update key separation, DLC extraction, package export,
compute facts, existing Unreal/RPF6 decryption and the host's oe calls. The real
Mac build succeeds with the expected shader keys and current-chain libraries.
The user confirmed that a match loads on Apple M6, macOS 27.2.1, Xcode 27.2
(27B52). A corrupted controller-button glyph was then reported; a synthetic
compressed IMAGE copy test reproduced stale guest storage (38 checks, one failure
on main), and the storage fix passes all 38 checks with API/Shader Validation
and deferred encoding off/on. The same-menu capture and the user's retest show the correct A/B button glyphs
with only that storage predicate changed. All 386 iOS shader libraries compile; the final unsigned IPA build succeeds (13.6 GB).
Match-end sequences and an iOS device run remain unverified. Startup also logs
the existing queue-memory usage query/reset and subpixel-precision-bias stubs;
these are unchanged and did not prevent the user's match-loading test.

Focused current-main GPU checks: empty direct grids pass; registered nested views
pass 48 checks; depth/stencil copies pass 20 checks in both encoder modes; upload
budget passes 261 checks in both modes without a batch-residency replacement.
The placed-texture sampling case passes. Its two allocation-threshold failures
also reproduce on untouched main (337 checks versus 340 with the new case),
with the same allocation delta; the thresholds are unchanged.

The two-game full regression runs cover The Binding of Isaac: Rebirth and
Catherine: Full Body, the lighter local routes for the NVN replay changes. Both
reach their frame targets with unchanged MSL/reflection/variants and no new red
categories. The raw reports are REGRESSION because their screenshots compare
random room/lighting states (Isaac) and different movie/scene progress (Catherine).
Identical-main controls reproduce both screenshot failures, with essentially
the same image differences. These are route limitations, not green screenshot
checks. Busy-machine samples do not establish a performance difference.

Reports: `pr1696-final-nvn`, `pr1696-isaac-control` and
`pr1696-catherine-control` under the harness reports folder.

C lint has no new findings. Python lint reports 22 diagnostics at an unchanged
packer Popen call; the same diagnostics reproduce in the original main source.
Archive, AES, package, chain and documentation tests pass (125 checks), and the
host's oe test passes all 75 checks.


Historical evidence from PR #1696: its combined candidate, based on upstream
`d365496d` with chain `337c3caedaa7`, was contributor-reported playable at 60 FPS
on a MacBook Air M1, iPhone Air and iPad Air M2. These are observations, not a
controlled performance comparison. Match-end scenes and commentary were not
independently verified. The current Mac and iOS libraries were freshly published with their generated
tables for chain `3dc8a0b295d9`; no old libraries were relabelled.

Mac verification route: Home -> Kick Off -> local player -> Ready ->
Single Match -> Manchester United/Liverpool -> kits -> Play Match. Plus at the
loading drill enters the match. Open a visible window with the Metal HUD for
the user to test this route.

## Found on this game

- [EA BIG4, EASF and NXSHADER archives](../../../docs/reference/shaders.md#ea-big4-easf-and-nxshader-archives).
- [Application crash-report switches](../../../docs/reference/services.md#application-crash-report-switches).
- [Compressed IMAGE copy storage](../../../docs/reference/rendering.md#gpu-copies-into-compressed-image-textures).
- [Empty direct compute grids](../../../docs/reference/rendering.md#direct-compute-dispatches-with-an-empty-grid).
- [Placed-texture parent residency](../../../docs/reference/rendering.md#views-of-placed-textures-need-their-parent-resident).
- [Nested views](../../../docs/reference/rendering.md#compatible-views-made-from-the-texture-under-a-view).
