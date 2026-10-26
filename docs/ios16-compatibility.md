# iPadOS 16 compatibility checkpoint

This branch carries the cumulative traditional Metal backend port and iPadOS 16.1 build changes. It is based on upstream commit `ebe75c9d5d34486f714678f850f0e74bbab398ac`. It needs the user's own game inputs; none are part of this branch.

## Rendering and memory

The traditional Metal backend replaces newer Metal interfaces, explicitly declares resources reached through argument buffers, indexes buffer addresses, and decodes unsupported BC formats. Embedded guest code stays in the executable's original signed text mapping while its writable data hole is populated separately. `dm/ios/tools/set_text_maxprot.py` updates the unsigned executable's maximum text protection before external signing.

GPU-written indirect records are sanitized on the GPU, in batches, without a mid-frame CPU wait. A draw's base instance is copied into its vertex driver constants at c0+0x644. Each command occurrence owns its constants; later draws cannot rewrite an earlier draw's SSBO table, and CPU initialization preserves the GPU-owned word even when encoding continues after a split commit. Returning to a direct draw invalidates the indirect vertex binding.

Guest pool aliases are not fully represented by Metal resource object identity. Render, compute and blit replay passes therefore use an explicit queue-local fence. NVN Barrier ends an active pass and invalidates argument state. Without real barriers, the device hit GPU Hang in a single-texture-sample 7x4 postprocess pass, then rejected later submissions.

Pipeline topology must match the actual draw. The former point-size workaround declared Point topology for triangle draws; Metal API validation rejects that combination. When an older compiler rejects an unused vertex point-size output for a non-point draw, the backend now compiles a separate vertex function whose point-size output is an unused user varying. It retains the original depth-range entry, argument layout, point function and transform-feedback function. At most two source compilations run concurrently.

Combined depth/stencil allocations declare PixelFormatView usage before they become attachments. iOS 16 validation requires this for the stencil-only X32 view, including views selected implicitly by depth/stencil mode rather than an explicit format change.

The non-point fallback needs supplemental copies of the user's original translated MSL. Before packing an IPA, export them into the iOS library folder; the normal IPA and Xcode resource staging already copy this nested directory:

```sh
python3 dm/ios/tools/export_nonpoint_sources.py \
  "$CACHE/b-$CHAIN" "$IOS_LIBS/b-$CHAIN" \
  "$IOS_LIBS/b-$CHAIN/nonpoint-v1"
```

`CHAIN` must match `dm/gfx/nvn/dmsh_chain.h`. The export does not change the translation chain or base libraries. Its manifest records the source and original iOS library SHA-256 values, both verified before runtime compilation. These game-derived sources belong beside the user's game inputs, never in Git.

Image-atomic companions also need image-coordinate bounds and array-layer addressing. The original linear companion calculated a raw pointer from projected coordinates without validating them, and its 2D macro ignored the layer of a 2D array. Shader Validation captured `atomic_fetch_max` past a 2,228,224-byte buffer immediately after entering the station. Enlarging the allocation cannot make arbitrary off-image coordinates safe. The legacy variant guards the operation before evaluating its pointer, returns zero for out-of-image coordinates, and includes the layer in the linear offset. The host copies every array plane into the private texture after the producer. Export these variants before packing as well:

```sh
python3 dm/ios/tools/export_legacy_atomics.py \
  "$CACHE/b-$CHAIN" "$IOS_LIBS/b-$CHAIN" \
  "$IOS_LIBS/b-$CHAIN/atomic-v1"
```

The export records the original source hash, bounded source hash and original library hash. The backend requires a matching chain, variant kind, bounded source and base library before compiling the companion. It does not modify the base chain libraries.

## Input and timing

Keyboard movement uses WASD; arrow keys turn the camera. J/K/U/I map to A/B/X/Y. Virtual face-button positions are A bottom, B right, X left and Y top, with hit testing following the same logical positions. The touch overlay hides while a physical controller, keyboard or mouse is connected and returns after disconnection. No visible right stick was added.

Mouse and trackpad camera input accepts GCMouse relative movement. UIKit hover and trackpad scroll gestures provide fallbacks, with recent raw input taking precedence over duplicate hover events. A single timer releases camera input after the last movement, including during touch tracking.

The FPS overlay counts actual drawable presentations, independently of attempted presents used for screenshot scheduling. A missing drawable or discarded presentation no longer increases the displayed frame counter.

