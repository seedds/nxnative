# LEGO® Harry Potter™ Collection (`010052A00B5D2000`) — the GLYPHFIX recipe

Update 1.0.2 (`v131072`), SDK 5.4.0, NVN. TT Games' own renderer — the Windows D3D11 DLLs ported to `nn::ro` modules: `main` is 750 instructions that mount the RomFS, `libHarryLauncher.nro` starts and loads `libharry1.nro` (Years 1-4) or `libharry2.nro` (Years 5-7) as a module. Modules `main`, `sdk` and the three NROs `lhp_switch_dll_launcher.nrr` registers; the host maps the rewritten NROs at start.

**This branch is the recipe for `Lego HP NX GLYPHFIX.app`.** It is the user's original app plus three replaced/added files (host, `nro0`, `env.txt`), and this revision adds the fold of 372 captured shaders (the profile's `shdump/`, `shader_map.tsv` and the compiled library) and a host gate for Years 1-4's present pacing:

| file | what it is | sha256 |
|---|---|---|
| `Contents/MacOS/dmhost` | the pinned host (series + `host-gate.patch`; not rebuilt here — [`host/README.md`](host/README.md)) | `52e08030da37f3fcffbb2c116acf4e5dff7e7075af21b5b919a807163d948bd7` |
| `Contents/Resources/dm/nro0.patched.bin` | `libharry2.nro` rewritten with this profile's two stubs — `publish_record` and **`blit_guard`** | `6729d1b3c0857fbb93ed6d94ceb4f4a5105f4fc112f376dcab18527d7c1a2496`, 18,762,448 B |
| `Contents/Resources/dm/env.txt` | [`env.txt`](env.txt): only `DM_PRESENT_SNAP=4`; Fullscreen and Upscaling are left to the settings window | `f9ea3a16fe0dd9b908d2000293debdf892eea7978089bd96559b83e5019c33c8` |

Everything else — `romfs.img`, `main.patched.bin`, the other NROs, the shader packages, `launcher.json`, the icon — is the source bundle's, byte for byte, except `shader_map.tsv` and the compiled `msl/` library: this revision folds the 372 shaders captured from play into them (the profile's `shdump/` and `shader_map.tsv` are the inputs; a full `build_ipa_from_nsp.py --platform mac-app` rebuilds the library). Nothing is codesigned or re-signed: an unsigned host inside a bundle whose signature no longer covers it is what runs.

[`patches.py`](patches.py) holds the two guest patches and [`PATCHES.md`](PATCHES.md) their sites, bytes and rebuild. This note is the artifact, the diagnosis, the mechanism, the numbers and what is left. The mechanism is written up at length in the host repository: `nxnative/docs/guestpatch-glyph-batch.md` (branch `lego-hp57-artifact`), with the measurements in `orch/guestpatch/REPORT.md` and the emission archaeology in `orch/dither`, `orch/emit`, `orch/state`.

Years 5-7's frame is also built by three routines this build does not order as the console does, and that was the crash on selection. The recorder (`nro0+0xd2ff0`) stores a pass's record in the live table; the begin-frame routine (`nro0+0x15f420`) copies the 24 entries into the published table a frame phase later; the replay (`nro0+0xd39c0` and eleven other sites) reads the published table in the same frame. Two threads, two locks that do not order them, and the records die with the frame. [`patches.py`](patches.py)'s `publish_record` publishes the record at the store — one `B` at `nro0+0xd3138` — and `dm/host/hle_nullpass.c` reads a load that still faults in the null page as zero (the emulator's recovery, natively) for the sites a frame reaches before it fills them. With both, Years 5-7 ran 90 s twice at 58.8-59.0 fps with no crash.

## The artifact

One presented frame, then gone:

* **The glyph/atlas lattice.** The 4096×2048 font atlas painted over the lower 5/7 of the screen as a lattice of glyph cells, blended over the live scene, HUD intact. At 1920×1080: cell pitch 18.89 × 14.96 px, a hard top edge at y = 310 (1 − 5/7 of 1080), full width down to y = 1077.
* **The in-level checkerboard.** The same blit over a dark scene: a dense black-and-white dither grid across the lower two thirds, read at first as a texture or sampler bug. It is the atlas's glyph interiors at a different scale — one defect, two appearances.

