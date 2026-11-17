# Game state

The games' own state: saves, scripted state, and behaviour that looks like a host bug but is not. Each entry: symptom, cause, fix, the games it was found on. The [symptom index](../agents/symptoms.md) lists every symptom.

## Save investigations

### Empty slots after a successful save

- **Symptom:** saved progress appears in Continue but the slot-selection screen is empty, including after a restart.
- **Cause to distinguish:** a failed write or commit versus persisted data rejected by the game's slot-population logic. The symptom alone establishes neither; the configuration-loading cause in issue #2930 is still open.
- **Investigation (not a production fix):** separate persistence from slot selection before changing filesystem behavior:

1. Stop the test process before copying its committed save and any sibling work directory. Keep the originals untouched, record file sizes and SHA-256 hashes, and point an isolated run's `DM_SAVE` at a writable copy. Disable cloud sync for that run so it cannot publish the experiment. Keep runs and save snapshots outside the repository.
2. Use `DM_CTL` to observe, press, and capture a screenshot at each transition. Mark separate windows for saving, returning to the title, entering the slot screen, and loading a slot. A title's Continue button can reflect in-memory state; test a fresh process too.
3. Start with `DM_FS=1` and `DM_FSLOG=<file>` (`DM_FSLOG_HASH=1` adds hashes of read buffers). The read log is not a write audit. If needed, temporarily instrument the actual filesystem entry points in `dm/host/hle_fs.c`: record the API, guest path or handle, open mode, offset, requested and transferred bytes, result, and host error on failure. Preserve `errno` and the guest result. Include create, truncate, read, write, close, rename/delete, and Commit; a path resolver being successful is not proof that any of those operations succeeded.
4. Compare a completed write buffer with the file in the committed directory after Commit. Restart from a copy of that snapshot and compare the bytes read back. Distinguish the expected truncate-before-write sequence from a file left empty, and check whether entering the slot screen writes again rather than assuming it reads. A resource-pack read is not a save read: attribute it by path and handle, not timing alone.
5. If the bytes persist and read back unchanged, follow the game's deserialization and slot-population branch. Inspect the metadata it actually uses and trace any validity or version check back to its configuration source and default. A missing sidecar path may belong to DeleteFile or a type query, not a failed ReadFile; a cloud-service string alone does not establish a network dependency.
6. A/B only on copies of the same committed snapshot. If a temporary change to a suspected UI filter restores the slots, record the exact main build ID, branch, inputs, and resulting UI. This localizes a rejection; it does not prove why the rejected metadata or configuration is wrong, nor justify disabling integrity checks in production. Keep fixed-address experiments restricted to the inspected build and out of generic filesystem code.
7. Remove temporary instrumentation and bypasses after preserving their diff outside the repository. Check the complete source against the intended baseline and rebuild if it changed; do not restore from a truncated tool excerpt. An eventual fix needs both a cold-start load and the save-to-slot-screen round trip; validate iOS separately from Mac evidence.

The motivating game's observations and open configuration-loading question are tracked in [issue #2930](https://github.com/seedds/nxnative/issues/2930); this recipe does not deploy a fix or a version-check bypass.

## Game behaviour that is not a host bug

### Most draws are in command buffers replayed later

