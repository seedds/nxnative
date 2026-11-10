# Super Nintendo Entertainment System – Nintendo Classics (`01008D300C50C000`)

Update 6.0.0 (v1966080) over the base game, SDK 20.5.21. Nintendo's own SNES emulator app: a Lua UI (`bootapp/`) over its own NVN renderer, one `.sfrom` per game under `titles/`, its sound through two audio renderers. NPLN / gRPC, Mnpp, LDN, BCAT and the friends and account clients are linked in. Modules `main`, `subsdk0` and `sdk`. On the Mac: the boot, the game list, a game started from it (Bubsy: its logos, title and first level under control), the in-game suspend menu, with the emulated console's sound, at 60 fps. Played by hand on an iPhone (iPhone19,4, A20 Pro, iOS 27.3 beta).

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | no system resource in the NPDM; `main` sets a heap of at most the pool − 64 MB |
| `modules main subsdk0 sdk` | `game.tsv` | the update's ExeFS carries `subsdk0` as a byte-identical copy of `sdk` (the same build ID; the base game's `subsdk0`–`subsdk2` are other modules); both are loaded as on Horizon |
| `shader_groups` (`/bootapp/`, `/titles/`, `/sysdata/`, `/crt240/`) | `game.tsv` | the shader split's groups |

The provisional profile was right as generated; the rest was general host work ([Found on this game](#found-on-this-game)).

## Online services

- Offline the app answers as every other offline game does: no network (`nifm`), no network service account, the friends calls unavailable, sockets failing with ENETDOWN, `mnpp:app` mission events accepted and never sent. Nothing asked for a Nintendo Switch Online membership on this route: the list opens, games start and play. No access check was patched or answered specially.

## Rendering

- 23 stage shaders, all GLSLC outputs: 10 in `bootapp/resources/shaders/*.nvnc` (the screen filters: bilinear, CRT, bicubic …), 2 in `sysdata/shaders/*.glslc`, the rest in `main`. Lower-left window origin (GL).
- `nvnDeviceGetInteger` items 0x60–0x68 are past the NVN header the host has (it ends at 0x61, `EVENTS_SUPPORT_REDUCTION_OPERATIONS`): red lines at boot, answered 0. Nothing visible depends on them on this route.

## Audio

- Two renderers: 48 kHz with 16 voices for the UI, and 32 kHz with 8 voices for the emulated console, opened when a game starts. The host drove only the first, so the games were silent; it now drives both, each through its own output ([A game with more than one renderer](../../../docs/reference/audio.md#a-game-with-more-than-one-renderer)). In Bubsy's first minute the second renderer had sound in 7,467 of 12,000 frames.

## Frame rate and memory

- 60 fps on the Mac in the list and in a game (frame interval p99 16.7 ms). Resident 0.66–0.71 GB and 0.94 GB of GPU memory.

## Input

- Docked, the 2D pad layout. The list: D-pad, A starts the selected game, X details, Y moves. In Bubsy, Plus is Start, B jumps and the D-pad walks; ZL + ZR opens the suspend menu (hold to rewind).

## Mac routes

- From a fresh save (`SV=` an empty folder with the work folder's `run/run.sh`), `DM_KEYS=40:A,55:Plus,60:Plus,66:Plus,70:Right~3,75:B,78:Right~4,85:ZL+ZR~0.5,90:B` starts the first game in the list (Bubsy), presses Start through its title, walks and jumps, opens the suspend menu and leaves it. The prewarm list was recorded on this route.

## Found on this game

General host changes first needed here; each applies to every game:

- **`mnpp:app`** served like `olsc:u` (`hle_sf.c`): [Mission events through `mnpp:app`](../../../docs/reference/services.md#mission-events-through-mnppapp).
- **`friends::GetPlayHistoryRegistrationKey` without a `Uid`** (`hle_friends.c`): [SDK 20's `friends::GetPlayHistoryRegistrationKey` without a `Uid`](../../../docs/reference/services.md#sdk-20s-friendsgetplayhistoryregistrationkey-without-a-uid).

Upstream had already made the others it needed, for other games: several audio renderers at once (`hle_audren.c`, [A game with more than one renderer](../../../docs/reference/audio.md#a-game-with-more-than-one-renderer)) and the local user's profile image (`hle_account.c`, [The local user's profile image](../../../docs/reference/services.md#the-local-users-profile-image)). Not yet re-run on this tree's host.

## Open

- Only Bubsy was played; other games of the list, rewind and the online play menu were not tried.
