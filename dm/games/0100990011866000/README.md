# Aokana - Four Rhythms Across the Blue (`0100990011866000`)

Update 1.01 (v65536) over the base cartridge, SDK 9.4.1. Unity 2018.4.22f1 IL2CPP with FMOD, on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Brought up on the Mac on 2026-11-04: boot, the disclaimer, the opening movie, the title, a new game into its first story lines, a save and its load, at 60 fps (2,400 frames at a p50 / p95 of 16.66 / 17.01 ms, 0 `[RED]` lines, 0 audio underruns, 1399 MiB). An iPad with an Apple M1 GPU ran the app too ([below](#ipad)). No rewriter cut and no host change was needed: the provisional profile's guesses boot and play as they are.

Build IDs: `main` `2ee74c89e223264e9456c118d0f3e361`, `subsdk0` `fbb0b46fb4fdbf4be47b76f2285f53ed`, `sdk` `290c9a04234847a176aa1c375c16af99`.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `subsdk0` is NintendoSDK_movie, which plays the RomFS's 17 MP4s through the host's movie pipeline; the opening movie decodes 1920×1080 H.264 with AAC stereo at 48 kHz ([Movies](../../../docs/reference/movies.md)) |
| the profile's other keys | [`game.tsv`](game.tsv) | the provisional profile's static guesses (`build_ipa_from_nsp.py` / `profile_guess.py`), unchanged: they boot and play |
| `depth_mode zero` | [`game.tsv`](game.tsv) | the heap check's boot reaches the game's `nvnDeviceSetDepthMode` with near = 0, so the pipeline bases the game can prebuild before that call, and the Mac prebuild, take the `main0_z` vertex entries |
| default present interval | none | the game sets interval 1 at the title and story and 2 while the opening movie plays (`the game sets present interval 1 → 2`, then `2 → 1`), which is the Switch's own pacing |

The heap check found no heap to set: the game's own `SetMemoryHeapSize` is satisfied by the profile's 5 GB block and 4.38 GB alias region. In its first 15 s in `nnMain` its address regions reach alias +3.21 GB; what play after the first story lines takes is not measured.

## Shaders

`shader_split.py` reads the Unity 2018.4.22 Shader objects of `Data/StreamingAssets/data.unity3d` through `unityfs.py`: 604 shaders, all 604 compiled for the Mac. The tables the split writes ([`shader_map.tsv`](shader_map.tsv), [`shader_facts.tsv`](shader_facts.tsv), [`shader_pairs.tsv`](shader_pairs.tsv), [`pso_pairs.tsv`](pso_pairs.tsv)) are this folder's; `shader_groups` and `shader_note` keep the provisional profile's values, since both are shader inputs. [`pso_manifest.tsv`](pso_manifest.tsv) is a recording of one Mac run over the disclaimer, the opening movie, the title and a new game's first lines (15 shaders, 7 render pipelines).

## Mac routes

- With no input the boot shows the disclaimer, then the sprite and NekoNyan logos, then the opening movie.
- `DM_KEYS="10:A,14:A,18:A,22:A,26:A,30:A,36:A"` dismisses the logos and the movie, takes Game Start at the title (frame 1200 of a headless run) and advances the first story lines (frame 1500 on).
- `DM_NVN_SHOT=1200,1500:shot_%d.png` gives the title screen and the first line.

## iPad

An IPA with this profile's values, built from a local tree a few commits behind this one (the Metal 4 specialization descriptor fix, #1213, included), installed in LiveContainer, with the launcher's Metal 4 pipelines on (`DM_PSO_MTL4=1`), on an iPad with an Apple M1 GPU (the log names the GPU, not the model or the iPadOS version):

- Two sessions (3000 and 4200 frames): frame time p50 16.7 ms, p95 17.7 / 18.7 ms; GPU p50 1.1–1.7 ms; footprint 1.1–1.4 GiB; 0 audio underruns; the opening movie played; 0 crashes. Both ended by switching away from the app, with audio and graphics suspended, not by the game's own exit.
- The only `[RED]` lines are `pulling library … not usable` (5 and 6 keys), which an IPA built without `IOS_PULL=1` is expected to log (the pulling libraries are not in the app).
- The second session found the 7 pipelines the first had written to its `pso_user.tsv` (25 shaders) and spent 0.0 ms on the submit thread for pipelines.

## The title loop

The title loop's guest CPU is mostly its BGM: in a 10 s `sample` of the title, 27 samples are in `NVorbis.VorbisCodebook$$DecodeScalar` and 6 in `NVorbis.BufferedReadStream$$Seek` (Il2CppDumper and `dm/tools/il2cpp_sym.py`), i.e. NVorbis, the C# Ogg Vorbis library, which IL2CPP compiled into `main`; the other samples are libil2cpp runtime or C helpers with no script method. FMOD is in `main` too but does not appear in the title's music path in that sample. The title and menus held 60 fps.

## Threads

Long automated play found a host bug: three of five sessions crashed at `BufferedReadStream.Seek` on FMOD's stream thread, after 619 to 1,501 steps (about once an hour), while IL2CPP's collector had the world stopped ([A pause acknowledged past its resume](../../../docs/reference/threads.md#a-pause-acknowledged-past-its-resume)). With the fix, 69 minutes of play through the story (6,331 steps) had no crash.

## Open

- Play past the first story lines was not tried, nor the System and Extra screens or the menus' other pages.
- Input was only A, on the Mac from `DM_KEYS`; a controller and the touch gamepad were not tried.
- An iPhone was not tried. The iPad sessions ended by leaving the app, so its quit path was not measured.