- **Symptom:** "stuck" at a screen with one draw per frame.
- **Cause:** usually not stuck: most draws are in recorded command buffers replayed later.
- **Fix:** dump the passes (`DM_NVN_PASSDUMP`, [04, Diagnostic switches](../agents/04-graphics.md#7-diagnostic-switches-host)) before assuming the game logic is blocked.

### Buttons mapped by position: B confirms

- **Symptom:** menus ignore A.
- **Cause:** not a host fault: the game maps its buttons by position, B confirms and A goes back (its prompts say so).
- **Found on:** [DARK SOULS: REMASTERED](../../dm/games/01004AB00A260000/README.md) (SDK 4.5.0). The first boot shows two notices before the title menu ("Unable to connect to the network", then "Game will start in offline mode"), and a new game first walks through Brightness, Options and Button settings.

### Any Lua error quits a LÖVE game

- **Symptom:** a crash in `main+0x339c00` a second or two into the title, in love-nx's video module teardown (`love::video::theora::Video` → `~Worker`; its thread released the worker that the closing module deletes too).
- **Cause:** the Lua state was closing: on the Switch, Balatro's `love.errhand` returns at once, so any Lua error quits. A `print` at the top of `love.errhand` in a copy of the RomFS (`DM_ROMFS=<copy>`) gave the error: `Shader uniform 'distortion_fac' does not exist` (the capture run had no shader library, so every program linked with no uniforms), then `bad argument #2 to 'send' (number expected, got boolean)` (a `bool` uniform reported as `GL_UNSIGNED_INT`).
- **Fix:** look for the Lua error first; the two errors here were host faults in the GL layer's shaders and uniforms (see [GL shaders put together at run time and captured](rendering.md#gl-shaders-put-together-at-run-time-and-captured) and [GL uniforms, pipelines and attributes](rendering.md#gl-uniforms-pipelines-and-attributes)).
- **Found on:** [Balatro](../../dm/games/0100CD801CE5E000/README.md) (SDK 18.3.2, LÖVE 11.4; one boot).
- **Check:** probing the destructors (`DM_PROBE`) and logging every object the Lua GC released (`DM_PROBE=main:0x18b920@x0+0`: the module proxies all at once) showed it was a shutdown, not a premature collection.

### TV latency calibration's late, beat-timed listening window

- **Symptom:** the TV latency calibration ("press A when you hear the fourth beat") ignores A.
- **Cause (not a host bug):** docked, the first boot runs it (scene script `main+0x4c0820`). It starts listening only `0x3c0` frames (16 s) after the prompt, through the active controller (`main+0x22b0` id `0x3ed`, virtual button 4 = A), and passes when about ten presses keep a steady offset within −5 to +13 frames of the cue (evaluation `main+0x2948c0`; offset = cue ticks at `+0x34` / about 17 ticks per frame, about 480 per beat).
- **Testing it:** scripted presses at fixed seconds drift off the beat; use frame-timed `DM_KEYS` at the bar period (117.12 frames at its 123 BPM) and read each press's offset with `DM_PROBE=main:2951f0@34`. A passed calibration is saved per TV panel identity (the host's all-zero one included, [Default display panel identifier](services.md#default-display-panel-identifier)) and not asked again.
- **Profile:** it plays handheld with Boost (`op_mode handheld`, `perf_mode boost`), which skips the TV setup and keeps the 1080p draw size.
- **Found on:** [Rhythm Heaven Groove](../../dm/games/0100D9F01D474000/README.md) (SDK 21.4.0).

### "Failed to authenticate" at the title: offline play only after a recent online login

- **Symptom:** A at the title shows "Connecting to Battle.net", then "Failed to authenticate. Please try again."; OK goes back to the title, and every try ends the same way. No `[RED]` line.
- **Cause (not a host bug):** the game's own policy. It allows offline play only within 30 days of its last Battle.net login, kept in its settings as "User Last Online"; a save that was never online has no such setting, so offline is refused, as on a console that never went online. The host is offline (nifm unavailable, the account token and Nintendo Account requests fail), and no answer of those services passes the check: the login is the game's own Battle.net request.
- **Fix:** a rewriter cut in the game's `patches.py` sets the offline-allowed flag, so the game behaves as a console that was online within 30 days; the title then goes to the main menu, where Offline is chosen when a character is made. The game's code is where the cause lies, so the cut is the fix: a fixed cut for 1.0.34.0 and the same cut as a function-matched rule (`MATCH_CUTS`) for the game's other builds that keep the function.
- **Found on:** [Diablo II: Resurrected](../../dm/games/0100726014352000/README.md) 1.0.34.0 (SDK 11.4.3). The check and its 30 days were first described in d2r-mobile's notes on a community offline fix; the cut here is its own.

## Scripted state

### The software keyboard in scripted runs (`DM_KBD`)

- **Symptom:** in a scripted run, a screen goes back by itself right after a confirm; the log shows `swkbd: … → no window, treated as cancel`.
- **Cause:** the confirm opened the system keyboard (`swkbd`, name entry). Without a window the host answers "cancel" by design.
- **Fix:** pass `DM_KBD=<text>` to answer it.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md) (its appearance choice).

### Player join screens (`hid::StartLrAssignmentMode`)

- **Symptom:** a player join screen that ignores A (a character select with four "L + R to join" slots after the title).
- **Cause:** the game calls `hid::StartLrAssignmentMode` and waits for L and R pressed together on the join screen itself, then a character choice with A.
- **Fix:** in `DM_KEYS` that is `L+R~0.5` while the screen is up (presses on the title before it are lost).
- **Found on:** [Super Mario 3D World + Bowser's Fury](../../dm/games/010028600EBDA000/README.md).
- **Note:** on the game select, holding the stick right for a second (`7:LRight~1`) switches to Bowser's Fury, which also uses `nvnTextureGetSwizzle` and `nvnTextureGetStorageClass`. Map and level movement read the left stick only: `DM_KEYS` takes `LLeft` / `LUp` / `LRight` / `LDown`. A working route into World 1-1 on a fresh save: `DM_KEYS=6:A,10:A,14:L+R~0.5,17:A*3/2,24:Plus*6/3,45:A*6/3,66:LUp~1,70:A,74:LRight~1,78:A,82:LLeft~1,86:A` (World 1-1 by frame ~6000).
