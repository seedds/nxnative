# Ace Attorney Investigations Collection (`010005501E68C000`)

Update 1.0.0.2 (v131072) over the base game, which is an XCI; its DLC NSP is a licence only. SDK 17.5.3. Unity 2022.3.17f1 IL2CPP on NVN. Modules `main`, `subsdk0` (the movie module, `NintendoSDK_movie`) and `sdk`. Plays on the iPhone; 60 fps, as on the Switch (present interval 1). On the Mac it was brought up through the first-boot settings, the title, Investigations 1's menus and Episode 1's opening and first investigation up to the first Logic prompt (25 minutes with no red line). Much of the bring-up was worked out first by armyja in a fork (GitHub issues #17, #18, #19 and pull requests #15, #16).

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `311deab1…` (the AddressRegionManager constructor at `sdk+0x139458`; the movs at `+0x139468` and `+0x1394fc`) | without it `StandardAllocator::Initialize(nullptr, pool − used)` (`main+0x874b6c`) aborts at its first `CentralHeap::Init`; see [Memory](#memory) |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000` | [`game.tsv`](game.tsv) | Engage's block: 256 MB in front + 4.375 GB alias region + 256 MB stack region + 128 MB heap region (the game never calls `SetHeapSize`) |
| `app_pool 0x40000000` (1 GB) | `game.tsv` | with Engage's 2.5 GB pool the live address regions reached the end of the alias region after 140 s; see [Memory](#memory) |
| `shader_groups _unitybuiltinshaders_=engine /Resources/=engine /StreamingAssets/=assets /Data/=data` | `game.tsv` | engine built-ins (the `unitybuiltinshaders` bundle, `Resources/`) go to engine, the player's serialized files to data, the other Addressables bundles to assets |

## Memory

- The NPDM declares 16 MB of system resource, so `IsVirtualAddressMemoryEnabled` is true: `nninitStartup` (`main+0x874b00`) hands application pool − used memory to StandardAllocator as virtual address memory, one 1 GB-aligned address region in the alias region, and never calls `SetMemoryHeapSize`. Unity's own allocator (`main+0x40f1d4` → `AllocateAddressRegion`) reserves its pools beside it, each aligned by its size.
- **Resolved: `StandardAllocator::Initialize` aborted in `nninitStartup`.** SDK 17's AddressRegionManager takes about 1 GB of the alias region as its node area first (the same node area as Engage's SDK): `CentralHeap::Init(nullptr, pool − used)` got no 1 GB-aligned region for 3.14 GB after it. The constructor (`sdk+0x139458`: limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte nodes, the area then registered as occupied) takes Engage's cut to 62 MB.
- **Resolved: then the alias region filled in minutes.** With Engage's 2.5 GB pool the game ran, but probes on `VammManager::AllocateAddressRegion`'s success path (`sdk+0x1395a8`: x21 size, x22 address) and `FreeAddressRegion` (`sdk+0x139df0`), with commits from the `MapPhysicalMemory` log, over the first five minutes (the title, Episode 1's opening, the first investigation) showed StandardAllocator reserving 2.44 GB and committing only 0.19 GB, Unity's pools holding 1.06 GB, and the live regions ending 4.37 GB into the 4.375 GB alias region after 140 s. Over ten minutes of the same route, a 1.5 GB pool (StandardAllocator 1.44 GB at +0.75 GB) let them reach +3.91 GB, a 1 GB pool +2.85 GB.
- **Current:** the pool is reported as 1 GB. StandardAllocator gets 0.94 GB (five times its use) right after the node area, and running out of address space is a crash, while a pool a little smaller than what the game maps in all (1.02 GB at the peak) changes nothing: the host does not enforce it and only `nninitStartup` reads it. This game is why `mem_map_limit` is off by default ([dm/games/README.md, Keys](../README.md#keys)).
- On the Mac about 1.55 GB resident and 1.25 GB of Metal allocations, flat from the first investigation on (25 minutes; address space at +2.81 GB).

## Rewriter

- **Resolved: a `jump table <addr>` assertion on a `br x18` with no table in front of it** (`main+0x1d271bc` and `+0x204ebec`). Not a jump table but an indirect tail call: an IL2CPP thunk loads the method pointer with `ldr x18, [x0, #0x18]`, moves its stack arguments with every other scratch register (`ldr x16, [sp]`, `ldp x17, x15, [sp, #8]`, …) and ends with `br x18`. The rewriter now turns it into `x16 = guest x18; br x16` (`tail_call` / `site_br_call`).

## Services

The C# file API and the real-time clock reach the SDK's C library (nnmusl), not `nn::fs` / `nn::time` directly.

- **Resolved: Addressables never started, then a null asset crash (`main+0x31100ec`).** The `DM_FS=1` output had no `StreamingAssets/aa/settings.json` at all. Unity 2022's C# file API goes `File.Exists` → `SystemNative_Stat` → `stat` = `open` + `fstat` + `close` (`sdk+0x948b0`). nnmusl's path check (`sdk+0x904e0`, then the converter `sdk+0x8ef50`) asks `nn::fs::IsPathMounted`, `GetPathLengthMax`, `GetUtf16PathLengthMax`, `GetEntryNameLengthMax` and `GetUtf16EntryNameLengthMax` before it calls `nn::fs::GetEntryType` / `OpenFile`, and the SDK's own versions read the empty mount table (`IsPathMounted` false; the next one aborted with Result 0x35f202). The host answers them from its mount table, with Horizon's 0x300-byte limits and no UTF-16 limit (`hle_fs.c`). The path .NET builds is `/rom:/…`; nnmusl's converter turns it back into `rom:/…`. armyja's fork (GitHub issue #18) patched the managed stat wrappers by address instead; the hooks need no per-build table.
- **Resolved: `std::terminate` (`Break` from `sdk+0x5bfa48`) at the first real-time read**, right after the first-boot dialog (`main+0x3f3f68` → `std::chrono::system_clock::now()`). `clock_gettime(CLOCK_REALTIME)` calls through a source pointer (`sdk+0xb05010`) that `nntimeStartup` sets once `time::Initialize` succeeds: 1 = `nn::time::GetStandardUserSystemClockCurrentTimespec`, 2 = the adjustable clock's (`sdk+0x8e414`). Both asked the time service; they are host take-overs now (`hle_time.c`), so the C library's real-time clock is the host's clock.
- Not needed in the end: the process heap of pull request #16 (`nninitStartup` calls `SetMemoryHeapSize` / `AllocateMemoryBlock` only without virtual address memory, and nothing else in `main` calls them) and the tolerant common string table of pull request #15 (1.0.0.2's `main` has the table intact at `main+0x46169ac`, 109 entries ending at `Hash128`).
- It calls `GetSharedFontAddress` without importing `ScalableFontEngineHelper::Decode`, so the host gives it the TTF itself (`font_skip`, see [Patrick's Parabox](../0100E44016C22000/README.md)).

## Files and shaders

- 4,951 stage shaders: 2,475 vertex / pixel pairs (4,294 in the player's serialized files, 517 built-ins, 140 in the Addressables bundles) and one geometry shader, which Metal cannot express (its build fails). Unity's player also has 11 GLSLC outputs of its own in `main` beside the game's shaders.
- **Unity 2022 `Shader` objects:** the program code keeps the 0x60-byte stage header, but from Unity 2022 the blob also holds each program's parameter tables as entries of their own (the parsed form's `m_ParameterBlobIndices`; their bytes start `PGlobals`), and the player's programs are listed in `m_PlayerSubPrograms[tier][].m_BlobIndex` (`m_SubPrograms` is empty); `unity_code_blobs` keeps only those. Subprogram version 202012090 (from Unity 2021.2) has no local keyword list, so `unityfs.subprogram` reads no second keyword array (with one it read the code length as a keyword count).
- The player's serialized files have no type trees and the Addressables bundles hold no `Shader`, except the `unitybuiltinshaders` bundle, whose tree has the same hash and is borrowed in the split's second pass. armyja's issue #17 proposed UnityPy and a reader of its own; neither was needed.
- Its `Shader` type hash (`ea424a5f…`) is the one Brain Memory Training Academy's stripped files carry; the Unity 2022.3 layout now written out in `unityfs.py` (`KNOWN_TREES`) was checked against this game's own tree: the same 2,324 nodes with the same alignment flags, and its 40 bundle `Shader` objects decode identically with either. The written-out tree is given only in the second pass: taken in the first, this game's player files came before its `unitybuiltinshaders` bundle and the built-ins' source column and package indices moved; with the second pass its tables come out byte-identical.
- Binding layout as Engage's (separate textures and samplers): all 4,950 translatable shaders decode every binding (`shaders with a binding whose slot was not decoded 0`), and 190 read `c2` rows as data.

## Mac routes

- A fresh save through the first-boot notices (the privacy prompt's second button is "Next"), the title and Investigations 1's Episode 1: `DM_KEYS="f600:A~0.3,f1200:A~0.3,f1800:A~0.3,f2400:A~0.3,f3000:Down~0.3,f3200:A~0.3,f3800:A~0.3,f4400:A~0.3,f5000:A~0.3,f5600:A~0.3,f6200:A~0.3,f6800:A~0.3*270/120"`. The first investigation in Edgeworth's office comes at about frame 18,000; at about frame 30,000 the first Logic prompt waits for L.

## Found on this game

General host changes first needed here; each applies to every game:

- **Indirect tail calls through `x18`** (`tail_call` / `site_br_call`), tried only where the rewriter used to stop: [Indirect tail calls through x18 in IL2CPP thunks](../../../docs/reference/rewriter.md#indirect-tail-calls-through-x18-in-il2cpp-thunks).
- **The SDK 17 C library's path checks** (`IsPathMounted` and the length-limit queries, `hle_fs.c`) and **the C library's real-time clock** (`hle_time.c`): [C library path checks ask the file system's limits](../../../docs/reference/services.md#c-library-path-checks-ask-the-file-systems-limits), [`clock_gettime(CLOCK_REALTIME)` through the time source pointer](../../../docs/reference/services.md#clock_gettimeclock_realtime-through-the-time-source-pointer).
- **Node area cut for a Unity 2022 game with virtual address memory** (`StandardAllocator::Initialize(nullptr, pool − used)`): [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area).
- **Unity 2022 `Shader` objects** (`unity_code_blobs`, no local keyword list in `unityfs.subprogram`): [Unity 2022 Shader objects with parameter blobs](../../../docs/reference/shaders.md#unity-2022-shader-objects-with-parameter-blobs).

## Open

- The one geometry shader does not build (Metal cannot express it).
- `game.tsv` has no `sys_resource` row, so the host reports its default, 32 MB (`game.py`, `hle_svc.c`), where the NPDM declares 16 MB.
