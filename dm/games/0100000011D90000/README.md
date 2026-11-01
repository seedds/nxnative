# Pokémon Brilliant Diamond (`0100000011D90000`)

Update 1.3.0 (v393216) over the base game. Unity 2019.4.27f1 IL2CPP, SDK 11.4.3, NVN, Wwise and FMOD. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`; the latter two are the builds used by [Metroid Dread](../010093801237C000/README.md). Plays on the Mac at 60 fps with 1.3.0. An unsigned IPA is built; iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| SDK node area 1 GB → 62 MB | `dm/tools/sdk_patches.py`, build `2d369d99…`, `sdk+0x34e0c8` / `+0x34e164` | `StandardAllocator::Initialize` aborts before `nnMain` without it; see [Memory](#memory) |
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000` | [`game.tsv`](game.tsv) | virtual address memory: StandardAllocator and Unity reserve regions independently |
| `app_pool 0x80000000` (2 GB) | `game.tsv` | 1 GB exhausts the game's malloc while loading; the hardware pool leaves little alias space beside StandardAllocator |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM's 16 MB system resource selects virtual address memory |
| `fps_target 60`, `present_interval display` | `game.tsv`, build-gated engine limiter | raises Unity's own cap and the interval-2 present cap; see [Frame rate](#frame-rate) |

No game-specific code cuts. `DM_FPS_TARGET=30 DM_PRESENT_INTERVAL=game` restores the original 30 fps.

## Memory

- **Resolved: abort before `nnMain`.** `nninitStartup` (`main+0x2c1cf0`) queries pool minus used memory, then calls `StandardAllocator::Initialize(nullptr, size)` at `main+0x2c1d5c`. It never sets a heap on the virtual-address-memory path. With the hardware pool and the original 1 GB node area, `CentralHeap::Init` fails: `Break` at `sdk+0x39fbd4`, backtrace `sdk+0x145038`, `sdk+0x145198`, `sdk+0x2f98e4`, `main+0x2c1d60`.
- The SDK's `AddressRegionManager` constructor (`sdk+0x34e0b8`) loads the node-area limit at `+0x34e0c8`, and registers its occupied length at `+0x34e164`. Both are `mov w8/w9,#0x3fe00000`. The same two cuts as Engage's SDK shrink them to 62 MB. A repeat boot then reaches `nnMain` without a `Break`.
- With the original 3.21 GB application pool and the node-area cut, mapped regions already reach +3.90 GB of the 4.375 GB alias region during startup; only 0.50 GB is committed in the first 12 seconds.
- **Resolved: 1 GB pool exhausted during loading.** Both a windowed run with audio and a headless silent run crash as commits reach 1.00–1.01 GB, on null reads at `main+0x2605cc` (IL2CPP) and `main+0x578624`. A 2 GB pool passes this load and reaches the language menu and opening dialogue. At that point the alias high-water mark is +2.75 GB and 1.01 GB is committed. Over the 30-minute opening / Twinleaf Town run the high-water mark stayed at +2.75 GB, with up to 1.50 GB committed. The house settled near 2.24 GB resident / 0.87 GB Metal; outdoors near 2.50 GB / 1.01 GB, with scene-related steps rather than sustained growth.

## Files and shaders

