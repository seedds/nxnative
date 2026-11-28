# Mortal Kombat 1 (`01006560184E6000`)

Update 1.29.0 (`v1900544`) over the base game, SDK 15.3.2 and NVN API 55.15. NetherRealm's Unreal Engine game uses Havok and Wwise; `subsdk0` is NintendoSDK_movie. The supplied `v1835008` add-on package is license-only and contains no game content.

Status: a prior Mac run entered gameplay. The user reports dropped frames during fights and effects, plus flicker/tearing in movie scenes. Runtime telemetry showed first-use shader and pipeline compilation causing multi-hundred-ms to 1.15-s hitches; the profile seeds 7,600 recorded entries. The acquired-image synchronization fix is now upstream ([PR #3372](https://github.com/yubismartlabs/nxnative/pull/3372)), but did not resolve movie flicker. iOS has not been checked, and measured address-region use exceeds the profile's iPhone layout.

## What it needs

| Profile setting | Why |
|---|---|
| `modules main subsdk0 sdk` | The game imports `subsdk0`, the NintendoSDK movie module, in addition to `main` and `sdk`. |
| `mem_reserve`, `mem_alias`, `mem_stack`, `app_pool`, `sys_resource` | These follow the NPDM and the game-created heap. The fresh Mac check measured regions through alias offset +62.06 GB inside the 64 GB Mac region; an iPhone layout has not been checked. |
| `depth_mode zero` | The game selects near-zero depth through `nvnDeviceSetDepthMode`, matching the offline pipeline bases. |
| `shader_groups` | The Unreal global and material shader archives are split from the game's containers and `main`. |

The profile leaves the game's frame cap and present interval in place. The game requests interval 2 on a 60 Hz display.

## Files and shaders

- The updated RomFS has 502 files and about 42.37 GB. Its Unreal pak sits beside two `.ucas` and two `.utoc` IoStore containers; the scanner reads container data through selected shader jobs.
- The fresh split contains 123,377 shader keys. The committed tables contain the selected shader map, translation facts, and vertex/pixel pairs. `pso_manifest.tsv` keeps the 7,600-entry live gameplay capture; all 1,419 entries from the later fresh boot capture were already in that seed.

## Mac route

- Launch the build with NX Play and continue into a match. Check gameplay, movies, cutscenes, audio and save behavior; the cutscene route and save flow are not yet documented as a repeatable route.

## Testing

- The prior Mac-only build entered gameplay. Its empty-seed run recorded 6,113 first-use shader and pipeline entries into the runtime cache. At frames 28,772 and 28,776, first-use compilation took about 1.08 s per frame (44/39 new pipelines and 38/35 shader libraries); GPU time was 15.8/7.7 ms.
- The Nov 26 seeded run prewarmed 2,929 selected entries from 7,340 manifest rows with 8 workers in 1.5 s; it prepared 2,469 render pipelines and 460 compute pipelines, with no failures. The live run grew the capture to 7,600 unique rows. The later frame-31,800 window measured p50 33.58 ms and p95 38.14 ms while GPU time was p50 6.16 ms and p95 8.59 ms, with no pipeline builds. It also logged a system memory pressure warning and about 100 MB free near frame 28,800.
- A separate clean bring-up on Nov 27 used fresh survey outputs and the earlier translation chain `b-8f3ad22a5c0c`. It reused all 123,377 libraries for profile input digest `3bf617487316`, reached `nnMain`, and ran 2,000 frames on Apple M2 near the game's 30 fps cadence with zero audio underruns. It applied no manual gameplay input. Its 1,419-row first-use capture is already covered by the 7,600-entry seed. This check predates the current upstream translation chain `a05cb08bf288`; a full build on the current chain remains to be done.
- The clean boot log reported `Bink: Mismatched number of tracks opened`. Movie behavior and the movie-specific flicker cause remain open. The final 7,600-row seed has not been tested after a restart.
- Not checked: iOS, online play, campaign completion, individual DLC content, and long-session memory behavior.

## Found on this game

- [Unreal IoStore containers are scanned only through selected shader jobs](../../../docs/reference/shaders.md#unreal-iostore-containers-are-scanned-only-through-selected-shader-jobs)

## Open

- Movie and cutscene flicker/tearing remain reported despite the acquired-image synchronization fix; the movie-specific cause is unknown.
- Frame pacing still misses the 33.3 ms target in later windows without pipeline builds.
- iOS and long-session memory behavior remain untested.
