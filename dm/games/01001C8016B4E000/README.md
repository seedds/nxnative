# FIFA 23 Legacy Edition (`01001C8016B4E000`)

EA proprietary engine, NVN, SDK 13.3.5. The profile registers base 1.0.0 (v0)
and update 1.0.3 (v196608); the supplied candidate runs update 1.0.3 with shaders
extracted from v0. `main` and `sdk` load; no game or SDK rewriter cuts are added.

## What it needs

The EA reader discovers EASF keys from the matching version's own ExeFS `main`,
verifies candidates against the archive's plaintext-prefix digest, and reads
BIG4/chunkzip/NXSHADER programs into the normal shader splitter and package writer.
No external EA key table or title-specific archive path is required.

The 5 GiB reservation leaves startup heap room with a 1.5 GiB alias region and
256 MiB stacks on iOS. The application pool is `0xCD500000`, system resource
reservation `0x800000`. These parameters were retained from Touchline23 and work
on the reported devices below; they are not a memory or compatibility guarantee
for every device. Audio downmix and general memory/resource loading use upstream.

## Builds and shader inputs

- Main 1.0.0: `da9da20e1649759ab2e3ebfbd6307c11`.
- Main 1.0.3: `4b713267a882da35b4789a5e45823ac4`.
- SDK: `43211771ed39e7fcef03d88e27091831`.
- Validated source candidate: `55d30a5d052c1d9ef50e8e540acedde2eba3520a`, based on
  upstream `d365496da3fac6133fa9654a60ac9afb4f63eb39`.
- Shader Chain: `337c3caedaa7`; profile inputs: `f3827e6db64f`.

A fresh upstream split finds 386 unique programs (187 vertex, 187 pixel, 12
compute), 12 compute-query rows and 187 single-stage cover pairs. These structural
tables were regenerated for upstream, not taken from old Touchline23 libraries.
The cover supplies pixel inputs through the normal pipeline; historical runtime
pairs and old PSO seeds are not imported. Mac and unsigned iOS builds completed.
No generated shader instructions, control sections, constants or libraries ship.

## Validation

The contributor reports approximately 30 minutes of stable manual testing of the
final candidate on all three devices. These are user-observed results, not an
automated benchmark, frame-time trace or independently captured FPS measurement.
Exact per-device duration, OS version and installed-binary hashes were not supplied.
The build identity above describes the candidate supplied in this contribution's
local validation session; it is not a cryptographic attestation of each installation.

| Platform | Device | Contributor-reported manual result |
| --- | --- | --- |
| iOS | iPhone Air | Stable 60 FPS |
| iOS | iPad Air M2 | Stable 60 FPS |
| macOS | MacBook Air M1 | Stable 60 FPS |

The agent separately checked startup, menus, Manchester United/Liverpool kit
previews and Old Trafford's opening scene with default pool chunks enabled after
the compressed-copy fix. Skin, kits, pitch, stadium, crowd and advertising rendered
normally. The build machine was Apple M1, macOS 27.0.1, Xcode 27.2; that OS version
must not be substituted for the unreported OS versions of the contributor's tests.

To reach the same scene on the Mac: Home -> Kick Off -> select a local player ->
Ready -> Single Match -> Manchester United/Liverpool -> kits -> Play Match. The
loading drill's Plus prompt enters the match. No online account is required.

Open: match-end/terminal cutscene coverage, independently listened-to commentary,
other-device coverage and multi-game regression runs. A pre-final diagnostic on
an older baseline crashed in an end-of-game cutscene; the final candidate was not
retested to the whistle, and the manual report does not establish that path.
The existing `t_image_alias` allocation-threshold failure under Shader Validation
also reproduces on unchanged upstream; it was not suppressed. Queue memory-counter
and subpixel-bias stubs and a cube-view fallback still report red lines.
No gameplay screenshots or private captures are included in this contribution.

## Found on this game

- [Application crash-report switches](../../../docs/reference/services.md#application-crash-report-switches-without-an-applet-proxy): avoid an SDK null read through an absent applet proxy.
- [Empty direct compute grids](../../../docs/reference/rendering.md#direct-compute-dispatches-with-an-empty-grid): zero work returns before Metal encoding; indirect dimensions remain GPU-owned.
- [Upload-only residency](../../../docs/reference/rendering.md#upload-only-residency-stays-with-its-upload-batch): bound staging lifetime to its upload batch; the independent upload-budget fixture reproduces the original failure.
- [Copies from registered views](../../../docs/reference/rendering.md#compatible-views-made-from-the-texture-under-a-view): compose source level/slice offsets from the resident root, including depth/stencil views.
- [Placed-view parents](../../../docs/reference/rendering.md#views-of-placed-textures-need-their-parent-resident-too): fragment sampling needs parent-texture as well as heap residency; queued heap retirement avoids debug-commit reentrancy.
- [Compressed IMAGE copy storage](../../../docs/reference/rendering.md#gpu-copies-into-compressed-image-textures): copy results must update the storage from which compressed IMAGE textures can later be uploaded. This restores the default-chunk path without a profile workaround; `t_compressed_copy` reproduces the cross-page loss.
