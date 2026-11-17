# LEGO® Harry Potter™ Collection (`010052A00B5D2000`)

Update 1.0.2 (`v131072`), SDK 5.4.0, NVN. TT Games' own renderer — the Windows D3D11 DLLs ported to `nn::ro` modules: `main` is 750 instructions that mount the RomFS, `libHarryLauncher.nro` starts and loads `libharry1.nro` (Years 1-4) or `libharry2.nro` (Years 5-7) as a module. Modules `main`, `sdk` and the three NROs `lhp_switch_dll_launcher.nrr` registers; the host maps the rewritten NROs at start.

Years 5-7's frame is built by three routines this build does not order as the console does, and that was the crash on selection. The recorder (`nro0+0xd2ff0`) stores a pass's record in the live table; the begin-frame routine (`nro0+0x15f420`) copies the 24 entries into the published table a frame phase later; the replay (`nro0+0xd39c0` and eleven other sites) reads the published table in the same frame. Two threads, two locks that do not order them, and the records die with the frame. [`patches.py`](patches.py) publishes the record at the store — one `B` at `nro0+0xd3138`, the profile's only cut — and `dm/host/hle_nullpass.c` reads a load that still faults in the null page as zero (the emulator's recovery, natively) for the sites a frame reaches before it fills them. With both, Years 5-7 ran 90 s twice at 58.8-59.0 fps with no crash.

**The module and the shader tables both reproduce from this branch.** `scripts/build_lego_hp.sh module` rewrites a stock `libharry2.nro` and checks the result against `sha256 56f3bf4f…` — the `nro0.patched.bin` the Mac app carries (13 s on an M1 Pro). `scripts/build_lego_hp.sh tables` splits the RomFS, merges the run-time dump the profile carries, and checks all four tables row for row (about 3 min).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | `main` sets a heap of at most the pool − 64 MB (a 3328 MB heap region); the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `shader_groups /.nro/=nro /shdump/=nro /harry1_dll/=harry1dll /harry2_dll/=harry2dll /launcher_dll/=launcherdll /.nrr/=nrr` | [`game.tsv`](game.tsv) | the three modules' and two DLL folders' shaders each get a package of their own; the dumped rows are filed into the NROs' two packages, where the app's build put them |
| the `STUBS` entry for build `7e19fa84…` | [`patches.py`](patches.py) | the pass table's publish at record time, without which Years 5-7 crashes at about frame 3,900 |
| `present_snap 4`, `present_upscale metalfx`, `fullscreen on`, `ui_republish 0` | [`game.tsv`](game.tsv) | the present path: a 4-deep ring of present snapshots, MetalFX's spatial scaler and the 5120×2880 fullscreen drawable — the configuration the user called "really really improved"; see [The present path](#the-present-path) |
| the same four, as environment variables | [`env.txt`](env.txt) | the app's settings window exports its own defaults (Upscaling=Standard, Display mode=Window, Frame buffering off) over the profile compiled into the host, so the bundle pins them; a key an env file sets is shown as fixed and is not overridden by Play |
| a 16 KB-page-aligned image, or a skewed one | `dm/host/loader.c` | `main`'s `.text` (0..0xbb8) and `.data` (0x3000..0x33c8) would share one 16 KB page, and Apple silicon refuses a page that is both writable and executable: the host maps such an image `SKEW` bytes past a page boundary and cannot seal the key page |

## Bring-up

```bash
./scripts/build_lego_hp.sh module      # reproduce nro0.patched.bin and verify its sha256
./scripts/build_lego_hp.sh tables      # re-split the RomFS + merge the dump, and verify all four tables
./scripts/build_lego_hp.sh survey      # what the game is made of; builds nothing
./scripts/make_lego_hp_app.sh --from "/path/Lego HP NX.app" --host dm/host/dmhost \
                             --out "/path/Lego HP NX (fixed).app"   # the Mac app: the two-file delta
```

The two steps are the recipe. The module: the NSP's `libharry2.nro` + this profile's `patches.py` + `dm/tools/patch_nso.py` = the module the app runs. The tables: `RomFS.img` + `ExeFS/main` through `shader_split.py`, then this profile's [`shdump/`](shdump/) through `dmsh_shdump.py` = the profile's map, facts, pairs and pipeline pairs. `NRO=`, `ROMFS=`, `PYTHON=` and `OUT=` override the defaults; the stock NRO and the image come from a `<id>_build/nro_v131072/nro0.nro` and a `<id>_build/extract/<version>/` the NSP script wrote, or from a fresh extraction of the update.

