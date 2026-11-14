# Nintendo Switch Sports Resort (`04008E001F098000`)

A Switch 2 game: update v65536 over the base game, SDK 22.2.10 built for the Switch 2 (`Ounce`), NVN2. Nintendo EPD's sead / agl / gsys with Havok. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`; `subsdk1` to `subsdk4` are not loaded.

**Runs on the Mac** (M6, macOS 27.2, 2026-11-12): from the safety and strap screens through the title, Select User, the sport menu and Tennis's settings into a doubles match, at 60 fps; a second start from its save. Scripted input cannot serve, so no point was played here ([Motion controls](#motion-controls)). bymaxmarc played matches with motion from a Joy-Con 2 and a DualSense on an M2 Max in a 37-minute session ([PR #2387](https://github.com/seedds/nxnative/pull/2387), with that pull request's versions of the host changes). Not run on an iPhone or iPad.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x2000000` | [`game.tsv`](game.tsv) | the NPDM's 32 MB of system resource: virtual address memory |
| `mem_reserve 0x318000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | the Switch 2's 9 GB pool and the 7.47 GB heap the game asks for (the NSP script's heap check). In its first 15 s the game's regions reach alias +2.82 GB of 4.38 GB; bymaxmarc saw +3.26 GB in play |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0, so the pipeline bases made before it runs take that mode |

