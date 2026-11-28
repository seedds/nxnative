# DRAGON BALL XENOVERSE 2 FOR NINTENDO SWITCH (`010078D000F88000`)

Update 1.26.01 (`v2621440`) over the base game. Nintendo SDK 10.4.1, NVN, Dimps' engine, Havok and CRIWARE. The host loads `main`, `subsdk0` (GLSLC), `subsdk1` (NintendoSDK_movie) and `sdk` with the existing rewriter. The registered build plays offline on the Mac. The unsigned IPA has been built and archive-verified. An iPhone run is still open.

## What it needs

| Setting | Why |
|---|---|
| `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `sys_resource 0x0` | The NPDM declares no system resource. The game requested a 3.14 GiB heap at startup, which fits the 3.25 GiB heap region. |
| `depth_mode zero` | The game sets near depth to zero. The prebuild uses the matching vertex shader entry point. |
| `romfs_marker data2.cpk` | The update's merged RomFS contains 14 CRI CPK archives. The packaged build includes the offline HTML content beside its RomFS. |

## Shaders

The full split has 8,014 keys (1,802 vertex, 6,204 pixel and 8 compute). It reads 3,963 keys from `main` and 4,051 from the `data/adam_shader` members of `data.cpk` and `data2.cpk`. All 8,014 built for Mac and iOS with Xcode 27.2.

## Frame rate

Dimps has separate controls for the base rate and for halving the simulation rate. On 1.26.01, Start Game restores a 60 Hz base rate with the half-rate flag set. The renderer receives 60 Hz for rendering and 30 Hz for simulation, and the game stays at 30 fps with a one-refresh NVN present interval. The flag also doubles the game clock's step. The rewriter selects the full base rate in the flag setter so the game configures its renderer and clock together.

Conton City's initializer has a separate 30 Hz request. It now passes through the incoming base rate that it saves on entry and restores on exit. The character menu and opening movie select their own 30 Hz base rate.

The game profile matches both functions and checks their original instructions in 1.25.02 and 1.26.01. A later build can reuse each patch if its code still matches. The rewriter leaves the shipped timing in place when a match fails.

On an Apple M6 Mac mini at 1600 × 900, 1.26.01 went from 33.44 ms to 16.78 ms median frame time in the opening story and from 33.46 ms to 16.80 ms standing still at the same spot in Conton City (about 30 to 60 fps in both). The opening movie and character selection ran at their requested 30 Hz.

## Mac playtest

The fresh-save check on an Apple M6 Mac mini with macOS 27.2.1 reached region selection, the opening logos and the autosave notice without missing shaders. The first-launch menus ran at about 59.5 fps at 1600 × 900. The user then confirmed offline gameplay. J is A on the Mac keyboard.

Both packages include the 81 DLC images the extractor could read. The local copy of add-on index 10 failed the NCA filesystem hash check and was excluded. Individual DLC gameplay and online play remain open.

## Found on this game

- [BNSH in EMZ archives](../../../docs/reference/shaders.md#bnsh-in-emz-archives).
