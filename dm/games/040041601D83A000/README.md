# Drag x Drive (`040041601D83A000`)

Plays on the Mac with a pair of Joy-Con 2s over Bluetooth LE, at the game's own 30 fps: boot, the title and play in its arena (M4 Pro, macOS 27.2, docked; the longest session about 6 minutes on this tree, 27 minutes on an earlier one). It needs [PR #1609](https://github.com/seedds/nxnative/pull/1609)'s four-byte draw tickets to boot.

## What it needs

- `nvn_draw_ctl on` ([PR #1609](https://github.com/seedds/nxnative/pull/1609), issue #1580). Without it, 5 s after boot, the game initializes a buffer in no memory pool (`[RED] BufferInitialize: pool has no storage (pool 0x0 … size 0x170)`) and crashes in `nvnMemoryPoolGetFlags`: the same host with the tickets turned off (#1609's switch) crashes there and with them on plays. The key does nothing until that pull request is in.
- `nn::hid::EnableUnintendedHomeButtonInputProtection` answered by the host ([Home button protection on a Switch 2 controller](../../../docs/reference/services.md#home-button-protection-on-a-switch-2-controller)): the game calls it for player 1 as the Joy-Con 2s join, and crashed there at `sdk+0x63ce10`.
- Joy-Con 2s: the launcher's Switch 2 controllers setting (`DM_SW2=1`), then Sync on each Joy-Con while the game runs. The pair joins as player 1 ([The Joy-Con 2 mouse sensor and pointer](../../../docs/reference/services.md#the-joy-con-2-mouse-sensor-and-pointer)).
- `render_cond on`: on in every session played; not measured without it.
- The rest of the profile is the survey's guess (`dm/games/profile_guess.py`), unchanged after play: the 9 GB pool of the Switch 2 and a heap region of the pool less 64 MB.

## Files

- A NUP of 1.9 GB: the base game and update v196608. The RomFS lists 772 files; the marker is `Sound/Resource/All_Static.bars.zs`. The Mac app is 2.0 GB.
- The display version of v196608 is not known here (the NUP's control data was not read): the profile's `build` row has `?`.

## Shaders

- 6,038 shaders (`Nin_Ounce_NVN2` BNSH), translated in about 4 minutes on the M4 Pro.
- The offline pipeline archive (`pso_offline.py`, 233 MB) is the slow part of a build: 50 of its 5,465 pipeline bases have pixel libraries of 400-720 KB, and `applegpu-nt` spends minutes on each, on one core.
- `pso_manifest.tsv` is the host's per-device list from the sessions played.

## Frame rate

- 30 fps, the game's own pace: frame time p50 33.33 ms in nearly every 300-frame window of a 6-minute session, GPU p50 7-11 ms (M4 Pro, docked). A few windows around menus and scene changes have frames of about 70 ms.

## Driving it on the Mac

- The controls are the Joy-Con 2s' mouse sensors: each Joy-Con pushes one wheel, so a pair is needed. Pair them by Sync while the game runs.

## Found on this game

- [Home button protection on a Switch 2 controller](../../../docs/reference/services.md#home-button-protection-on-a-switch-2-controller): the crash as the Joy-Con 2s join.

## Open

- Booting needs [PR #1609](https://github.com/seedds/nxnative/pull/1609).
- Once, opening the controller applet (`nn::hid::ShowControllerSupport`) from a game thread with a small stack crashed in the host's own log line (SIGBUS just below the stack pointer, in `fprintf` under `focus_sync`): [Host calls on small guest stacks](../../../docs/reference/memory.md#host-calls-on-small-guest-stacks), fixed by [PR #1943](https://github.com/seedds/nxnative/pull/1943).
- `render_cond` without play to show what it changes.
- Online play, iOS: not tested.