- RomFS: 18,022 files, 4.71 GB, including 14,052 UnityFS bundles (3.83 GB), Wwise banks and streams, and 18 WebM movies. The player's managed metadata is `Data/Managed/Metadata/global-metadata.dat`.
- The split finds 14,441 stage shaders (7,247 vertex, 7,173 pixel, 20 compute and one geometry), including 11 GLSLC outputs in `main`. All 14,441 compile for Mac and iOS, with no unresolved bindings or unfillable driver-bank reads; 47 shaders read `c2` as data.
- **Resolved: missing skinning compute shader, black 3D behind the dialogue.** Stripped `ComputeShader` objects in `Data/Resources/unity default resources` have type hash `1a95529ac2eb1a97cd9d2a4356bd722f`. `unityfs.py` now has Unity 2019.4.27f1's release layout, checked against [AssetRipper's dump](https://github.com/AssetRipper/TypeTreeDumps/blob/main/StructsDump/release/2019.4.27f1.dump): 116 nodes with matching alignment flags, both objects read to their exact ends. Unlike newer Unity versions, each named kernel holds its code directly. `Internal-Skinning` contributes 16 kernels and `Internal-BlendShape` three; the missing runtime key `2c1e06378edbd813` matches `Internal-Skinning#main.0` exactly. With these kernels the professor, character selection, household and outdoor character models render correctly, with no missing shader in the tested route.
- The rewriter handles six offset jump tables and one indirect call through `x18` in `main`, with no additional rewriter mechanism.

## Frame rate

- **Resolved: gameplay returns to 30 fps after 60 fps menus.** The game's IL2CPP initialization loads 30 at `main+0x2bc13f8` and calls the `Application.set_targetFrameRate` wrapper at `+0x26a0240`. The engine's icall registration at `+0xe34e60` resolves to `+0xdc8e70` → `+0x5fa640`, which stores the integer at `main+0x4afc868`; its getter at `+0x5fa630` reads the same field. The host's integer-FPS limiter row is gated on main build `94ceae32…` and keeps it at `fps_target`, even when the game resets it.
- Display pacing alone leaves that 30 fps engine cap. The profile also uses `present_interval display`, because the game asks for interval 2 independently. A headless Mac run with both changes reports 60.0 fps outdoors (frame-time median about 16.5 ms). `DM_WINDOW=0` still creates a window because the Mac front end tests whether the variable exists; timing comparisons omit it and use `DM_PRESENT_LINK=0` to avoid occluded-window display-link stalls.
- **Current: movement and collision comparison passed.** Separate launches from the same save, at 30 and 60 fps, received `LDown~0.5`, `LRight~1`, `LUp~1`, with screenshots before and after each hold. Static-background phase correlation found offsets of (0, 0), (0, 1), (4, 1) and (-1, 4) pixels respectively: the same route and wall contact, within frame/input sampling differences. This checks that walking does not double in speed; it does not establish correctness of every later physics system. Captures are `run/shots/rate{30,60}-{start,down,right,wall}*.png` in the build folder.

## Mac testing

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`; its final line runs the game with the correct paths. Saves are under `<NSP folder>/0100000011D90000_build/run/save`. Use `DM_CTL=<file>` for input and screenshots, and `DM_KBD=<name>` for scripted name entry. Logs, screenshots and analysis stay in the NSP folder.

## Testing

- 2026-10-26, Mac: a fresh save through language selection, Rowan's introduction, character and name entry, the player's room, downstairs dialogue, Twinleaf Town movement and the rival encounter. The 30-minute run had no red line; its final SIGALRM was the test timer. Both audio outputs reported zero underruns.
- An extended reload run reached the rival's upstairs room, Route 201 and Verity Lakefront. It continued for over 90 minutes at 30 fps, with about 2.68 GB resident / 1.10 GB Metal in the lakefront scene. After about 58 minutes the audio watchdog intermittently reported device callback stalls and restarted the AudioUnits; each recovered, with no game hang. The cause of those host-device stalls is unconfirmed.
- Autosave created `SaveData.bin` and `Backup.bin` (979,108 bytes each); a separate launch reloaded the player outside the house correctly.
- Reloading exercises the VP8 logos and opening movies. The host now decodes the real movies without errors and displays their pictures, with no depth-test override. A 106-second sample decoded 3,182 packets at about 1 ms per packet.
- The final rebuilt host passed a three-minute boot with real audio muted: VP8 decoded, 30 fps, both outputs reporting zero underruns and no red line. SIGALRM ended the timed run.
- The 60 fps update passed separate 30/60 movement comparisons and indoor/outdoor transitions with autosave. The host's integer cap changed 30 → 60 as intended and outdoor logs report 60.0 fps. The extended 60 fps route run had one 211 ms Core Audio callback stall; the existing watchdog restarted the AudioUnit and playback recovered, as in the earlier 30 fps run.
- The final unsigned 60 fps IPA is 4.6 GB (4,965,541,636 bytes), version 1.3.0, bundle ID `org.nxnative.0100000011d90000`; it contains no code signature. Its 21,392 Metal libraries cover all 14,441 stage shaders and their required variants. The packaged executable contains the new Unity limiter and its exact build gate.
- [PR #190](https://github.com/seedds/nxnative/pull/190) independently found the shared SDK cut and the same 19 built-in compute shaders on 1.0.0. Its shader tables describe that build; this profile uses the 1.3.0 split. Its AudioOut-stop / FMOD finding is included as a general fix, with a guard preserving buffers queued before Start; the 1.3.0 run passed character selection before that fix.
- Host tests pass, including synthetic VP8 / VP9 NV12 and RGBA decoding, AudioOut stop / queue stress and integer / float / millisecond / vsync frame-limit regressions. The graphics Python suite passes (41 tests, two skipped); Metal depth / stencil clear, stencil-view, read-only depth and upload-budget regressions pass. Rewriter patch, chain and documentation checks pass.

## Found on this game

- SDK 11.4.3's instance of [the SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area), with the cut keyed by the shared SDK build ID.
- Legacy kernels held directly in [Unity ComputeShader objects](../../../docs/reference/shaders.md#unity-computeshader-objects).
- [VP8 decoding through libvpx](../../../docs/reference/movies.md#vp8-uses-the-same-libvpx-worker-and-output-paths).
- [Zero initialization of depth and stencil textures](../../../docs/reference/rendering.md#new-textures-start-as-zero).
- [AudioOut stop releases pending buffers](../../../docs/reference/audio.md#stopping-an-audioout-releases-its-pending-buffers), reported on 1.0.0 in PR #190.
- [Unity's integer target frame rate](../../../docs/reference/frame-rate.md#unitys-integer-target-frame-rate).

## Open

- [FPSLocker Warehouse](https://github.com/masagrator/FPSLocker-Warehouse/blob/v4/README.md#L847) flags physics issues above 30 fps for this exact build. A short movement check cannot rule out every physics issue in later areas.
- First battle and later areas remain untested.
- User testing on iOS remains required.
- Recheck the intermittent Core Audio callback stalls during a long run on the final host build.
