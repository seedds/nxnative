# BioShock 2 Remastered (`01002620102C6000`)

Update v65536 = 1.0.2, SDK 9, NVN. Blind Squirrel's port of 2K Marin's BioShock 2, on the same technology as
[BioShock Remastered](../0100AD10102B2000/README.md): `main` (14.9 MB) and `sdk` (5.3 MB), `rtld` replaced by the host,
Scaleform menus, Bink movies, its maps as `.bsm` packages, two `bulkcontent` archives of 1.2 and 1.7 GB, and its shaders
in two caches of its own.

**Plays on the Mac** (M1 Pro, 16 GB, macOS 26.6.2): the two legal pages of a fresh save, then the game past them at its
own 30 fps with no `[RED]` line and no shader-map miss in headless runs. It was also played in a Mac window: the game
loads, plays, and the first level is walked through. The memory split is the survey's, not measured against hardware
([What is open](#what-is-open)). Nothing here was run on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `romfs_marker bulkcontent/bulkpatch2.blk` | [`game.tsv`](game.tsv) | the RomFS's largest archive; the game has no small file that names it |
| memory keys: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so `main` sets its own heap of at most the pool less 64 MB |
| `shader_groups` | `game.tsv` | the split's grouping of the RomFS folders |
| the `shadercache.nx` reader, both container versions | `dm/gfx/tools/shader_split.py` (`shadercache_nx_framing`, `shadercache_nx`) | this title's caches are version 28, whose blocks carry a raw length as well ([Shaders](#shaders)) |
| the web view's answer to `window.close()` | `dm/host/web_apple.m` | both legal pages end with the browser's `close()` ([The legal pages](#the-legal-pages)) |

There is no `patches.py`: this build needs no rewriter cut, no frame-step stub and no allocator replacement.

## Files

- RomFS: 645 files, 10.4 GB. `maps/*.bsm`, `bulkcontent/bulkpatch1.blk` (1.2 GB) and `bulkpatch2.blk` (1.7 GB),
  `binkmovies/`, `flashmovies/`, `sounds_nx/`, `system/`, `shadercache.nx` (31,604,010 B) and `startupshadercache.nx`
  (2,880 B), plus the HtmlDocument NCAs' pages, which `nx_extract --html` writes to `<build>/html/html<n>`.
- Modules: `main` 14,869,749 B, `sdk` 5,372,559 B, `rtld`.
- The control data names it "BioShock 2 Remastered" and carries its icon, so the app needs no `--icon`.

## Shaders

20,376 rows in `shader_map.tsv`: **20,367 out of `shadercache.nx`** and **9 out of `startupshadercache.nx`**.
The caches hold 20,384 records; 8 repeat a key and `shader_split.py` collapses them. Every shader translates, and the Mac
build compiled 20,376 of 20,376 in 4:14 on an M1 Pro — the whole set exists before the game's first frame, so nothing is
captured.

Both files are the same container as the predecessor's, at version 28: `{u32 version, u32 block count}`, then per block
`{u32 stored length, u32 raw length, one zlib stream}` — the raw length is 32 KiB in `shadercache.nx` and 30,178 B in
`startupshadercache.nx`, which is why it is stored per block. A reader that assumed version 13's `{u32 stored length, one zlib
stream}` rejected both files and split 0 shaders; `shadercache_nx_framing()` now identifies the framing by walking the
block table to the end of the file, in either shape.

## The legal pages

A fresh save boots into `legalterms.htdocs/termsofservice.html`, then `privacypolicy.html`, exactly as in the predecessor:
each confirm is a bare `close()`, which `web_apple.m` answers as the page's end with exit reason 4. Both pages accept, the
game writes its own **`FirstRun.nx`** into the save, and it starts on every later run without them. No save file has to be
copied in.

## What is open

- **The memory split is the survey's.** The game boots, plays and saves with `mem_reserve 0x100000000` /
  `mem_alias 0x10000000`; those numbers were never compared with hardware, and a long session in a streamed area may want
  more alias room.
- **Depth of play stops at the first level.** The game loads and plays in a Mac window, and the first level is walked
  through; nothing past it was played. No save later in the game and no `smoke.tsv`/`bench.tsv` route exist yet. Headless
  runs reach frames after both legal pages, at 30 fps.
- **The iPhone was not built.** The Mac app is the only artifact so far.
- **`depth_mode` is unrecorded**, so an offline pipeline archive has no observed mode; **`pso_manifest.tsv` is empty**,
  so pipelines compile on first draw.
- The app is ad-hoc signed: it runs here, but `spctl` rejects it and it is not notarized.
