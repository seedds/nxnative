# Chained Echoes (`0100C510166F0000`)

Update 1.1.1 (v131072) over the base game, SDK 14.3.0, NVN API 55.15. Unity 2020.3.36f1 IL2CPP on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`; the `sdk` is the build of Batman: Arkham Knight 1.0.5 ([0100ACD0163D0000](../0100ACD0163D0000/README.md)). Plays on the Mac at 60 fps, as on the Switch: the title and its attract sequence, Start, and the prologue's dialogue in Glenn's bedroom. A double-clickable Mac app is packed; iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Node area 1 GB → 62 MB | `dm/tools/sdk_patches.py` SDK-wide rules, `sdk` build `2aef2cff…`, `sdk+0x12d8b0` / `+0x12d94c` (implementation 3 of 8) | without it `StandardAllocator::Initialize` aborted before `nnMain` ([The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area)) |
| Region alignment 4 MB / 32 MB / 1 GB → 2 MB (SDK 20's) | `sdk_patches.py` SDK-wide rules, the same build, `sdk+0x12daa4` / `+0x12daac` / `+0x12dab0` (implementation 3 of 4) | Unity's regions would take twice their size in address space |
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | the NPDM's 16 MB of system resource: virtual address memory |
| 5 GB guest layout: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000` | `game.tsv` | Ace Attorney Investigations Collection's layout ([010005501E68C000](../010005501E68C000/README.md)) |
| `app_pool 0xCD500000` (3.2 GB) | `game.tsv` | the survey's guess, unmeasured: see [Memory](#memory) |

No code of the game is cut: `main` and `subsdk0` take no cuts, and `sdk` takes the five of the two SDK-wide rules. The game asks for present interval 1, so no frame-rate key is needed.

## Memory

- `nninitStartup` gives the application pool to `StandardAllocator` as address regions (virtual address memory, the NPDM's 16 MB of system resource), so the SDK's node area and region alignment decide whether the prologue's allocations fit.
- **Current:** a 2-minute play run mapped the alias region up to +3.5 GB of its 64 GB, 1.95 GB resident and 1.01 GB of Metal allocations, with 36 memory pools (5 resident) and 245 shared textures. Nothing grew between frames 4,200 and 7,200.
- The pool is still the survey's guess: the NSP script did not heap-check this build, because the profile reached it through `DM_GAMES_DIR` and was read as registered rather than provisional ([fixed since](../../../docs/reference/packing.md#a-provisional-profile-loses-its-tables-to-its-own-derived-copy)).

## Files and shaders

- RomFS: 663 entries, 748 MB (`RomFS.img`, 738,474,696 bytes), everything under `Data/`.
- 3,832 shaders: 1,877 vertex, 1,881 pixel, 73 compute and one geometry shader. All built for the Mac with no failure, and no run met a shader missing from the map.
- The prewarm list (`pso_manifest.tsv`, 63 keys: 45 shaders and 18 render pipelines) was recorded from a run through the title into the prologue (`DM_PSO_MANIFEST=off DM_PSO_RECORD`); the host prewarms it in 74 ms of background work.

## Frame rate

- **Current:** 60 fps on an M4 (frame time p50 16.67 ms, p95 17.4 ms, 300-frame windows over 7,200 frames), GPU p50 2.0 ms / p95 7.1 ms, 60 to 87 draws and about 1.9 M translated instructions a frame, CPU about 43% of one core (194 ms/s on the main thread). The game asks for present interval 1; no key is needed.

## Audio

- One `AudioOut` (opened at boot from `main+0x31e6b0`, started from `main+0x110fc0c`). In a 2-minute run the device took 26,000 callbacks of 240 frames each with 0 underruns and 4 blocks queued at each callback.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` (the `--platform mac-app` pack adds the app); its last line runs the game.
- Scripted, from a fresh save: `DM_KEYS="16:A,22:A,28:A,34:A,40:A,46:A,52:A,58:A,64:A,70:A,76:A"` presses A from 16 s on; the title's Start leads through the new-game screens into the prologue's bedroom dialogue by about 30 s.
- The packed app shows its settings window first (console mode, language, display mode, rumble, cloud saves); Return starts the game, whose window takes the keyboard.

## Testing

- 2026-11-12, Mac (M4, macOS 27.2): boot to `nnosInitialize` and `nnMain`, title and attract, Start, the new-game screens and the prologue's dialogue in Glenn's bedroom; 60 fps over 7,200 frames; the packed app (`Chained Echoes.app`, 1.0 GB, ad-hoc signed) launched from Finder and played the same way, and the prewarm list was recorded and loaded. No red line was left in the last runs.
- Not tested: battles, saves and loading, the movie module's cinematics, controllers (the keyboard and `DM_KEYS` only), a long session's memory, and iOS.

## Open

- The prewarm list covers the title and the prologue only; battles, menus and the later maps build their pipelines on first use.
- The heap check has not run; the pool is the survey's guess.
- The build was made with Xcode 27.3's Metal compiler, not the tested 27.2 (`--allow-other-metal`, [issue #242](https://github.com/seedds/nxnative/issues/242)); the picture looked right in the title and the prologue.
- iOS: no IPA was built.

## Found on this game

- [A provisional profile loses its tables to its own derived copy](../../../docs/reference/packing.md#a-provisional-profile-loses-its-tables-to-its-own-derived-copy): the build took the provisional profile for a registered one, derived it onto its own folder and skipped its heap check.
- The SDK 14.3.0 instance of [the SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area) and of the 2 MB region alignment (Batman: Arkham Knight's `sdk` build), the first game to take both only from the SDK-wide rules.
