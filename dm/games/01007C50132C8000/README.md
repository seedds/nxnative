# Ace Angler: Fishing Spirits (`01007C50132C8000`)

Switch update 1.2.3 (`v589824`), Nintendo SDK 13.3.5 and NVN API 55.15. The game uses Unity 2021.2.17f1 IL2CPP, CRIWARE archives and NintendoSDK_movie. The existing rewriter handles the three modules with SDK-wide rules.

The Apple M6 Mac on macOS 27.2.1 reached the Joy-Con notices, the title screen and the first-run license agreement from a fresh save at 1920 × 1080. The title at rest had a median frame time of 16.67 ms, with stereo audio through CoreAudio and no `[RED]` lines on that route.

## What it needs

| Setting | Why |
|---|---|
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `sys_resource 0x1000000` | NPDM specifies 16 MiB of system resource. The game uses virtual address memory and makes no `SetHeapSize` call at startup. |
| `stdalloc_cap 0x80000000` | The uncapped SDK allocator reserved 3.12 GiB and mapped 0.43 GiB in the first 15 seconds. Unity's later reservations reached +4.51 GiB, beyond the iPhone's 4.375 GiB alias region. With the cap and the iPhone-sized layout, live regions reached +3.36 GiB after 35 seconds on the Mac. The reported Switch application pool is still 3,285 MiB. |
| `depth_mode zero` | The game sets near depth to zero. The pipeline prebuild and live renderer use the same vertex shader entry point. |

## Shaders

The update has 24,809 UnityFS bundles in 73 CRI CPK archives. The split finds 31,933 distinct stages, including 108 compute and two geometry shaders. The old split found 1,522. The split took 18 seconds with the fix and 12 seconds before it on the same Apple M6. All stages compiled for Mac and iOS with Xcode 27.2.

## Found on this game

- [Unity asset bundles inside CRI CPK archives](../../../docs/reference/shaders.md#unity-asset-bundles-inside-cri-cpk-archives).

## Mac controls

Press J (A) to confirm the Joy-Con safety notice and at the title prompt. K is B and Return is +. The Mac performance HUD is enabled with `DM_METAL_HUD=1`.

## Open

- Gameplay past the first-run license agreement, physical Joy-Con input, multiplayer and iPhone gameplay still need testing.
