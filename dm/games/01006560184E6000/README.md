# Mortal Kombat 1 (`01006560184E6000`)

Update 1.29.0 (`v1900544`) over the base game, SDK 15.3.2 and NVN API 55.15. NetherRealm's Unreal Engine game uses Havok and Wwise; `subsdk0` is NintendoSDK_movie. The supplied `v1835008` add-on package is license-only and contains no game content.

Status: the Mac build completed its shader translation and entered gameplay. The active run held the game's 30 fps cadence and the user reports normal frame rate and audio. Screen tearing remains reported in movies and cutscenes; that issue is open. iOS has not been checked, and the measured address-region use is beyond the profile's iPhone layout.

## What it needs

| Profile setting | Why |
|---|---|
| `modules main subsdk0 sdk` | The game imports `subsdk0`, the NintendoSDK movie module, in addition to `main` and `sdk`. |
| `mem_reserve`, `mem_alias`, `mem_stack`, `app_pool`, `sys_resource` | These follow the NPDM and the game-created heap. The Mac run maps the engine's regions as far as +62 GB in the host's 64 GB alias region; an iPhone layout has not been checked. |
| `depth_mode zero` | The game selects near-zero depth through `nvnDeviceSetDepthMode`, matching the offline pipeline bases. |
| `shader_groups` | The Unreal global and material shader archives are split from the game's containers and `main`. |

The profile leaves the game's own frame cap and present interval in place. The game requests interval 2 on a 60 Hz display; the host's presentation remains synchronized to the display.

## Files and shaders

- The updated RomFS has 502 files and about 42.37 GB. Its Unreal pak sits beside two `.ucas` and two `.utoc` IoStore containers; the generic shader scanner must skip those multi-gigabyte files and read container data only through selected shader jobs.
- The full Mac split produced 123,377 shader keys. The committed tables contain the selected shader map, translation facts, and vertex/pixel pairs. No curated pipeline prewarm seeds have been recorded yet.

## Mac route

- Launch the build with NX Play and continue into a match. Check gameplay, movies, cutscenes, audio and save behavior; the cutscene route and save flow are not yet documented as a repeatable route.

## Testing

- The full Mac-only build completed the shader split and translated shader library, then launched into gameplay. The user's active run reported normal gameplay frame rate and audio.
- Not checked: iOS, online play, campaign completion, individual DLC content, and whether optional frame buffering changes movie/cutscene tearing.

## Found on this game

- [Unreal IoStore containers are scanned only through selected shader jobs](../../../docs/reference/shaders.md#unreal-iostore-containers-are-scanned-only-through-selected-shader-jobs)

## Open

- Movie and cutscene screen tearing is reported by the user. Frame buffering remains an optional setting and is off by default; its effect on this symptom has not been checked.
- iOS and long-session memory behavior remain untested.
