# XCOM 2 Collection (`0100D0B00FB74000`)

## Status

**Playable on the Mac, confirmed by the user.** Program 0 displays the offline terms page, A advances, and the in-game mode switch works after open users are handed over. A repeatable gameplay route, longer sessions and iOS have not been validated. The profile lists both game programs so the build packs the complete collection, rather than just the 30.4 MB launcher.

## Version and programs

The base package registers Application version 0. The update registers Patch version 131072 (`v131072`). The launcher and program 1 report **1.0.3** in their control data; program 2 reports **1.0.1**. Filenames do not establish those display versions.

| Program | Profile | Control version | RomFS |
|---|---|---|---|
| 0, launcher | this profile | 1.0.3 | 30.4 MB |
| 1 | [0100D0B00FB74001](../0100D0B00FB74001/README.md) | 1.0.3 | 13.32 GiB |
| 2 | [0100D0B00FB74002](../0100D0B00FB74002/README.md) | 1.0.1 | 9.81 GiB |

Program 2 exists only in the update and has a patched data section. The extractor verifies its entire patched data against the integrity tree before selecting a compatible base NCA. [Programs added by updates](../../../docs/reference/packing.md#programs-added-by-updates)

The full splits produced 33,034 shaders for program 1 and 39,939 for program 2, with no unreadable-file reports.

## Remaining validation

- Repeated switching in both directions, save/load and longer gameplay sessions.
- iOS, which has not been tested.

The launcher memory partition comes from its provisional heap check. The child profiles allow the same 3.25 GiB heap, but their actual requirements remain unvalidated. [Multi-program applications](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram)

## Found on this game

- **Resolved in extraction.** The shader split returned zero shaders despite decoding the packages. [Unreal shared control and code arrays](../../../docs/reference/shaders.md#unreal-shared-control-and-code-arrays). Rendering and gameplay remain untested.

- **Resolved on the Mac.** A closed the terms applet but the launcher reopened it. The page calls `window.close()`, whose result must be 4. [Offline applet pages close their browser window](../../../docs/reference/services.md#offline-applet-pages-close-their-browser-window). The user confirmed acceptance after the host update.

- **Resolved on the Mac.** Selecting the in-game mode switch did nothing because no open users were handed over from the previous program. [Open users handed over between programs](../../../docs/reference/services.md#open-users-handed-over-between-programs). The user confirmed switching with the handoff fix.
