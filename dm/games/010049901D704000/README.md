# Shin chan: Shiro and the Coal Town (`010049901D704000`)

Update 1.0.2 (v131072) over the base game, SDK 15.3.2. Unity 2021.3.18f1 IL2CPP with FMOD on NVN. Modules `main`,
`subsdk0` (NintendoSDK_movie) and `sdk`. 7,119 shaders, 56 of them compute. Playable on the iPhone 16 Pro.

## What it needs

- **Nothing in `patches.py`.** The game registers no 60 fps cut: it paces itself at 30 fps (frame time p50 33.3 ms) and
  the present interval it sets is what the host follows.
- **The alias region must be larger than Unity's own default.** See [Memory](#memory).
- **Nothing else**: the Unity 2021.3 `ComputeShader` layout this game needs is `COMPUTE_2021_3` in `unityfs.py`, which
  main already carries (issue #440); before it the split dropped the file's compute kernels and the character models
  collapsed into a blob. See [Files and shaders](#files-and-shaders).

## Memory

The provisional profile (a 5 GB reserve holding a 4.375 GB alias region) let Unity map 4.26 GB of that region and then
write at `NULL + 0x3fffe0` — a 4 MB pool − 0x20 — at `main+0x546d70`, `CRASH sig=11`, about five seconds into the first
scene. That is the signature of [Size-class alignment doubles Unity's regions](../../../docs/reference/memory.md): the
region ran out of address space and the game's virtual allocator took the null.

Cutting `VammManager::AllocateAddressRegion`'s size classes to 2 MB (`sdk_patches.py`, the sdk build is Paper Mario's
`8618895246b420329b77746b960a66ab`) did **not** move the high-water mark (4.25 GB), so the fix here is the partition:

| key | value | |
|---|---|---|
| `mem_reserve` | `0x200000000` (8 GB) | was 5 GB |
| `mem_alias` | `0x180000000` (6 GB) | was 4.375 GB |
| `mem_stack` | `0x10000000` | |
| `app_pool` | `0xCD500000` | |
| `sys_resource` | `0x1000000` | the NPDM's size |

`hle_svc.c`'s `mem_init_once` requires `alias <= reserve - 256 MB` and `stack <= reserve - 256 MB - alias`; a profile
that breaks it aborts at start-up with `FATAL guest address-space profile has invalid native boundaries` and no game
log at all, which is easy to mistake for a broken install. With this profile the alias high-water mark is 4.52 GB of
6 GB after two minutes of play.

## Files and shaders

The RomFS is 9,058 files (2.9 GB), almost all `Data/StreamingAssets` UnityFS bundles. `zip_store` keeps `.bundle`,
`.dat`, `.json`, `.mp4` and `.ress` stored rather than deflated.

`Data/Resources/unity default resources` carries stripped `ComputeShader` objects (class 72, type hash
`abd9135b8ce83d043fef4e9ec7f53366`) that no bundle lends a type tree to, so they need the `COMPUTE_2021_3` layout main
has had since #440. Without it the split printed

```
[RED] …/unity default resources: ComputeShader objects that no bundle lends a type tree to and unityfs.py has no
      layout for, their kernels are left out (class 72 …)
```

and the game then dispatched a program with no compute shader (`[RED] the dispatched program has no shader`), which is
the GPU skinning pass: every character in the scene collapsed into one blob at the origin while the static geometry
looked correct. The profile's `shader_map.tsv` holds all 7,119 shaders (56 compute) once that layout is in place.

## Frame rate

30 fps: `frame time p50 33.3 ms / p95 34.0 ms` over 300-frame windows on an iPhone 16 Pro, no frame step. Not measured
against a 60 fps cut; Unity 2021.3 titles in this repository do take one (Sea of Stars), and this game's `GetCommandMemoryUsed`
pacing has not been examined.

## Audio

FMOD inside `main`, on the SDK's audio renderer; nothing game-specific was needed.

## Mac routes

None recorded: this port was brought up on an Intel Mac, which cannot run the host (it runs the game's own arm64
code), so every observation here comes from the iPhone's own log.

## Testing

Played on an iPhone 16 Pro: the opening movie, the first town, dialogue, the field and the first save, with the launcher
settings screen in Simplified Chinese. Verified in the log: 0 `CRASH`, 0 `is not in the shader map`, 0
`has no compute shader` over 900+ frames, alias high-water 4.52 GB of 6 GB, footprint about 1.3 GB.
