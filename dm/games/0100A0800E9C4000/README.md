# The Witcher 3: Wild Hunt — Complete Edition (`0100A0800E9C4000`)

Update 4.04b (v458752), SDK 15.3.0, on NVN (29,673 shaders). CD PROJEKT RED's REDengine 3 in Saber Interactive's Switch port. Modules `main` and `sdk`. The game keeps its shaders in REDengine's own shader caches, tessellates its terrain, and asks for an alias region of 48 GiB.

On the Mac (2026-11-13, M1 Max, macOS 27.2, `main` at 48dca7a4 with #2430, a window, to frame 8,400): the title, a save loaded from the main menu, and White Orchard (road, hillside, the windmill in the distance) with the HUD and the minimap, at 60 fps (frame time p50 16.67 ms, p95 16.68 ms; GPU p50 10.5 ms, p95 11.9 ms), footprint 2.6 GB, no crash; the terrain and the water drawn by `main`'s tessellator (quads, `fractional_even`, from frame 3,835) with no `tessellated draw not replayed` line. Its `[RED]` lines: the partial library's two (no stamp, no `c1_full.tsv` for it), `svc GetInfo: unknown info type 10 (answered 0)` once at start, and the terrain material copy's integer target ([Both](#on-ios)). The run used the 603 shaders a recorded session drew (a partial library, `dmsh_build.py` with their keys), not the NSP script's full build. Earlier runs with this PR's original tessellation commit, on `main` at 34608c72 and ca26ef65, gave the same (GPU p50 11.8 and 10.6 ms). Earlier, on a fork with the same game support: a 19-minute session through the Kaer Morhen bedroom and White Orchard at 30 fps.

On the Mac (2026-11-18, M1 Max, macOS 27.3, Xcode 27.2, Metal 32023.922, `c25d4011` with the reader and profile, chain `ed6b4dc38d3c`), both full `--platform mac` builds compiled all 29,673 shaders. The earlier table had 713 hand-written pairs in addition to the 24,617 generated pairs. With just the generated pairs, the Metal source, libraries and vertex padding were identical. Both builds loaded the same White Orchard save and drew the same terrain and water through frame 8,100 at 60 fps (frame time p50 16.67 ms, p95 16.68 ms, GPU p50 14.1 ms with the extra pairs and 14.3 ms with the generated pairs). [Melaylou's comparison](https://github.com/seedds/nxnative/pull/2494#issuecomment-6983864880)

On an iPad Pro 13-inch (M4, 16 GB, iPadOS 27.2) and an iPhone Air (2026-11-12, the NSP script's `--platform ios --romfs separate` IPA, signed with the extended virtual addressing entitlement, on `main` at 34608c72 with the three PRs' commits; [On iOS](#on-ios)): the same save in White Orchard. The iPad held 60 fps (frame time p50 16.67 ms, p95 16.76 ms, none dropped; GPU p50 12.3 ms, footprint 2.8 GB, thermal state nominal) for 2¼ minutes of play. The iPhone runs it, but not smoothly ([On iOS](#on-ios)).

## What it needs

| Setting | Where | Why |
|---|---|---|
| A 50 GiB alias region: `mem_alias 0xC80000000`, `mem_reserve 0xCA8000000` | `game.tsv` (iOS; the Mac gives every game at least 64 GiB) | A `DT_INIT_ARRAY` constructor reserves 48 GiB at `main+0x89e8`, and later allocations use fixed offsets up to alias +48.97 GiB, so no smaller alias works. It is a virtual-address reservation; only touched pages are committed |
| The extended virtual addressing entitlement (iOS) | the IPA's signature: `DM_ENTITLEMENTS` for `build_ipa.sh` ([On iOS](#on-ios)) | without it iOS gives an app about 6 GB of address space and the 50 GiB reservation fails at start |
| REDengine 3 shader cache readers | `shader_split.py` (#2430) | Reading the material and static caches alongside the loose shaders brings the split from 563 to 29,673 shaders ([REDengine 3 shader caches](../../../docs/reference/shaders.md#redengine-3-shader-caches)) |
| SeparateSamplerId samplers | `dmsh_reflect.py`, `nvn_mtl.m` (on `main`, `ca26ef65`) | 245 of the 602 shaders a session drew have a type-2 sampler, without which they do not load and the screen stays black ([A separate sampler ID in a uniform buffer](../../../docs/reference/shaders.md#a-separate-sampler-id-in-a-uniform-buffer-handle-type-2)) |
| Tessellated draws | `nvn_tess.inc`, `dmsh_tess.py` (on `main`, `48dca7a4`) | The terrain and the water are quad patches (7 hull and 13 domain shaders, `fractional_even`); without it they are skipped and White Orchard has no ground ([Tessellation stages drawn without a Metal tessellator](../../../docs/reference/shaders.md#tessellation-stages-drawn-without-a-metal-tessellator)) |
| `fps_target 60`, `present_interval display` | [`game.tsv`](game.tsv) | 60 fps (30 on the Switch); the player's **Frame rate** picks 30 or 60 ([Frame rate](#frame-rate)) |
| `depth_mode zero` | `game.tsv` | The game sets near = 0 depth (`nvnDeviceSetDepthMode`). The offline pipeline archive must use that depth mode to serve the game's pipelines at startup |
| `shader_facts_manual.tsv` | the profile | the image formats of compute shader `11130a61f4a6f3ec` and the texture types of `a52ac3530463868c` and `eacf9338c2835b9e` |
| `pso_manifest.tsv` | the profile | 1,031 prewarm rows (588 shaders, 434 render pipelines, 9 compute) from two Mac sessions in White Orchard |
| `modules main sdk`, the memory rows, `shader_groups` | `game.tsv` | the provisional profile's guesses, kept |

This 4.04b profile has no `patches.py`. For region `0100BFE00E9CA000` 3.7 (v327680), copy [the histogram repair](histogram_37.py) to `<build folder>/profile/0100BFE00E9CA000/patches.py` in its provisional profile, then rerun the NSP build. The rewriter applies the repair to main build `4ffb62f1cd9e17f87e4e019731bfb0c4`.

## Frame rate

- **Current: 60 fps** (30 on the Switch), 30 with the player's **Frame rate** (`DM_FPS_TARGET=30`). The game asks for present interval 2 once, at window creation, and has no other limit: with `present_interval display` the host logs `present cap 60 fps (game profile fps_target) applied by the present interval alone`, and White Orchard ran at 16.67 ms frames (above). With `DM_FPS_TARGET=30` the cap is 30 (`present cap 30 fps (DM_FPS_TARGET)`): 33.33 ms frames (p95 33.4 ms), none dropped.
- **Speed:** its logic runs on real time (2026-11-05, on the fork: 60.0 fps at the same game-clock speed as 30).
- While the save loads, frames take about 29 ms (p95 30 ms, one of 150 ms as the world appears); once in White Orchard they are back at 16.67 ms.

## Rendering

- **720p.** Docked and handheld both render 1280×720: `bin/config/platform/nx/rendering.ini` sets `Resolution="1280x720"` and `ForcedRendererResolution*`, with no docked variant, and the size is also fixed in code. The dynamic resolution only lowers it when the GPU is late. A larger render size needs code patches (a separate proposal).
- **Handheld** (`DM_OPMODE=0`, checked on the fork): the game switches to its larger handheld HUD and draws a little less scene detail (118 render passes in a White Orchard frame against 132 docked).
- **Open: exposure validation** ([#3048](https://github.com/seedds/nxnative/issues/3048)). The reporter's other region, `01003D100E9C6000` 4.04d, has a concurrent histogram clear and fill and exposure stuck at maximum. The reporter's `DM_NVN_DISPATCH_BARRIER=full` A/B corrected the picture at the same 60 fps on M2 Pro. This 4.04b profile contains the same five exposure shaders, and the histogram reducer hit its loop cap in [two of four runs](https://github.com/seedds/nxnative/pull/2494#issuecomment-6983864880). Both regions still need a test of the clear-to-fill boundary alone ([finding](../../../docs/reference/rendering.md#compute-histogram-clear-races-its-atomic-fill)).
- **3.7 comparison.** In region `0100BFE00E9CA000`, live startup traced clear (`265d0ac1fad2877a`), fill (`0cac2751729c3f1c`) and reduce (`e32b3a98a677d10b`) sharing SSBO 0, 1,024 bytes, from the NVN dispatch call at `main+0x2101164`. There was no barrier between clear and fill in the same concurrent encoder. The reducer walks 256 bins until the count exceeds a percentile of the expected pixel count. Metal 4 replay on M6 lost counts in 24 of 800 concurrent runs with a fixed 640×360 input. The worst histogram held 168,364 of 230,400 counts, below the controlled test's 90% threshold. An execution-only clear-to-fill barrier preserved every count in 400 runs. A barrier that also flushes to device memory preserved every count in 800 runs, and the original reducer returned finite luminance (1.249832). The connection to the 4.04b reducer's loop cap is open.
- **3.7 repair checked locally.** The game calls its compute wrapper for the fill at `main+0x1c50ef4` and records barrier `0x21` after it at `main+0x1c50fcc`. The rewriter patch makes the same NVN barrier call just before the fill, preserves the fill's arguments and return address, and checks the original instructions at both call sites. Startup replay showed clear, barrier, fill, barrier, reduce, with all 24,336 samples on the M6. The 4.04 build's corresponding fill site is still open.
- **Open: terrain material mip copy** (vertex `ff4b69b477ff89e6`, fragment `2dcecea911db30ef`). The older iOS build refused a `float4` output into `R16Uint`, and the Mac logged an integer-target warning. The November 18 Mac build prepared the pipeline with the typed-output side library and the warning was gone. The route reached other draws using the same fragment shader but never sampled the `R16Uint` texture, so the texture read and resulting mip values still need a capture.

## Not checked

- Other regions (Velen, Novigrad, Skellige), combat, cutscenes in the engine, long free play.
- Clean pipeline prewarm and gameplay on the rebased reader and profile, and the typed-output path on iOS.

## On iOS

- **Build:** the entitlements file names both memory entitlements; the IPA keeps the game files out (they are about 35 GB):

  ```sh
  cat > extended_va.entitlements <<'EOF'
  <?xml version="1.0" encoding="UTF-8"?>
  <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
  <plist version="1.0"><dict>
  <key>com.apple.developer.kernel.increased-memory-limit</key><true/>
  <key>com.apple.developer.kernel.extended-virtual-addressing</key><true/>
  </dict></plist>
  EOF
  DM_ENTITLEMENTS=$PWD/extended_va.entitlements python3 scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform ios --romfs separate --gpu none -o TheWitcher3.ipa
  ```

  Keep both entitlements when re-signing. A re-signing tool may append the team ID to the bundle ID (`org.nxnative.0100a0800e9c4000.<team>`): `scripts/push_romfs.sh` then needs the installed one (`xcrun devicectl device info apps`).
- **Memory:** both devices granted the 50 GiB region (`alias region 50.00 GB`; the game's 48 GB region at alias +0.06 GB). Footprint 2.7–2.9 GB, no memory warning.
- **iPad Pro (M4):** 60 fps throughout (above).
- **iPhone Air:** 60 fps while cool (GPU 9.5–11.5 ms), then about 40 fps once iOS reports thermal state serious (2¼ minutes in; GPU 19–21 ms): the host's 30 fps hold for a hot iPhone ([A thermally serious iPhone holds 30 fps](../../../docs/reference/frame-rate.md#a-thermally-serious-iphone-holds-30-fps)) applies to frame-step games only, and this game is paced by its present interval. With the Frame rate at 30 it falls to 20 fps after a heavier moment (casting a sign, among others) and stays there until a light screen such as the map: the same frame (about 800 draws) takes 39 ms of GPU instead of 13, with the device at thermal state fair. That is the GPU clock settling at its floor while the game presents every third tick ([A probe's grace lasts while its frames keep getting cheaper](../../../docs/reference/frame-rate.md#a-probes-grace-lasts-while-its-frames-keep-getting-cheaper), whose fix is the frame step's too). Open: both need the present-interval pacing to follow the thermal state and to recover the clock.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game. The RomFS image is about 35 GB.
- From the title to the newest save, the presses the run sent (`DM_CTL`, a few seconds apart): `A`, `B`, `LDown` twice, `A`, `A`, `A`.

## Found on this game

- Host fix awaiting in-game verification: thin strokes disappeared from UI text on 4.04d. The same shader is present in 4.04b → [The reserved sampler at pool slot zero](../../../docs/reference/nvn-objects.md#the-reserved-sampler-at-pool-slot-zero).
- Open: sunlit surfaces stay white after NaN enters eye adaptation. The 3.7 trace and shader replay confirm the clear-to-fill race. A single-barrier test in the reported 4.04d scene is pending → [Compute histogram clear races its atomic fill](../../../docs/reference/rendering.md#compute-histogram-clear-races-its-atomic-fill).
- Resolved: the split found 563 shaders, none of them a material, lighting, hull, domain or compute shader → [REDengine 3 shader caches](../../../docs/reference/shaders.md#redengine-3-shader-caches).
- Resolved: a black screen; 245 of the 602 shaders a session drew did not load (`B binding fp_t_cb720898_2000005ASmplr has no resolved slot number`) → [A separate sampler ID in a uniform buffer (handle type 2)](../../../docs/reference/shaders.md#a-separate-sampler-id-in-a-uniform-buffer-handle-type-2).
- Resolved: no ground in White Orchard, `primitive type 14 not implemented` → [Tessellation stages drawn without a Metal tessellator](../../../docs/reference/shaders.md#tessellation-stages-drawn-without-a-metal-tessellator).

Otherwise it runs on mechanisms found on other games, including SDK 15.3's NVN and services.
