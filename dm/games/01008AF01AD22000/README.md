# The Patrick Star Game (`01008AF01AD22000`)

Update 1.0.1 (v1) over the base game, SDK 16.2.4. Unity 2022.3 IL2CPP with FMOD (one AudioOut) on NVN. Modules `main`, `subsdk0` (`NintendoSDK_movie` for intro cinematics) and `sdk`. Plays on the Mac at 30 fps (present interval 2) as on the Switch. Reached: the title screen, menus, intro cinematics, and active gameplay with rendering and audio.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xcd500000` | `game.tsv` | the 5 GB block for Unity games with virtual address memory |
| `stdalloc_cap 0x3c000000` | `game.tsv` | caps StandardAllocator's region at 960 MB to preserve alias space for Unity's internal address allocations; see [Memory](#memory) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0, so pipeline bases take that mode |
| `shader_groups /Data/=data` | `game.tsv` | Unity Shader objects in `Data/` |
| `romfs_marker Data/resources.assets` | `game.tsv` | the RomFS marker file |

## Frame rate

- Target 30 fps (`present interval 2`), matching the Switch.
- Intro cinematics decode smoothly through VideoToolbox and play at 30 fps.

## Memory

- **`stdalloc_cap 0x3c000000`**: The NPDM declares 16 MB of system resource (`sys_resource = 0x1000000`), enabling virtual address memory. Unity's `nninitStartup` (`main+0x111fc00`) queries `TotalMemorySize - UsedMemorySize` (3.14 GB) and passes the entire size to `StandardAllocator::Initialize`. With the 3.2 GB hardware pool, this exhausted the alias region for subsequent `AllocateAddressRegion` calls. Capping `StandardAllocator` at 960 MB preserves 3.38 GB in the alias region for Unity's own allocators.
- Virtual address high-water mark stays around +2.75 GB / 4.38 GB in play with zero allocation failures.

## Sound and movies

- Intro cinematics (H.264 / AAC via `subsdk0` `NintendoSDK_movie`) decode and present cleanly with audio.
- `save:/` mounts and commits normally.