- The NROs are rewritten beside `main` and `sdk` (`collect_nros`), so `patches.py` is applied to `libharry2.nro` without any per-module registration beyond its build ID.
- `scripts/build_ipa_from_nsp.py` runs `dmsh_shdump.py` between the split and the row-for-row comparison whenever the profile has a `shdump/` directory, so a full build reproduces the committed map instead of stopping on it.
- The key page cannot be sealed on the skewed image: `install_key_page` skips `mprotect` when the base is not 16 KB aligned and logs the patch-area bytes the skew keeps writable in the same breath.

## Shaders

- **372 keys** the split reads from the three NROs (252 pixel, 120 vertex: 186 GLSLC outputs in each of `libharry1.nro` and `libharry2.nro`, none in the 741 KB launcher). Verified: `shader_split.py` on `RomFS.img` + `ExeFS/main` alone reproduces those rows and their labels exactly.
- **1,024 more** from a run-time dump. The engine assembles those programs in RAM (`…\SwitchFinalD11DLLStatic\…` paths in the NROs), so their bytes exist only in a run. [`shdump/`](shdump/) holds the 1,677 pairs one playthrough with `DM_NVN_SHDUMP` wrote — 1,391 distinct keys, 286 of them captured twice (at `SetShaders`, where the key is computed and the bytes are still valid, and at first draw) — and 367 of them already in the split's map. `dm/gfx/tools/dmsh_shdump.py` recomputes each pair's `sha256(control[0:nctl] + code)[:16]`, **rejects** a pair whose hash is not the key in its file name, and appends the row plus its package record; `scripts/build_ipa_from_nsp.py` runs it after the split, so the profile's map is what the split plus its own dump reproduces.
- The map is **1,396 keys** in two packages: `ps_nro.bin` 1,028 records, `vs_nro.bin` 368. `/shdump/=nro` in [`game.tsv`](game.tsv) files the dumped rows into the same two.
- **The app's map labelled those 1,024 rows `/.nro/libHarryLauncher.nro#RAW@<n>`, and `<n>` was not an offset in any file** — it was the package record index, printed as hex by a one-off script that appended the dump by hand. The bytes are not in `libHarryLauncher.nro`. This branch writes the dump's own provenance instead (`/shdump/<key>.ctl#SHDUMP@0x0`). Keys, stages and package files are identical; the record order — and so the `index` column — is not, because that script appended in file-name order and the tool appends in key order. The same 1,396 programs compile either way, but a rebuild's `.bin` packages are not byte-identical to the app's.
- Two containers no reader decodes: the launcher's own zlib package (`launcher_dll/launcher.pkdswitch`, 158 members) holds 13 more keys, and 489 of the 865 programs the game draws sit in the four `.DATNX` block archives (12.9 GB, LZHAM in zlib framing). Both are written up in `docs/reference/shaders.md`.


## The present path

The improvement the user called "really really improved" is the **presentation**, not the drawable and not a GPU throttle. At a fixed 2560×1440 drawable, per 100 presents (the judge reads the presented texture back on a 160×90 grid; wall = `white>0.06 & nbrh>38`, `docs/finalab-report.md` §4):

| present path | wall /100 | `nbrh` max |
|---|---|---|
| bilinear, no snapshot ring | **15.58** | 46.91 |
| bilinear, no ring, real GPU serialisation (`DM_NVN_PASSGPU_SERIAL=1`) | **15.00** | 51.09 |
| bilinear, ring 4 | **1.68** | 39.46 |
| MetalFX, ring 4 | **0.00** | 32.64 |