It is a *presentation* defect: the guest's own frame is what the host drew, and the next presented frame is clean. In level it fires 23.0 times per 1,000 presents and 118.2 per 1,000 **moving** presents; 495 of 497 captured episodes are exactly one present long. Years 1-4 (`nro1`) never show it and never make the call.

## How it was diagnosed

1. **Years 5-7 against Years 1-4.** `libharry2.nro` (`nro0`) and `libharry1.nro` (`nro1`) are near-identical ports of the same renderer. The blit at `nro0+0x6d9810` and its caller `nro0+0x6d9f64` have no counterpart in `nro1`'s image, and `nro1` never shows the artifact. That narrowed it to a Years 5-7-only draw path, not the shared UI renderer or the host.
2. **Per-present metrics, not per-frame ones.** The host reads the presented texture back on a 160×90 grid (`DM_NVN_DRAWSTAT`), and the detector counts a wall as `white>0.06 AND nbrh>=43 AND nbrh/lstd>=0.70`. The class is one present long, 495 of 497 episodes, and its rate scales with *motion* — what a per-present capture of a single draw produces and a persistent frame defect does not.
3. **The draw, found by its geometry.** The entity's draw method calls one helper, `bl #0x6d9810` at `0x6d9f64` (vtable `0xe19430`). That helper emits **one quad**: uv is the unit square by construction (`mov x10,#0x3f8000003f800000` at `0x105314`), and the rect is a constant — 2/7 and 5/7 at `0x9e4be4/0x9e4be8`, mapped by the writer's static transform `x = 2c−1, y = 1−2c` to the lower 5/7 of the screen. 186 of 186 detections had identical geometry, so it is not a degenerate glyph or a measure pass.
4. **The emission: it lands in the glyph run.** The helper goes through the ordinary UI vertex emitter `0x1053b0`. The emitter *begins* the batch it writes into (`0xedb80(4, 0x25, handle, 2, 0)` at `0x10541c`) and every four vertices appends with `0x15d720` and **batch 0** — the batch base at `0x11539c8` (0x88 bytes), which is the run the HUD's glyph quads are in. So a whole-texture blit is appended to a run that will be drawn with the run's texture, not the blit's.
5. **Why the atlas stamps opaque.** The run is drawn with the 4096×2048 R8 font atlas and the SDF text shader. That shader's coverage is the *screen derivative* of uv: one whole-texture quad spans the atlas's full uv range in a single quad, so the derivative-scaled edge collapses to a 1-bit threshold and every texel above it — the atlas's glyph interiors — stamps opaque. A text draw, whose uvs move a few hundredths per glyph, never does this.
6. **Why the blit's own texture never rebinds the draw.** `begin` records the texture at `[0x11539c8+0x30]`, but the draw record for the run was made at the run's *first* `begin`; the blit's handle (`[0x11b0bb8]`) arrives later and never reaches the record. The blit is a legitimate primitive in the wrong run.
7. **Ruled out.** Not the host's unresolved-texture placeholder (`dummy_tex()` is a 1×1 zero RGBA8 texture; it magnifies to a flat colour, not a 19×15 px lattice). Not `ui_republish` (both A/B arms ran `DM_NVN_UI_REPUBLISH=0`, one module byte apart). Not submission lag (`DM_NVN_SUBMIT_LAG` N=0..4 left the wall bit-identical on the same frames) and not `DM_NVN_ATLASGUARD`, which fired 1,016 times on legitimate scene draws without moving it.

## The fix

| site | old | new | what it does |
|---|---|---|---|
| `nro0+0x6d9f64` | `2bfeff97` (`bl #0x6d9810`) | `ce2a2c14` (`b #0x11e4a9c`) | jumps to a 52-byte stub that decides whether the blit may append |

