# Kirby Air Riders (`040077201BE50000`)

A Switch 2 game: update v458752 (display version 1.3.3), SDK 20.5.26 built for the Switch 2 (`Ounce`), NVN2. The engine's namespace is `phx`, its platform layer `Aurora` (`WindApplication`); PhysX 5.2 and NPLN for online play. Modules `main`, `subsdk0` (CUDA), `sdk`; `subsdk1` (NGX) is shipped but `main` imports nothing from it. **Plays on the Mac** (M4 Pro, 48 GB): title, attract mode, menus, a lesson and Air Ride races from start to the results screen, on Floria Fields and other courses (the course is random), with scripted input and sound on. It holds 57 to 60 fps in races after the first run, at about 10 GB resident. No IPA has been built, and a player has not yet played it through a window on this build.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB. The alias region mapped up to +13.78 GB of its 16 GB, 5.0 GB at most at once, through a race; about 10 GB resident, 4.5 GB of it allocated on the GPU |
| `sys_resource 0x2000000` | `game.tsv` | the NPDM declares 32 MB, so virtual address memory is on |
| `modules main subsdk0 sdk` | `game.tsv` | `main` imports 49 `cu*` functions from `subsdk0` (CUDA), nothing from `subsdk1` (NGX) |
| `romfs_marker Asset/assets.cat` | `game.tsv` | the archives' catalog |
| `shader_groups /Asset/=shader /Bin/=shader` | `game.tsv` | where the shaders are |
| `tex_flush_upload`, `tex_dirty_upload` | `game.tsv` | textures the CPU fills while the game plays, without an NVN call over the texels: without them some props and panels are magenta (all-zero ASTC blocks). Binding `game`: no general rule tells a texture written this way from one a shader writes, whose content an upload would replace ([`tex_flush_upload`](../README.md)) |

There is no game-specific patch, and only v458752's build row: the base game, v0, has not been run. The binding of this bring-up is `build` only for that row.

## Files

- A NUP container of 41.8 GB: a base package (v0) and an update package (v458752), each a whole RomFS of 19.5 and 19.8 GB. Packing takes about 6 minutes.
- The RomFS: `Asset/assets_a000.dat` (16.6 GB) and `assets_b000.dat`, with their catalog `assets.cat`; `Asset.Streaming/` (`C93975A2MV.bin`, 4 GB, and 611 `.soundstream` files); `Bin/` (a schema, a config, the upscaler `Nvn2-upper_left.ndsr` and the NRO `Module/EffectScript.nro`); `startup.dat`.
- `assets_a000.dat` holds 194,045 zstd frames, 36.5 GB decoded, of flatbuffers tagged at +4 (`PMAP`, `PMTR`, `PANM`, `PMSH`, `PSND`, `PEFF`, ...).

## Shaders

