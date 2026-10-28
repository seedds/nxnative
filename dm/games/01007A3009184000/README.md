# Princess Peach: Showtime! (`01007A3009184000`)

The base game, 1.0.0 (v0). Unreal Engine 4.27 with SF_GLSL_SWITCH shader archives and a version-11 pak, SDK 15, NVN. Modules `main`, `subsdk0` and `sdk`. Plays on the Mac from the full shader set built out of the pak. No IPA has been built; iOS is untested.

## What it needs

No rewriter cuts and no `patches.py`. The profile's memory keys and the pak-index key handling below are the whole game-specific part.

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x1028000000`, `mem_alias 0x1000000000`, `mem_stack 0x10000000` | [`game.tsv`](game.tsv) | virtual address memory: `nninitStartup` reserves a 62 GB region and FMallocBinned3 takes 37 GB of it (see [Memory](../../../docs/reference/memory.md)) |
| `sys_resource 0x5000000` | `game.tsv` | the NPDM declares 80 MB of system resource |
| `app_pool 0xCD500000` | `game.tsv` | held through the boot, the opening and the played stages |
| `ue_pak_key` (kept out of the repository) | a local provisional profile | the pak index key is derived at run time; see [The pak index and its key](#the-pak-index-and-its-key) |

## The pak index and its key

`PJ037-Switch.pak` (mount `../../../`, 103,924 files) is a version-11 pak whose primary index is AES-256-ECB over the whole 1.9 MB index blob — the "frozen index". Gildor's UEViewer asserts on it (`assert(!bIndexIsFrozen)`) and CUE4Parse / FModel do not implement it either, so the reading lives in `uepak.py` here.

The key is never a 32-byte window of `main` (no static key exists to find): the game derives it at run time and builds the AES-256 key schedule inside its decoder (`main+0x27E14AC`, the schedule call at `main+0x27E1508`; guest addresses are image offsets of the patched `main`). One boot under lldb with a breakpoint on the schedule call catches the 32-byte key in `x1`:

```
lldb -- dmhost <app>/Contents/Resources/dm      # dmhost takes the app's dm dir as its one argument
b -a <main base>+0x27E1508 ; continue             # <main base> is in the loader's log line
memory read --format x --size 1 --count 32 $x1
```

Put the key as `ue_pak_key	<64 hex digits>` into a local provisional profile (the NSP folder's `_build/profile/<id>/game.tsv`; `DM_GAMES_FIRST=1` makes every tool take it over the repository copy — the key itself stays out of the repository). `decode_ue_shaders` then hands it to `uepak.py job`, which decodes both `SF_GLSL_SWITCH` archives straight out of the pak: `ShaderArchive-Global` (1,563 shaders) and `ShaderArchive-PJ037` (55,311), LZ4 inline, no Oodle pass.

The index was traced with two host diagnostics this port added: `DM_FSLOG`'s guest call-site column (the read funnel of the index load pointed at the pak code) and `DM_MEMGREP=<hex>` (the decoded index and its mount FString in the guest's staging region). Neither is needed to play.

## Files and shaders

- RomFS: the pak holds the game content; movies are VP9 in `.pak`, stored uncompressed with `.metallib`.
- The split finds 56,594 shaders: 10,068 vertex, 45,424 pixel, 1,021 compute and 81 geometry. All 56,594 build for the Mac; a first build takes about 100 minutes at 8 jobs. `c1_guard` leaves 63 shaders without a bounded-read proof.
- The full control section matters here: UE 4.27's compute shaders carry texture and image descriptors past the keyed 0x880 prefix, so `nvnProgramSetShaders` keeps a control section of its own length when it is longer (`hle_nvn.c`), and `DM_NVN_SHDUMP` saves it whole (`nvn_mtl.m`).
- Unbound `texture_buffer` slots read as 2D-texture dummies before the texture-buffer kind was added to the dummy table (`nvn_refl.m`); compute kernels hit them.

## Checked

- The whole shader set builds: `shaders 56594: ok 56594`, plus 2,908 shadow-input and 9,962 pull libraries, 0 compile failures.
- The packed Mac app (7.8 GB with the 8.1 GB cache) boots to the title and into stages with no red line but the translator's bindless-handle notes; the opening, played stages and a fresh undumped stage render from the cache.
- Python tool tests pass for the touched tools (`uepak.py` list/job on the real RomFS, `build_ipa_from_nsp.py` compiles).
