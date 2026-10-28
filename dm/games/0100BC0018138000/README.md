# Super Mario RPG

Title ID `0100BC0018138000`, update 1.0.1 (`v65536`), Unity 2021.3.15f1 IL2CPP, Nintendo SDK 14.3.1, NVN.

## Current status

On the NX2Apple host used for bring-up, the native Mac app reaches the rendered title logo and plays the opening cinematic with the three SDK cuts and a 2 GB application pool. Startup and asset-loading crashes described below are resolved on this route. Current nxnative runtime validation, interactive gameplay, longer sessions and iOS remain unverified. This is a draft bring-up profile, not a full playability claim. Two NVN functions report unimplemented during the opening sequence: `nvnCommandBufferSetSubpixelPrecisionBias` and `nvnCommandBufferSetConservativeRasterDilate`.

## Memory and startup

The profile uses a 5 GB reservation, 4.375 GB alias region, 256 MB stack and NPDM's 16 MB system resource. `nninitStartup` at `main+0x62de90` initializes StandardAllocator over `pool - used`.

The provisional hardware pool (`0xcd500000`) reserves `0xc9400000` bytes. Before the SDK cuts, `CentralHeap::Init` returns ENOMEM (12) after virtual reservation fails with `0x1803`; the uninstrumented host exits 42 before `nnMain`.

SDK build `2587a39ac2547a780ec0e4b77e22afab` has its AddressRegionManager constructor at `sdk+0x12d8a0`. Cuts at `+0x12d8b0` and `+0x12d94c` reduce the node-area limit and occupied length from `0x3fe00000` to `0x3e00000`. A third cut at `+0x12daac` reduces alignment of regions of 1 GB and more from 1 GB to 32 MB.

The three cuts alone still crash at `main+0x627a74` during asset loading: a 32 MB allocation returns null, then the game writes through it. The alias high-water mark reaches +4.25 GB of 4.375 GB while only 0.75 GB is mapped. Its initial 3.14 GB StandardAllocator reservation crowds out Unity's separate virtual pools. The profile reports a 2 GB pool (`app_pool 0x80000000`), reducing that initial reservation and leaving room for Unity. A native run then reaches the title logo and opening movie instead of crashing within the first few seconds. The installed app applies the same setting through `Resources/dm/env.txt` (`DM_PROF_APP_POOL`); rebuilt hosts take it from the profile.

The build-ID-gated cuts assert each original instruction. The bring-up run passed 7,500 frames with no crash markers. The current upstream host also has an automatic StandardAllocator cap, but the default room is about 4.06 GB in this layout, above this game's 3.14 GB request, so it does not reduce that reservation. The tested 2 GB application-pool setting is retained; its behavior on a freshly built upstream host still needs runtime validation.

## Found on this game

- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area): newly registered SDK build.
- [Sizing the pool and alias region for virtual address memory](../../../docs/reference/memory.md#sizing-the-pool-and-alias-region-for-virtual-address-memory): a 2 GB pool balances StandardAllocator with Unity's separate address reservations.

## Open validation

Build and test current nxnative, then verify interactive gameplay, scene transitions and longer sessions before claiming full playability. Existing shader libraries were reused; no shaders were rebuilt for the memory fix.
