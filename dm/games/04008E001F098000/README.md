# Nintendo Switch Sports Resort (`04008E001F098000`)

Work in progress. With the pull requests below, it boots on the Mac and plays to its title, Select User, the menus, golf and tennis at its own 60 fps (M4 Pro, macOS 27.2; GPU about 10 ms a frame in a 2560x1440 window). On main alone it does not boot. Several pictures are still wrong, and motion controls and Miis do not work ([Open](#open)).

## What it needs

- [PR #1561](https://github.com/seedds/nxnative/pull/1561): SDK 21's `GetVirtualOutputDeviceProperties`. The game asks for it after the safety screen; without it the SDK connects to `auddmg:u` itself, which the host does not serve, and aborts (`Result 0xa99`).
- [PR #2105](https://github.com/seedds/nxnative/pull/2105): file calls on a mount only the SDK holds. The game copies its VoiceText dictionaries into a RAM disk it mounts as `vtapi` and reads them back; without it, `Result 0x202` at `main+0x414dbc` at boot.
- [PR #2106](https://github.com/seedds/nxnative/pull/2106): the controller icon colours and the controller IMU, which the SDK's own functions ask the hid service the host does not serve (`Result 0xe15` at boot), and the IMU's samples, from which the SDK's sensor fusion calibrates the Joy-Cons and gives the game their motion.
- [PR #2103](https://github.com/seedds/nxnative/pull/2103): the fragment's shading-rate size (system register 0x84) reads 1 x 1. 612 of its pixel shaders read it; without it the sea is black on the title screen and in the menus (chain change).
- [PR #2264](https://github.com/seedds/nxnative/pull/2264): six task and mesh shaders whose payload is past Metal's 16 KB ([Shaders](#shaders)).
- [PR #2271](https://github.com/seedds/nxnative/pull/2271): integer reads of game uniforms as volatile loads. On main some of its material pipelines take Metal's compiler minutes and gigabytes each, the prebuild runs `MTLCompilerService` out of memory, and the pipeline archive loses whole files, so the game compiles pipelines for minutes at every start.
- [PR #2273](https://github.com/seedds/nxnative/pull/2273): a colour clear under a partial channel mask leaves the other channels. Without it outfits are black in the menus (Customize, Select User) while they are right in play.
- [PR #2274](https://github.com/seedds/nxnative/pull/2274), for Miis: the player's own Mii head models (system data `0400000000000849`) and Mii database (system save `8000000000000030`), from `DM_SYSTEM_DIR` ([Miis](#miis)). The game runs without them, with no Mii heads and only the six default Miis.
- [PR #2104](https://github.com/seedds/nxnative/pull/2104): a read-only mount of the game's own save. Without it Select User offers Begin Career to a user who has a career, and the game aborts on its own save check (`SaveDataErrorType::UpdateOrderForNotUsable`) the next time it saves.
- The rest of the profile is the survey's guess (`dm/games/profile_guess.py`) and the NSP script's heap check (a 7.47 GB heap, `mem_reserve`; `depth_mode zero`), unchanged after play.

## Miis

The game draws Mii heads from system data `0400000000000849` and lists the console's Miis from the mii service's database; both come from the player's own console, never from the app ([PR #2274](https://github.com/seedds/nxnative/pull/2274)):

- `~/Library/Application Support/nxnative/system/sysdata/0400000000000849/`: the archive's files (`ShapeMid.dat`, `ShapeHigh.dat` and six `OunceTexture*.dat`). The Switch's MiiModel archive (`0100000000000802`) does not do: the game reads only the `OunceTexture*` files.
- `~/Library/Application Support/nxnative/system/save/8000000000000030/MiiDatabase.dat`: the Mii database, the same file as on the Switch.

With both in place the Miis are listed first on Select User and their heads draw in the menus and in play.

## Files

- A dump folder: the base game and update v65536. The RomFS lists 48,576 files (9.71 GB); the marker is `Tex/RBake_Scene_AnnaBoxingDesign00Scene_d1_t0.bntx.zs`. The Mac app is 11 GB.
- The display version of v65536 is not known here: the profile's `build` row has `?`.

## Shaders

- 11,059 shaders (`Nin_Ounce_NVN2` BNSH), translated in about 9 minutes on the M4 Pro.
- On main, 6 are refused at the mesh stage (4 task, 2 mesh shaders: `shared memory of its own, or stage memory past Metal's limit`, issue #1753). Their task shaders hand 21-28 KB of payload to their mesh shaders, past Metal's 16 KB payload and 32 KB of threadgroup memory. [PR #2264](https://github.com/seedds/nxnative/pull/2264) passes the payload through a device buffer, and all 11,059 build with it. No draw with them has been seen yet, so the scene that uses them is not known.
- `pso_manifest.tsv` is the host's per-device list from the sessions played (5,934 rows).

## Found on this game

- [The fragment's shading-rate size reads 1 x 1](https://github.com/seedds/nxnative/pull/2103) (PR #2103).
- [A read-only mount of the game's own save data](https://github.com/seedds/nxnative/pull/2104) (PR #2104).
- [File calls on a mount only the SDK holds](https://github.com/seedds/nxnative/pull/2105) (PR #2105).
- [Switch 2 controller icon colours, IMU samples and sensor fusion](https://github.com/seedds/nxnative/pull/2106) (PR #2106).
- [A task shader's payload past Metal's 16 KB](https://github.com/seedds/nxnative/pull/2264) (PR #2264).
- [Integer reads of game uniforms and Metal's compile time](https://github.com/seedds/nxnative/pull/2271) (PR #2271).
- [A colour clear under a partial channel mask leaves the other channels](https://github.com/seedds/nxnative/pull/2273) (PR #2273).
- [A console's own Mii database and system data](https://github.com/seedds/nxnative/pull/2274) (PR #2274).

## Open

- Golf: past the fairway, the ground and the sea drew as one flat pale blue on main, so trees and cliffs looked as if they float. On a tree with the Ampere accuracy series of issue #2040 merged (#2041 to #2067) they draw right (one hole checked); which of those PRs fixes it is not narrowed down.
- Golf: the minimap has black squares at its corners.
- Tennis: on the VS screen, the left player's partner is drawn with horizontal stripes.
- The title and menus show a faint vertical seam down the middle of the sky.
- Gameplay is letterboxed (black bars above and below); not checked whether that is the game's own framing.
- Motion: with [PR #2106](https://github.com/seedds/nxnative/pull/2106) the Joy-Cons' motion reaches the game and its sensor fusion calibrates them on its own while they lie still (the game no longer asks), with two Joy-Con 2 as a pair. Not checked: whether swings in play feel right (axes, signs), a single Joy-Con, handheld. The calibration is not kept across starts, as a console keeps it.
- Miis: with [PR #2274](https://github.com/seedds/nxnative/pull/2274) and the player's files ([Miis](#miis)) the console's Miis are listed and drawn. The game's "Add User" opens the system Mii editor (`AppendMii`), which the host still answers as cancelled, so no new Mii can be made in the game. Not checked: the boxing arms an earlier run lacked, with the head models now in place.
- Online play, iOS: not tested.