For Pokemon Legends: Z-A, use `DM_PRESENT_INTERVAL=game`. The game sets interval 2; the former `display` override ignored it and is not a correct-speed 60 fps patch. This checkpoint follows the game's timing instead.

## Validation and limits

Local device package v48 initially passed 8,000 frames and camera rotation, but the longer run failed with GPU Hang at frame 65,655. That checkpoint was insufficient evidence of stability. Metal API validation then identified the invalid point/triangle topology combination; after correcting that, it identified the missing stencil-view usage flag.

On 2026-10-25, v50 ran with both Metal API Validation and Metal GPU Validation enabled, loaded the station save, and continued for 5,160 frames without API assertions or command-buffer failures in log.txt. Fifty-seven non-point vertex variants compiled successfully. This was not a clean shader-validation result: Shader Validation writes to the system log by default and zero-fills invalid reads. Its reports were not yet being collected. Validation substantially reduces performance; its frame rate is not representative of normal play.

Normal-speed v51 failed at frame 4,068 after horizontal rotation and upward camera input. The first fault was a 60x33 sampling pass; a later address fault and SubmissionsIgnored were consequences, not independent first causes. The API fixes are necessary but insufficient. Source review then found that raw sampler IDs had no submission-owned strong references when descriptor re-registration released the table's reference. V52 retains all encoded sampler states until GPU completion and records selected sampling bindings in memory, emitting them with the associated command buffer's error. V52 still failed during the same camera sequence, and disabling split submissions did not fix it. No sampler replacements were observed in that reproduction, so sampler retirement was not established as its cause.

Enabling Shader Validation's abort-on-fault stopped the app immediately after loading the station and exposed an MTLGPUDebugBufferErrorLog, confirming that the zero-fill mode had hidden a buffer error. V53 reads the public MTLCommandBuffer.logs container on completion, including function debug locations, rather than relying on system-log forwarding. It captured invalid atomic_fetch_max writes from the image-atomic pass, including offsets 2,231,976 through 2,589,172 past a 2,228,224-byte companion buffer. V54 installs the guarded, array-aware companion described above. It synchronizes all three 480x270 array planes and passed more than 2,552 frames with API and shader validation, including camera rotation, without validation reports. A separate normal-speed run reached about 15,720 frames without a GPU error, then crashed with CPU SIGBUS in the first store of SDK memset (sdk+0x56e3ac). This is not a stability pass.

The subsequent dirty-page lifetime audit found that protection could hold a range descriptor across concurrent pool retirement. Retirement restored RW and published live=0, then the old protection call could set the now-untracked memory read-only again. The deterministic t_dirty_retire_race control reproduced an EFAULT writing into the retired pool. V55 holds the pool lifecycle mutex across protection, preventing both retirement and slot reuse until the final mprotect has landed. The regression and existing shared-page/write-preparation tests pass. Terminal CPU fault diagnostics now include ESR, si_code, VM protection and matching live/retired dirty slots. The observed V54 CPU crash has not been conclusively tied to this race.

V56 retains the lifetime fix and registers the optional fault diagnostic through a callback so crash.c still links independently. The terminal diagnostic was checked with an intentional read-only write fault; it reported WnR=1 and VM protection READ. A normal-speed iPad station run reached 31,500 frames (about 17.5 minutes), including 443 controller actions: title confirmation, 80 four-direction camera cycles, and a further 20 camera/movement cycles. Intermediate screenshots confirmed actual camera and character response. No CPU crashes, command-buffer errors or shader reports appeared; the final footprint was 2,363 MiB. This covers this station reproduction, not every subsequent scene. The automatic input queue was cleared and DM_CTL removed from the next-launch environment afterward. These fixes are included in this branch and still require upstream review.

This is a limited real-device checkpoint, not a claim that every scene or other game is correct. Longer play, all scene assets, controller variants and regressions in other games still need testing. The broad backend port should be reviewed separately from the smaller input and synchronization fixes before upstream integration.

Checks passed: complete unsigned arm64 iOS Release build targeting iOS 16.1; targeted cross-compilation of rendering and input units; 10 present-pacer checks; 12 shader-chain consistency tests using Python 3.15. Upstream's build script syntax requires Python 3.12 or newer.

## A10X UI address fault checkpoint (2026-10-26)

