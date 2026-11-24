# South Park: The Stick of Truth

Switch update 1.01 (`v65536`), Obsidian's Onyx engine, SDK 4.5.0 and NVN. Tested on an Apple M6 with macOS 27.2.1 through the opening movies, the title menu, character creation and the first bedroom scene. The patched game runs at 60 fps in the bedroom. The base version has no registered profile.

## Profile

- The game requests a 3.14 GiB heap. The 5 GiB address space fits it with 256 MiB reserved at the front, a 3.25 GiB heap region, a 256 MiB stack region and a 1.25 GiB alias region. The NPDM declares 16 MiB of system resource. The Mac address-region probe recorded no reservations during the first 15 seconds of boot.
- The existing GLSLC reader extracts 351 distinct shaders, 292 from `main` and 59 from `engine/shaders` in RomFS. The game reads its `OAF!` resource archives and Bink movies directly. Audio uses six-channel SDK AudioOut.
- The prewarm list records the title, character creation and opening house scene. The iPhone starts with the full touch gamepad so the right stick and D-pad are available.

## Frame rate

- The game requests NVN present interval 1 throughout the tested scenes. Forcing display pacing still gave 30 fps. Its startup configuration first calls the Onyx rate setter with 60, then the game's own caller supplies 30 at `main+0x2e5588`. The setter at `main+0x97b40` stores the rate and its reciprocal. The loop at `main+0x978a8` uses the reciprocal as its minimum interval. It computes the update delta from elapsed system ticks and passes it to the update callbacks.
- [The game-level patch](patches.py) passes 60 through the same setter. The rewriter checks the full caller and its direct callees and verifies the original instruction bytes. The exact same rule applied through the rewriter to base 1.0.0 and update 1.01. Gameplay verification used 1.01.
- In the idle bedroom on the same Mac, median frame time fell from 33.4 to 16.7 ms. The engine's time accumulator advanced by 12.05 seconds in both runs over the same wall-clock interval. The startup Bink logos still presented at about 30 fps.

## Run on a Mac

Build the NSP folder with [the Mac development build](../../../docs/development.md#a-mac-only-build-for-development) and run the launch command it prints. Press A at the title prompt and select New Game. After the opening movie, make the five character choices, select Complete and let the opening house scene finish.

## Open

- Device gameplay on iOS.
- Combat timing and later scenes at 60 fps.
