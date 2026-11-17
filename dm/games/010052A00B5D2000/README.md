# LEGO® Harry Potter™ Collection (`010052A00B5D2000`)

Update 1.0.2 (`v131072`), SDK 5.4.0, NVN. TT Games' own renderer — the Windows D3D11 DLLs ported to `nn::ro` modules: `main` is 750 instructions that mount the RomFS, `libHarryLauncher.nro` starts and loads `libharry1.nro` (Years 1-4) or `libharry2.nro` (Years 5-7) as a module. Modules `main`, `sdk` and the three NROs `lhp_switch_dll_launcher.nrr` registers; the host maps the rewritten NROs at start.

Years 5-7's frame is built by three routines this build does not order as the console does, and that was the crash on selection. The recorder (`nro0+0xd2ff0`) stores a pass's record in the live table; the begin-frame routine (`nro0+0x15f420`) copies the 24 entries into the published table a frame phase later; the replay (`nro0+0xd39c0` and eleven other sites) reads the published table in the same frame. Two threads, two locks that do not order them, and the records die with the frame. [`patches.py`](patches.py) publishes the record at the store — one `B` at `nro0+0xd3138`, the profile's only cut — and `dm/host/hle_nullpass.c` reads a load that still faults in the null page as zero (the emulator's recovery, natively) for the sites a frame reaches before it fills them. With both, Years 5-7 ran 90 s twice at 58.8-59.0 fps with no crash.

**This branch reproduces the rewritten module byte for byte.** `scripts/build_lego_hp.sh module` rewrites a stock `libharry2.nro` and checks the result against `sha256 56f3bf4f…` — the `nro0.patched.bin` the Mac app carries (13 s on an M1 Pro). The rest of the build needs the shader tables, which are the derived profile's, not this profile's: see [Shaders](#shaders).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | `main` sets a heap of at most the pool − 64 MB (a 3328 MB heap region); the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon |
| `shader_groups /.nro/=nro /harry1_dll/=harry1dll /harry2_dll/=harry2dll /launcher_dll/=launcherdll /.nrr/=nrr` | [`game.tsv`](game.tsv) | the three modules' and two DLL folders' shaders each get a package of their own |
| the `STUBS` entry for build `7e19fa84…` | [`patches.py`](patches.py) | the pass table's publish at record time, without which Years 5-7 crashes at about frame 3,900 |
| a 16 KB-page-aligned image, or a skewed one | `dm/host/loader.c` | `main`'s `.text` (0..0xbb8) and `.data` (0x3000..0x33c8) would share one 16 KB page, and Apple silicon refuses a page that is both writable and executable: the host maps such an image `SKEW` bytes past a page boundary and cannot seal the key page |

## Bring-up

```bash
./scripts/build_lego_hp.sh module      # reproduce nro0.patched.bin and verify its sha256
./scripts/build_lego_hp.sh survey      # what the game is made of; builds nothing
```

The module step is the whole recipe: the NSP's `libharry2.nro` + this profile's `patches.py` + `dm/tools/patch_nso.py` = the module the app runs. `NRO=`, `PYTHON=` and `OUT=` override the defaults; the stock NRO comes from a `<id>_build/nro_v131072/nro0.nro` the NSP script wrote, or from a fresh extraction of the update.

- The NROs are rewritten beside `main` and `sdk` (`collect_nros`), so `patches.py` is applied to `libharry2.nro` without any per-module registration beyond its build ID.
- A build that points `DM_GAMES_DIR` at a derived profile must not also see this profile's `patches.py`: `patch_nso.py` asserts a build's `STUBS` is registered once, so a copy of the derived profile made for its shader tables has to leave `patches.py` out — the registered one is the stub.
- The key page cannot be sealed on the skewed image: `install_key_page` skips `mprotect` when the base is not 16 KB aligned and logs the patch-area bytes the skew keeps writable in the same breath.

## Shaders

- The split reads **372 keys** from the three NROs (196 in `libharry1.nro`, 231 in `libharry2.nro` minus the overlap), none from the 741 KB launcher.
- The map the app ships holds **1,397 keys**. 493 of them (364 pixel, 129 vertex) came from a `DM_NVN_SHDUMP` of 924 pairs, appended by [`dm/gfx/tools/dmsh_shdump.py`](../../../dm/gfx/tools/dmsh_shdump.py) — the tool recomputes each pair's key and refuses a pair whose `sha256(control[0:nctl] + code)` is not the key it was written under.
- The launcher's own package (`launcher_dll/launcher.pkdswitch`, 158 zlib members indexed by `launcher.pkiswitch`) holds 13 more, and no reader decodes it.
- 489 of the 865 programs the game draws sit in the four `.DATNX` block archives (12.9 GB, LZHAM in zlib framing). No reader decodes them; an issue before that change, per `CONTRIBUTING.md`.
- The tables are not in this branch: a registered profile's tables must be what the split reproduces from the game files row for row, and a dump-derived row has no such producer. They live in the derived profile beside the game (104 KB) and the app carries a copy.

## Open

- **The glyph lattice**, the screenshot that started this: 11 of 51 frames. Nothing here addresses it and nothing measured moved it — `DM_NVN_SUBMIT_LAG` N=0..4 (hold proven effective, 11,585/11,700 released by a later submit, 60.0 fps) left the wall bit-identical on the same frames; `DM_NVN_ATLASGUARD` fired 1,016 times on legitimate scene draws and the wall stayed. Cause unresolved.
- A one-frame screen-change wall (53.12 luma) — root-caused, not fixed.
- Artefact B, a ~65-luma single-frame colour-triangle post-process spike — untouched (64.98/65.01 in every sweep).
- Artefact C, menu digit flicker on A — no capture ever succeeded.
- Saves contaminated by recovery runs — the mechanism is proven, no repair; triage only.
- The full app build was not re-run from this branch: its shader step needs the derived profile's tables (above). The app itself was built and run on the Mac; **no IPA was built and iOS is untested**.

## Found on this game

- [NRO modules that keep their shaders in a zlib package](../../../docs/reference/shaders.md#nro-modules-that-keep-their-shaders-in-a-zlib-package)
- [Shaders whose bytes exist only in a run-time dump](../../../docs/reference/shaders.md#shaders-whose-bytes-exist-only-in-a-run-time-dump)
