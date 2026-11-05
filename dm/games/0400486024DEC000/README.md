# The Legend of Zelda: Ocarina of Time

The Switch 2 base dump reaches its title screen on the Mac. VideoToolbox decodes the 1920×1080 AV1 track. The movie library runs its NDVE compute shaders to produce the background behind the logo and menu. Gameplay validation is open.

## Profile

SDK 22.2.8, NVN API 231.40, with Nintendo's sead, agl and gsys. The three loaded modules are `main`, `subsdk0` (NintendoSDK_movie), and `sdk`. The NPDM declares 48 MiB of system resources. Startup requests a 6.20 GiB heap, which fits in the profile's 12 GiB reservation with the stack and alias regions. The registered build is the supplied base dump (`v0`).

## Mac dry run

Use the rewritten `patched_v0` and the original extracted `base/romfs`. Build the host with `make -C dm/host GAME=0400486024DEC000` and use the [Mac shader build](../../../docs/development.md#build-by-hand-mac). From the repository, set `DUMP` to the folder containing `base` and `WORK` to the build and run workspace beside it.

```sh
mkdir -p "$WORK/run/save" "$WORK/run/temp" "$WORK/font"
python3 dm/tools/make_ext_font.py "$WORK/font/nintendo_ext.ttf"
DM_ROOT="$WORK" DM_ROMFS="$DUMP/base/romfs" DM_SAVE="$WORK/run/save" \
  DM_TEMP="$WORK/run/temp" DM_MSL_DIR="$WORK/shader/cache" DM_FONT_DIR="$WORK/font" \
  DM_NVN_SHMAP="$PWD/dm/games/0400486024DEC000/shader_map.tsv" \
  DM_WINDOW=1 DM_METAL_HUD=1 DM_FOCUS_KEEP=1 DM_AUDIO=1 \
  dm/host/dmhost "$WORK/patched_v0"
```

`DM_FOCUS_KEEP=1` lets the title keep playing while another app has focus. The profile prewarms the 11 NDVE compute pipelines used by the title. On the Apple M5 that took 17.7 s on a cold run, with a progress screen, and 7 ms with the archive warm. Compiling the same pipelines during the first submission had blocked the first movie frame for tens of seconds. The larger scene shader set also triggers a separate graphics-pipeline prebuild. On the first run with that set, the startup screen took 86.9 s to hand out all pairs, and the remaining workers finished after 110.6 s total.

Start a new game to reproduce the opening scene transition. With the test save and title flow, `DM_KEYS=f2600:A,f2950:A` advances the menu automatically. `DM_NVN_SHDUMP` saves control sections and code for missing keys to the directory it names.

For a run past the opening, copy the committed save folder to the new run's `DM_SAVE` and load slot 00 from the menu. The checkpoint contains both `slot_00/progress.sav` and `slot_00/caption.sav`. The load screen identifies it as **Kokiri Forest Autosave**. A windowed reload reached the playable scene with the HUD and controls, so rendering can be tested without replaying the movie.

## Found on this game

- The online library's [eventfd for its poller](../../../docs/reference/services.md#eventfds-served-offline) and the [virtual audio input list](../../../docs/reference/audio.md#no-virtual-audio-input-devices-sdk-22) blocked startup.
- The SDK's [NDVE pack](../../../docs/reference/shaders.md#the-movie-librarys-ndve-shader-pack) contains 22 compute programs. The title route uses 11.
- The [raw write log and pool lookup](../../../docs/reference/frame-rate.md#a-full-frame-shader-overflows-the-raw-write-log) took the windowed title movie from a 47.6 ms median to 16.67 ms on an Apple M5, at 1920×1080 decode. A steady 300-frame window averaged 60.0 fps and its GPU median was 15.94 ms. A simultaneous ten-worker shader build caused additional stalls. The same run returned to 60.0 fps when the build paused.
- A [play report send](../../../docs/reference/services.md#an-asynchronous-play-report-opens-prepou) aborted at the first gameplay transition after a failed `prepo:u` connection. A Mac dry run with the offline completion finished the 9,254-picture opening movie and continued submitting the live scene. The title-only shader set left the scene black.
- The white 3D scene was agl's [conditional depth clears](../../../docs/reference/rendering.md#conditional-rendering-evaluated-at-replay) run unconditionally (resolved; the profile sets `render_cond on`). The main opaque pass tests `EQUAL` against a copy of the prepass depth that agl clears under a condition right after copying, a clear the console skips. Without the key the copy was 1.0, no lit geometry passed, and the full-screen sky (`943d8c0ece545cc8`) showed through. With the key the saved Kokiri Forest scene renders: Link, the ground, the props, lighting and the HUD.
- Mesh-shaded models were missing, among them the door of Link's house: the split read six stage slots of each BNSH program, and NVN2 keeps [task and mesh shaders in slots 7 and 8](../../../docs/reference/shaders.md#task-and-mesh-shaders-in-nvn2-bnsh-program-slots-7-and-8) (resolved). The new split adds 491 mesh and 17 task shaders to the map, 53 of the 54 keys the Kokiri Forest scene had asked for. 511 of their 520 build; the door renders.
- The first 3D scene exhausted Metal memory when [bindless pool walks churned finalized textures](../../../docs/reference/nvn-objects.md#a-bindless-pool-churns-finalized-textures-within-a-submission). On the Apple M5, the failing run reached 18,930 MiB of residency. With submission-local backing, residency was 7,946 MiB about ten seconds after the movie and 7,871 MiB at 35 seconds. The opening 3D scene and its subtitles presented, and the test host exited normally.

## Open

- Of the task and mesh shaders, nine do not translate: eight foliage shaders stop at Ampere opcode `0x243` (packed conversion, source A `RZ`, mode `0x0000c4ff`; three of them, `604134a5262ff670`, `ecead5cd0569091f` and `fed302d791a4ba43`, are drawn in Kokiri Forest), and one at ISBERD mode `0xb8` in a task or mesh shader. The scene also asks for `d17eefaff4f0bc23`, whose code is in none of the shader archives.
- The title-only build logged 3,331 distinct mapped libraries it could not open, including 363 shaders that failed when setting up programs. A targeted build for those programs and their dependencies translated 6,132 of 6,134 shaders. The two failures are foliage compute shaders `8692b34b521225be` and `83df46a0b1b379b4`, both with Ampere opcode `0x243`.
- With `render_cond` on and the shader build still incomplete, Kokiri Forest showed a hard horizontal cut, sky colour above the horizon over the trees. The volumetric fog composite (`888c1068ac8f7043`, agl) takes a pixel whose R32Float depth copy reads above 0.9999 as sky and paints the sky lookup there. The pass that fills that copy draws with agl's full-screen vertex shader `fb1ed54be1baced1`, which that build had not reached yet, so the host skipped the draw (`draw skipped: no render pipeline`) and dropped the target's store. Retest with the stamped full build. The sky lookup's peaks (about 105, 67 and 36 in RGB) are unverified against the console.
- The opening scene calls `nvnCommandBufferDrawMeshTasksIndirect`, which the host does not implement. On `v0` the probe caught the return at `main+0x2be2dc0`; the branch builds the command buffer pointer in `x0` and the indirect argument address in `x1`. It does not set `x2` on that path. A separate sample of the first 64 bytes at the indirect address returned zeros after the call; the populated record's layout is still unverified. The title-only run logged 11 keys missing from the map. The expanded run captured seven of them before the memory failure. A byte-for-byte comparison found none of those seven code areas in the extracted shader package.
- The first opening-scene dry run with stable residency took 37–39 ms median per frame, with 13–14 ms of GPU work. The expanded Kokiri Forest run reached 11,059 MiB of host footprint and 22,316 deferred releases at frame 900. Across the preceding 300 frames, frame time was 59.55 ms median, submit/present 54.89 ms and GPU time 15.37 ms. The full compiler was paused during the run. The capture logged repeated walks of a texture pool of about 33,000 descriptors; profile which events invalidate those snapshots and how long their resources remain in flight. This is separate from title movie pacing.
- The Nov. 4 Lichii branches offer two shader leads. Its [packed conversion lowering](https://github.com/zortos293/Lichii/commit/35c043b3efaa147d1c1afe8a52cdf19fa92b77bf) handles opcode `0x243` with source A set to `RZ` and mode `0x0000c4ff`; both of this game's failed foliage programs use two registers and mode `0x000004ff`, which needs independent verification. Its [floating-point global reduction](https://github.com/zortos293/Lichii/commit/3aa89d1cdbe58a2bd5ded06b9c615c78da2ade70) matches a separate `F32FtzRn` diagnostic in the Mac shader build, where the translator drops that operation. Neither of the two shaders in the sky lookup and draw uses those instructions or the gather opcodes from Lichii's [fog work](https://github.com/zortos293/Lichii/commit/375c4a472bd95fdfaae0de6e3f192499cd14f806).
