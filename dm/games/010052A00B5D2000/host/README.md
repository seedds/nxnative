# The host side of the GLYPHFIX (`nxnative`, not pushed)

The bundle the fix ships as — `Lego HP NX GLYPHFIX.app` — is the user's original app plus **three files**:
`Contents/MacOS/dmhost`, `Contents/Resources/dm/nro0.patched.bin` and `Contents/Resources/dm/env.txt` (built by
[`scripts/make_lego_hp_app.sh`](../../../../scripts/make_lego_hp_app.sh)). The env pins, the module's patch and this profile's
`game.tsv` are pushed with this branch; the **host is not**: it lives on `lego-hp57-artifact` in the `nxnative` working clone,
and no ref of that clone has been pushed. This folder carries that branch's delta as an mbox series, so the recipe
reproduces from the pushed state alone. The host carries no part of the glyph fix: the whole fix is the guest patch in
[`patches.py`](../patches.py) (`blit_guard`, [PATCHES.md](../PATCHES.md)).

## The pins

| | |
|---|---|
| host source | `nxnative`, branch **`lego-hp57-artifact`** as it stood when the series was cut |
| base | `a662f29ee79109ab54bbd7210a660247b9c69813` — *shader_split: a Yaz0 member that expands is not overlap (GitHub issue #640)*, the merge base with `main` |
| series end | **`7e72ed449b9505d1c7803fe2a76d717827e7f729`** (32 commits) — the branch has since advanced; the **series is the pin**, not the branch tip |
| delta | [`lego-hp57-artifact.patch`](lego-hp57-artifact.patch): `git format-patch a662f29..7e72ed4`, 548,724 bytes, no binary hunks |
| tree after applying | `6147769164def6b1c311f99d343a7cc50d27908a` = `7e72ed4^{tree}` |
| host binary | `99100d734bea3c12a3d2e68c62699e3e190e4d4b2d8ba815d1cc924f1798d723`, 3,031,368 bytes — an **artifact pin** |
| module | `nro0.patched.bin` `6729d1b3c0857fbb93ed6d94ceb4f4a5105f4fc112f376dcab18527d7c1a2496`, 18,762,448 bytes ([`./scripts/build_lego_hp.sh module`](../../../../scripts/build_lego_hp.sh)) |
| profile | [`game.tsv`](../game.tsv): `present_snap 4`, `present_upscale off`, `fullscreen on`, `ui_republish 0` |
| pins in the app | [`env.txt`](../env.txt), sha256 `001874a5220c2d565759ab0fdd1abe2987e86810768b6542a138ba53e78a1db2`, byte-identical to the bundle's `Contents/Resources/dm/env.txt` |

The pinned host was built while this profile still said `present_upscale metalfx`, so its compiled default is MetalFX. The
bundle sets `DM_PRESENT_UPSCALE=off` in `env.txt`, and `fx_mode()` in `dm/gfx/nvn/nvn_mtl.m` gives the environment precedence
over `DM_GAME_PRESENT_UPSCALE`; GLYPHFIX therefore presents bilinearly. A host rebuilt from this branch's `game.tsv` compiles
`off` directly — and, like every rebuild, is not byte-identical (see below).

## Applying it and building the host

```bash
git clone <nxnative> nxnative && cd nxnative
git checkout -b lego-hp57-artifact a662f29ee79109ab54bbd7210a660247b9c69813
git am /path/to/nxnative-recipes/dm/games/010052A00B5D2000/host/lego-hp57-artifact.patch
git rev-parse HEAD^{tree}      # 6147769164def6b1c311f99d343a7cc50d27908a — the series' tree
mkdir -p dm/games/010052A00B5D2000
cp /path/to/nxnative-recipes/dm/games/010052A00B5D2000/game.tsv dm/games/010052A00B5D2000/
make -C dm/host GAME=010052A00B5D2000
shasum -a 256 dm/host/dmhost   # builds; NOT 99100d73… (see below) — ship the pinned binary
```

The applied tree has no `dm/games/010052A00B5D2000`: the profile was written on this branch, after the host forked from
`main`, and `make` stops with *no game profile 010052A00B5D2000* without it. Only the `game.tsv` is needed for the host
build (the profile's `patches.py` and `shdump/` are guest-side and are not read by `make`).

A re-applied series does **not** get HEAD `7e72ed4`: `git am` writes its own committer and time. The tree hash is what the
build reads, and it is identical. `99100d73…` is the binary every number in the note was measured with — the pin is the
check. Byte-identical rebuilds are not claimed (the compiler, the SDK and the arch flags decide that); only the tree is pinned.

## What in the series is load-bearing

Four commits carry the crash fix and the profile/switch plumbing. The rest are the instrumentation that measured the
artifacts: they do nothing unless their switch is set, and they are the reason the numbers can be reproduced.

| commit | what it does |
|---|---|
| `1cfb6d7` | `dm/host/hle_nullpass.c` (new, 184 lines): a load that still faults in the null page reads as **zero**, for the sites a Years 5-7 frame reaches before the pass table is filled. With `patches.py`'s `publish_record` this is the crash fix — and the reason the recipe's note names a host file that is not in this repository. Also the first NVN sampler-wrap mapping. |
| `ddb29c2` | the sampler wrap table follows `NVNwrapMode` (6 is mirrored repeat): a correctness fix to the same table. |
| `0417814` | `ui_republish` through the game profile: `game.py` → `DM_GAME_UI_REPUBLISH`, `nvn_mtl.m` `uip_draw`. The switch is shipped **0**, and exists so the A/B could be run from the profile. |
| `3de7c53` | the four profile keys this profile carries: `present_snap` as a **depth** (2/3/4 → `DM_GAME_PRESENT_SNAP_DEPTH`), `present_upscale`, `fullscreen`; `settings.tsv`'s defaults from the profile; `front_mac.m` starting the window fullscreen. |

The other 28 are `diag(nvn)` / `fix(nvn)` instrumentation and docs: present capture and read-back (`DM_NVN_PRESENTCAP`,
`DM_NVN_DRAWSTAT`), the UI ring and staging array (`DM_NVN_RINGTRACE`, `DM_VARRAY`, `DM_NVN_UVRING`, `DM_NVN_POSDUMP`,
`DM_NVN_STATEREC`), queue depth and serialisation (`DM_NVN_PIPE`, `DM_NVN_GPU_LAG`, `DM_NVN_SUBMIT_LAG`,
`DM_NVN_FULLSYNC`), the draw-list bisect, and the vertex / index snapshot fixes for the replay tool (`DM_NVN_SNAP`, **off**
by default after `d5a967c` — they freeze data the engine fills after recording).

## What was verified

```
git am a662f29 + the series   ->  32 commits, tree 6147769164def6b1c311f99d343a7cc50d27908a  (== 7e72ed4^{tree})
make -C dm/host GAME=010052A00B5D2000, this profile's game.tsv copied in  ->  builds, 3,037,128 bytes,
  sha256 ac540e0cc8e2919d1ec07a17e360a9f4b2bfe575327b02f16f00cbf960cf0ee3
  its gen/010052A00B5D2000/game_profile.h carries PRESENT_SNAP 1, PRESENT_SNAP_DEPTH 4, PRESENT_UPSCALE "off",
  FULLSCREEN 1, UI_REPUBLISH 0 — this game.tsv is what the host compiles
scripts/build_lego_hp.sh module  ->  nro0.patched.bin 6729d1b3…, byte-identical to the GLYPHFIX bundle's module
scripts/make_lego_hp_app.sh      ->  dmhost 99100d73…, module 6729d1b3…, env.txt identical; three files changed
```

The rebuild is **not** byte-identical to `99100d73…`: this profile (with `present_upscale off` and the recipe's profile path,
which the generated header names in its first line and `-g` records) links to `ac540e0c…`, and the earlier profile to
`50cd08a2…`. **Ship the pinned binary**; treat the sha as an artifact pin, not a reproducible build. Its bytes are the same
in `Lego HP NX GLYPHFIX.app` and in the earlier `Lego HP NX (fixed).app`.

`dm/games/game.py` on this branch carries the same parser and `game_profile.h` change as the series (`present_snap` depth,
`present_upscale`, `fullscreen`, `ui_republish`) so that this profile's `game.tsv` compiles without the patch.
