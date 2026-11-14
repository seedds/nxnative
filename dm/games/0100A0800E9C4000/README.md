# The Witcher 3: Wild Hunt — Complete Edition (`0100A0800E9C4000`)

Update 4.04b (v458752), SDK 15.3.0, on NVN (29,673 shaders). CD PROJEKT RED's REDengine 3 in Saber Interactive's Switch port. Modules `main` and `sdk`. The game keeps its shaders in REDengine's own shader caches, tessellates its terrain, and asks for an alias region of 48 GiB.

On the Mac (2026-11-13, M1 Max, macOS 27.2, `main` at 48dca7a4 with #2430, a window, to frame 8,400): the title, a save loaded from the main menu, and White Orchard (road, hillside, the windmill in the distance) with the HUD and the minimap, at 60 fps (frame time p50 16.67 ms, p95 16.68 ms; GPU p50 10.5 ms, p95 11.9 ms), footprint 2.6 GB, no crash; the terrain and the water drawn by `main`'s tessellator (quads, `fractional_even`, from frame 3,835) with no `tessellated draw not replayed` line. Its `[RED]` lines: the partial library's two (no stamp, no `c1_full.tsv` for it), `svc GetInfo: unknown info type 10 (answered 0)` once at start, and the terrain material copy's integer target ([Both](#on-ios)). The run used the 603 shaders a recorded session drew (a partial library, `dmsh_build.py` with their keys), not the NSP script's full build. Earlier runs with this PR's original tessellation commit, on `main` at 34608c72 and ca26ef65, gave the same (GPU p50 11.8 and 10.6 ms). Earlier, on a fork with the same game support: a 19-minute session through the Kaer Morhen bedroom and White Orchard at 30 fps.

On an iPad Pro 13-inch (M4, 16 GB, iPadOS 27.2) and an iPhone Air (2026-11-12, the NSP script's `--platform ios --romfs separate` IPA of this branch, signed with the extended virtual addressing entitlement, on `main` at 34608c72 with the three PRs' commits; [On iOS](#on-ios)): the same save in White Orchard. The iPad held 60 fps (frame time p50 16.67 ms, p95 16.76 ms, none dropped; GPU p50 12.3 ms, footprint 2.8 GB, thermal state nominal) for 2¼ minutes of play. The iPhone runs it, but not smoothly ([On iOS](#on-ios)).

## What it needs

| Setting | Where | Why |
|---|---|---|
| A 50 GiB alias region: `mem_alias 0xC80000000`, `mem_reserve 0xCA8000000` | `game.tsv` (iOS; the Mac gives every game at least 64 GiB) | A `DT_INIT_ARRAY` constructor reserves 48 GiB at `main+0x89e8`, and later allocations use fixed offsets up to alias +48.97 GiB, so no smaller alias works. It is a virtual-address reservation; only touched pages are committed |
| The extended virtual addressing entitlement (iOS) | the IPA's signature: `DM_ENTITLEMENTS` for `build_ipa.sh` ([On iOS](#on-ios)) | without it iOS gives an app about 6 GB of address space and the 50 GiB reservation fails at start |
| REDengine 3 shader cache readers | `shader_split.py` (#2430) | 29,673 shaders are in `content/*/shadernx.cache`, the DLCs' and `content/content0/staticshadernx.cache`; without the readers the split finds 563 ([REDengine 3 shader caches](../../../docs/reference/shaders.md#redengine-3-shader-caches)) |
| SeparateSamplerId samplers | `dmsh_reflect.py`, `nvn_mtl.m` (on `main`, `ca26ef65`) | 245 of the 602 shaders a session drew have a type-2 sampler, without which they do not load and the screen stays black ([A separate sampler ID in a uniform buffer](../../../docs/reference/shaders.md#a-separate-sampler-id-in-a-uniform-buffer-handle-type-2)) |
| Tessellated draws | `nvn_tess.inc`, `dmsh_tess.py` (on `main`, `48dca7a4`) | The terrain and the water are quad patches (7 hull and 13 domain shaders, `fractional_even`); without it they are skipped and White Orchard has no ground ([Tessellation stages drawn without a Metal tessellator](../../../docs/reference/shaders.md#tessellation-stages-drawn-without-a-metal-tessellator)) |
| `fps_target 60`, `present_interval display` | [`game.tsv`](game.tsv) | 60 fps (30 on the Switch); the player's **Frame rate** picks 30 or 60 ([Frame rate](#frame-rate)) |
| `depth_mode zero` | `game.tsv` | The game sets near = 0 depth (`nvnDeviceSetDepthMode`); the pipeline archive built offline is otherwise built for −w and unused, and a first start builds its 25,330 pipelines during play (about 3.5 minutes) |
| `shader_pairs_manual.tsv` | the profile | 713 vertex / pixel pairs the files cannot show: those an early profile saw at run time, and covers that give a set of vertex shaders every output two pixel shaders read |
| `shader_facts_manual.tsv` | the profile | the image formats of compute shader `11130a61f4a6f3ec` and the texture types of `a52ac3530463868c` and `eacf9338c2835b9e` |
| `pso_manifest.tsv` | the profile | 1,031 prewarm rows (588 shaders, 434 render pipelines, 9 compute) from two Mac sessions in White Orchard |
| `modules main sdk`, the memory rows, `shader_groups` | `game.tsv` | the provisional profile's guesses, kept |

No code patches (no `patches.py`).

## Frame rate

- **Current: 60 fps** (30 on the Switch), 30 with the player's **Frame rate** (`DM_FPS_TARGET=30`). The game asks for present interval 2 once, at window creation, and has no other limit: with `present_interval display` the host logs `present cap 60 fps (game profile fps_target) applied by the present interval alone`, and White Orchard ran at 16.67 ms frames (above). With `DM_FPS_TARGET=30` the cap is 30 (`present cap 30 fps (DM_FPS_TARGET)`): 33.33 ms frames (p95 33.4 ms), none dropped.
- **Speed:** its logic runs on real time (2026-11-05, on the fork: 60.0 fps at the same game-clock speed as 30).
- While the save loads, frames take about 29 ms (p95 30 ms, one of 150 ms as the world appears); once in White Orchard they are back at 16.67 ms.

## Rendering

- **720p.** Docked and handheld both render 1280×720: `bin/config/platform/nx/rendering.ini` sets `Resolution="1280x720"` and `ForcedRendererResolution*`, with no docked variant, and the size is also fixed in code. The dynamic resolution only lowers it when the GPU is late. A larger render size needs code patches (a separate proposal).
- **Handheld** (`DM_OPMODE=0`, checked on the fork): the game switches to its larger handheld HUD and draws a little less scene detail (118 render passes in a White Orchard frame against 132 docked).

## Not checked

- Other regions (Velen, Novigrad, Skellige), combat, cutscenes in the engine, long free play.
- The NSP script's full Mac build on this `main` (the run above used a partial library).

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
- **Both:** one pipeline is refused, the terrain material copy (vertex `ff4b69b477ff89e6`, fragment `2dcecea911db30ef`): a `float4` output into an `R16Uint` target (`[RED] failed to create render pipeline`). The Mac builds the pipeline but, since `main`'s check of integer targets, names the shader: `[RED] shader 2dcecea911db30ef_b writes colour target 0, an integer format, as a float, and has no integer entry point`. It is the terrain's mip-chain pass: it samples the level above of the same `R16Uint` texture as a float and writes the four floats, so a `fout` row alone would not make it right (the texture read is float too). Open.

## On the Mac

- After the NSP script's `--platform mac` build, its printed command runs the game. The RomFS image is about 35 GB.
- From the title to the newest save, the presses the run sent (`DM_CTL`, a few seconds apart): `A`, `B`, `LDown` twice, `A`, `A`, `A`.

## Found on this game

- Resolved: the split found 563 shaders, none of them a material, lighting, hull, domain or compute shader → [REDengine 3 shader caches](../../../docs/reference/shaders.md#redengine-3-shader-caches).
- Resolved: a black screen; 245 of the 602 shaders a session drew did not load (`B binding fp_t_cb720898_2000005ASmplr has no resolved slot number`) → [A separate sampler ID in a uniform buffer (handle type 2)](../../../docs/reference/shaders.md#a-separate-sampler-id-in-a-uniform-buffer-handle-type-2).
- Resolved: no ground in White Orchard, `primitive type 14 not implemented` → [Tessellation stages drawn without a Metal tessellator](../../../docs/reference/shaders.md#tessellation-stages-drawn-without-a-metal-tessellator).

Otherwise it runs on mechanisms found on other games, including SDK 15.3's NVN and services.
