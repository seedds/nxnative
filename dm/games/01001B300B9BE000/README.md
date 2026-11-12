# Diablo III: Eternal Collection (`01001B300B9BE000`)

Update 2.7.7.92380 (v1441792), SDK 8.2.0, NVN. Blizzard's own engine (the survey's Unreal 4 marker in `main` is a false one), FMOD, its data as SNO assets in Blizzard's console packages (`CPKs/*.cpk`, `Update/CPKs/*.cpk`: 37 files, 16 GB). Modules `main` and `sdk` only. Three DLC NSPs (indices 100–102) hold a licence and no content.

On the Mac: the autosave notice, the Blizzard logo movie, the gamma screen, the "New on Nintendo Switch" and patch notices, hero creation (the lit 3D heroes and the class preview movies), naming the hero (`DM_KBD`), the main menu, Normal difficulty, the Act I opening cinematic, then the road into New Tristram as a Wizard: walking, Magic Missile, a zombie killed and experience gained, about seven minutes in play. 60 fps throughout (in play frame p50 16.67 ms, p95 17.0 ms, GPU 5.4 ms p50), sound (6-channel AudioOut), no red line; 2.09 to 2.13 GB resident and 1.85 to 1.86 GB of Metal allocations from the start of play (frame 11,100) to its end (frame 35,100). Offline: the menu shows "Disconnected from Diablo Network", as expected. Not run on the iPhone yet.

## What it needs

The provisional profile's guesses held: no key needed changing. One tool change was needed, general (see [Found on this game](#found-on-this-game)).

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM declares 16 MB of system resource: virtual address memory |
| 5 GB layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the game sets no heap; it takes its memory as address regions in the alias region (`nn::os::AllocateAddressRegion`: 0.75 GB from `main+0xa70`, 0.36 GB from `main+0xa36c58` at boot) and maps them. On the Mac they reach alias +3.00 GB in seven minutes of play, within the 4.38 GB alias region |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 at boot |
| `shader_groups /CPKs/=cpks /Update/=update` | `game.tsv` | the packages; see [Shaders](#shaders) |

No `patches.py`: the rewriter needs no cut.

## Shaders

- **Current:** 1,324 (562 vertex, 762 pixel, no compute), from the zlib streams of the packages: [Diablo III's console packages](../../../docs/reference/shaders.md#diablo-iiis-console-packages). The split reads all 16 GB in 26 s.
- The packages start `D4 C3 B2 A1` (version 7) and are not CRI's CPK despite the extension. Each file in them is stored or one zlib stream behind `{u32 decoded length, u16 stored length}`; the game's SNO assets (`0xdeadbeef` headers) are such streams, and the shader assets hold each program as a control section (0x850 bytes) directly followed by its code area. The archive's own tables (bit-packed, with 64-bit name hashes) are not read; the reader is at `main+0x76088c` and `main+0x7611b0` in 2.7.7.
- Each shader is a program of one stage (`#D3@` in `SINGLE_STAGE`): the pairs table pairs every vertex shader with a cover of the pixel shaders' inputs.
- All 34 programs the game asked for on the way to its first menus (dumped with `DM_NVN_SHDUMP` before the reader existed) are among them, 33 in `ClientCommon.cpk`; no draw on the route above asked for a shader the map lacks.

## Mac routes

- Fresh save to hero creation: `DM_KEYS="25:A*4/3"` passes the autosave notice, the logo movie and the gamma screen; the "New on Nintendo Switch" notice comes up at about 45 s and closes with B, then the patch notice with B again (a B pressed while the notice is still fading in does nothing; a B on hero creation is Back). [`smoke.tsv`](smoke.tsv) stops at the first notice.
- Hero creation starts on a random class. A names the hero through the software keyboard: set `DM_KBD=<name>`, or the Mac host waits in its input box. Then A on Start, A on Normal, and Plus then A 2 s later end the Act I cinematic (which of the two did was not checked); the hero stands on the road below New Tristram. The road goes right from the start and then up past a fence; the rock ledge straight up from the start is a dead end.
- Console controls: the left stick moves, A is the first skill (the Wizard's Magic Missile, aimed at the nearest enemy), R a potion.

## Found on this game

General tool changes first needed here; each applies to every game:

- **Diablo III's console packages** (`dm/gfx/tools/d3cpk.py`, `d3cpk_jobs` / `d3cpk_shaders` in `shader_split.py`): [Diablo III's console packages](../../../docs/reference/shaders.md#diablo-iiis-console-packages).

## Open

- Not run on the iPhone. The 5 GB block is near the 6 GB of address space an iPhone without the extended addressing entitlement has; the address regions the game maps there in longer play are not measured.
- Long frames while walking in New Tristram, about four a minute: most are 65 to 131 ms of the game's own CPU work (submit about 3 ms, GPU about 5 ms), a few are first-use pipeline builds (23 to 37 ms). Their cause (streaming SNO assets on the main thread, or the host's file reads) is not measured.
- Only update 2.7.7 is registered; the base game alone (v0) is not checked.
- The archive tables are not read, so no file names or hashes are known; a shader file split across streams would be left out with a red line naming it (none in 2.7.7).