The older A10X device needs the Tier 1 argument encoder, baseline LL/SC host atomics, verified per-slice layered companions and buffer-backed fragment zero-write companions. Reaching its language screen did not establish stability: the original UI submission failed at frame 55, after which rejected submissions caused flashing. Splitting only the selected UI draws localized the fault to four draws using the same vertex/fragment pair and nonzero UV flags.

An independent render probe reproduced the address fault with the captured constant, vertex, index and C1 bytes and freshly allocated resources. Changing only texture swizzle still failed. Setting only the vertex flags to zero completed with 54,720 nonclear pixels. Keeping the original flags and changing four dead index initializers to zero also completed all four previously failing draws, with readback coverage of 54,720, 6,480, 54,720 and 6,480 pixels. This supports a legacy compiler workaround for the inactive guarded-address path; the exact compiler instruction causing the fault has not been inspected.

`export_guarded_index_sources.py` proves a complete same-guard coordinate/index/load chain with closed uses. When the loads are active, both seeds are overwritten before use. When inactive, no seed-dependent memory access is semantically executed. The exporter recognizes token structure rather than shader keys or temporary IDs, rejects unproven shapes, changes only the reflected `entry_z`, and retains the original point and capture/TF functions verbatim. Fourteen exporter/regression tests pass, including 288 executed active/inactive semantic cases. Export after the verified non-point sources:

```sh
python3 dm/ios/tools/export_guarded_index_sources.py \
  "$IOS_LIBS/b-$CHAIN" "$IOS_LIBS/b-$CHAIN/guarded-index-v1"
```

The runtime uses these companions only below Apple4/Mac1, verifies the original source, transformed source, base library and reflection hashes, and retains the base function when no proof record exists. An invalid present record fails that non-point variant. The original shader chain and libraries remain unchanged.

The installed v72 guarded-index host passed a 25-second native-pacing startup beyond frame 479 and a separate 32-second dynamic-pacing startup beyond frame 900 without a GPU error or abort. Screenshots at 16 and 24 seconds showed the complete language screen. With `DM_DISPLAY_HZ=120`, `DM_PRESENT_HZ=120` and `DM_ZA_FPS_MODE=dynamic60`, measured display callbacks were approximately 119.97 Hz while actual drawable presentations were approximately 31–34 fps on this device. The steady game-clock window ratios were 0.999390 and 0.999752. This is an uncapped request within the display's supported rate, not evidence of 120 rendered fps.

A nonconfirming virtual ZL hold was observed in the game's actual GetNpad return, including press/release state changes. No language confirmation, character creation or battle input was automated. The game remains at language selection with clean requested settings. Nonzero fragment image writers and layered shaders without proofs still report unsupported, so these checks do not establish complete playability.

## Tier 1 sampler-limit companion (2026-10-26)

Metal Tier 1 argument buffers allow at most 16 indirect sampler resources in one function. SPIRV-Cross can nevertheless declare a sampler beside a texture that the shader accesses only with `read()`. On the A10X character profile screen, fragment shader `a6c06cda878a4fe0_b` declared 17 samplers and Metal rejected its pipeline with `Total number of indirect argument buffer resources exceeded for samplers (17/16)`. The geometry, hair, eyes and clothing still rendered, but the rejected transparent-material pass left the face dark.

`export_sampler_limit_sources.py` scans the complete source, strips comments and literals for identifier analysis, and removes only scalar sampler declarations whose identifiers occur nowhere outside their exact argument-structure declarations. It never removes a texture, changes a texture operation or renumbers an argument ID. Sampler arrays, macros, writable image companions, reflection disagreement and shaders without enough unused samplers fail closed. Export the companions beside the chain-matched iOS libraries:

```sh
python3 dm/ios/tools/export_sampler_limit_sources.py \
  "$CACHE/b-$CHAIN" "$IOS_LIBS/b-$CHAIN" \
  "$IOS_LIBS/b-$CHAIN/sampler-limit-v1"
```

The Tier 1 runtime verifies the chain, proof kind, source, base-library and reflection hashes, removes the same authorized binding from its private argument-encoder reflection, and compiles the companion with invariant preservation and fast math disabled. Tier 2 devices continue to load the original metallib. The profile-screen shader's unused sampler at argument ID 36 is reduced from 17 to 16 while all 32 `read()` calls through its texture remain unchanged. Eight focused exporter tests, the existing iOS compatibility tests, 21 related exporter tests, `git diff --check` and an unsigned A10 Release host build pass. This is source and package validation only; the unsigned build still requires user sideloading and real-device confirmation.
