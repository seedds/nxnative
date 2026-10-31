# Borderlands 3 (`01009970122E4000`)

Mac bring-up of Switch update 1.0.3 (`v196608`), SDK 16.2.3, UE4/NVN, Wwise and Havok. Modules `main` and `sdk`. Main build `ae2768797e3337ee209d7eab9574d475`; SDK build `835c229763ce5c1816a70f875bd70ccd`.

## Tested status

Tested on Apple M4 / macOS 27.2 on 2026-10-31. The title menu and opening Chapter 1 gameplay load; the Beastmaster character walks and jumps, normal save/quit writes the character and profile, and Continue loads the saved character after restarting the app. The player also reported playing successfully afterward. The final packaged app was tested with isolated saves in Docked/standard mode and its saved Handheld/MetalFX settings. Normal user saves were backed up and were not replaced by test saves.

This is opening-area validation, not a full-game compatibility claim. Later chapters, combat beyond the opening, each DLC's gameplay and long sessions remain unchecked. No iPhone build is claimed: see the memory settings below. An existing pipeline-archive compaction warning (issue #342) preserves its files and did not prevent these tests.

## Reproduce the Mac build

Use the repository's normal [Mac app pipeline](../../../docs/development.md#a-mac-app) with your own base game, update, optional DLC and keys:

```sh
python3 scripts/build_ipa_from_nsp.py /path/to/your/Borderlands3-inputs \
  -k /path/to/your/prod.keys --platform mac-app \
  --work-dir /path/with/space/borderlands3-work \
  --out /path/to/Borderlands3.app --app-name "Borderlands 3"
```

The input set tested included DLC indices 2, 3, 4, 5, 11 and 12. The corrected full scan found 84,949 unique shaders: 28,712 vertex, 55,703 pixel, 531 compute and 3 geometry; all compiled on the tested revision. The first build is lengthy. The committed map uses the canonical full-scan package numbering; the tested local incremental rebuild retained previous indices to reuse cached compilation. Their complete shader-key/stage sets and parsed fact tables match. Normal rebuilds regenerate `c1_full.tsv` and `cb_extent.tsv`; do not reuse tables from a different inputs stamp. No extracted shader packages, binaries, game assets, keys or saves are included here.

## Required profile settings

- `tex_flush_upload on` and `tex_dirty_upload on`: sampled sparse-layout textures are populated by the CPU, including 128×128 BGRA8 landscape heightmaps with eight mips. Prepopulated storage must be uploaded before an older GPU-only alias can be returned.
- `mem_alias 0x1000000000` with a roughly 64 GiB virtual reservation: the game's default UE region reserves about 62 GiB, and its allocator takes about 37 GiB of virtual space. These are address-space reservations, not physical RAM requirements; the opening gameplay HUD showed roughly 4.1–4.3 GiB app memory in local runs.
- Keep the game allocator intact. The survey found the FMallocBinned3 name getter in vtable slot 17, outside the automatic rewriter's supported layouts. No guessed allocator cut or game-specific executable address patch was introduced. A practical iPhone address-space layout needs separate work.

## Found on this game

- **Command-memory exhaustion before gameplay:** UE supplies valid 40/56-byte command-memory grants. The host discarded grants below 64 bytes and lost guest-backed origins needed to reclaim overflow segments, reaching its 1 GiB fatal cap near frame 3005. Accepting a grant large enough for an operation header fixes reclamation while retaining per-record and jump-space checks. The final local run passed frame 10,200, gameplay and saving without that fatal error; this does not establish unlimited-duration stability. See [command-memory segments](../../../docs/reference/nvn-objects.md).
- **Compact legacy UE pak v6:** the per-file index has 32-bit stored/raw sizes, an 8-bit method, no hash or serialized block size, relative compressed block offsets, and 64 KiB blocks. It coexists with standard v6 and the existing v4 reader; every candidate layout is validated before selection. Encrypted index alignment can repeat its prefix. See [Unreal pak readers](../../../docs/reference/shaders.md).
- **Missing inline shader records:** an Oodle payload need not contain a literal LSLG signature. Framed probing plus decoded-record validation recovered 696 shaders missed by the literal-only scan, including all six keys missing at runtime. False candidates are ignored without discarding later valid records. See [compressed inline signatures](../../../docs/reference/shaders.md#oodle-inline-shader-signatures-need-not-be-literal).
- **Stretched title terrain and stale materials:** sparse sampled storage could already be populated when a replacement texture object was initialized. Dirty tracking had no later write to detect, and the older GPU alias hid the new heightmap. Extend first-content detection for `tex_dirty_upload` profiles before alias lookup. Keep exclusions for image/compressible/linear/multisample/virtual/CPU_NO_ACCESS storage. See [populated sparse sampled storage](../../../docs/reference/nvn-objects.md#populated-sparse-sampled-storage-hidden-by-an-older-texture-alias).

A broader GPU-ownership experiment was discarded: changing compressed-copy ownership disrupted the existing storage mirroring and reintroduced purple surfaces. It is not included in this change. The retained first-use fix rendered the fresh title scene correctly and preserved normal opening gameplay.

## Regression coverage

The tested local build passed 179 NVN command checks, 201 initial-texture checks, 83 readback/copy checks, 318 image-alias checks, 44 writable-image checks, 12 compact/archive/inline reader tests, 13 shader-chain tests and 11 documentation tests. Fixtures are synthetic; the texture regression samples three literal-colored mips of a new sparse RGBA object over previously empty GPU storage.

The submission is rebased onto newer upstream code without changing the installed, player-tested app. Checks rerun on the submission and unavailable cross-game checks are listed in the pull request; do not interpret earlier counts as validation of every upstream change.
