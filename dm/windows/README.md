# Building on Windows (experiment)

This folder lets a Windows PC turn a game's NSP into a Mac app with the real NSP script, `scripts/build_ipa_from_nsp.py`. **It is an experiment**: it has been run on four games, and it does not make the iPhone IPA yet. Nothing outside this folder depends on it.

## How it works

The script runs unchanged, through an adapter that Python loads into every process (`winshim/sitecustomize.py`, by `PYTHONPATH`):

- **Mac tools:** `xcrun metal` and `metallib` go to Apple's Metal Developer Tools for Windows; `xcodebuild`, `sysctl` and `sw_vers` get stand-in answers.
- **Windows differences:** binary file descriptors, `os.pread`, `/` in relative paths, a no-op `fcntl`, and `\n` line endings, so that output files have the same bytes as on a Mac.
- **What needs Apple's SDK comes from a kit**, made on a Mac per game: the host, the launcher and the font (`winshim/win_main.py`, `winshim/win_pack.py`). A kit is tied to one commit: the package and the host should come from the same one.

## Use

On the Mac, once per game and commit:

```bash
dm/windows/build_spirv_windows.sh /path/to/work
python3 dm/windows/make_kit.py "/path/to/Game.app" /path/to/kit
python3 dm/windows/make_package.py windows.zip /path/to/kit /path/to/work/spirv-cross.exe
```

A game the PC has to work out for itself (no profile, or a version its profile does not register) has no host until the PC has made its profile, and compiling the host needs no game files:

```bash
python3 dm/windows/new_kit.py /path/to/kit <title ID> "Game Name" /path/to/any/kit
dm/windows/kit_watch.sh <user@pc> "<ssh options>" <short name> <title ID> /path/to/kit
```

Start the PC's build with `--kit C:\dmb\kits\<short name> --work C:\dmb\w-<short name>`. `kit_watch.sh` waits for the shader split, fetches the profile, compiles the host (`kit_host.py`) and sends it back before the build needs it. The PC skips the heap and address region checks, which boot the game on a Mac.

On the PC, with Python 3.14 (Python 3.15 has no Windows `lz4` build yet) and the .NET 10 SDK: unzip to a short folder such as `C:\dmb`, put the NSP in `game` and `prod.keys` in `keys`, and run `RUN_BUILD.bat`. It writes `results.zip` and `app.zip`. Game files and keys never belong in this repository.

Back on the Mac, `compare_app.py` and `compare_build.py` compare a build's file hashes with a Mac build's, and `drive.sh` and `look.sh` drive a run through `DM_CTL`.

`link/` sets up SSH from the Mac to a limited account on the PC, so the Mac can run the build and fetch its results. Run its `SETUP.bat` on the PC as administrator, with the Mac's public key beside it as `mac_key.pub`. `UNDO.bat` removes it.

## Results

Run from commit `ddec9071` on a Windows 11 laptop (i7-1165G7, 15.4 GB of memory):

| Game | Shaders | Build | Files byte-identical to the Mac's app | Libraries with the same shader code |
|---|---|---|---|---|
| Devil May Cry 3 Special Edition | 74 | 2 min | 29 of 34 | 181 of 181 |
| Luigi's Mansion 3 | 5,317 | 36 min | 587 of 595 | 13,296 of 13,312 |
| Donkey Kong Country Returns HD, base version, derived profile | 16,718 | 51 min | Not compared | Not compared |
| Furi Definitive Edition, no profile, guessed on the PC | 5,435 | 25 min | Not compared | Not compared |

The last two had no Mac build to compare with. Donkey Kong was played into level 1-1 and Furi into its first boss fight, with no crash or GPU error, in short scripted runs. Mario Kart 8 Deluxe's base version cannot be built on any machine: it is a 32-bit program, so it needs update 4.0.0 or 4.0.1.

For the first two, the differences are build timestamps, the compiler stamp, the icon's encoding and the signature the app does not have, plus the pipeline archive, left out on purpose. The 16 libraries use Metal 4.0's barrier and atomic forms where the Mac uses 4.1's.

