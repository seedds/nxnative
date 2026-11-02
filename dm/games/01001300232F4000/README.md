# OCTOPATH TRAVELER 0, ASIA (`01001300232F4000`)

Update 1.0.5 (v327680) over the base XCI, SDK 19.3.13. Square Enix, Unreal Engine 5 on its mobile renderer (`SF_GLSL_SWITCH_MOBILE`), NVN, CRIWARE sound and movies. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. This ASIA version has its own title and program ID; `main` is build `e3e401c28d9fd3b1f9e033bb7bb31d42`, while `sdk` (`7bba185a35ee9b4d0af66a4df910a823`) and `subsdk0` (`ca7f78a09c547c54beac3a33ed6bf364`) match the [western release](../01005270232F2000/README.md).

Status: runs successfully on iOS, confirmed by the user on 2026-11-02 with the 1.0.5 test IPA and its separate RomFS image. All 10,324 iOS shaders compiled. The profile targets 60 fps; no device model, measured frame rate, memory figures or detailed gameplay route was supplied. Mac gameplay has not been tested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 3.75 GB guest block: `mem_reserve 0xF0000000`, `mem_alias 0xC8000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | room for the SDK node area, engine address region, later regions, stacks and heap; see [Memory](#memory) |
| FMallocBinned3 constructor cut and engine region gate | engine-wide rewriter rules, `dm/tools/ue_malloc.py` and `dm/tools/ue_region.py` | replace the allocator's large address reservation and size the engine region to the host's available alias space |
| SDK node area cut, 1 GB → 62 MB | `dm/tools/sdk_patches.py`, shared SDK build `7bba185a…` | the same SDK implementation as the western release; no ASIA-specific patch |
| `fps_target 60` | `game.tsv`, `hle_fpslock.c` | writes Unreal's `t.MaxFPS` and `rhi.SyncInterval`; see [Frame rate](#frame-rate) |
| `tex_dyn_slots on` | `game.tsv` | dynamic texture and sampler slots in the mobile renderer; rationale in the [western shader note](../01005270232F2000/README.md#shaders) |
| `depth_mode zero` | `game.tsv` | the mobile renderer's near = 0 depth convention, also used when building pipeline archives |
| `shader_groups ShaderArchive-Global-=global ShaderArchive-=game` | `game.tsv` | separate the engine's global shaders from the game's material shaders |

## Memory

- **Current:** the NPDM declares an 80 MB system resource and uses virtual address memory. The profile uses the western release's memory partition and the shared SDK's node-area cut; its [memory note](../01005270232F2000/README.md#memory) explains the mechanism.
- The rewriter discovers FMallocBinned3 at `main+0x9f480`, its constructor call at `+0x12e40`, and its vtable at `+0x5b09f38`. The engine region's size is stored at `+0x12e20`. These are observations for this build, not fixed address overrides in the profile.
- Device memory use and long-run growth have not been measured for ASIA.

## Frame rate

- **Current:** `fps_target 60`, using the general Unreal console-variable discovery. This build's `t.MaxFPS` is at `main+0x7c768d0`, `rhi.SyncInterval` at `+0x7b35690` and `r.Streaming.PoolSize` at `+0x7c74608`.
- The frame-limit mechanism is covered by the [western frame-rate note](../01005270232F2000/README.md#frame-rate). `DM_FPS_TARGET=30` keeps the original frame limit for lower power use. Actual device frame rate has not been recorded for ASIA.

## Files and shaders

- 370 RomFS files totaling about 3.88 GiB: `Octopath_Traveler0/Content/Paks/pakchunk0-Switch.pak` (the profile's `romfs_marker`), 356 loose CRIWARE `.awb` sound banks and one `.usm` movie, among other files.
- The game's own Oodle decoder decoded 10,596 shader records: 1,219 from the Global archive and 9,377 from the game archive. The split produced 10,324 unique shaders; all compiled for iOS with the locked Metal compiler.
- Shader tables were generated independently from ASIA's files. `shader_pairs.tsv` matches the western release, while package ordering in `shader_map.tsv` differs. The [western file and shader notes](../01005270232F2000/README.md#files) cover the encrypted pak and archive format.
- Both supplied DLC packages (indices 1 and 2) were extracted and bundled as `aoc/1.img` and `aoc/2.img`. Their in-game effects have not been checked separately.

## Rewriter

- **Current:** `main`, `subsdk0` and `sdk` rewrite successfully. The general Unreal rules find the allocator, address region and console variables from the code; this profile needs no `patches.py` or ASIA-specific host changes.
- `main` has a 71.21 MB range without unwind entries, analysed in 298 pieces. Its rewrite finds 78 spin loops.

## iOS testing

- **Confirmed:** the user reports that the ASIA 1.0.5 build runs on iOS (2026-11-02).
- The test build uses `scripts/build_ipa_from_nsp.py --platform ios --romfs separate`, bundle ID `org.nxnative.01001300232f4000`. The IPA contains the rewritten code, iOS shaders and two DLC images; the matching `romfs.img` is copied into the app's Documents directory.
- IPA ZIP CRC validation passed. Its `romfs.id` matches the delivered image (`size=4161860131 index=a3e1685f04afcc3e`). File Sharing is enabled; the executable carries the Increased Memory Limit entitlement for the sideloading tool to preserve.

## Found on this game

No new general host or translator fix was needed. The existing engine-wide rewriter rules and shared SDK cuts cover ASIA's build.

## Open

- Mac shader compilation and gameplay testing remain incomplete; the partial Mac cache was retained when switching to the iOS build.
- Record the device model, measured frame rate, memory use and a gameplay route, including battles, audio, input, saving and loading.
- Check the two DLC packages' in-game effects and the base game without update 1.0.5.
