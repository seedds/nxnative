# The host side of the Years 5-7 fix (`nxnative`, not pushed)

The bundle the fix ships as — `Lego HP NX (fixed).app` — is the user's app plus **two files**: `Contents/MacOS/dmhost` and `Contents/Resources/dm/env.txt` (built by [`scripts/make_lego_hp_app.sh`](../../../../scripts/make_lego_hp_app.sh)). The env pins and this profile's `game.tsv` are pushed with this branch; the **host is not**: it lives on `lego-hp57-artifact` in the `nxnative` working clone, and no ref of that clone has been pushed. This folder carries that branch's delta as an mbox series, so the recipe reproduces from the pushed state alone.

## The pins

| | |
|---|---|
| host source | `nxnative`, branch **`lego-hp57-artifact`** |
| base | `a662f29ee79109ab54bbd7210a660247b9c69813` — *shader_split: a Yaz0 member that expands is not overlap (GitHub issue #640)*, the merge base with `main` |
| HEAD | **`7e72ed449b9505d1c7803fe2a76d717827e7f729`** (32 commits) |
| delta | [`lego-hp57-artifact.patch`](lego-hp57-artifact.patch): `git format-patch a662f29..HEAD`, 548,724 bytes, no binary hunks |
| tree after applying | `6147769164def6b1c311f99d343a7cc50d27908a` = `7e72ed4^{tree}` |
| host binary | `99100d734bea3c12a3d2e68c62699e3e190e4d4b2d8ba815d1cc924f1798d723` |
| module | `nro0.patched.bin` `56f3bf4fdbb712f3c4f7d1c32d921efd38ec6abde304616ab29df82150ce9820` ([`./scripts/build_lego_hp.sh module`](../../../../scripts/build_lego_hp.sh)) |
| profile | [`game.tsv`](../game.tsv): `present_snap 4`, `present_upscale metalfx`, `fullscreen on`, `ui_republish 0` |
| pins in the app | [`env.txt`](../env.txt), byte-identical to the bundle's `Contents/Resources/dm/env.txt` |

## Applying it and building the host

```bash
git clone <nxnative> nxnative && cd nxnative
git checkout -b lego-hp57-artifact a662f29ee79109ab54bbd7210a660247b9c69813
git am /path/to/nxnative-recipes/dm/games/010052A00B5D2000/host/lego-hp57-artifact.patch
git rev-parse HEAD^{tree}      # 6147769164def6b1c311f99d343a7cc50d27908a — the branch's tree
cp -R /path/to/nxnative-recipes/dm/games/010052A00B5D2000 dm/games/   # the profile is this branch's, not the host tree's
make -C dm/host GAME=010052A00B5D2000
shasum -a 256 dm/host/dmhost   # compare with 99100d73… above
```

The applied tree has no `dm/games/010052A00B5D2000`: the profile was written on this branch, after the host forked from `main`, and `make` stops with *no game profile 010052A00B5D2000* without it. Copying the folder in is what the working clone does; `DM_GAMES_DIR` pointed at it does the same.

A re-applied series does **not** get HEAD `7e72ed4`: `git am` writes its own committer and time. The tree hash is what the build reads, and it is identical. `99100d73…` is the binary every number in the note was measured with — the pin is the check. Byte-identical rebuilds are not claimed (the compiler, the SDK and the arch flags decide that); only the tree is pinned.

## What in the series is load-bearing

Four commits carry the fix. The rest are the instrumentation that measured it: they do nothing unless their switch is set, and they are the reason the numbers can be reproduced.

| commit | what it does |
|---|---|
| `1cfb6d7` | `dm/host/hle_nullpass.c` (new, 184 lines): a load that still faults in the null page reads as **zero**, for the sites a Years 5-7 frame reaches before the pass table is filled. With `patches.py`'s one cut this is the crash fix — and the reason the recipe's note names a host file that is not in this repository. Also the first NVN sampler-wrap mapping. |
| `ddb29c2` | the sampler wrap table follows `NVNwrapMode` (6 is mirrored repeat): a correctness fix to the same table. |
| `0417814` | `ui_republish` through the game profile: `game.py` → `DM_GAME_UI_REPUBLISH`, `nvn_mtl.m` `uip_draw`. The switch is shipped **0**, and exists so the A/B could be run from the profile. |
| `3de7c53` | the four profile keys this profile now carries: `present_snap` as a **depth** (2/3/4 → `DM_GAME_PRESENT_SNAP_DEPTH`), `present_upscale`, `fullscreen`; `settings.tsv`'s defaults from the profile; `front_mac.m` starting the window fullscreen. |

The other 28 are `diag(nvn)` / `fix(nvn)` instrumentation and docs: present capture and read-back (`DM_NVN_PRESENTCAP`, `DM_NVN_DRAWSTAT`), the UI ring and staging array (`DM_NVN_RINGTRACE`, `DM_VARRAY`, `DM_NVN_UVRING`, `DM_NVN_POSDUMP`, `DM_NVN_STATEREC`), queue depth and serialisation (`DM_NVN_PIPE`, `DM_NVN_GPU_LAG`, `DM_NVN_SUBMIT_LAG`, `DM_NVN_FULLSYNC`), the draw-list bisect, and the vertex / index snapshot fixes for the replay tool (`DM_NVN_SNAP`, **off** by default after `d5a967c` — they freeze data the engine fills after recording).

## What was verified

```
git am a662f29 + the series   ->  tree 6147769164def6b1c311f99d343a7cc50d27908a  (== 7e72ed4^{tree})
make -C dm/host GAME=…, this profile copied in  ->  builds (3,025,688 bytes)
  its gen/010052A00B5D2000/game_profile.h, minus its first line, is byte-identical to the header the pinned
  binary was built from (both: PRESENT_SNAP 1, PRESENT_SNAP_DEPTH 4, PRESENT_UPSCALE "metalfx", FULLSCREEN 1,
  UI_REPUBLISH 0, and every other macro) — so this game.tsv is the compiled profile that binary carries
scripts/build_lego_hp.sh module  ->  nro0.patched.bin 56f3bf4f…
scripts/make_lego_hp_app.sh      ->  dmhost 99100d73…, env.txt identical, file inventory identical to the shipped app
```

The rebuild is **not** byte-identical to `99100d73…` — it is `50cd08a2b4a2d38e3b2a0b20d3cf354a8462c754684b964b4d4a4d3f743d50d0`. The pinned host was built with the profile at the NSP script's path (`…/nsp/010052A00B5D2000_build/profile/010052A00B5D2000/game.tsv`), which the generated header names in its first line and `-g` records; the macros are the same. **Ship the pinned binary**; treat the sha as an artifact pin, not a reproducible build.

`dm/games/game.py` on this branch carries the same parser and `game_profile.h` change as the series (`present_snap` depth, `present_upscale`, `fullscreen`, `ui_republish`) so that this profile's `game.tsv` compiles without the patch; its one cosmetic difference is the `ui_republish` comment, which states the shipped `0` rather than the ON arm's `3`.
