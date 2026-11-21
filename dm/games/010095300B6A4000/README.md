# South Park: The Stick of Truth

Switch update 1.01 (`v65536`), Obsidian's Onyx engine, SDK 4.5.0 and NVN. Tested on an Apple M6 with macOS 27.2.1 through the opening movies, the title menu, character creation and the first bedroom scene. The base version has no registered profile.

## Profile

- The game requests a 3.14 GiB heap. The 5 GiB address space fits it with 256 MiB reserved at the front, a 3.25 GiB heap region, a 256 MiB stack region and a 1.25 GiB alias region. The NPDM declares 16 MiB of system resource. The Mac address-region probe recorded no reservations during the first 15 seconds of boot.
- The existing GLSLC reader extracts 351 distinct shaders, 292 from `main` and 59 from `engine/shaders` in RomFS. The game reads its `OAF!` resource archives and Bink movies directly. Audio uses six-channel SDK AudioOut.
- The prewarm list records the title, character creation and opening house scene. The iPhone starts with the full touch gamepad so the right stick and D-pad are available.

## Run on a Mac

Build the NSP folder with [the Mac development build](../../../docs/development.md#a-mac-only-build-for-development) and run the launch command it prints. Press A at the title prompt and select New Game. After the opening movie, make the five character choices, select Complete and let the opening house scene finish.

## Open

- Device gameplay on iOS.
