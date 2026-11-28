# Devil May Cry 3 Special Edition (`01007B600D5BC000`)

Version 1.0.1 (v65536), Nintendo SDK 8.2.0, NVN and FMOD. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The SDK and movie module builds are the same as [Link's Awakening 1.1.0](../01006BB00C6F0000/README.md).

## What it needs

| Setting | Evidence |
|---|---|
| 4 GiB guest reservation with 256 MiB each for alias and stack | The NPDM declares zero system resource and the game requests a 3 GiB heap at boot. |
| `depth_mode zero` | The game sets the NVN window to upper-left origin and uses zero-to-one depth. The offline Metal pipeline archive uses the same depth mode. |
| `pad_layout full` | Exposes the D-pad on iOS for [Free Style's style switching](https://news.capcomusa.com/brasil_unity/blog/2020/01/17/modo-free-style-confirmado-em-dmc3se-para-switch). |

## Files and shaders

- The RomFS has 189 files under `data/dmc3/`. The ZIP archive `dmc3-0.nbz` contains 4,937 members, mostly PAC and PACK files. Movies and FMOD banks are loose.
- The 74 unique shaders (32 vertex, 42 pixel) are embedded BNSH programs in `main`. The existing shader splitter reads them and they all compile for Metal.

## Mac testing

- On an Apple M6 with macOS 27.2.1 and Xcode 27.2, the game passed the intro movies, rendered the opening cutscene and reached the first playable fight in Mission 1. It presented at about 60 fps and used about 1.2 GiB resident after pausing. The log had no missing shaders or `[RED]` lines through that point.
- From a new save, use A on the save and privacy notices, + to skip the title movie, then A for New Game and to confirm the tutorial, Gold and Original mode options. Advance through the tutorial pages, choose Mission Start, then dismiss the controls overlay to begin the fight.

## Found on this game

- [Flushing an AudioOut at the end of a movie](../../../docs/reference/audio.md#flushing-an-audioout-at-the-end-of-a-movie) fixes the crash as the intro closes its audio output.

## Open

- The unsigned iOS build includes the RomFS. iPhone and iPad gameplay, later missions and Free Style remain unverified.