27,568 in [`shader_map.tsv`](shader_map.tsv): 13,078 pixel, 8,092 compute, 6,322 vertex, 76 geometry. All of them translate. A build takes about 25 minutes on an M4 Pro and 33 GB. See [PMAP records in phx's PDAT archives](../../../docs/reference/shaders.md#pmap-records-in-phxs-pdat-archives).

## The picture

The 3D renders at 1536x864, with the swap chain 2560x1440 and the window crop 1920x1080: shots of the swap chain hold the picture in their top-left three quarters. Lessons play a short movie of the real game, which is a reference for how a scene should look.

Moving objects get their ambient light from eight values per object in a storage buffer (record size 128 bytes, the object's index at +460 of its 480-byte record), which the compute shader `f59db619b5c0be33` works out each frame from probe atlases named in c[4]; the vertex shader `c564233f7917accc` mixes them with a 12x12 RGBA32F cube. Before [the table of handles](../../../docs/reference/shaders.md#a-table-of-handles-longer-than-the-array), most objects' values were black: Kirby was dark red and black underneath, and the ground and props were darker across the course. The far shadow (`WFarShadow`, 4096, 2048 or 1024 square by scene, all three textures on one storage address) is drawn when a scene loads, then its mips by copies level to level; the map in the lesson is the same with and without `render_cond`.

## Driving it on the Mac

`python3 scripts/build_ipa_from_nsp.py <folder> --platform mac` builds the Mac shaders and the host and prints the run command. Headless, through `DM_CTL`: the title is up about 40 s after boot. Attract mode takes over shortly after, so what a press does depends on when it lands: look at a shot before the next press. From a save that has seen the menu: `A` at the title, `A` for the main menu's Air Ride, `A` for Race, `Plus` on the character screen, `Plus` on the take-off screen; the race starts about 30 s later. The first boot opens on the Lessons popup, with `A`, `A` into Lessons, and `A`, `A` into the first one. The machine moves by itself, `LLeft` and `LRight` steer.

## Found on this game

- [PMAP records in phx's PDAT archives](../../../docs/reference/shaders.md#pmap-records-in-phxs-pdat-archives): its shaders, and a loose NDSR file.
- [A guard band cull in every vertex shader (0x80d)](../../../docs/reference/shaders.md#a-guard-band-cull-in-every-vertex-shader-0x80d).
- [Ampere forms with a handle in a register](../../../docs/reference/shaders.md#ampere-forms-with-a-handle-in-a-register-and-sureds-other-operations): TMML (0xf69, 0x36a) in 4,776 pixel shaders.
- [An array of uniform-buffer handles with its sampler word in c2](../../../docs/reference/shaders.md#an-array-of-uniform-buffer-handles-with-its-sampler-word-in-c2): most of its lit materials.
- [The topology table passed to a helper function](../../../docs/reference/shaders.md#the-topology-table-passed-to-a-helper-function): 26 geometry shaders.
- [A table of handles longer than the array](../../../docs/reference/shaders.md#a-table-of-handles-longer-than-the-array): characters with no ambient light, and the dark road and ground.
- [A captured draw's vertex IDs far apart](../../../docs/reference/shaders.md#a-captured-draws-vertex-ids-far-apart): the memory watchdog at 28.3 GB.
- [CPU writes with no flush, caught by page protection](../../../docs/reference/nvn-objects.md#cpu-writes-with-no-flush-caught-by-page-protection-tex_dirty_upload): magenta props and panels.
- [One key for the two forms of the Opus decoder](../../../docs/reference/audio.md#one-key-for-the-two-forms-of-the-opus-decoder): a crash at the first decoder finalized.
- [Whether a vibration device is mounted](../../../docs/reference/services.md#whether-a-vibration-device-is-mounted): a crash soon after boot in a run with a window, not in the headless runs.
- [`nfp`'s extended application area](../../../docs/reference/services.md#nfps-extended-application-area).
- [`nvnTextureBuilderGetViewOffset`, `nvnTextureViewCompare`, `nvnBlendStateSetAdvancedNormalizedDst`](../../../docs/reference/nvn-objects.md#nvntexturebuildergetviewoffset-nvntextureviewcompare-nvnblendstatesetadvancednormalizeddst).

## Open

- A crash seen once in dozens of runs, in a run slowed by another game on the same GPU: the game's own wait at `main+0x2ecff24` (`ldarb` on a thread object at +0x285, in a loop that sleeps between reads) read an alias page just unmapped, right after `[mem] alias region mapped` fell. Not caused by the host's page protection, which does not unmap; whether the host unmaps earlier than Horizon would is not known.
- The 12x12 cube the vertex shader mixes with the probe values holds 4 to 27 with `tex_flush_upload`, and read zeros without it.
- [The guard band cull's](../../../docs/reference/shaders.md#a-guard-band-cull-in-every-vertex-shader-0x80d) real meaning, and the vertex output at `a[0x5c]` that 4,771 vertex shaders write and the translator drops (probably a shading rate, not checked).
- `[RED] nn::codec VP9: a packet refused with 12 pictures waiting for a released buffer` (4 lines at the first movie); the movies seen play.
- A `pso_manifest.tsv` has not been recorded, so the first race compiles pipelines as it goes.
- What `nvnTextureViewCompare`'s return value means is taken from its name.
- City Trial played by hand, Road Trip, online, the base game, iOS: not tested.
