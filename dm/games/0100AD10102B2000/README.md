# BioShock Remastered (`0100AD10102B2000`)

Update v65536 = 1.0.2, SDK 9, NVN. Blind Squirrel's port of Irrational's Unreal Engine 3 game: `main` (16 MB) and `sdk` (5.3 MB), `rtld` replaced by the host,
Scaleform menus (`.gsc`), Bink movies (`.bik`), its maps as `.bsm` packages, two 1 GB `bulkcontent` archives, and its shaders in one container of its own
(`shadercache.nx`). No middleware beyond Scaleform and Bink.

**Plays on the Mac** (M1 Pro, 16 GB, macOS 26.6.2): the two legal pages of a fresh save, the title and difficulty menus, the plane cabin from the opening and the
ocean after the crash, at the game's own 30 fps, with no `[RED]` line and no shader-map miss in the runs below. The memory split is the survey's, not measured
against hardware ([What is open](#what-is-open)). Nothing here was run on an iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `romfs_marker bulkcontent/bulkpatch1.blk` | [`game.tsv`](game.tsv) | the RomFS's first large archive; the game has no small file that names it |
| memory keys: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource, so `main` sets its own heap of at most the pool less 64 MB |
| `shader_groups` | `game.tsv` | the split's grouping of the RomFS folders |
| the `shadercache.nx` reader | `dm/gfx/tools/shader_split.py` (`shadercache_nx`) | the title ships its shaders in a container of its own ([Shaders](#shaders)) |
| the web view's answer to `window.close()` | `dm/host/web_apple.m` | both legal pages end with the browser's `close()`, which the host did not answer at all ([The legal pages](#the-legal-pages)) |

There is no `patches.py`: this build needs no rewriter cut, no frame-step stub and no allocator replacement.

## Files

- RomFS: 665 files, 12.1 GB. `maps/*.bsm` (11 files, 0.9 GB), `bulkcontent/bulkpatch1.blk` and `bulkpatch2.blk` (1 GB each), `binkmovies/*.bik` (4.0 GB),
  `flashmovies/`, `sounds_nx/`, `shadercache.nx` (33.5 MB), and the HtmlDocument NCAs' pages, which `nx_extract --html` writes to `<build>/html/html<n>`.
- Modules: `main` 16.0 MB, `sdk` 5.3 MB, `rtld`.
- The control data names it "BioShock Remastered" and carries its icon, so the app needs no `--icon`.

## Shaders

20,427 rows in [`shader_map.tsv`](shader_map.tsv): **20,419 out of `shadercache.nx`** and 8 out of `main` (the Phyre reader). Every one translates; the Mac
build compiles 20,427 of 20,427 in 3:23 on an M1 Pro (about 101 a second, none from the cache), so the whole set exists before the game's first frame and no
capture pass is needed.

The container, measured from the file:

- `{u32 version 13, u32 block count 5557}`, then 5,557 × `{u32 stored length, one zlib stream}`. Every block inflates to exactly 32,768 bytes except the last
  (20,133).
- Joined in file order they are 182,079,141 bytes holding 20,421 NVN records — `{16-byte header: u32 control length 0x880, u32 code length, u32, u32}`, the code
  area, the control section — of which 20,419 keys are distinct. All graphics; this title ships no compute shader.
- **2,622 records cross a 32 KiB block boundary**, so the reader joins the blocks before scanning: a per-block scan finds 18,041 distinct keys, which is the
  wrong answer and looks plausible.

## The legal pages

A fresh save boots into `legalterms.htdocs/termsofservice.html`, then `privacypolicy.html`, both in the RomFS's HtmlDocument NCA. Each page's confirm is a bare
`close()` (`BUTTON_CONFIRM_INDEX = 1`) with no navigation — the browser's own close, which the Switch's web applet reports as the page's end.

The host's web view had no answer to it: in a windowed run the button did nothing, the page stayed up, and the game asked for it again, so this
title could not be played past its first screen. `web_apple.m`'s injected script now answers `close()` like the applet does, and the page ends with **exit
reason 4**.

Reason 4 is not a guess: this build's legal-page loop reads the reason from the SDK's getter and accepts nothing else — `main+0x999198` is `cmp w0, #4` /
`b.ne` back to the loop. Reasons 0 to 3 and 5 were measured to be re-shown (each with the callback URL, the page's own URL and no URL at all).

With it, both pages accept, the game writes its own **`FirstRun.nx`** (4 zero bytes) into the save and boots straight to the title on every later run.
The two save files a player used to copy in to get past this screen are therefore no longer needed: the game writes one of them itself, and `BioUserSettings.nx`
was only ever the settings the game already creates.

The same page pair and the same bare `close()` are in BioShock 2 (`01002620102C6000`) and BioShock Infinite (`0100D560102C8000`), whose pages end the same way —
their bring-ups needed the same save-file workaround.

## What is open

- **The memory split is the survey's.** The game boots, plays, saves and streams its maps with `mem_reserve 0x100000000` / `mem_alias 0x10000000`; those numbers
  were never compared with what hardware gives it, and a long session in a streamed area may need more alias room.
- **Depth of play is unmeasured.** The routes above reach the plane cabin and the ocean after the crash. A save later in the game, a long session, and the
  in-game menu system were not exercised. There is no `smoke.tsv` and no `bench.tsv` route yet.
- **The iPhone was not built.** The Mac app is the only artifact so far.
- **`depth_mode` is unrecorded**: no run's NVN depth calls were read for the profile, so an offline pipeline archive has no observed mode to use.
- **`pso_manifest.tsv` is empty**: no prewarm seed list has been recorded for the profile yet, so pipelines compile on first draw.