The stub (appended at `0x11e4a9c..0x11e4acc`, after `publish_record`'s stub, which does not move — the patch area starts at `0x11c0000` and 150,224 bytes are written):

```
adrp x8,#0x1153000 ; add x8,x8,#0x9c8      &ui_batch[0]
ldr  w9,[x8,#0x10] ; cbz w9,emit           vertices in the open run; empty → the blit starts its own
ldr  x9,[x8,#0x30] ; adrp x10,#0x11b0000
ldr  x10,[x10,#0xbb8] ; cmp x9,x10 ; b.eq emit
b skip                                      another element's texture in the run → do not append
emit: mov x0,x19 ; bl #0x6d9810
skip: b #0x6d9f68
```

* **The two compared addresses.** `[0x11539c8+0x30]` is the texture handle the open UI batch recorded at its last `begin`; `[0x11b0bb8]` is the blit entity's own texture handle. `[0x11539c8+0x10]` is the vertex count in the open run.
* **Why a guard and not a deletion.** The blit is a real draw: when the run is empty, its own `begin` takes effect first and the quad is drawn with its own texture; when the run already carries that same handle, it belongs there. Only a **non-empty run carrying a different texture** is the defect. Deleting the call would remove the entity's own visual everywhere, and the guard leaves the helper (`0x6d9810`) and the emitter (`0x1053b0`) untouched.
* **The call and the way back.** The stub sets `x0 = x19` (the original call's argument, whose `mov x0,x19` at `0x6d9f60` stays), calls the untouched helper, and branches to `0x6d9f68` — the instruction after the original call site. `x8/x9/x10` are dead there (execution resumes with `ldr x0,[x19,#8]`, and `x19` is callee-saved by the callee), so no live register is clobbered.
* **Four bytes, and 52 appended.** The module grows from 18,762,396 B (`56f3bf4f…`, before the guard) to 18,762,448 B (`6729d1b3…`).
* **Years 1-4 are untouched.** `STUBS` is keyed to `nro0`'s build ID `7e19fa8490f09d9d2a71f74f5903211d`, so `nro1` keeps its shipped bytes; and `nro1`'s image has no `0x6d9810` for a guard to reach.

In this corpus the guard always takes the skip branch: the entity's handle `0x7a056f30` is never the handle the glyph run recorded (the draw's atlas is `st0` slot 6, id 70095). On this path the blit's intended visual never rendered — the A/B measures what removing the wrong append does to the frame.

## Build

```bash
# The module — the fix. Needs the game's stock libharry2.nro and patch_nso.py's python (capstone 5.x).
DM_GAME=010052A00B5D2000 DM_GAMES_DIR="$PWD/dm/games" \
  /opt/homebrew/bin/python3.14 dm/tools/patch_nso.py \
  "<build>/nro_v131072/nro0.nro" "<out>" nro0
# -> <out>/nro0.patched.bin  6729d1b3…  18,762,448 B   (summary line: stub 2, cuts 2, patch 150224)
# or, with the hash checked for you (13 s on an M1 Pro):
./scripts/build_lego_hp.sh module

# The host — pinned, not rebuilt; host/README.md has the series and why the sha is an artifact pin.
# The bundle — an APFS clone of the source app plus the host, the module and env.txt:
./scripts/make_lego_hp_app.sh \
  --from   "/Volumes/Workspace/Lego HP-NX/Lego HP NX.app" \
  --host   "/path/to/dmhost" \
  --module "<out>/nro0.patched.bin" \
  --out    "/Volumes/Workspace/Lego HP-NX/Lego HP NX GLYPHFIX.app"
```

`--from` must be a bundle this profile was built for: its `nro0.patched.bin` is either `56f3bf4f…` (before the guard) or the GLYPHFIX `6729d1b3…`; anything else is refused. The script checks the host, the module and `env.txt` against their hashes and refuses to write a bundle that is not the three-file delta. `NRO=`, `ROMFS=`, `PYTHON=` and `OUT=` override `build_lego_hp.sh`'s defaults; `./scripts/build_lego_hp.sh tables` re-splits the RomFS, merges the profile's [`shdump/`](shdump/) and checks all four tables row for row (about 3 min).

## How it was verified

Two arms, one host, one save, one route, one level; the only difference between them is the module (`blit_guard` present or not):

| | unpatched `56f3bf4f…` | patched `6729d1b3…` |
|---|---|---|
| presents in level | 21,876 | 22,155 + 22,818 = **44,972** |
| moving presents (`r1>5`) | 3,360 | 3,811 |
| calibrated lattice rows | **1,221** (55.8/1,000 presents, 327.1/1,000 moving) | **0** |

* **Zero in 3,811 moving presents.** At the corpus rate 104.8 were expected (`P(0) = 2.9e-46`); at the unpatched arm's own rate, 1,247. The Poisson 95 % upper bound is **0.79 per 1,000 moving** — 150× below the corpus baseline and 414× below the arm measured in the same session.
* **An independent reproduction, other arms and routes.** Six unpatched arms over four key routes, 107,160 presents: **113** font-atlas checkerboard fires. One patched arm, 23,700 presents: **0**. (The patched arm's four `lattice` rows at `nbrh 38.4-38.5` are the front end's own scene, below the rule's 43 — the host's looser trigger, not the class.)
* **Text is legible.** Both patched arms render "Press Any button" in the game's own font, with hearts, portraits and the spell icon — the guard does not touch the glyph quads.
* **Years 1-4 still boot and render.** With the patched module installed, `nro1` (`5ef802d1…`, the shipped bytes) ran 11,660 presents and rendered the Years 1-4 title screen with legible text, no crash.
* **No new capture class, no crash, no fault.** Every class in the patched arms exists in the unpatched arm; the `[RED]` shader-map misses are the same (96-101 vs 98-99).
* **Frame time unchanged.** p50 **16.66 ms** in both arms; p95 median 16.96 (unpatched) vs 16.95 (patched); worst window p95 39.68 vs 21.59 ms; 60.0 fps in both.

Reproducibility of the recipe itself (no host launch, hashes only):

```
patches.py + patch_nso.py + stock nro0   ->  6729d1b3…  == GLYPHFIX's nro0.patched.bin, byte for byte
git am host series on a662f29            ->  32 commits, tree 6147769164def6b1c311f99d343a7cc50d27908a
make -C dm/host GAME=010052A00B5D2000    ->  builds (series + host-gate.patch: 52e08030…; the pin is an artifact)
make_lego_hp_app.sh on the original app  ->  dmhost 52e08030…, module 6729d1b3…, env.txt f9ea3a16…
                                             the other 6,499 files sha256-identical; romfs.img same size and mtime (clonefile)
```

## Known residual

In play, a **small glyph flash** is occasionally still visible — far smaller than the atlas wash, but there. The guard is conditional by design, and two cases still let the whole-texture blit into the text run: when the run is **empty** (the stub jumps straight to the blit, which then begins its own run — correct for the blit, but if the text draw then joins that run, the contamination is the other way), and when the run **already carries the blit's own handle**. Either is a plausible source of the residual. The next step is an **unconditional variant** — skip the append whatever the run holds — to find out whether those two paths are the residual or something else; the price is the blit's own visual on this path, which the guard's measurements say never rendered anyway. This branch deliberately ships the conditional form that measured 0 in 44,972 presents.

## The present path (why these four values)

The improvement the user called "really really improved" is the **presentation**, not the drawable and not a GPU throttle. At a fixed 2560×1440 drawable, per 100 presents (the judge reads the presented texture back on the same 160×90 grid; wall = `white>0.06 & nbrh>38`):

| present path | wall /100 | `nbrh` max |
|---|---|---|
| bilinear, no snapshot ring | **15.58** | 46.91 |
| bilinear, no ring, real GPU serialisation (`DM_NVN_PASSGPU_SERIAL=1`) | **15.00** | 51.09 |
| bilinear, ring 4 | **1.68** | 39.46 |
| MetalFX, ring 4 | **0.00** | 32.64 |

* **The snapshot ring is the carrier.** Each present is copied into a host texture of its own, so the guest's next frame cannot overwrite the texture the screen is showing: 15.58 → 1.68 at the same drawable, and the lattice's contrast goes (`nbrh` max 46.9 → 39.5).
* **GPU load alone changes nothing.** Real serialisation on the dirty configuration leaves it at 15.00 with the lattice intact (`nbrh` up to 51.09).
* **The drawable is not a shippable knob.** No switch sets it: it is the fullscreen window's backing store, and `front_mac.m` consults `DM_FULLSCREEN`, never a size. At 5120×2880 the same path leaves 1.6-1.70 per 100 with `nbrh` max 39.5, **below** the photographed lattice's 43.4-46.2.
* **`ui_republish` stays 0.** It is inert at this configuration (its flagged population is identical to OFF: OFF 1.70 / 1.60, ON 1.66 / 1.66) and harmful with 2 submissions in flight: it writes one 5 MB pool block three times a frame with no ordering against the GPU, and put the wall back at 8.90 per 100 in one of two ON runs.
* **MetalFX is off.** Its 0.00 was a measurement, not an absence: the spatial scaler anti-aliases the glyph edges, so the *residual* lattice reads `nbrh ~39.5` with white under the 0.06 companion line, it hid the residual from the detector, and it costs GPU time every frame.

These values are in [`game.tsv`](game.tsv) and the snapshot depth again in [`env.txt`](env.txt). Fullscreen and upscaling are deliberately **not** pinned in `env.txt` any more: the settings window owns them. The pinned host is built from this branch's `game.tsv` plus [`host-gate.patch`](host-gate.patch) (the Years 1-4 pacing gate) and is not byte-identical to a rebuild (see [`host/README.md`](host/README.md)).

## Shaders

* **372 keys** the split reads from the three NROs (252 pixel, 120 vertex: 186 GLSLC outputs in each of `libharry1.nro` and `libharry2.nro`, none in the 741 KB launcher). Verified: `shader_split.py` on `RomFS.img` + `ExeFS/main` alone reproduces those rows and their labels exactly.
* **1,396 more** from a run-time dump. The engine assembles those programs in RAM, so their bytes exist only in a run. [`shdump/`](shdump/) holds the 2,112 pairs one playthrough with `DM_NVN_SHDUMP` wrote — 1,763 distinct keys, 63 of them captured twice, 1,677 already in the split's map. `dmsh_shdump.py` recomputes each pair's `sha256(control[0:nctl] + code)[:16]`, **rejects** a pair whose hash is not the key in its file name, and appends the row plus its package record.
* The map is **1,769 keys** in two packages: `ps_nro.bin` 1,362 records, `vs_nro.bin` 406. `/shdump/=nro` files the dumped rows into the same two.
* **The app's map labelled those 1,396 rows `/.nro/libHarryLauncher.nro#RAW@<n>`, and `<n>` was not an offset in any file** — it was the package record index, printed as hex by a one-off script that appended the dump by hand. This branch writes the dump's provenance instead (`/shdump/<key>.ctl#SHDUMP@0x0`). Keys, stages and package files are identical; the record order — and so the `index` column — is not. The same 1,769 programs compile either way, but a rebuild's `.bin` packages are not byte-identical to the app's.
* Two containers no reader decodes: the launcher's own zlib package (`launcher_dll/launcher.pkdswitch`, 158 members) holds 13 more keys, and 489 of the 865 programs the game draws sit in the four `.DATNX` block archives (12.9 GB, LZHAM in zlib framing).

## Open

* The **residual glyph flash** above; the unconditional variant is the next test.
* A one-frame screen-change wall (53.12 luma) — root-caused, not fixed.
* Artefact B, a ~65-luma single-frame colour-triangle post-process spike — untouched (64.98/65.01 in every sweep).
* Saves contaminated by recovery runs — the mechanism is proven, no repair; triage only.
* The full app build was not re-run from this branch: the split and the tables it checks are verified, but the Metal compile is pinned to the translation chain's Xcode 27.1 (`dm/gfx/tools/dmsh_chain.lock`) and this Mac has 27.2, so the library step would have to be forced with `DM_ALLOW_OTHER_METAL`. The app itself was built and run on the Mac; **no IPA was built and iOS is untested**.

## Found on this game

* [NRO modules that keep their shaders in a zlib package](../../../docs/reference/shaders.md#nro-modules-that-keep-their-shaders-in-a-zlib-package)
* [Shaders whose bytes exist only in a run-time dump](../../../docs/reference/shaders.md#shaders-whose-bytes-exist-only-in-a-run-time-dump)
