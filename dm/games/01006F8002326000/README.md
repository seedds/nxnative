# Animal Crossing: New Horizons (`01006F8002326000`)

Update 3.0.3 (v2228224) over the base game, with the Happy Home Paradise add-on mounted. SDK 20.5.4. Nintendo EPD's own engine (code name Park; sead / agl / gsys, NintendoWare G3d / Ui2d / Vfx2 / Atk / Font, Havok, Pia and NEX for local and online play) on NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. Plays on the iPhone; 30 fps, as on the Switch.

On the Mac: a new game through Tom Nook's counter (name entry on the game's own keyboard, birthday, look, hemisphere, island choice), the in-flight movie with its sound, the island's creation and the arrival (Timmy's welcome, the player at the dock under control); no red line but the ones below. 1.85 GB resident and 1.15 GB of Metal allocations at the dock.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `render_cond on` | [`game.tsv`](game.tsv) | the game wraps its clears in render conditions that skip a target nothing drew into since its last clear, as Tears of the Kingdom does ([0100F2C0115B6000](../0100F2C0115B6000/README.md)). Without the key, four clear-only passes wiped the composite before the window copy at frame 900, and every frame came out black (the island probe and the loading icon were drawn into render targets) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource (META +0x14 = 0): `nninitStartup` sets a heap (3.09 GB) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region |
| `mem_used 0x7a00000` (122 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory (`main` alone is 63 MB of text and 15 MB of bss); the NSP script's guess after the rewrite, which leaves a 3.09 GB heap |
| `pad_layout full` | `game.tsv` | the 3D layout: the D-pad holds the tool ring and the right stick turns the camera |
| `shader_groups` `/Shader/` `/System/` `/Effect/` `/Model/` `/Layout/` `/Swkbd/` | `game.tsv` | see [Files and shaders](#files-and-shaders) |

## Rewriter

- **Resolved: a `B()` assertion at a spin wait, a branch into the patch area beyond the ±128 MB a `b` reaches.** Its `main` was the first module to put branches out of range of the patch area: 63 MB of text and 51 halfword jump tables (`ldrh wE, [xT, xI, lsl #1]; …; adr x18, #L; add x18, x18, xE, lsl #2; br x18`). Each got a landing pad for every one of the 65,536 values an entry can hold (768 KB a table), whatever the switch's size: 40 MB, and the spin-wait stubs placed after them were out of reach. `half_count` now pads only up to a table's largest entry; 7 of the 51 cannot be read and keep all 65,536. The patch area went from 42 MB to 7.9 MB.
- The spin-wait search through stores finds 166 loops in `main` (162 before), and the movie library's thread loop is wider in `subsdk0` ([development.md, Power](../../../docs/development.md#power)).

## Threads

- **Resolved: a black screen with the plane icon for 77 s after the opening movie** (also about 40 s of the same at boot), the CPU nearly idle. `SaveProc` yielded about 2,600 times a second, the power line's top yielder with as many back-off sleeps. The thread was working, not waiting: its grid walks over the new island (`main+0x157d020`) call `nn::os::YieldThread` once per cell, and the back-off made each yield a 250 µs sleep. A yield whose caller's x19–x28 changed since the last one now counts as progress and returns at once (`thr_yield` in `hle_thread.c`). The island now shows about 10 s after the movie, as with `DM_YIELD_BACKOFF=0`. A temporary log of which of x19–x28 move at each yield, per thread, told the working loops from the polls.

## Services

- **Resolved: abort in `sm::Initialize` under `sf::hipc::InitializeHipcServiceResolution`, right after the first frames** (from `main+0x5b8198`). `main` links its own clients of `olsc:u` (cloud save data backup settings) and `npns` (push notifications), which talk HIPC through `sm` and `svcSendSyncRequest`. For each user the game asks whether backup is on and switches it off where it is (`main+0x2809c8c`); a failed connect aborts as well. `hle_sf.c` takes over the SDK's HIPC client entry points (`InitializeHipcServiceResolution`, `ConnectToHipcService`, `SendSyncRequest`, `CloseClientSessionHandle` and the control requests `QueryPointerBufferSize`, `ConvertSessionToDomain`, `DuplicateSession(Ex)`): `olsc:u` gets a host session, and every request on it succeeds with zeroed output, which reads as backup off for every user. Any other name is refused with a red line, as `sm` refuses a service nobody registered (`npns` was never connected). `DM_SF_TRACE=1` dumps each request.
- **Current: device save data.** 3.0.3's `main` mounts no account save data; the island is in device save data (`MountDeviceSaveData`, `GetDeviceSaveDataSize`, `ExtendDeviceSaveData`), which the host gives the account save's host directory and transaction (one player). A second mount name over a save already mounted shares that mount's transaction (`mount_ex`); it used to clone the committed directory again, over the first name's pending writes. Also `fs::Commit` with a `CommitOption` (its one flag clears the restore flag) and `GetSaveDataRestoreFlag` (never restored from a backup). A game whose `main` imports `MountSaveData` too gets a device save of its own (Mario Kart 8 Deluxe, [0100152000022000](../0100152000022000/README.md)); this game does not, so its islands stay where they are.
- **Resolved (PR #25): `fs::Commit(const char* const*, int)`, the mount-list overload.** The SDK's own code opened fsp-srv and aborted, leaving the save writes uncommitted. The host commits the list as one transaction when every name is the same host save; cache and temporary storage in the list are skipped, and a list of two separate saves is refused without committing either.
- Found in `main`'s imports before the first boot: the adjustable user clock (`time::AdjustableUserSystemClock::GetCurrentTime`, which the game's days follow) reads the time service's shared memory in the SDK (`MonotonicSystemClock::GetCurrentTime`, `sdk+0x4421cc`), which the host never maps, so it is the host's clock, as the user clock; `time::GetStandardSteadyClockCurrentTimePoint`, the free-function form of the steady clock; `oe::GetRegionCode` (from the language: Japan, the Americas, Europe or Hong Kong / Taiwan / Korea); the SDK 20 album calls `SaveAndShareScreenshot` with a share ID and `GetSharePostedLastUrl` (no album: they fail).

## Saves

- A save is not reached on the Mac: the game first saves when the player sleeps at the end of the first day (the `−` menu says "You can't save right now." before that). Until then it only probes for `main.dat`, `mainHeader.dat` and `Moved/personal.dat` in its device save data.

## Movies

- The in-flight movie `Movie/OpeningMovie.webm` is VP9 1080p with Vorbis, demuxed by `movie::Extractor` and decoded on the host (`hle_movie.c`).
- **Resolved: the movie showed as a black screen with the loading icon, one picture every few seconds, while its sound decoder kept up.** The player holds every output it is offered (16 of 16 video, 59 of 64 audio), so how far its extractor can get ahead of the picture on screen is the video slots plus the 4 queued packets. With 8 video slots that was 0.4 s, and the audio behind the next video packet, whose clock the player follows, ran dry: 86 pictures in 25 s. With 16 or 32 slots all 1,960 play at 30 fps. A video decoder now has 16 (`MD_NOUT_VIDEO`); a slot is allocated when first used, so the 8 more cost 24 MB of NV12 at 1080p only in a player that holds them all.

## Files and shaders

- RomFS: 97,997 zstd files (`.zs`, most of them models), BWAV sound, a few Yaz0 SARC archives (`.szs`). The profile's `romfs_marker` is `Movie/OpeningMovie.webm`.
- **Current:** 9,766 BNSH programs: `Shader/Park_UBER_All` (3,385) and the other bfsha / sharcb in `Shader/`, agl's `System/Agl/agl_resource` (3,206), particle effects in `Effect/` (2,522), models' own bfsha in `Model/`, layouts (bgsh) in `Layout/`, the game's keyboard in `Swkbd/` and 40 in `main`.
- The keyboard's ocean background has 8 geometry shaders, which do not translate; the name entry draws without them.
- Prewarm list: recorded from a fresh save through the counter, the movie and the arrival to the dock (965 lines).

## Screen shape (`DM_SCREEN_ASPECT`, current)

With `DM_SCREEN_ASPECT=1.43` (launcher setting *Picture shape*, offered because the profile has `screen_aspect on`; the host side is Breath of the Wild's, [its note](../01007EF00011E000/README.md#screen-shape-dm_screen_aspect-current)) the game renders a 1920x1344 picture of the screen's own shape instead of a 16:9 one: 16:9's sides with more above and below, the layouts in their own shape, and their edge widgets at the taller screen's edges. The stubs in `patches.py` (3.0.3 `main` only) each take one bit of `DM_SCREEN_ASPECT_SITES`:

- **Sizes** (bits 0-4, 7, 8). The platform config (vtable `main+0x47fa0e0`, no RTTI) hands out the display size three ways: the graphics create arg (`main+0x5c9b44`: +0x60 the display size from the table at `main+0x4076728`, +0x20 1280x720), slot 7's static display size and slot 8's static 1280.0 x 720.0, and slot 3's arg +0x14. Each takes the screen's height (width x the output's height / 1920, in integers: 1920 → 1344). Every frame `main+0x1527ebc` reads the resolution table's entry into the display buffer's crop and the 3D views' render scale: without its two loads (`main+0x1527f00`, `main+0x1527f2c`) the 1920x1344 window was cropped back to its top 1920x1080. The layouts' DrawInfo takes the framebuffer's size at `main+0x31cee60` / `main+0x31cee9c` for nn::ui2d's scissors and captures.
- **The view** (bit 5). The island camera keeps its vertical angle in radians at +0x2cc (0.384, 22 degrees) and reads it twice (`main+0x7a895c`, `main+0x7a89a8`); both reads, and the other camera path's half angle (`main+0x386a7a0`), are widened to 2 atan(tan(fovy/2) k), k = (16/9) / ratio. The aspect already follows the taller viewport, so before this the picture kept the vertical angle and lost a fifth of each side.
- **Layouts' projection** (bit 6). `nn::ui2d::DrawInfo::SetProjectionMtx` (`main+0x265100`) gets the layouts' perspective projection of a 16:9 rectangle (m0 1.545, m5 2.747, fovy 40 degrees); its y row is divided by the vertical stretch (layout height / 720) before the return, so layouts keep their shape, centred. Only a projection within 5% of 16:9 is divided: the game copies a DrawInfo's projection into another, and without the check the title's layouts were divided twice.
- **Edges and backgrounds** (bit 9, `duo_anchor.c`, run after `nn::ui2d::Pane::Calculate` has computed a pane's global matrix, `main+0x27d570`). The game's panes are its own classes (no RTTI): what draws is told by its vtable's DrawSelf (slot 21) against `nn::ui2d::Pane::DrawSelf` (`main+0x27e2c8`, empty). A full-screen or full-height background leaf is stretched to the taller screen; a widget (the topmost pane of its chain that is not a null pane, alignment or layout-sized container) beyond 230 units from the middle towards a corner (|x| 400 or more), or 270 nearer the middle, moves out by the extra half-height with its children. Layouts are 1920x1080 (793 of 959) or 1280x720; the distances are in 720 units and scaled by the root pane's size. Layouts placed by their root (balloons over characters) are left alone. `DUO_ANCHOR` is the code of `duo_anchor.c` built with the command in the comment above it.

Checked on a Mac (M1, macOS 27, headless, an island save) against 16:9: the 3D view keeps 16:9's sides and shows more above and below; the title's version, press-A and settings lines, the island's ZL icon, clock, minimap and button guides sit at the new edges; the pockets, the phone (its backdrop's light band stretched) and the save dialog keep their shape. On an iPhone Duo (iOS 27.1) an earlier build of this looked right to its player. Open: an NPC's dialog box stays at its 16:9 place (-320), not at the taller screen's bottom edge; the phone's blurred backdrop shows a hard edge at its top left.

## Mac routes

- From a fresh save, `DM_KEYS="40:A*51/2,145:B*12/0.4,151:Down,152:Right*4/0.4,154.5:A,155.5:Right*4/0.4,158:A,159:Down*2/0.4,160.5:Left*2/0.4,162:A,163.5:Plus,167:A*4/2,175:Plus,178:A*146/2,179:Plus*30/4"` names the player "tom" on the keyboard, takes the defaults, watches the movie and reaches the dock at about frame 14,100 (470 s).

## Found on this game

General host changes first needed here; each applies to every game:

- **The host's shaders compiled when the host is built** (`dm/gfx/msl`, `host_msl_lib`): the HUD's `Compiled 2 shaders … 102.70ms` in play on an iPhone 17 Pro Max came from clear-triangle libraries compiled from source when a new room first issued a scissored clear; [The host's own shaders compiled from source at run time](../../../docs/reference/shaders.md#the-hosts-own-shaders-compiled-from-source-at-run-time).
- **Halfword jump tables padded to their largest entry** (`half_count` in `patch_nso.py`): [Halfword jump table landing pads sized to the largest entry](../../../docs/reference/rewriter.md#halfword-jump-table-landing-pads-sized-to-the-largest-entry).
- **Yields that make progress return at once** (`thr_yield` in `hle_thread.c`): [Yields that make progress return at once (`thr_yield`)](../../../docs/reference/threads.md#yields-that-make-progress-return-at-once-thr_yield) and [development.md, Power](../../../docs/development.md#power) ("Yield loops").
- **The game's own HIPC clients answered by host sessions** (`hle_sf.c`, `DM_SF_TRACE`), **device save data and shared transactions** (`mount_ex`), **`fs::Commit` with a mount list or a `CommitOption`**, and the adjustable user clock, region code and SDK 20 album and steady-clock calls: [Game-linked HIPC clients of system services](../../../docs/reference/services.md#game-linked-hipc-clients-of-system-services), [Device save data on the account save's directory](../../../docs/reference/services.md#device-save-data-on-the-account-saves-directory), [`fs::Commit` with a list of mounts](../../../docs/reference/services.md#fscommit-with-a-list-of-mounts), [Steady clock, clock snapshots and user clocks](../../../docs/reference/services.md#steady-clock-clock-snapshots-and-user-clocks), [Region code from the language](../../../docs/reference/services.md#region-code-from-the-language), [Album calls fail with no album](../../../docs/reference/services.md#album-calls-fail-with-no-album).
- **16 output slots for video movie decoders** (`MD_NOUT_VIDEO` in `hle_movie.c`): [Video output slots: how far the extractor gets ahead](../../../docs/reference/movies.md#video-output-slots-how-far-the-extractor-gets-ahead).
- **Functions with function constants specialized by the pipeline** (`fn_spec`): the HUD's `Compiled 2 shaders … Consider adopting MTL4Compiler` during play on the iPhone with Metal 4 pipelines on: [Functions with function constants specialized by the pipeline, not the library](../../../docs/reference/shaders.md#functions-with-function-constants-specialized-by-the-pipeline-not-the-library).
- [The alpha test through a library of its own](../../../docs/reference/rendering.md#the-alpha-test-through-a-library-of-its-own): the island map's corruption (issue #73) improved with an alpha test in a downstream runtime (PR #430).
- **The game's view in the shape the game presents** (`front_picture_shape`; with `DM_SCREEN_ASPECT=1.43` the 1920x1344 picture was fitted into a 16:9 view on the phone): [The picture laid out in the shape the game presents](../../../docs/reference/rendering.md#the-picture-laid-out-in-the-shape-the-game-presents).

## Open

- Check the island map (issue #73) with the host's alpha test.
- `game.tsv` still describes the game as "In bring-up", while it plays on the iPhone.
- A save on the Mac (the end of the first day) has not been reached.
- The keyboard's 8 geometry shaders do not translate.
