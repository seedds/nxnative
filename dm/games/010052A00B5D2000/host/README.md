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
| delta | [`lego-hp57-artifact.patch`](lego-hp57-artifact.patch): `git format-patch a662f29..7e72ed4`, 548,724 bytes, no binary hunks; then [`host-gate.patch`](host-gate.patch), the Years 1-4 present-pacing gate |
| tree after applying | `6147769164def6b1c311f99d343a7cc50d27908a` = `7e72ed4^{tree}` |
| host binary | `52e08030da37f3fcffbb2c116acf4e5dff7e7075af21b5b919a807163d948bd7`, 3,037,192 bytes — an **artifact pin** (series + `host-gate.patch`) |
| module | `nro0.patched.bin` `6729d1b3c0857fbb93ed6d94ceb4f4a5105f4fc112f376dcab18527d7c1a2496`, 18,762,448 bytes ([`./scripts/build_lego_hp.sh module`](../../../../scripts/build_lego_hp.sh)) |
| profile | [`game.tsv`](../game.tsv): `present_snap 4`, `present_upscale off`, `fullscreen on`, `ui_republish 0` |
| pins in the app | [`env.txt`](../env.txt), sha256 `f9ea3a16fe0dd9b908d2000293debdf892eea7978089bd96559b83e5019c33c8`: only `DM_PRESENT_SNAP=4`; fullscreen and upscaling are left to the settings window |

The pinned host is built from the series plus [`host-gate.patch`](host-gate.patch); it carries the Years 1-4
present-pacing gate described below. The profile still compiles `present_upscale off` and `fullscreen on`, but
`env.txt` no longer pins either: the settings window owns **Fullscreen** and **Upscaling**, and only the
present-snapshot depth (`DM_PRESENT_SNAP=4`) is pinned. A host rebuilt from this branch's `game.tsv` and the two
patches is not byte-identical (see below).

## Applying it and building the host

```bash
git clone <nxnative> nxnative && cd nxnative
git checkout -b lego-hp57-artifact a662f29ee79109ab54bbd7210a660247b9c69813
git am /path/to/nxnative-recipes/dm/games/010052A00B5D2000/host/lego-hp57-artifact.patch
git rev-parse HEAD^{tree}      # 6147769164def6b1c311f99d343a7cc50d27908a — the series' tree
git am /path/to/nxnative-recipes/dm/games/010052A00B5D2000/host/host-gate.patch
git rev-parse HEAD^{tree}      # the gate adds its own commit on top; the host binary pin includes it
mkdir -p dm/games/010052A00B5D2000
cp /path/to/nxnative-recipes/dm/games/010052A00B5D2000/game.tsv dm/games/010052A00B5D2000/
make -C dm/host GAME=010052A00B5D2000
shasum -a 256 dm/host/dmhost   # builds; NOT 52e08030… (see below) — ship the pinned binary
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
make -C dm/host GAME=010052A00B5D2000, this profile's game.tsv copied in  ->  builds, 3,037,192 bytes,
  sha256 52e08030da37f3fcffbb2c116acf4e5dff7e7075af21b5b919a807163d948bd7  (series + host-gate.patch)
  its gen/010052A00B5D2000/game_profile.h carries PRESENT_SNAP 1, PRESENT_SNAP_DEPTH 4, PRESENT_UPSCALE "off",
  FULLSCREEN 1, UI_REPUBLISH 0 — this game.tsv is what the host compiles
scripts/build_lego_hp.sh module  ->  nro0.patched.bin 6729d1b3…, byte-identical to the GLYPHFIX bundle's module
scripts/make_lego_hp_app.sh      ->  dmhost 52e08030…, module 6729d1b3…, env.txt identical; three files changed
```

The rebuild is **not** byte-identical to `52e08030…`: this profile (with `present_upscale off` and the recipe's profile path,
which the generated header names in its first line and `-g` records) links to `ac540e0c…` before the gate, and the gate build
to `52e08030…`. **Ship the pinned binary**; treat the sha as an artifact pin, not a reproducible build. `Lego HP NX GLYPHFIX.app` is
the earlier `Lego HP NX (fixed).app` clone with only the module replaced, so it carries this same host; the script's
`HOST_SHA` check is what holds that.

## The Years 1-4 present-pacing gate

`nro1` (`libharry1.nro`, Years 1-4, build `2ac11df4c46d710f4ee2dfe6668e7da7`) has no frame limit of its own; on the
console the swapchain's own release of a texture paces it (a frame cannot be drawn until a texture comes back, once per
display period). The present-snapshot ring (`DM_PRESENT_SNAP=4`) copies each present into a host texture and frees the
guest's texture at once, so that release disappears and Years 1-4 runs 2-4 guest frames per displayed one: the user's own
run measured 149.5 guest frames/s against 52.4 presents/s (2.85 per present), while the original app (no ring) runs Years
1-4 at 58.2 guest frames/s. Years 5-7 (`nro0`) limits its own rate and is unaffected.

[`host-gate.patch`](host-gate.patch) adds a 15-line check in `nvnWindowAcquireTexture` (`dm/gfx/nvn/hle_nvn.c`): while the
loaded module is `nro1` with that build ID, the acquire also waits for the texture's display slot on top of the ring's
capacity wait, restoring the console's pacing. Every other module, including Years 5-7's `nro0`, takes the ring path
unchanged. With a 4-deep ring, the fixed host logs once:
`present snapshots: nro1 (Years 1-4) has no frame limit of its own, so the acquire keeps the console's swapchain pacing …`.

Verification of the fix copy: `nro0` and `nro1` byte-identical to GLYPHFIX; Years 1-4 loads `nro1`, logs the gate line
once, 58.7 guest frames/s; Years 5-7 loads `nro0`, **no gate line**, 59.2 guest frames/s, no crash. The 2x itself could
not be reproduced on this session's display (its display link is dead, so both the old and the new host pace at ~60);
the "before" number is the user's run and the corpus. Host rebuilds here are not reproducible (a second build of the same
source differs), so the pin is the measured binary.

`dm/games/game.py` on this branch carries the same parser and `game_profile.h` change as the series (`present_snap` depth,
`present_upscale`, `fullscreen`, `ui_republish`) so that this profile's `game.tsv` compiles without the patch.
