# Oceanhorn: Monster of Uncharted Seas (`01007D7001D0E000`)

Update 1.0.1 (`v65536`), Cornfox & Bros.' engine, Nintendo SDK 3.7.0 and NVN.
This is the registered update. The profile uses the shared host and rewriter.

## On the Mac

Use the command printed by `scripts/build_ipa_from_nsp.py --platform mac`.
At the title, press A to open the save selector and A again to start the first
slot. On the test Mac, `DM_KEYS='f940:A~0.2,f1080:A~0.2'` selected a fresh slot.

The Apple M6 Mac mini (16 GB, macOS 27.2.1 `26B131`) rendered the title,
save selector and full prologue, then reached the starting camp and wrote the
first autosave. Selecting slot 1 after relaunch returned to the island.
The game ran in a window with the Metal HUD. At the camp, frame time through
frame 10200 was 16.67 ms median and 16.81 ms p95 over 300 frames, with about
1.4 GB resident. The first run through the prologue spent 439 ms compiling
two new pipelines at a scene transition.

Both the Mac and unsigned iOS builds compiled all 172 shaders on November 23,
2026, with Xcode 27.2 (`27B52`), Metal `32023.922`, and shader chain
`108c5f92c3c4`. The iOS bundle includes the RomFS image and shader libraries.

## What it needs

| Setting | Why |
|---|---|
| `modules main sdk` | The update's `subsdk0` and `sdk` are byte-identical. Loading `sdk` once provides the imports and runs the SDK constructors once. |
| `mem_reserve 0xf0000000` and `sys_resource 0x0` | The game requests a 2.96 GB heap, which fits in a 3 GB heap region. Its NPDM declares zero system resource. |
| `depth_mode zero` | The game selects near=0 at run time. The offline pipeline build uses the same `main0_z` vertex entry point. |
| `romfs_marker data.vfs` | `data.vfs` is the game's main asset archive. The audio archive is `oceanhorn.bfsar` and the music streams are under `stream/`. |

## Found on this game

- **Resolved.** [Cornfox VFS2 archives](../../../docs/reference/shaders.md#cornfox-vfs2-archives)
  hold the game's GLSLC shaders. The shared splitter finds 57 vertex and 115 pixel shaders.

## Open

- The base version, later gameplay and iPhone runtime still need validation.
- One Mac run had a 3.56 s present stall with no pipeline builds or texture
  uploads. The cause is unknown. The USB audio output also stopped callbacks
  twice in that run, and the host's watchdog restarted the AudioUnit both times.
