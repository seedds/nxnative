# CAPTAIN TSUBASA 2: WORLD FIGHTERS (`01009720213B0000`)

Update 1.0.6 (`v327680`), Unreal Engine 5.5.4, Nintendo SDK 18.3.1, NVN, Wwise audio and Bink 2 movies. The host loads `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. The game's IoStore container holds the global and material shader libraries. The split found 21,903 distinct shaders, including 6,577 compute shaders.

On 2026-11-24, the user accepted the EULA and successfully played the first offline match in the HUD-enabled development build on an Apple M6 Mac (macOS 27.2.1, Xcode 27.2). The run used the profile's 4.375 GiB alias region and reported all five supplied add-ons as owned. The pipeline prewarm list covers startup through the title and EULA.

At a 1280×720 drawable, the later 300-frame windows ending at frames 15,300–16,200 ran at 29.9–30.0 fps (median frame time 33.33–33.39 ms). An earlier 681 ms frame logged 651 ms spent on new pipelines and shader libraries. The highest reported memory sample was 3,702 MB resident and 1,653 MB allocated by Metal. The log reached frame 16,200 with no crash or audio device underruns.

## What it needs

| Setting | Why |
|---|---|
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `app_pool 0xCD500000` | The existing engine-region gate fits Unreal's initial 62 GiB request into 4.06 GiB with this alias layout. The SDK's 62 MiB node-area and 2 MiB alignment rules match SDK 18.3.1. |
| `sys_resource 0x1400000` | The NPDM declares 20 MiB of system resource memory. |
| `depth_mode zero` | The game selects this depth mode at startup. The profile gives offline pipeline bases the same mode. |
| `tex_dyn_slots on` | Vertex shaders that choose buffer textures at run time read their handles from the NVN driver tables. The existing [dynamic slot binding](../../../docs/reference/shaders.md#texture-slots-picked-at-run-time) lets those shaders index the bound textures. |
| `fps_target 60` | The shared [Unreal frame-rate controls](../../../docs/reference/frame-rate.md#unreals-frame-limit-and-present-interval-found-by-name) set `rhi.SyncInterval` to one vsync and raise the game's positive `t.MaxFPS` limit to 60. The rewriter finds both variables by name. |

The values and exact build IDs are in [game.tsv](game.tsv).

## Found on this game

- [Unreal's FMallocBinned3 replaced by the host allocator](../../../docs/reference/memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator). The vtable header points to typeinfo at `main+0x7b7d060`, which names `FMallocBinned3`. The allocator's constructor at `main+0x7a360` is called from `main+0x1f7180` and requests 38 GiB of address space. With a 4.375 GiB alias region the original rewrite crashed at `main+0x1134910`, from the constructor. The matcher recognises the RTTI-enabled vtable and the host keeps its header when installing the replacement.
- [DLC extraction rejects an archived filename](../../../docs/reference/packing.md#dlc-extraction-rejects-an-archived-filename). Each supplied add-on (indices 4–8) has a 16-byte placeholder at `/D:/sky2_auto/MakeData/Dlc/DummyData/fantastic.dat`. The image writer preserves its path and bytes.
- Four vertex shaders (`1c094490012bdedc`, `10ce50b04ef9cb54`, `6b54e649d0e78bf3`, `d6b9a12aff78aa90`) were rejected in the first match with dynamic slots disabled. Their translated buffer-texture accesses use handles from the separate texture and sampler tables in `c2`. With `tex_dyn_slots` enabled, all four linked and bound buffer textures during the offline match replay on the Apple M6. The log recorded no shader load failures or skipped draws, and the user reported that the match looked good.

## On the Mac

Use the run command printed by `--platform mac`. If the title reports a transmission error, close the notice, select Yes to continue offline and close the offline confirmation. Press A on the title to reach the publisher's EULA on the first run.

On 2026-11-25, the user replayed the same offline match with `DM_FPS_TARGET=60` and reported normal player movement and match-clock speed. On the Apple M6 at 2560×1440, the 300-frame windows ending at frames 5,400–6,300 ran at 59.8–59.9 fps (median 16.71–16.74 ms). A later 247 ms frame spent 231 ms loading new pipelines and shader libraries. Select 30 fps in the launcher's Frame rate setting, or use `DM_FPS_TARGET=0` to restore the game's own timing controls.

## Open

- Bink reported a track-count mismatch three times. The replay also logged audio output underruns. Their audible effect and whether they are related remain unverified.
- In-game DLC effects, online play, and iPhone gameplay remain unverified. The profile registers update 1.0.6.