## The queue and the IPA

`queue.py <list.tsv>` runs on the Mac and works through a list of games on the PC, one at a time, or two with `--parallel 2`: each game then gets its own result folder on the PC (`run_all.py --out`) and 8 shader workers (`--jobs`), the steps on the Mac take turns, and a second game starts only while the PC's processes use under 7 GB and it has 60 GB free. Alone, `run_all.py` gives the shaders twice the threads' workers (16 on an i7-1165G7; about 25% faster than 8 on Alex Kidd), since each worker spends much of its time starting `metal.exe` and writing files. Defender exclusions for the build folder and the Metal tools saved about a fifth of the shader time; a single game's shaders stay bound by `dmsh_build.py`'s Python thread. Each row is: short name, title ID, name, source files on the share (several joined with `|`, a `.zip` is unpacked on the PC), and a mode. A blank mode builds the Mac app and the IPA, then plays the app on the Mac for about 4 minutes with screenshots (`drive.sh`). `ipa` builds only the IPA, for a game whose Mac app already exists (`run_all.py --ipa-only`). Results go to `queue/<short>/`, IPAs to `queue/ipas/`, and one JSON line per game to `queue/results.jsonl`; a rerun skips the games already there. `base_kit.py` makes the kit every game shares; the queue copies it per game.

The PC asks the Mac for what needs Apple's tools by leaving a marker in its work folder: `NEED_HOST` (the Mac compiles the host from the profile the PC worked out: `kit_host.py`) or `NEED_IOS`. For the IPA, the PC does everything but the executable: extraction, rewriting, the iOS shaders (`--gpu none`) and the packing (`dm/ios/tools/pack_ipa.py`), then writes Unix modes into the zip's directory, which Windows files lack. `ios_exe.py` runs a copy of `build_ipa.sh` that stops before its packing, on the rewritten modules and the profile only, with stand-ins for the RomFS and the shader library (about 25 s). Games that make code at run time (`nn::jit`) get no IPA: they need captures from a Mac run.

`watchdog.py` starts with the queue: it stops a `metal` or `metallib` on the PC whose CPU total has not changed for 8 minutes (a `metallib` once hung for 12 minutes and the build waited on it; stopped, the build went on with no error logged).

## Open

- **The iPhone IPA.** Devil May Cry 3, Lake, Asterix and Obelix Slap them All, Blasphemous 2 and Mario vs. Donkey Kong have Windows-built IPAs whose structure was checked (the executable's mode, Mach-O, the minimum iOS, the increased-memory entitlement); none has been run on an iPhone yet. They have no pipeline archive, so pipeline games build their bases on the first launch.
- **The pipeline archive.** Apple's Windows tools (32023.850) stop at OS 26: `metal-tt` refuses macOS 27 and iOS 27, and its backend refuses libraries targeting macOS 27.2. So a Windows-built app builds its bases behind "Preparing pipelines" on first launch: about 11 s on an M4 Pro for Luigi's Mansion 3's 3,852, and it should grow with a game's pair count. Play from a cold start then matched a Mac-built app (one run each, each with an empty `DM_PSO_USER` and `DM_PSO_ARCHIVE`): 50 slow frames against 47, 14 over 100 ms in each, worst 884 ms against 880 ms. Runs that share the user list in Library are not a fair comparison.
- **Moving the fixes.** The adapter and `make_package.py`'s two patches are stand-ins for portability fixes in the tools themselves. Changing a tool under `DMSH_CHAIN` changes the chain version, so those need the usual rebuild.
- **Guessed profiles.** One game is not a rate: some will not boot without the checks the PC skips, or without real fixes, and debugging them needs a Mac.
- **This commit.** The results above come from `ddec9071`; the package has not been rebuilt and rerun on later commits. On this branch, `kit_host.py` and `new_kit.py` have been run on Furi's profile, but `kit_watch.sh` has only been syntax-checked: the run itself used an earlier copy of it.