Render conditions, which the host evaluates for every game ([Conditional rendering evaluated at replay](../../../docs/reference/rendering.md#conditional-rendering-evaluated-at-replay); until 2026-11-12 the profile key `render_cond`): the game wraps draws in render conditions; drawn anyway, Select User's background is grey and the outfits are black ([Rendering](#rendering)).

There is no game-specific patch and no rewriter cut. Everything else it needed is in the host, in general: see [Found on this game](#found-on-this-game).

## Files

- A dump folder (`base/`, `updates/65536/`): `build_ipa_from_nsp.py <folder> --platform mac` packs it with no keys. The display version is not known (a dump has no control data): the `build` row has `?`.
- The RomFS holds 48,576 files, 9.7 GB packed; the marker is `Tex/RBake_Scene_AnnaBoxingDesign00Scene_d1_t0.bntx.zs`.
- An FTP dump keeps a `.listing` in every folder. Packed with them, the game crashed at `main+0x9d7c60` listing its `Parameter` folders; the packer now leaves them out ([A dump tool's `.listing` files in the game's folders](../../../docs/reference/packing.md#a-dump-tools-listing-files-in-the-games-folders)).

## Shaders

- 11,059 shaders (`Nin_Ounce_NVN2` BNSH) in [`shader_map.tsv`](shader_map.tsv); a full Mac build takes about 9 minutes on the M6, 4:46 for a second one.
- 6 are refused (4 task, 2 mesh shaders of the sea, agl `AnnaSea`): their task payloads (21-28 KB) are past Metal's 16 KB ([issue #1753](https://github.com/seedds/nxnative/issues/1753); FifteenPen's [PR #2264](https://github.com/seedds/nxnative/pull/2264) passes them through a device buffer). No draw has been seen to use them.
- 612 pixel shaders read the fragment's shading-rate size; until it read 1 x 1 the sea and the avatars' hair and clothes were black ([The fragment's shading-rate size reads 1 x 1](../../../docs/reference/shaders.md#the-fragments-shading-rate-size-reads-1-x-1)).
- [`pso_manifest.tsv`](pso_manifest.tsv) is this Mac's own list (3,474 shaders, 1,868 render, 41 compute and 22 task / mesh pipelines): a first start, the menus and a doubles match. The pull requests' lists were recorded by an older host: none of their render or mesh pipeline keys matches the current one's.

## Rendering

- **Render conditions.** Drawn unconditionally (as the host did for games without the profile key `render_cond`, before it evaluated them for every game), Select User's background is grey instead of the court and the outfits are black, in the menus only. FifteenPen measured why the outfits are black: their material shader `c30f0edd6c669a61` reads R and A of a screen-space shadow target that the menu blurs under a channel mask of G and B, and both read 0 there. Evaluated, the background and the outfits are right, on the same route from the same save (A/B, 2026-11-12) ([Conditional rendering evaluated at replay](../../../docs/reference/rendering.md#conditional-rendering-evaluated-at-replay)). FifteenPen's [PR #2273](https://github.com/seedds/nxnative/pull/2273), a clear under a partial channel mask that keeps the other channels, is not needed with it. What a console does with a clear under a channel mask is not known: the game's `nvnCommandBufferClearColor` (`sdk+0x652380`) jumps through a pointer the system's NVN driver fills, so the game's files do not show it.
- agl's upscaler packs its input into an RGB9E5 texture through an R32_UI render-target view; drawn in the base format the picture was solid red ([A render-target view of another class](../../../docs/reference/rendering.md#a-render-target-view-of-another-class-the-texture-is-upgraded-not-drawn-in-its-base-format)).
- `[RED] texture type mismatch` for shader `7309860342dd3b14_b` (a 2D texture declared where a 2D array is bound), once in a match; its effect is not checked.

## Frame rate

- In the doubles match on the M6 (window, 2026-11-12): frame time p50 16.67 / p95 17.3-17.9 ms, NVN submit and present about 3.4 ms, CPU about 92% of one core.
- bymaxmarc's measurements on the M2 Max (scene loads of 110-225 ms, a 250 ms GPU frame at boot) are in [PR #2387](https://github.com/seedds/nxnative/pull/2387)'s note.
- Out of focus the game pauses and its GPU queues wait on it for 6-10 s at a time (`[RED] queue 4: an event wait has held it …`); runs set `DM_FOCUS_KEEP=1`.

## Motion controls

- The game reads motion through the SDK's sensor fusion (`hid::UpdateSensorFusion` → `GetNpadImuSamples`) and supports Handheld | JoyDual only, so the keyboard, a DualSense or a single Joy-Con is shown to it as a Joy-Con pair ([The Npad IMU sample holds the acceleration first](../../../docs/reference/services.md#the-npad-imu-sample-holds-the-acceleration-first), [A controller in a style the game does not support](../../../docs/reference/services.md#a-controller-in-a-style-the-game-does-not-support)).
- Without a motion controller a point cannot be played. `Shake` does not serve; nor did any of ten scripted moves of bymaxmarc's [PR #2386](https://github.com/seedds/nxnative/pull/2386) (swings about ±x, ±y, ±z and a lift, in one 60 s and one 90 s run): the racket follows them, and the serve prompt stays.

## Miis

- The game mounts the Switch 2's Mii model data, system data `0400000000000849`, as `MiiResource:`. The host refused it (`[RED] fs mount system data MiiResource: the host has no 0400000000000849`), and bymaxmarc's session of 2026-11-11 ended 3.7 minutes in, in two-player Avioneta (frame 10,065), with a guest crash reading address 0xc at `main+0x1cdcfb4`: the game's Mii code read a part size through the archive's null file pointer. The host now serves its Mii stand-in there ([The Switch 2's Mii model archive](../../../docs/reference/services.md#the-switch-2s-mii-model-archive)); Mii heads build and draw nothing.
- With every texture part of that stand-in empty (size 0), a two-player skate session of 2026-11-12 ended 193 s in, on leaving the practice zone, with `CRASH sig=11 addr=0xc pc=h_nvnTextureGetFlags+0`, `x0=0`: a renderer of the game's Mii models (`main+0x3fbb420`) registered the model's texture 1, whose part the stand-in had left out, and got a null texture ([A Mii part the stand-in leaves out](../../../docs/reference/services.md#a-mii-part-the-stand-in-leaves-out)). Every texture part of the stand-in is now one blank texture.
- With both, a player played an 11-minute session on an M2 Max on 2026-11-12, two-player skate (the practice zone, then leaving it) and two-player Avioneta, with no crash. The crashes need a second player, which `DM_KEYS` and `DM_CTL` cannot drive, so no scripted run reproduces them.

## Driving it on the Mac

The work folder `04008E001F098000-work` beside the dump has `run/boot.sh <log> <secs> [VAR=value…]` (`DMHOST=` for another host), `run/go.sh <shot> <wait> <presses…>` for a run with `DM_CTL`, `run/to_select.sh` and `run/play.sh <prefix> <host> <secs>`: from the save in `run/save`, A presses through the title, Select User, Tennis's match settings and the court into a doubles match at its serve prompt (about 2 minutes), then `Shake` every 2 s with a shot every 30 s.

## Open

- A point played on the Mac in this tree (needs a motion controller, or scripted moves that serve).
- Sports other than tennis.
- Mii heads need the player's own console files: without the system data the game mounts as `MiiResource` (`0400000000000849`) the host's stand-in for it draws none ([Miis](#miis)). With that archive and the console's Mii database under `DM_SYSTEM_DIR` ([A console's own Mii database and system data](../../../docs/reference/services.md#a-consoles-own-mii-database-and-system-data)) it lists the player's Miis and draws their heads (FifteenPen, PR #2274, M4 Pro); not run in this tree, which has no console files.
- The 6 refused mesh shaders, above.
- iPhone and iPad: no IPA built.

## Found on this game

- [A dump tool's `.listing` files in the game's folders](../../../docs/reference/packing.md#a-dump-tools-listing-files-in-the-games-folders)
- [File calls on a mount in the SDK's own table](../../../docs/reference/services.md#file-calls-on-a-mount-in-the-sdks-own-table)
- [A read-only mount of the game's own save data](../../../docs/reference/services.md#a-read-only-mount-of-the-games-own-save-data)
- [The virtual output device's properties, and the OutputStream open without flags](../../../docs/reference/audio.md#the-virtual-output-devices-properties-and-the-outputstream-open-without-flags) and [The output device notification under its SDK 21 and 22 name](../../../docs/reference/audio.md#the-output-device-notification-under-its-sdk-21-and-22-name)
- [A controller in a style the game does not support](../../../docs/reference/services.md#a-controller-in-a-style-the-game-does-not-support)
- [Switch 2 hid calls that open the hid service](../../../docs/reference/services.md#switch-2-hid-calls-that-open-the-hid-service), [The SDK's IMU handles are the console's six-axis handles](../../../docs/reference/services.md#the-sdks-imu-handles-are-the-consoles-six-axis-handles), [The Npad IMU sample holds the acceleration first](../../../docs/reference/services.md#the-npad-imu-sample-holds-the-acceleration-first), [The SDK's sensor fusion needs sensor noise and a stored calibration](../../../docs/reference/services.md#the-sdks-sensor-fusion-needs-sensor-noise-and-a-stored-calibration)
- [Home button protection on a Switch 2 controller](../../../docs/reference/services.md#home-button-protection-on-a-switch-2-controller)
- [The Switch 2's Mii model archive](../../../docs/reference/services.md#the-switch-2s-mii-model-archive) and [A Mii part the stand-in leaves out](../../../docs/reference/services.md#a-mii-part-the-stand-in-leaves-out)
- [The fragment's shading-rate size reads 1 x 1](../../../docs/reference/shaders.md#the-fragments-shading-rate-size-reads-1-x-1)
- [A render-target view of another class: the texture is upgraded, not drawn in its base format](../../../docs/reference/rendering.md#a-render-target-view-of-another-class-the-texture-is-upgraded-not-drawn-in-its-base-format)
- [Conditional rendering evaluated at replay](../../../docs/reference/rendering.md#conditional-rendering-evaluated-at-replay)
- [Tests of one uniform word folded into switches](../../../docs/reference/shaders.md#tests-of-one-uniform-word-folded-into-switches) and [Specialized second functions in the offline pipeline archive](../../../docs/reference/shaders.md#specialized-second-functions-in-the-offline-pipeline-archive)
- [A console's own Mii database and system data](../../../docs/reference/services.md#a-consoles-own-mii-database-and-system-data): its Mii heads come from the Switch 2's own archive, `0400000000000849`.