- **The snapshot ring is the carrier.** Each present is copied into a host texture of its own, so the guest's next frame cannot overwrite the texture the screen is showing: 15.58 → 1.68 at the same drawable, and the lattice's contrast goes (`nbrh` max 46.9 → 39.5). MetalFX's spatial scaler adds the rest at 2560 (1.68 → 0.00).
- **GPU load alone changes nothing.** Real serialisation on the dirty configuration leaves it at 15.00 with the lattice intact (`nbrh` up to 51.09), so this is not a throttle effect and "the GPU is too fast" explains none of what is left.
- **The drawable is not a shippable knob.** No switch sets it: it is the fullscreen window's backing store, and `front_mac.m` consults `DM_FULLSCREEN`, never a size. At the user's 5120×2880 the same path leaves 1.6-1.70 per 100 with `nbrh` max 39.5, **below** the photographed lattice's 43.4-46.2; those flags are a bright-content population, 63 of the 90 at the same present numbers in both arms.
- **`ui_republish` stays 0.** It is inert at this configuration — its flagged population is identical to OFF (OFF 1.70 / 1.60, ON 1.66 / 1.66) — and harmful with 2 submissions in flight: it writes one 5 MB pool block three times a frame with no ordering against the GPU, and put the wall back at 8.90 per 100 in one of two ON runs. Shipping `3` would ship a switch that does no work the draws can see.

These four values are in [`game.tsv`](game.tsv) — they are compiled into the host as `DM_GAME_*`, and the header this profile generates is macro for macro the one the pinned host was built from — and again in [`env.txt`](env.txt) (see below). The host that carries them is **not** pushed: [`host/`](host/README.md) has the branch, its HEAD and the patch series, and `99100d73…` is the binary the numbers belong to.

## The app as shipped

`Lego HP NX (fixed).app` is `Lego HP NX.app` with exactly **two files** different, confirmed by `diff -rq` over `Contents`:

| file | what it is |
|---|---|
| `Contents/MacOS/dmhost` | the host built from `lego-hp57-artifact` with this profile compiled in, sha256 `99100d734bea3c12a3d2e68c62699e3e190e4d4b2d8ba815d1cc924f1798d723` |
| `Contents/Resources/dm/env.txt` | [`env.txt`](env.txt): `DM_PRESENT_SNAP=4`, `DM_PRESENT_UPSCALE=metalfx`, `DM_FULLSCREEN=1` — the app's settings window exports its own defaults (Upscaling=Standard, Display mode=Window, Frame buffering off) over the compiled profile, and a key an env file sets is shown as fixed and is not overridden by Play (`dm/mac/Launcher.swift`, `LauncherModel.apply`) |

Everything else — `romfs.img`, the patched modules (the app already carries `nro0.patched.bin` `56f3bf4f…`), the shader packages, `launcher.json`, the icon — is the source bundle's. `./scripts/make_lego_hp_app.sh` does the copy (an APFS clone), installs both files, checks both sha256s, and **codesigns nothing**: an unsigned host inside a bundle whose signature no longer covers it is what runs.

## Open

- **The glyph lattice**, the screenshot that started this: 11 of 51 frames. No switch below moved it — `DM_NVN_SUBMIT_LAG` N=0..4 (hold proven effective, 11,585/11,700 released by a later submit, 60.0 fps) left the wall bit-identical on the same frames; `DM_NVN_ATLASGUARD` fired 1,016 times on legitimate scene draws and the wall stayed. The present path above did: at the shipped configuration no present reaches the photographed contrast (~25,000 presents across both arms, `nbrh` max 39.5 against the lattice's 43.4-46.2). Why the ring removes it — the displayed texture is a copy the guest cannot write — is the mechanism; the frame the guest produced is unchanged.
- A one-frame screen-change wall (53.12 luma) — root-caused, not fixed.
- Artefact B, a ~65-luma single-frame colour-triangle post-process spike — untouched (64.98/65.01 in every sweep).
- Artefact C, menu digit flicker on A — no capture ever succeeded.
- Saves contaminated by recovery runs — the mechanism is proven, no repair; triage only.
- The full app build was not re-run from this branch: the split and the tables it checks are verified, but the Metal compile is pinned to the translation chain's Xcode 27.1 (`dm/gfx/tools/dmsh_chain.lock`) and this Mac has 27.2, so the library step would have to be forced with `DM_ALLOW_OTHER_METAL`. The app itself was built and run on the Mac; **no IPA was built and iOS is untested**.

## Found on this game

- [NRO modules that keep their shaders in a zlib package](../../../docs/reference/shaders.md#nro-modules-that-keep-their-shaders-in-a-zlib-package)
- [Shaders whose bytes exist only in a run-time dump](../../../docs/reference/shaders.md#shaders-whose-bytes-exist-only-in-a-run-time-dump)
