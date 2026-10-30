# Bloodstained: Ritual of the Night (`0100BF500207C000`)

Update 1.60 (v851968) over the base game, SDK 7.6.0. ArtPlay / 505 Games, Unreal Engine 4.22 (version 8 paks, version 1 shader code archives) on NVN. Modules `main` (138.6 MB of text), `subsdk0` and `sdk`. No game-specific executable cuts. Plays on the iPhone at 60 fps (30 on the Switch), with the right colours since the texture keys below.

## What it needs

| Setting | Where | Why |
|---|---|---|
| A second patch area | `dm/tools/patch_nso.py` (TWO), the loaders | `main`'s text (138.6 MB) is longer than one patch area reaches with a B |
| FMallocBinned3 found by the rewriter (`dm/tools/ue_malloc.py`, name getter slot 17) and the UE 4.22 slot layout | `dm/host/hle_uemalloc.c` | the constructor reserves 37 GB of address space; the host runs Unreal's allocator on a host malloc zone instead. On 1.60 the chain finds the constructor at `main+0x59f3980`, called at `+0x5b3a868`, and the vtable at `+0xc3b8fa0` |
| Version 8 paks and UE 4.22's version 1 shader code archives | `dm/gfx/tools/uepak.py` | the shader split found no shaders otherwise |
| Engage's 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | 256 MB in front + 4.375 GB alias region + 256 MB stack region + 128 MB heap region; in its first 15 s the game's regions reach alias +2.82 GB |
| `present_interval display` | `game.tsv` | the game asks for present interval 2; Unreal steps by elapsed time, so one frame per display tick plays at normal speed at 60 fps |
| `tex_flush_upload on`, `tex_dirty_upload on` | `game.tsv` | see [Rendering](#rendering) |

## Rendering

- **Resolved: a purple / magenta tint on the characters and props.** Unreal 4.22 streams the top mips of the characters' 256×256 ASTC 6×6 ramp and matcap textures (SPARSE flag 0x20) by writing them from the CPU straight into the texture's storage after the texture was first used, with no NVN copy and no flush. The host kept a GPU-only texture's first, partly written content: all-zero top levels, which ASTC decodes as magenta. Guest memory held the right blocks (`DM_CTL` `passdump`, then `texraw` / `texlist` / `memdump` of the textures bound to Miriam's toon material `978e34f7cd2b4709`, counting zero ASTC blocks). The two texture keys write-protect the registered textures and upload them when the CPU writes. Both are compile-time keys (the host's generated header), so `DM_PROFILE` at run time does not apply them: rebuild the host. Checked on the Mac at the galleon cutscene (59 fps) and on the iPhone, 2026-10-29, and again on the Mac on main `2006d5ed` with these four changes (58 fps), 2026-10-30.

## Versions

- 1.60 (v851968) only. The base game v0 (SDK 5.3, Unreal Engine 4.18, version 4 paks with the shader archive layout before version 1) also played on the iPhone, from a provisional profile on an older main, with other keys (`sys_resource 0x1e00000`, `romfs_marker BloodstainedRotN/Content/Paks/pakchunk0-Switch.pak`) and a fix for the audio renderer event that Unreal destroys right after opening the renderer, which is not in this repository. It has not been rebuilt on this main. A profile that holds both versions needs these keys per build, which `game.tsv` cannot express yet.

## Open

- **`ue_region.py` prints a `[RED]` line on 1.60's `main`:** "0 reservations in nninitStartup (0x5b39e40); 0x5b3a590: x0 is no base variable". UE 4.22's `nninitStartup` passes a stack slot as the region's base (`add x0, sp, #0x10`) and keeps the size in a bss variable (`main+0xcc55aa0`, taken from `+0xcc55aa8` when it is 0, rounded up to 4 KB). It skips the reservation when the size is 0. This is not one of the forms the chain knows, so no gate is placed. The game runs with the profile's memory keys as it did before `ue_region.py` existed. What size it reserves at run time was not measured.

## Driving it on the Mac

- About 79 s from launch reaches the galleon cutscene, where Miriam and Johannes show the texture fix; the cutscene needs + to advance.
