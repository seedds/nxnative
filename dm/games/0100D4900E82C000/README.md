# Metro 2033 Redux (`0100D4900E82C000`)

Base game 1.0.0 (v0), SDK 8.2.0, NVN, 4A Engine (`uEngine :: Secondary` and `uEngine :: PhysX simulator` are its profiling markers in the log). Modules `main` (build `85c362cc9790f0edf23749b041953c51`) and `sdk` (`3ae34e689d7a35f13cbf42bfd672af8b`); there is no `subsdk*` module. The RomFS holds 66 files in 6.25 GB, most of it in 27 `.vfs0` archives of 4A's own format, and the whole shader library is one loose file at its root, `/shaders.nx` (16.5 MB, 854 shaders: 463 pixel, 391 vertex). The profile is the NSP script's provisional one, copied unchanged: no key was added or edited, and no rewriter patch or tool change belongs to this title. Two host fixes were found on it and are on `main` ([Found on this game](#found-on-this-game)).

**Plays on the Mac**: the 4A logo, the intro movie, the title, the main menu, the play-style and difficulty screens, the PROLOGUE card, the opening level and first-person gameplay with driven stick input. The stock run held 29.6–30.0 fps in all 17 of its windows (13 exactly 30.0; frame time p50 33.33 / p95 33.35 ms, GPU p50 5.08–6.17 ms), with 0 `[RED]` lines, 0 shaders missing from the map and a graceful exit. A double-clickable 6.4 GB Mac app was packed and verifies (`codesign -v --strict` exit 0). No IPA was built, so iOS is untested. That run's host predates the two fixes below, and the profile was not run again on `main` since they landed ([Open](#open)).

## Found on this game

- [The AudioOutBuffer handed to the SDK is its own 40-byte object](../../../docs/reference/audio.md#the-audiooutbuffer-handed-to-the-sdk-is-its-own-40-byte-object) (resolved, PR #2739): the resampler read the previous block's data size as its buffer's capacity and replayed 170 ms old audio in one block of four.
- [A 4-sample target over a single-sample image's pool storage](../../../docs/reference/rendering.md#a-4-sample-target-over-a-single-sample-images-pool-storage) (resolved, PR #2789): the effects pass drew into Metal memory nothing read ([Rendering](#rendering)).
- The note also relies on [GLSLC outputs in main and in loose files](../../../docs/reference/shaders.md#glslc-outputs-in-main-and-in-loose-files) for the shader file, and on [A present interval that is the game's only limit, in display mode](../../../docs/reference/frame-rate.md#a-present-interval-that-is-the-games-only-limit-in-display-mode) for the frame-rate lever this profile does not use.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | `main` imports 220 of `sdk`'s exports — C / libc 110, `nn::os` 29, `nn::audio` 20, `nn::hid` 17, `nn::oe` 10, `nn::fs` 9, `nn::mem` 7, `nn::vi` 6, other C++ 5, `nn::account` 4, `nn::diag` 2 and `nvnBootstrapLoader`. No `subsdk*` module is shipped. |
| 3.25 GB heap region: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NPDM declares no system resource, so `main` sets a heap of at most the pool − 64 MB. The run logs `[mem] heap 1.98 GB (heap region 3.25 GB)`, so its own request fits well inside the guess. |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource: `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `romfs_marker content00.vfs0` | `game.tsv` | the first archive at the RomFS root |
| `zip_store .nso:.nss:.nx:.ogv:.png:.vfs0:.vfx:.metallib` | `game.tsv` | the archives and the shader file are stored, not deflated, in the packed app |
| `shader_groups /nxLauncher.nspd/=nxlaunchernspd /2033/=2033` | `game.tsv` | the provisional groups name the two other RomFS directories. Neither rule matches a shader: all 854 keys are read from `/shaders.nx` and land in the `other` group (`ps_other.bin` / `vs_other.bin`). The host reads the library by key. |

No code of the game is cut and no rewriter patch is involved: the stock host and chain run this build.

**Resolved: stale audio in one block of four** (PR #2739, on `main` since e251e168). Before it the host answered `GetAudioOutBufferBufferSize` from a private 24-byte view of `nn::audio::AudioOutBuffer`, where the SDK's object is 40 bytes `{next, buffer, buffer_size, data_size, tag}` with the data pointer at +0x08, the capacity at +0x10 and the data size at +0x18. This game reads that value as the capacity of the buffer its 44.1 → 48 kHz resampler writes into (`main+0xb38450`), so with the old offsets it saw the previous block's data size — 8188 bytes instead of 8192 — the resampler wrote nothing, and the block went out holding the audio it already carried: 24.73 % of appends replayed content from four blocks (170 ms) earlier. With the SDK's layout the value is the true 8192 and the stale re-appends measure 0.00 % of 2,065 blocks, sample-exact against the device dump. No profile key is involved on either side.

## Files and shaders

- RomFS: 66 files, 6.25 GB. 27 `.vfs0` archives (5.82 GB) carry the bulk, with 8 `.html` pages, 7 `.dat`, 4 `.ogv` movies (0.34 GB), 3 `.css`, the `2033` folder (4 files, 0.34 GB) and the `nxLauncher.nspd` legal / control / meta trees.
- **Current:** 854 shaders (463 pixel, 391 vertex), every one from `/shaders.nx` — 16,473,485 B at the RomFS root, read by the split's generic GLSLC reader (control marker at +0x10). 913 variation pairs in `shader_pairs.tsv`; 15 vertex shaders are single-stage and take the one pixel cover `0f0f9aad985c3cec`.
- All 854 compile for the Mac (`shaders 854: ok 854`, 0 failed), `c1_full.tsv: not proven bounded: 0 shaders`, and no run logged a shader missing from the map. `pso_pairs.tsv` is the empty provisional table.
- **Open:** `pso_manifest.tsv` is the provisional empty list, so the first start builds the pipeline bases itself (854 shaders / 217 render pipelines in 220 ms, one base written for the depth convention below). A recorded list is the obvious next win, not a fault.
- The `.vfs0` archives are not read by anything in this repository. The shader library is whole without them: all 854 keys come from `/shaders.nx`, the runs logged 0 keys not in the map, and the GLSLC marker appears 0 times in `main` and `sdk`, so the executables hold no shader either ([Open](#open)).

## Memory

- The game sets a 1.98 GB heap inside the 3.25 GB region the keys reserve, and the run reaches 1,484 MB resident with 1,195 MB of GPU allocations at frame 5,400 — flat over the last 3,000 frames (1,471 MB at frame 2,400), 11 memory pools (3 resident), 1,763 shared textures, no freed-but-alive block.
- The keys are the survey's provisional guesses and that run is what verified them: the app booted, played the route and never failed an allocation. The alias region's high-water mark was not measured ([Open](#open)).

## Rendering

- The layer is created 1920×1080 and the game's first render targets are 1280×720 (a `SetLayerCrop 0,0 1280x720` at start, 1920×1080 later), so the 3D picture is 720p scaled into the layer. Shader route B (translated Switch originals).
- **Resolved: the effects were drawn where nothing read them** (PR #2789, on `main` since c25315f5). 4A draws its particles, beams, decals, the weapon's laser sight, the muzzle flash and the spiderwebs into a 640x360 **4-sample** `RGBA8_SRGB` colour target (ser 2630) over the bytes of the 1280x720 single-sample scene target (ser 2611), 0x3c0000 bytes each, and the upscale reads those bytes as the scene. The host gave the two descriptions two Metal allocations; it now places them in one heap ([A 4-sample target over a single-sample image's pool storage](../../../docs/reference/rendering.md#a-4-sample-target-over-a-single-sample-images-pool-storage)). Checked on an M1 Max with the PR's first version (the laser beam and dot, the muzzle flash and the sparks drawn); the landed version, with its one-time layout check (`storage pair layout:` in the log), was not run on this game.
- **Open: the laser sight's draw lost its vertex attribute** on the host the port was made with. The draw binds a 6,216,736-vertex stream, reads 140 of them through indices the CPU cannot read, and feeds a `Short4` attribute (format 0x2d) to a float input, which Metal's fetch cannot read: the CPU conversion took the whole stream (99 MB against the 64 MiB per-frame staging), logged `[RED] vertex attribute conversion: a range of 6216736 vertices exceeds the per-frame staging (format 0x2d)` and left the attribute unbound. Since 69e7b11b (PR #1129) such a layout draws with the vertex shader's pulling pipeline, which decodes the format on the GPU for the vertices drawn only ([Attributes the vertex fetch cannot read, pulled](../../../docs/reference/rendering.md#attributes-the-vertex-fetch-cannot-read-pulled)), so on `main` the CPU conversion should only meet this draw while that pipeline is built in the background. Not run: whether red line 90 still appears, and the draw with it. PR #2790 (bounding the CPU conversion by the index type) is not on `main`.
- **Current, harmless:** the packed pipeline archive was built for `depth near=-w` while this game asks for `near=0`, so the device builds the bases it lacks (one new base, 0.1 MB, written to `00012-1.mtl4a`) and the log names the profile key that would avoid it (`depth_mode`). Nothing visibly wrong goes with it, and the key was not added because the profile is the packer's unchanged.
- Screenshots looked at: the title, the main menu, the play-style and difficulty screens, the PROLOGUE card and first-person gameplay with the rifle and the blue wrist device; the last is [the gameplay frame in `docs/images`](../../../docs/images/metro-2033-redux.jpg).

## Frame rate

- **Current:** 30 fps, which is the game's own pacing. It sets `nvnWindowSetPresentInterval(2)` once (`main+0xa83f0c`), the host keeps it in game mode (`present tick 60 Hz, interval mode game (original semantics) (present interval set by the game 2)`), and the profile has no `fps_target`, `present_interval` or `fps_max` row.
- The stock run's 17 windows read 29.6–30.0 fps (13 at exactly 30.0); frame time p50 33.33 / p95 33.35 / max 33.41 ms, NVN submit+present p50 1.5–2.3 ms, GPU p50 5.08–6.17 / p95 6.12–7.39 ms, CPU 190–212 % of one core at 5.3–6.3 W. The frame time is one display interval pair, not a cap the Mac is pushing against.
- 60 fps is reachable with `present_interval display` (and `fps_target 60` for the launcher row): a fork carrying those two keys measured 60.0 fps in gameplay (frame time p50 16.67 ms, GPU p50 2.24–3.19 ms) and its 638 s driven run reached frame 35,700 without a crash. That build is **not** what ships here, and no frame-rate key was added.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py "<NSP folder>" -k <prod.keys> --platform mac-app`: extract, rewrite, split (854 shaders), Mac shaders (854 of 854), host, pipeline archive, then the app. The tested bundle is 6.4 GB and 45 files, `codesign -v --strict` exit 0.
- From a fresh save, headless (`DM_AUDIO=0`, `DM_WINDOW` **unset**, `DM_OE_SCRIPT=focus=1@5`, `DM_CTL=<file>`): `A` skips the intro movie, `A` at the title opens the main menu (NEW GAME), `A` takes the play style (Survival / Spartan), `A` the difficulty (NORMAL …), `A` closes the PROLOGUE card, and the opening level loads into first person. The "PRESS (A) TO SKIP" prompt on the load before the title needs one `A`, the title one more.
- Stick input is `LUp~<s>` / `LLeft~<s>` / `LRight~<s>` / `LDown~<s>`; `shot <png>` captures the next presented frame headless, and `DM_NVN_SHOT=<frame>:<path%d>` works before the first drawable too. About 3.5 minutes of stick input in gameplay drove frames 18,000–30,900 on the fork build.
- `DM_WINDOW=0` **opens** a window (the host tests `!getenv("DM_WINDOW")`), so leave it unset. Running `Contents/MacOS/dmhost` directly writes the log to stdout, not to `~/Library/Logs/nxnative/<id>.log`; only the launcher opens that file.
- With `--no-prebake` the first frame arrives at about 45 s and the title at about 2 min: the pipeline bases are built on first start because `pso_manifest.tsv` is empty.

## Open

- No run on `main` since PR #2739, PR #2789 and PR #1129's pulled vertex conversion landed: the effects ([Rendering](#rendering)), the laser sight's draw and red line 90, and the `storage pair layout:` line.
- No IPA or iPhone / iPad run: no iOS build, no install, no iPhone memory measurement.
- No save/load route: the game autosaves on the way into the level, but loading a save was not driven, and nothing past the opening level was played.
- 30 fps is the game's own present interval (2). The 60 fps result was measured on a fork carrying `present_interval display` and is not shipped here.
- The `.vfs0` archives (27 files, 5.82 GB) are 4A's own format and nothing reads them. That does not matter for the shaders — they are in `/shaders.nx` — but whether something the game needs lives only inside them is unknown.
- The alias region's high-water mark, later levels, the launcher's settings screen, other controller layouts and a windowed run were not measured. Sound was measured on the block accounting through a muted device; nobody has listened to it.
