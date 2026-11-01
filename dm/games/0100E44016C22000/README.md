# Patrick's Parabox (`0100E44016C22000`)

Update 1.0.1 (v65536) over the base game, SDK 13.3.7. Unity 2020.3.30f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (the movie module, which the game never uses) and `sdk`. Plays on the iPhone. On the Mac: the first-boot language menu (all ten languages drawn), the title, the hub, Intro 1 solved and Intro 2 with undo, reset and the menu, at 60 fps with music and saves (progress kept across a restart); no red line, about 0.42–0.48 GB resident, flat over four minutes.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `9c73fb73…` (the constructor at `sdk+0x37c3a0`; the movs at `+0x37c3b0` and `+0x37c44c`) | `StandardAllocator::Initialize` aborted at its `CentralHeap::Init`; see [Memory](#memory) |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | `game.tsv` | the layout of Ace Attorney Investigations Collection ([010005501E68C000](../010005501E68C000/README.md)) and Brain Memory Training Academy ([0100F5D01DA3A000](../0100F5D01DA3A000/README.md)): a 1 GB pool (in place of the provisional profile's hardware pool) and the 5 GB block (256 MB in front + 4.375 GB alias region + 256 MB stack region + 128 MB heap region) |
| Shared fonts as plain TTFs | host, `hle_sdk16.c` `font_skip` (every game) | Japanese, Korean and Chinese text drew nothing; see [Fonts](#fonts) |
| `shader_groups /Resources/=engine /Data/=data` | `game.tsv` | engine built-ins (`Resources/`: `unity default resources`, `unity_builtin_extra`) go to engine, the player's serialized files to data |
| No `pad_layout` (the 2D default) | `game.tsv` | controller only; see [Input](#input) |

## Memory

- The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true: `nninitStartup` (`main+0x1cd010`) hands application pool − used memory to StandardAllocator as virtual address memory and never calls `SetMemoryHeapSize`, as in Ace Attorney Investigations Collection and Brain Memory Training Academy.
- **Resolved: `StandardAllocator::Initialize` aborted** at `sdk+0x324dec`, called from `nninitStartup` at `main+0x1cd080`: the same `nninitStartup` and the same cut as theirs, on this build's constructor. With the 1 GB pool and the 5 GB block it boots to its title with no red line, and the alias region is mapped up to +1.5 GB with 0.27 GB in use.
- To find the constructor on a new `sdk` build, search its text for the two `mov w8 / w9, #0x3fe00000` words (`08fca752`, `09fca752`); `sdis.py --fn` names the function `AddressRegionManagerC2Emm` from its dynamic symbols.

## Fonts

- **Resolved: Japanese, Korean and Chinese text drew nothing** (the language menu showed three blank lines), Latin text was fine. The game's one font, Jost (a Unity dynamic font), falls back to the OS fonts for those glyphs, and Unity's Switch player loads them from the shared fonts: `main+0x2390b0` maps the names `Switch_Font_STD`, `Switch_Font_CN_Simplified`, `Switch_Font_CN_Extended`, `Switch_Font_CN_Traditional`, `Switch_Font_KR` and `Switch_Font_NIN_Extended` to types 0–5 and hands `GetSharedFontAddress` / `GetSharedFontSize` straight to FreeType's `FT_New_Memory_Face` (`main+0x23b2e0`). The host gave every SDK 11+ game the 8-byte BFTTF header in front, so every face failed to open. The host now gives a game that imports no `ScalableFontEngineHelper::Decode` the TTF itself (`font_skip` in `hle_sdk16.c`, `dm_main_imports` in `loader.c`; the `shared_font` key, which no profile sets, overrides it).
- To see which fonts a Unity game asks for, probe its name mapper's entry: `DM_PROBE=main:2390b0` on 1.0.1 (the function is the one `main` caller of `GetSharedFontLoadState`).

## Input

- Controller only (stick or D-pad to move, A redo, B undo, X reset, + menu): it stays docked at 1920x1080 with the 2D pad layout.

## Files and shaders

- 486 shaders, all Unity built-ins (`Data/Resources/unity default resources`, `unity_builtin_extra`) plus the player's 11 GLSLC outputs in `main`. The whole game draws with three shader pairs. The built-in VR blit's geometry shader does not build.
- **Resolved: the split stopped with "no bundle has a Shader type tree of the same hash to borrow"** (hash `a30e3dcf…`). There are no bundles, and the files carry no type trees. One 18 KB shader bundle of Fire Emblem Engage (2020.3.18f1) carries that hash; its tree is now in `unityfs.py`'s table (`SHADER_2020_3` in `KNOWN_TREES`: per subprogram global and local keyword indices, every subprogram in `m_SubPrograms`, no parameter blobs, keyword names or stage counts), equal to Engage's node for node (2,179 nodes, the alignment flags included). Each of the game's 48 `Shader` objects reads to its exact end. The structs it shares with the 2022.3 layout are written once (`_SHADER_COMMON`), and the 2022.3 layout comes out byte-identical.

## Mac routes

- A fresh save solves Intro 1: `DM_KEYS="f700:A,f1000:A,f1300:A,f1600:A,f2400:Right,f2450:Down,f2500:Down,f2550:Down,f2600:Down,f3000:Up*3/30,f3090:Right*3/30,f3180:Down*2/30,f3240:Up*3/30,f3330:Left*4/30"` (English, the title, into Intro 1 from the hub at about frame 2,700, solved and on to Intro 2 at about frame 3,500).

## Found on this game

General host changes first needed here; each applies to every game:

- **The shared-font address is the TTF** unless the game is on SDK 11 or later and imports `ScalableFontEngineHelper::Decode` (`font_skip`, `dm_main_imports`): [Shared font address: BFTTF header or bare TTF](../../../docs/reference/services.md#shared-font-address-bfttf-header-or-bare-ttf), [dm/games/README.md, Keys](../README.md#keys) (`shared_font`).
- **Unity 2020.3's `Shader` layout written out** (`SHADER_2020_3`, `_SHADER_COMMON`): [Unity Shader layouts written out for players without bundles](../../../docs/reference/shaders.md#unity-shader-layouts-written-out-for-players-without-bundles).
- **Finding the AddressRegionManager constructor on a new `sdk` build**: [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area).

## Open

- The built-in VR blit's geometry shader does not build.
- The A button: docs/development.md says "A redo", the `game.tsv` header "A to confirm".
