# Pikmin 2 (`0100D680194B2000`)

Base game 1.0.0 (v0), SDK 14.3.0. Nintendo European Research & Development's (NERD) proprietary GameCube layer (**Hagi** / **VirtualFlipper**), on **Vulkan**. The PowerPC code is statically recompiled into `main` (`Statifier true`), with no JIT. Hagi emulates the GameCube hardware: Flipper GX on Vulkan with run-time SPIR-V translation, AX DSP audio streaming from `rom://pikmin2/AudioRes/Stream/`, and emulated GameCube flash memory / save cards. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Plays on the Mac at 60 fps.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `op_mode handheld` | [`game.tsv`](game.tsv) | standard handheld layout; docked uses TV scaling |
| `pad_layout full` | `game.tsv` | the GameCube controller layout mapped to the Switch Pro / full controller |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk1`–`subsdk4` are replaced by the host's Vulkan layer |
| 9 GB block: `mem_reserve 0x240000000`, `mem_alias 0x150000000`, `mem_stack 0x10000000` | `game.tsv` | Hagi calls `AllocateAddressRegion` for 4.002 GB (`0x100200000`) to map the entire 32-bit GameCube address space; see [Memory](#memory) |
| Cut at `main+0x2ba5b4` | [`patches.py`](patches.py) | `_ZN5movie` destructor unconditionally calls `AbortImpl` during video teardown; NOP'd to allow clean cutscene transitions |
| Node area cut 1 GB → 62 MB + null heap guard | `dm/tools/sdk_patches.py`, `sdk` build `2aef2cffb104ee7c9e212fd2f4c731e9` | AddressRegionManager node area compaction and early return on uninitialized `heap_base` in `CentralHeap::Free` |

## Memory

- NPDM declares 8 MB of system resource. The game calls `SetHeapSize` for a 2.64 GB heap (`0xa8e00000`), so the heap region is 2.75 GB.
- **GameCube Virtual Memory mapping:** During `VirtualMachine::Initialize` (`main+0x49eb20`), `Memory::Initialize` (`main+0x2a3154`) invokes `nn::os::AllocateAddressRegion` requesting `0x100200000` bytes (4.002 GB) to map the 32-bit GameCube address space. In a default compact guest configuration, this request fails with `0x1803` (`ResultOutOfMemory`), cascading into virtual machine destruction. Sizing `mem_alias` to `0x150000000` (5.25 GB) and `mem_reserve` to `0x240000000` (9 GB) gives both the SDK AddressRegionManager node area and Hagi's GameCube region ample contiguous address space.
- `CentralHeap::Free` (`sdk+0x11de60`): early deallocation before heap initialization dereferences null `heap_base` (`x8 == 0`); patched with `cbz x8, #0x11df44`.
- Footprint on the Mac: ~1.8 GB resident, flat past Day 1 Valley of Repose.

## Vulkan & Graphics

98 `vk*` imports through the host's Vulkan layer (`hle_vk.m`), with Hagi generating SPIR-V shaders dynamically at run time. The host compiles pipelines directly to Apple Metal.
- Handheld mode viewport scaling preserves the 16:9 Switch presentation with letterboxing on wider Mac displays.
- Stable 60 fps pacing maintained across title sequence, pre-rendered VP9 cutscenes, and in-game 3D exploration.
