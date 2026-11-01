# Brain Memory Training Academy (`0100F5D01DA3A000`)

Base game 1.0.0 (v0), SDK 16.2.4. Unity 2022.3.5f1 IL2CPP with FMOD on NVN. Modules `main`, `subsdk0` (the movie module) and `sdk`; the `sdk` is Unicorn Overlord 1.05's build ([010069401ADB8000](../010069401ADB8000/README.md)). Plays on the iPhone. On the Mac: the title, the controls screen, the level select and the levels with their Level Complete / Level Failed screens, at 60 fps with real sound and saves, no red line; about 0.75 GB resident, flat over 20 minutes.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `12f16103…` (the constructor `AddressRegionManagerC2` at `sdk+0x134768`; the movs at `+0x134778` and `+0x13480c`) | `StandardAllocator::Initialize` aborted in `nninitStartup`; see [Memory](#memory) |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x40000000` | [`game.tsv`](game.tsv) | Ace Attorney Investigations Collection's layout ([010005501E68C000](../010005501E68C000/README.md)): a 1 GB pool and the 5 GB block (256 MB in front + 4.375 GB alias region + 256 MB stack region + 128 MB heap region) |
| `shader_groups /Resources/=engine /Data/=data` | `game.tsv` | engine built-ins (`Resources/`: `unity default resources`, `unity_builtin_extra`) go to engine, the player's serialized files to data |
| No `pad_layout` (the 2D default) | `game.tsv` | controller only; see [Input](#input) |

## Memory

- The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true: `nninitStartup` (`main+0x73ad60`) hands application pool − used memory to StandardAllocator as virtual address memory and never calls `SetMemoryHeapSize`, as in Ace Attorney Investigations Collection.
- **Resolved: an `nn::diag` abort at `sdk+0x126490`** (`StandardAllocator::Initialize`, called from `nninitStartup` at `main+0x73adcc`); the NSP script's heap check reported "the host stopped before nnMain". The `sdk` build is Unicorn Overlord's, whose NPDM declares no system resource, so it never needed the cut; Unicorn Overlord runs the same with it. The constructor takes it at `+0x134778` / `+0x13480c`.
- Measured with probes on `VammManager::AllocateAddressRegion`'s success path (`sdk+0x1348b8`: x21 size, x22 address) and `FreeAddressRegion` (`sdk+0x135100`): StandardAllocator takes 960 MB at +64 MB (it ends at +1.0 GB), and Unity's own regions (`main+0x11bb4cc`) hold about 0.54 GB more. Over four minutes of level loads the live regions end at +2.0 to +2.1 GB of the 4.375 GB alias region. In 20 minutes the mapped high-water mark stopped at +2.51 GB (about 4 minutes in), at most 0.5 GB mapped, about 0.75 GB resident on the Mac. The bring-up run (five minutes of level loads) had the high-water mark at +2.25 GB.
- With this layout the game boots to its title in 5 s with no red line.

## Input

- Controller only (stick or D-pad, A, B, +): the game never reads the touch screen, so it stays docked at 1920x1080 and the virtual gamepad starts in the 2D layout.

## Files and shaders

- 496 stage shaders in the player's serialized files (`Data/*.assets`, `Data/Resources`) and `main`: 439 engine built-ins, 46 of the game's, 11 of the player's in `main`, and the built-in VR blit's geometry shader, which does not build.
- **Resolved: the split stopped with "no bundle has a Shader type tree of the same hash to borrow".** There are only `Data/*.assets`, `level*` and the two engine files in `Data/Resources`, none with type trees, and no bundle. The `Shader` type hash there (`ea424a5f…`) is the one in Ace Attorney Investigations Collection's bundles (2022.3.17f1): the same layout. `unityfs.py` now carries that layout written out (`KNOWN_TREES`: about 30 structs; `tree_from_layout` builds the tree a file with type trees would have), and the split's second pass gives it to the stripped files when no bundle lent a tree of that hash.
- It calls `GetSharedFontAddress` without importing `ScalableFontEngineHelper::Decode`, so the host gives it the TTF itself (`font_skip`, see [Patrick's Parabox](../0100E44016C22000/README.md)).

## Mac routes

- `DM_KEYS="f400:A,f700:A,f1000:A,f1200:Right,f1300:A"` starts a new game, enters level 1 and clears it (Level Complete at about frame 1,350).

## Found on this game

General changes first needed here; each applies to every game:

- **Written-out `Shader` layouts for Unity players with no bundle to borrow a tree from** (`KNOWN_TREES`, `tree_from_layout`, given in the split's second pass): [Unity Shader layouts written out for players without bundles](../../../docs/reference/shaders.md#unity-shader-layouts-written-out-for-players-without-bundles).
- **The node area cut on an `sdk` build whose first game never needed it** (Unicorn Overlord's): [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area).

## Open

- The built-in VR blit's geometry shader does not build.
- `game.tsv` has no `sys_resource` row, so the host reports its default, 32 MB (`game.py`, `hle_svc.c`), where the NPDM declares 16 MB.
