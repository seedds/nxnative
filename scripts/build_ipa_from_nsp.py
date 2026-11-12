#!/usr/bin/env python3
# Unsigned IPA or a device-ready Xcode project straight from the NSPs: identify the game (title ID → game profile dm/games/<id>/) → extract (nx_extract) → rewrite the code modules (patch_nso.py)
#   → (a game that compiles shaders at run time: capture them, by booting it in the Mac host for a few seconds; an Unreal game: decode its shaders with its own Oodle, the same way)
#   → split out the shaders and check them against the profile's tables (shader_split.py) (a Vulkan game without SPIR-V files, Super Mario Galaxy, boots the same way to capture the SPIR-V shaders it generates;
#   a game with Trinity packs, Pokémon Legends: Z-A, has them decoded for the split by its own Oodle decoder, run in the Mac host's tool mode)
#   → iOS shader libraries (dmsh_build.py) → build_ipa.sh. A Vulkan game (main imports Vulkan, not NVN: Hades) skips the NVN shader steps: vk_shaders.py translates its SPIR-V shaders instead.
#   Which of these steps a game needs follows from its files (dm/games/profile_guess.py shader_sources), not from its profile. A game with no matching profile is surveyed (<NSP folder>/<title ID>_survey/survey.md), then gets a provisional profile guessed from
#   the survey (<build folder>/profile/<title ID>/, the build folder being <NSP folder>/<title ID>_build unless --work-dir names another; outside the repository, found through DM_GAMES_DIR) and a best-effort IPA that may not boot; SURVEY_ONLY / --survey-only stops after the survey.
# PLATFORM = 'mac' / --platform mac is a development option (the Mac is the development host, not a platform for players): the same steps up to the shaders, but only the Mac's
#   (the shader build without IOSOUT, a Vulkan or GL game's libraries without --ios), then the Mac host for the profile and the button-icon font; no iOS shaders, Xcode project or IPA.
#   The extracted files are kept whatever KEEP_EXTRACTED says (the game runs from them), and the last line is the command that runs the game in the Mac host, its save in <id>_build/run/save.
# PLATFORM = 'mac-app' / --platform mac-app: everything 'mac' builds, then a self-contained, double-clickable Mac app of the game (dm/mac/pack_app.py) in the NSP folder, <game name>.app (OUT_IPA / -o
#   names another path): the launcher dm/mac/launcher.c, the Mac host, the RomFS (APFS clones on the build's volume), the Mac shader libraries, the DLC and the rewritten modules inside it; the save
#   in ~/Library/Application Support/nxnative/<title ID>/, the log in ~/Library/Logs/nxnative/<title ID>.log. Signed ad-hoc, or with MAC_SIGN / --mac-sign; no hardened runtime, no notarization.
# SEEN_SHADERS / --seen-shaders, with PLATFORM 'mac' or 'mac-app': only the shaders the profile's shader_seen.tsv lists and the last runs' logs reported missing, in minutes instead of hours; the rest do not draw (build_shaders).
# Two ways to run it: open this file in Spyder and press F5 (it uses the values under "Settings" below; an empty NSP folder or prod.keys is asked for at run time);
#   or in a terminal: python3 scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> [options] (values given on the command line override "Settings", -h lists every option;
#   without keyboard input a missing value is not asked for but reported as an error; exit code 0 on success, 1 on error, 130 when interrupted, 143 when stopped by SIGTERM, 129 when the terminal closed (SIGHUP)). Command-line options entered in Spyder's run configuration work the same way.
# Uses only three things: this repository, a folder holding one game's NSPs (the base game, with or without an update; DLC NSPs, when present, are packed too and unlock the add-on content already in the game), and prod.keys.
#   Instead of NSPs, the folder may hold the game's files as a dump (base/, updates/<version>/, dlc/; scripts/pack_dump.py lists the layout): that needs no prod.keys. A game with no profile is surveyed from those files. A dump has no control data: the name comes from the profile (a provisional one takes the folder's name), and the app has no icon unless --icon / ICON gives it one (--fallback-icon / FALLBACK_ICON: only when the game has none).
#   Or the folder may hold the game as one NUP container (a .nup file: base game, updates and DLC; scripts/pack_nup.py), or the .nup file may be given in its place: it needs no prod.keys, like a dump; a game with no profile is surveyed from the container's files (its newest version's code modules and RomFS file headers),
#   and its RomFS images are written straight from it; the name, display version and icon come from the NACP and icon it carries, else as for a dump (its name record is a packer's label, not the game's), and --icon / ICON gives the app an icon when it carries none.
#   The newest version in the folder is packed. A version the profile registers (with every version in its shader_versions in the folder) is built from the profile as it is; any other
#   version (the base game alone, another update) from a profile derived from it in <build folder>/profile: the same settings, and the version's own shaders (derive_profile).
# Every intermediate file is under the build folder, <id>_build/ in the NSP folder unless --work-dir / WORK_DIR names another: nsp/ (NSZ / XCZ files converted to NSP / XCI with nsz -D, kept for the next build; pip install nsz, or an nsz 5.0 or later executable on PATH), extract/ (the extracted game files, kept for the next build unless KEEP_EXTRACTED / --no-keep-extracted), aoc/ (one RomFS image per DLC index), tools/ (the Mac host for a run-time shader capture), patched_*/, shader/ (shader package and caches, kept so the next build is fast), logs/ (each step's full output), tmp/, ipa/. The finished IPA also goes beside the build folder by default (the NSP folder, without --work-dir).
# The DLC, the rewrite, the shader split and the iOS (or Vulkan) shaders are reused when all their inputs are as at their last successful run (the game files, the tools, the profile, Xcode and the in-repo spirv-cross: fingerprint() keys beside their output);
#   the iOS shaders' inputs are the shader package's content and the chain's own tools, so a split that runs again and writes the same package leaves them reused.
#   The chain version header dm/gfx/nvn/dmsh_chain.h (one version for every game) is written again when a chain change was committed without it (chain_header: it follows from the source alone),
#   so a run may leave it changed. The c1 / uniform extent tables (c1_full.tsv, cb_extent.tsv) are not in the repository: the iOS shader step's dmsh_build.py makes them in the profile folder from
#   this build's shaders, where git ignores them, and a reused step makes them again when they do not name its libraries' stamp (chain_tables).
# The shader translator is the in-repo dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate and the extractor the in-repo dm/tools/nx_extract_bin/nx_extract: native universal executables (dm/tools/native_tools.sh), so no .NET is needed and an Intel Mac runs them too (the translator's fingerprint must match dmsh_chain.lock: a rebuild has another one and dmsh_build refuses it). SPIRV-Cross is the in-repo dm/gfx/tools/spirv_cross_bin and must match the lock too.
# The shader libraries are built through dm/gfx/tools/own_compilers.py. The Metal compiler must match the lock (Xcode 27.2): another one stops an NVN game's build before its shader steps,
# unless ALLOW_OTHER_METAL / --allow-other-metal. Issue #242 reported corruption with another compiler; the cause is still unconfirmed.
# Python 3.12+ is required for bounded archive mappings inside images. The Python running this file needs lz4, capstone 5.x, fonttools and zbic (and backports.zstd before Python 3.14); also needed: Xcode, xcodegen, zstd. SPIRV-Cross is the in-repo build. A first run takes 2 (Metroid Dread) to 10 minutes (Breath of the Wild: 5 of them the shader split), longer when shader libraries must be compiled; a repeat run with unchanged inputs takes about 20 s (Metroid Dread), mostly packing the IPA. Key values are never printed.
# The console shows one line per stage ([k/n] stage, with a bar, done/total, percent, elapsed time and ETA where the work can be counted); the tools' own output goes only to logs/ (PROGRESS / --progress). --progress json writes newline-delimited JSON events to stdout.
import ast, collections, contextlib, ctypes, errno, getpass, hashlib, importlib.util, json, os, re, shlex, shutil, signal, struct, subprocess, sys, tempfile, threading, time, types
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dm/tools"))
import romfs as rfs
import proc_group   # a tool's run in a process group of its own, stopped whole
import run_saves   # cloud saves off for the build's host runs (tool_env, the prebuild)
import nso   # the code module loader: nso.Error is a damaged module (its file cut short or not what its header says)
import tree_tar   # one file in place of a build's folders of many small files
import shader_lib   # the shader libraries as one file, read in place
import shader_cache   # the compile cache as one file, read in place
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pack_dump   # a folder of a game's files instead of NSPs
import pack_nup   # a game in a NUP container instead of NSPs

# ---- Settings ----
NSP_DIR = ''                            # folder holding the NSPs, or the game's .nup file (empty = ask at run time)
PROD_KEYS = ''                          # path of prod.keys (empty = ask at run time)
PACKAGE = ''                            # version packed into the IPA (empty = the newest in the folder; Mario 'v327680' i.e. 1.2.1, or 'v0')
TITLE_ID = ''                           # the game to pack when the folder holds several, e.g. a cartridge dump with two games: '0100FD8022DAA000' (empty = the folder's one game)
WORK_DIR = ''                           # the game's build folder (empty = <title ID>_build in the NSP folder); the extracted files, shader cache and temporary files live there, and the IPA (or app) goes beside it unless OUT_IPA names it
OUT_IPA = ''                            # output IPA (empty = <the profile's ipa> in the NSP folder); with PLATFORM 'mac-app' the output app (empty = <game name>.app in the NSP folder)
APP_NAME = ''                           # app name on the home screen (empty = the game name in the base NSP)
BUNDLE_ID = ''                          # the app's bundle ID (empty = saved per-game Xcode ID, else org.nxnative.<base game title ID in lowercase>)
ICON = ''                               # the app icon, a JPEG or PNG of any size (empty = the game's icon from its control data; a dump has none, a NUP may have none)
FALLBACK_ICON = ''                      # the app icon only when ICON is empty and the game has none of its own (a dump, a NUP without one); empty = such an app's icon is blank
ROMFS = 'bundle'                        # 'bundle' = the game files go into the IPA (or the Xcode app), as always; 'separate' = they stay out (DM_COPY_ROMFS=NO): the IPA is small, and its rebuilds do not pack or send the RomFS again.
                                        #   Copy it to the phone once per game version with the scripts/push_romfs.sh command printed at the end (the app checks that the image is the one it was built for)
SHADER_FILES = False                    # True = an NVN game's shader library goes into the IPA as its separate files, for install tools that cannot copy a large file (AltServer: 2 GiB; the game files
                                        #   then need ROMFS 'separate' too). False = the library is one file, which installs faster and other install tools take whatever its size
PREPARE_XCODE = False                   # prepare a per-game Xcode project and add it to the workspace instead of packing an IPA; requires KEEP_EXTRACTED
PLATFORM = 'ios'                        # 'ios' = the IPA (or the Xcode project); 'mac-app' = a double-clickable Mac app of the game; 'mac' = development: build only what running the game on this Mac needs (Mac shaders, the Mac host), no iOS shaders,
                                        #   Xcode project or IPA; keeps the extracted files and prints the command that runs the game (needs Apple silicon)
MAC_SIGN = ''                            # PLATFORM 'mac-app': the codesign identity for the app, e.g. 'Developer ID Application: <name> (<team>)' (empty = ad-hoc); never with the hardened runtime
TEAM_ID = ''                            # optional Apple team ID; empty = DM_TEAM_ID environment, saved Xcode selection, or select in Xcode
XCODE_WORKSPACE = ''                    # shared .xcworkspace (empty = NxNative.xcworkspace beside the game's NSP folder)
LANGUAGE = ''                           # game language, a code or a name: 'ja', 'fr', 'de', 'es', 'zh-Hans', 'zh-Hant', 'ko', 'Japanese', ... (empty = English); it must be one the game supports (the console line lists them);
                                        #   DM_LANG=<code> in the app's Documents/env.txt changes it without a rebuild
KEEP_EXTRACTED = True                   # keep the extracted game files after the IPA is built (saves extraction next time; as big as the game files); False deletes them
DM_ENV_FILE = ''                        # optional: environment switches for a diagnostic build (one KEY=VALUE per line)
SEEN_SHADERS = False                    # PLATFORM 'mac' / 'mac-app': build only the shaders the profile's playtests used (shader_seen.tsv) and those the last runs' logs reported missing, not the whole map; areas the list does not cover draw without the rest until this runs again
PSO_GPUS = ''                           # NVN games: GPU architectures the app gets the shader pairs' pipeline bases for, made on this Mac (metal-tt, comma-separated: 'g18p' = A19 / A19 Pro of the iPhone 17 family;
                                        #   another device's name is in its log when the app has none for it), so the device does not build them on its first start; each adds about 650 MB to the app (Breath of the Wild) and minutes to a first build; 'none' = no archive;
                                        #   empty = by the game's profile: its pso_gpu key (whitespace-separated architectures, joined with commas) when it sets pso_mtl4 on (the Metal 4 path, the only one that reads the archive on iOS, is on for it),
                                        #   else 'g18p' for a game without the key, and 'none' without pso_mtl4 on
                                        #   PLATFORM 'mac-app': the Mac app's archive (Resources/pso), empty = this Mac's GPU (e.g. 'g14s' for an M2 Max), 'none' = the app builds its bases on its first start
SHOW_FPS = True                         # True shows the game's frame rate on the iPhone (frames on screen per second, top left); DM_SHOW_FPS=0 / 1 in the app's Documents/env.txt changes it without a rebuild
MOTION = True                           # True passes a gyro controller's motion (on iOS also the phone's own) to games that read it; False builds with DM_MOTION=0, for when motion misbehaves (DM_MOTION=1 in the app's Documents/env.txt turns it back on without a rebuild)
SURVEY_ONLY = False                     # a game with no profile in dm/games: only write the survey, no provisional profile and no IPA
PROFILE_OUT = ''                        # where a provisional profile goes (empty = <build folder>/profile, i.e. <NSP folder>/<title ID>_build/profile unless WORK_DIR is set; the profile is <this>/<title ID>/)
PROGRAM_INDEX = 0                       # a multi-program application (Super Mario 3D All-Stars: a launcher and one program per game): which program to pack, 0 = the application's own; its title ID is the application's + the index.
                                        #   A game of several programs (its profile's key programs) is packed as one from program 0: this setting packs one of them alone, for bring-up
ALLOW_DAMAGED = False                   # a damaged NSP (files failing the NCA's hash check: an incomplete copy or a broken download), or files whose bytes do not start as their names promise (a .zs that is no zstd stream: a NUP or a dump has no hashes), stop the build; True writes those files as read and goes on
ALLOW_SHADER_REGRESSIONS = False        # shaders that translated in this game's previous build and fail with the current translation chain stop the build (logs/shader_regressions.log lists them);
                                        #   True packs the app without them
ALLOW_OTHER_METAL = False               # an NVN game's shaders are built only with the Metal compiler they were tested with (dm/gfx/tools/dmsh_chain.lock: Xcode 27.2); another one stops the build,
                                        #   as its shaders can render wrong pictures (GitHub issue #242). True builds them with this Mac's Metal compiler anyway
PROVISIONAL = False                     # build from a provisional profile even when dm/games has one for the game (to test the guesses; the profile goes to PROFILE_OUT / <build folder>/profile)
HEAP_CHECK = True                       # a provisional profile whose game has virtual address memory and sets a heap: boot it on the Mac until its heap request (a few seconds) to size the heap region; with virtual address memory, the address region check too (also for a derived profile)
HEAP_CHECK_SECS = 30                    # the longest heap check (it stops at the first heap request, or HEAP_CHECK_NNMAIN_SECS into nnMain without one)
HEAP_CHECK_NNMAIN_SECS = 5
HEAP_CHECK_ROUNDS = 3                   # boots of the heap check: each fix of the heap region lets the next boot reach the checks behind it
REGION_CHECK_NNMAIN_SECS = 15           # the address region check: the regions a game takes in nninitStartup and its first seconds in nnMain
SWITCH_ALIAS = 0x1000000000             # the Switch's alias region (64 GB), which holds what a game reserves unchanged

PROGRESS = 'auto'                       # stage lines on the console: 'bar' (one line per stage, redrawn in place), 'lines' (a plain line every BEAT seconds), 'off' (only each stage's start and end); 'auto' = bar in a terminal or Spyder, lines when the output goes to a file; 'json' = one JSON event per line, for a program that shows the build
BEAT = 30                               # seconds between progress lines in 'lines' mode
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin']   # searched after the caller's PATH (tool_env): GUI apps such as Spyder do not get the shell's PATH


class BuildError(Exception):
    pass


def clock():
    return time.strftime('%H:%M:%S')


def say(msg):
    PROG.say(f'[{clock()}] {msg}')


def hms(s):   # seconds → m:ss, or h:mm:ss from an hour on
    s = int(s); h, m = divmod(s // 60, 60)
    return f'{h}:{m:02d}:{s % 60:02d}' if h else f'{m}:{s % 60:02d}'


def size_unit(v):   # (unit, divisor) for a byte count
    return next(((u, d) for u, d in (('GB', 2**30), ('MB', 2**20)) if v >= d), ('KB', 2**10))


def amount(v, unit=''):   # a count, or a byte size when unit is 'B'
    if unit == 'B':
        u, d = size_unit(v); return f'{v / d:.1f} {u}'
    return f'{v:,}' + (f' {unit}' if unit else '')


# ---- Progress on the console ----
class Progress:
    """The console view of the build: one line per stage, "[k/n] stage · step", with a bar, done/total, percent, elapsed time and ETA when the work can be counted, else a spinner and the elapsed time.
    Modes: bar = the line is redrawn in place (a terminal, Spyder's console) and replaced by a ✓ line when the stage ends; lines = a plain line when a stage or step starts, every BEAT seconds and at the end
    (for output piped to a file); off = only each stage's start and end lines. Counters come from set(), or from a poll function that runs in a thread of its own (it may walk a big folder) and returns set()'s arguments.
    Messages go through say(), which prints them above the live line. json (for a program that shows the build) = one JSON object per stdout line instead: the events stage, step, progress, stage_end, say, shaders_stale and game;
    a count change goes out at most every JSON_INTERVAL seconds (the ticker sends one held back), any other change at once"""
    SPIN, BLOCKS, POLL, JSON_INTERVAL = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏', ' ▏▎▍▌▋▊▉', 1.0, 0.5

    def __init__(self, mode='off'):
        self.mode, self.lock, self.k, self.n, self.cur, self.shown, self.ticker, self.tstop = mode, threading.RLock(), 0, 0, None, 0, None, None

    def plan(self, n):   # the next n stages are numbered 1..n
        with self.lock: self.k, self.n = 0, n

    def add(self, n):   # n more stages than planned (one that turns out to be needed: a provisional profile's heap check)
        with self.lock: self.n += n

    def stage(self, name, poll=None, step='', **kw):
        """Start the next stage, ending the current one. kw: set()'s arguments"""
        with self.lock:
            self.end()
            self.k += 1; now = time.time()
            self.cur = types.SimpleNamespace(name=name, step=step, unit='', total=None, done=None, est=False, note='', t0=now, hist=collections.deque(), beat=now, pstop=None, jlast=None, jstate=None)
            if self.mode == 'json':
                self._json({'ev': 'stage', 'k': self.k, 'n': self.n, 'name': name, 'step': step, 't': round(now, 3)})
            elif self.mode != 'bar': self._out(f'[{clock()}] [{self.k}/{self.n}] {name}' + (f' · {step}' if step else '') + ' …')
            self.set(**kw)
            if self.mode != 'off' and not self.ticker:
                self.tstop = stop = threading.Event()
                self.ticker = threading.Thread(target=self._tick, args=(stop,), daemon=True); self.ticker.start()
        if poll: self.watch(poll)

    def watch(self, poll):
        """Call poll() over and over until the stage ends or watch() is called again (None: stop); poll returns set()'s arguments, or None to leave the line as it is"""
        with self.lock:
            st = self.cur
            if not st: return
            if st.pstop: st.pstop.set()
            st.pstop = stop = threading.Event()
        if poll: threading.Thread(target=self._poll, args=(st, poll, stop), daemon=True).start()

    def set(self, done=None, total=None, step=None, unit=None, est=None, note=None):
        """done / total: the count of the current step (unit 'B' = bytes); est: total is an estimate (shown with ~); note: shown after the count;
        step: the step within the stage (a new step starts with no count and restarts the ETA)"""
        with self.lock:
            st = self.cur
            if not st: return
            if step is not None and step != st.step:
                st.step, st.done, st.total, st.unit, st.est, st.note, st.beat = step, None, None, '', False, '', time.time(); st.hist.clear()
                if self.mode == 'lines': self._out(f'[{clock()}] [{self.k}/{self.n}] {st.name} · {step} …')
                if self.mode == 'json': self._json({'ev': 'step', 'k': self.k, 'name': st.name, 'step': step})
            if unit is not None: st.unit = unit
            if total is not None: st.total = total
            if est is not None: st.est = est
            if note is not None: st.note = note
            if done is not None:
                st.done = done; now = time.time(); st.hist.append((now, done))
                while len(st.hist) > 2 and st.hist[1][0] < now - 60: st.hist.popleft()   # the ETA follows the rate of the last minute
            if self.mode == 'json': self._json_progress(st)

    def _json_progress(self, st, force=False):   # a progress event when anything changed; a count change alone waits JSON_INTERVAL unless forced (the caller holds the lock)
        state = (st.step, st.total, st.unit, st.est, st.note, st.done)
        if state == st.jstate: return
        now = time.monotonic()
        if not force and st.jstate is not None and state[:-1] == st.jstate[:-1] and now - st.jlast < self.JSON_INTERVAL: return
        st.jlast, st.jstate = now, state
        self._json({'ev': 'progress', 'k': self.k, 'name': st.name, 'step': st.step, 'done': st.done, 'total': st.total,
                    'unit': st.unit, 'est': st.est, 'note': st.note,
                    'frac': min(st.done / st.total, 0.99) if st.total and st.done is not None else None,
                    'eta': self._eta(st), 'elapsed': round(time.time() - st.t0, 3)})

    def _json(self, obj):   # one JSON object per stdout line (a message's newlines are escaped)
        obj.setdefault('t', round(time.time(), 3))
        print(json.dumps(obj, ensure_ascii=False, allow_nan=False), flush=True)

    def end(self, summary='', ok=True, warn=False):
        """End the current stage with a ✓ (or ✗) line: its elapsed time and a summary. warn: it finished without what it makes (a ⚠ line; the JSON event has ok true and warn true)"""
        with self.lock:
            st, self.cur = self.cur, None
            if not st: return
            if st.pstop: st.pstop.set()
            if self.mode == 'json':
                self._json_progress(st, force=True)   # the last count, if it was held back
                self._json({'ev': 'stage_end', 'k': self.k, 'name': st.name, 'ok': ok, 'summary': summary, 'elapsed': round(time.time() - st.t0, 3), **({'warn': True} if warn else {})})
                return
            self._out(f'[{clock()}] {"⚠" if warn else "✓" if ok else "✗"} [{self.k}/{self.n}] {st.name} ({hms(time.time() - st.t0)})' + (f': {summary}' if summary else ''))

    def event(self, ev, **fields):   # a build event beside the stage lines (json mode only: the human modes get the message that goes with it from say())
        with self.lock:
            if self.mode == 'json': self._json({'ev': ev, **fields})

    def say(self, msg):
        with self.lock:
            if self.mode == 'json': self._json({'ev': 'say', 'msg': msg})
            else: self._out(msg)

    def close(self, ok=True):   # end the current stage and stop the ticker (the mode stays: a message after it is still JSON in json mode)
        with self.lock:
            self.end(ok=ok)
            if self.tstop: self.tstop.set()
            self.ticker = self.tstop = None

    def _count(self, st):   # "1.9/~5.0 GB", "1,203/19,592 shaders", "2,301 files" (no total); '' without a count
        if st.done is None: return ''
        if not st.total: return amount(st.done, st.unit)
        t = '~' if st.est else ''
        if st.unit == 'B':
            u, d = size_unit(st.total); return f'{st.done / d:.1f}/{t}{st.total / d:.1f} {u}'
        return f'{st.done:,}/{t}{st.total:,}' + (f' {st.unit}' if st.unit else '')

    def _eta(self, st):   # seconds left at the rate of the last minute; None while unknown
        if not st.total or len(st.hist) < 2: return None
        (t1, d1), (t2, d2) = st.hist[0], st.hist[-1]
        if t2 - t1 < 3 or d2 <= d1: return None
        return max(st.total - st.done, 0) * (t2 - t1) / (d2 - d1)

    def _fields(self, st):   # (head, fraction done or None, count, note, times)
        head = f'[{self.k}/{self.n}] {st.name}' + (f' · {st.step}' if st.step else '')
        frac = min(st.done / st.total, 0.99) if st.total and st.done is not None else None   # 100 % only when the stage ends
        eta = self._eta(st)
        return head, frac, self._count(st), st.note, hms(time.time() - st.t0) + (f' · ETA {hms(eta)}' if eta is not None else '')

    def _line(self, st, width):
        head, frac, count, note, times = self._fields(st)
        head = f'{self.SPIN[int(time.time() * 8) % len(self.SPIN)]} {head}'
        pct = f'{frac * 100:.0f}%' if frac is not None else ''
        for note in (note, ''):   # a narrow console loses the note first, then the bar
            rest = '  '.join(x for x in (' · '.join(x for x in (count, note) if x), times) if x)
            barw = min(20, width - len(f'{head}  {pct:>4}  {rest}') - 3)   # the bar takes what the console's width leaves, up to 20 cells; none below 8
            if frac is None or barw >= 8: break
        if frac is not None and barw >= 8:
            cells = frac * barw; full = int(cells); part = int((cells - full) * 8)
            pct = ('█' * full + (self.BLOCKS[part] if part else '')).ljust(barw, '░') + f' {pct:>4}'
        return '  '.join(x for x in (head, pct, rest) if x)[:width - 1]

    def _out(self, text):   # a whole line above the live one (the caller holds the lock)
        if self.shown: sys.stdout.write('\r' + ' ' * self.shown + '\r'); self.shown = 0
        print(text, flush=True)
        if self.mode == 'bar' and self.cur: self._draw()

    def _draw(self):
        line = self._line(self.cur, shutil.get_terminal_size((80, 24)).columns)   # Spyder has no size to ask, and a line that wraps there cannot be redrawn: 80 columns
        sys.stdout.write('\r' + line + ' ' * max(self.shown - len(line), 0)); sys.stdout.flush()
        self.shown = len(line)

    def _tick(self, stop):
        while not stop.wait(0.25 if self.mode == 'bar' and not in_ipython() else 1):   # Spyder's console gets fewer redraws
            with self.lock:
                if stop.is_set(): return   # close() stopped this ticker while it waited for the lock
                st = self.cur
                if not st: continue
                try:
                    if self.mode == 'bar': self._draw()
                    elif self.mode == 'json': self._json_progress(st)   # a count that set() held back
                    elif self.mode == 'lines' and time.time() - st.beat >= BEAT:
                        st.beat = time.time(); head, frac, count, note, times = self._fields(st)
                        self._out(f'[{clock()}] {head}: ' + ', '.join(x for x in (f'{frac * 100:.0f}%' if frac is not None else '', count, note, times) if x))
                except Exception: pass   # progress never stops the build, and one bad line does not stop the next

    def _poll(self, st, poll, stop):
        while not stop.is_set():
            t, step = time.time(), st.step
            try: r = poll()
            except Exception: r = None   # progress never stops the build
            with self.lock:   # a count read for a step that ended meanwhile is dropped
                if r and self.cur is st and not stop.is_set() and ('step' in r or st.step == step): self.set(**r)
            stop.wait(max(self.POLL, (time.time() - t) * 5))   # a slow poll (a walk over tens of thousands of files) runs less often


PROG = Progress()


def progress_mode(want):   # auto: redraw in a terminal or in Spyder (IPython handles \r), plain lines when the output goes to a file or a pipe
    if want in ('bar', 'lines', 'off', 'json'): return want
    if want != 'auto': raise BuildError(f'PROGRESS / --progress must be auto, bar, lines, json or off: {want}')
    return 'bar' if in_ipython() or (sys.stdout and sys.stdout.isatty()) else 'lines'


def tree_size(d):   # (files, bytes) under d; (0, 0) when it does not exist yet
    n = b = 0; todo = [str(d)]
    while todo:
        try: it = rfs.os.scandir(todo.pop())
        except OSError: continue
        with it:
            for e in it:
                try:
                    if e.is_dir(follow_symlinks=False): todo.append(e.path)
                    else: n += 1; b += e.stat(follow_symlinks=False).st_size
                except OSError: pass
    return n, b


def in_ipython():   # Spyder (and Jupyter) run this file inside IPython: input works there, and sys.exit must not be called (it piles an error onto the console)
    import builtins
    return hasattr(builtins, '__IPYTHON__')


LANG_NEAR = {'en-US': 'en-GB', 'en-GB': 'en-US', 'fr': 'fr-CA', 'fr-CA': 'fr', 'es': 'es-419', 'es-419': 'es', 'pt': 'pt-BR', 'pt-BR': 'pt'}   # the same language for another region


def language_code(s):
    """A game language as typed (a code such as ja / zh-Hans, any case, _ for -, or its name such as Japanese) → its language code (nacp.LANGUAGES); None when it is none of them"""
    k = s.strip().lower().replace('_', '-')
    for c, n in nacp.LANGUAGES:
        if k in (c.lower(), n.lower()): return c
    return {'en': 'en-US', 'english': 'en-US', 'zh-cn': 'zh-Hans', 'zh-tw': 'zh-Hant'}.get(k)   # the settings' older codes for Chinese


def game_language(want, asked, have):
    """The language code to pack: want when the game supports it, else the same language for another region (en-GB for en-US, es for es-419, ...); have: the game's languages (empty: unknown, want as is).
    A language the game lacks stops the build when it was asked for; English, the default, then falls back to the game's first language"""
    if not have or want in have: return want
    if LANG_NEAR.get(want) in have: return LANG_NEAR[want]
    if asked: raise BuildError(f'the game has no {want} ({dict(nacp.LANGUAGES)[want]}); its languages: {language_list(have)} (--language / LANGUAGE)')
    return have[0]


def languages_control(*dirs):
    """The control.nacp that lists the game's languages: the first of the folders' (the packed version's, then the base game's) that lists any, else the first one's path. An update, a NUP's
    especially, may carry no control data: the language check and the launcher settings (of the Mac app and the IPA) then read the base game's, the same file"""
    paths = [d / 'control.nacp' for d in dirs]
    return next((p for p in paths if nacp.languages(p)), paths[0])


def language_list(codes):
    names = dict(nacp.LANGUAGES)
    return ', '.join(f'{c} ({names[c]})' for c in codes)


def ask(prompt, what):   # what: what is missing, for the error message
    if PROG.mode == 'json': raise BuildError(f'{what} not given: JSON mode does not prompt; give it on the command line (-h for usage)')
    if not in_ipython() and not (sys.stdin and sys.stdin.isatty()):
        raise BuildError(f'{what} not given and there is no keyboard input to ask for it: give it on the command line (-h for usage)')
    s = input(prompt).strip().strip('"').strip("'").replace('\\ ', ' ')   # dragged-in paths come quoted or with escaped spaces
    return os.path.expanduser(s)


def run(cmd, log, env=None, cwd=None, on_line=None, ok=(), fatal=True):
    """Run a subprocess with all its output written to log (the console shows the stage line instead: the shader build alone emits tens of thousands of compiler warnings);
    stop on a non-zero exit not in ok, printing the log's last lines. on_line: called with every output line (build_ipa.sh's own step lines move its stages on).
    fatal False: such an exit only prints those lines and returns its code (the caller ends the stage and goes on without what the command makes).
    The command and everything it starts run in a process group of their own (proc_group): an interrupt, a SIGTERM (cli) or an error here (a log that cannot be written)
    stops the whole group, not only the command; a process of the group still running proc_group.DRAIN s after the command's own exit is stopped, and the step fails
    as for an exit code (what it writes may be incomplete; the code is 1 when the command's own was 0)"""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    tail = collections.deque(maxlen=40)
    with open(log, 'w') as f, proc_group.Group(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as g:
        for line in g.lines():
            f.write(line); tail.append(line)
            if on_line:
                try: on_line(line.rstrip('\n'))
                except Exception: pass   # progress never stops the build (an interrupt raised in it does)
        rc = g.finish()
    bad = rc and rc not in ok
    if bad or g.stragglers:
        tool = Path(cmd[1] if cmd[0] in (sys.executable, 'bash') else cmd[0]).name
        why = f'{tool} exit code {rc}' if bad else f'{tool} exited, but {proc_group.describe(g.stragglers)} still ran {g.drain:g} s later and was stopped: what it writes may be incomplete'
        if fatal: PROG.end(ok=False)
        PROG.say(f'---- last lines of {log} ----\n' + ''.join(tail).rstrip('\n'))
        if not fatal:
            if not bad: PROG.say(f'[RED] {why}')
            return rc if bad else 1
        raise BuildError(f'{why} (full output: {log})')
    return 0


def build_id(nso):
    with open(nso, 'rb') as f:
        f.seek(0x40); return f.read(16).hex()


def _gone(func, path, exc):   # shutil.rmtree's onexc hook (Python 3.12+, the script's floor)
    if not (isinstance(exc, OSError) and exc.errno == errno.ENOENT): raise exc   # anything but "already gone" is a real failure


def rmtree(p):
    """Make p gone. An entry that vanishes between the listing and its unlink is the goal reached, not an error: on an exFAT / FAT drive macOS keeps each AppleDouble `._x` beside `x`
    and drops it when `x` is deleted, so the walk's later unlink of `._x` raised FileNotFoundError and left the folder half deleted"""
    if p.is_symlink() or p.is_file(): p.unlink()
    elif p.exists(): shutil.rmtree(p, onexc=_gone)   # Python 3.13 and later pass these by themselves


NX_EXTRACT = 'dm/tools/nx_extract_bin/nx_extract'   # the committed native executable (dm/tools/native_tools.sh), built from dm/tools/nx_extract


def nx_extract_source_sha(repo):   # the same convention as nx_extract --source-sha: the sha256 of Program.cs and nx_extract.csproj, in that order, first 16 digits
    return hashlib.sha256(b''.join((repo / 'dm/tools/nx_extract' / f).read_bytes() for f in ('Program.cs', 'nx_extract.csproj'))).hexdigest()[:16]


def find_nx_extract(repo, env):
    """The committed nx_extract, a native executable (no .NET needed, GitHub issue #1853), once it has shown that it runs and that it was built from the in-repo source: after an edit
    to Program.cs the old executable would extract with the old code"""
    nx = repo / NX_EXTRACT
    try: r = subprocess.run([str(nx), '--source-sha'], capture_output=True, text=True, env=env, timeout=60, errors='replace')
    except (OSError, subprocess.SubprocessError) as e: raise BuildError(f'nx_extract does not start ({e}): {nx}') from e
    got, want = r.stdout.strip(), nx_extract_source_sha(repo)
    if r.returncode or not got: raise BuildError(f'nx_extract does not run ({(r.stderr.strip().splitlines() or [f"exit code {r.returncode}"])[0]}): {nx}')
    if got != want: raise BuildError(f'nx_extract was not built from the in-repo source (embedded {got} / in repo {want}): rebuild it with dm/tools/native_tools.sh nx_extract')
    return nx


def swap_in(tmp, dst):
    """Publish the finished output tmp (a sibling of dst, so the same volume) as dst. dst is always the whole old output, the whole new one, or absent; never half deleted, which a
    reuse check could take for a finished output: the old one is renamed aside first (<dst>.old.partial: as every unfinished folder, a name the build ignores), the new one renamed in,
    and only then the old one deleted. Recoverable, not atomic: two renames. What stops it before the new one is in (a failure, an interrupt or SIGTERM, also between the two
    renames) puts the old one back, judged by what is on the disk rather than by which step ran; if that rename fails too, BuildError says where the old output is kept. An aside
    folder an earlier run left is put back when nothing is in place (a hard kill between the renames: the old output, whole) and deleted only when the new output went in (the old
    one's remains, cut short while being deleted)"""
    old = dst.with_name(dst.name + '.old.partial')
    here, aside = (lambda: dst.is_symlink() or dst.exists()), (lambda: old.is_symlink() or old.exists())
    if aside():
        if here(): rmtree(old)
        else: old.rename(dst)
    if not here(): tmp.rename(dst); return
    try:
        dst.rename(old)
        tmp.rename(dst)
    except BaseException as e:
        if aside() and not here():
            try: old.rename(dst)
            except OSError as back: raise BuildError(f'publishing {dst} failed ({e!r}), and the old output could not be put back ({back}): it is kept in {old}') from e
        raise
    rmtree(old)


def free_gb(p):
    while not p.exists(): p = p.parent
    return shutil.disk_usage(p).free / 2**30


def tool_env(environ=os.environ):
    """The environment the build's tools run in: the caller's PATH first, so the tools it selects (a venv's python3, mise's zstd) are the ones that run,
    then TOOL_PATHS for the tools it lacks (a GUI app such as Spyder gets only the system PATH, or none). Cloud saves are off (run_saves.cloud_off): the build's host runs
    (captures, checks, the prebuild) play on scratch saves, and one inherited from the shell would pull the player's cloud save into them and send theirs over it."""
    return run_saves.cloud_off(dict(environ, PATH=os.pathsep.join([environ.get('PATH') or '/usr/bin:/bin:/usr/sbin:/sbin'] + TOOL_PATHS)))


# ---- NSP identification ----
def is_package(path):   # what nx_extract's OpenNsp opens: an XCI / XCZ by the card header's magic at 0x100, else a PFS0 (NSP, NSZ)
    with open(path, 'rb') as f: h = f.read(0x104)
    return h[:4] == b'PFS0' or h[0x100:] == b'HEAD'


KEYS_RED = r'^\[RED\] (.*prod\.keys has no .*|.*no title key for rights ID .*)$'   # nx_extract's line for keys that cannot open a package (exit code 6)


@contextlib.contextmanager
def package_reader(repo, keys, env):
    """Yield a reader of the packages under the keys, with a temporary folder for its logs (the work folder is named after the title ID, which is what is being looked for):
    reader.contents(paths) and reader.check_keys(fam)"""
    with tempfile.TemporaryDirectory(prefix='title_probe_') as t:
        work, nx = Path(t), []
        class Reader:
            @staticmethod
            def contents(paths):
                """Every package's content metas in one run (`nx_extract <keys> --contents <package>...`): {path: [(type, title ID, version)]}, or {path: why} for a package that cannot
                be read or holds none. A package's title ID and version are what its content metas say, never its file name. Keys that cannot open the meta NCAs stop here, with
                nx_extract's line; any other failure of the run stops with its output"""
                paths = list(paths)
                if not paths: return {}
                if not nx: nx.append(find_nx_extract(repo, env))
                log = work / 'logs/contents.log'
                try: run([nx[0], keys, '--contents', *paths], log, env=env, ok=(5,))
                except BuildError as e:
                    text = log.read_text(errors='replace') if log.exists() else ''
                    m = re.search(KEYS_RED, text, re.M)
                    if m: raise BuildError(m.group(1)) from e
                    tail = '\n'.join(text.splitlines()[-20:])
                    raise BuildError(f'the packages\' content metas could not be read ({e}):\n{tail}') from e
                out, cur = {}, None
                for line in log.read_text(errors='replace').splitlines():
                    if m := re.match(r'package (\d+) ', line): cur = paths[int(m[1])]; out[cur] = []
                    elif cur is not None and (m := re.match(r'content (\w+) ([0-9a-f]{16}) (\d+)$', line)): out[cur].append((m[1], int(m[2], 16), int(m[3])))
                    elif cur is not None and line.startswith('[RED] '): out[cur] = line.split(': ', 1)[-1]
                if len(out) != len(paths): raise BuildError(f'nx_extract --contents listed {len(out)} of {len(paths)} packages')
                return out

            @staticmethod
            def check_keys(fam):
                """Stop when the keys cannot open the game's packages (`nx_extract <keys> --keys <package>...`: every NCA's header, an NSZ's too), before nsz -D, the survey and the
                extraction, which need the same keys and would stop minutes later. Keys older than the game lack its master key: the message names that key and the firmware it came with.
                Any other failure of the check is left to those steps, which report it themselves"""
                if not nx: nx.append(find_nx_extract(repo, env))
                log = work / 'logs/keys.log'
                paths = list(dict.fromkeys([fam['base'], *(p for _, p in fam['updates']), *(p for _, _, p in fam['dlc'])]))   # a merged package is the base game, the update and DLC: checked once
                try: run([nx[0], keys, '--keys', *paths], log, env=env, ok=(6,))
                except BuildError: return
                m = re.search(KEYS_RED, log.read_text(errors='replace'), re.M)
                if m: raise BuildError(m.group(1))
        yield Reader


def name_version(path):   # the [v<N>] tag of a file name: only checked against the version the package's content meta gives (scan_nsps)
    m = re.search(r'\[v(\d+)\]', path.name)
    return int(m.group(1)) if m else None


COMPRESSED = {'.nsz': '.nsp', '.xcz': '.xci'}   # nsz's compressed NSP / XCI and what nsz -D makes of them
PACKAGE_SUFFIXES = ('.nsp', '.xci', '.nsz', '.xcz')


def files_with_suffix(folder, *suffixes):
    """The files of folder whose extension is one of suffixes in any letter case (`Game [id].NSP` from a download or a card dump): Path.glob('*.nsp') compares case-sensitively on every
    POSIX system, a case-insensitive Mac volume included. An AppleDouble file does not count: on an exFAT / FAT drive macOS keeps a file's extended attributes (a download's quarantine
    flag) in `._<name>`, one beside each game file"""
    return sorted(p for p in folder.iterdir() if p.suffix.lower() in suffixes and not p.name.startswith('._') and p.is_file())


def extract_flags(app):   # nx_extract's options shared by its extraction calls: the application to read (--app), the program of a multi-program application and a damaged NSP.
    # With PROGRAM_INDEX, app is that program's title ID (set in main): its application is app - PROGRAM_INDEX, whose one Application content meta lists every program's NCAs
    return (['--app', f'{app - PROGRAM_INDEX:016x}'] + ([f'--program={app:016x}'] if PROGRAM_INDEX else [])
            + (['--allow-damaged'] if ALLOW_DAMAGED else []))


def scan_nsps(nsp_dir, contents_of, want=None):
    """Group by game: {application title ID: {'base': path, 'base_version': N, 'updates': [(version, path)], 'dlc': [(title ID, version, path)]}}. Every package's title IDs and
    versions are those of its content metas, contents_of(paths) -> {path: [(type, title ID, version)] or why it has none} (package_reader), never its file name: a [v<N>] tag that says otherwise is
    reported and ignored (GitHub issue #1629). A merged package (several content metas: base game + update + DLC in one NSP, a cartridge dump with its update) counts as each of
    them: nx_extract takes the base game's, the update's or a DLC's NCAs from it by content meta type and title ID (--app). An NSZ / XCZ is listed as it is (its meta NCAs are not
    compressed) and converted afterwards (decompress_nsz). want: the game to pack when the folder holds several (--title-id / TITLE_ID: a cartridge dump with two games,
    Super Mario Galaxy + Super Mario Galaxy 2)"""
    fams = {}
    def add(p, tid, version):
        low = tid & 0xFFF   # base …000, update …800, DLC = base + 0x1000 + index
        app = tid if low == 0 else tid & ~0xFFF if low == 0x800 else (tid & ~0xFFF) - 0x1000
        f = fams.setdefault(app, {'base': None, 'base_version': 0, 'updates': [], 'dlc': []})
        if low == 0: f['base'], f['base_version'] = p, version
        elif low == 0x800: f['updates'].append((version, p))
        else: f['dlc'].append((tid, version, p))
    packages, listing = [], files_with_suffix(nsp_dir, *PACKAGE_SUFFIXES)   # a base game may be a cartridge dump (.xci: Monster Hunter Rise); nx_extract reads its secure partition
    for p in listing:
        if p.stat().st_size == 0:   # an empty file left by an incomplete download (the Zelda BotW folder holds a 0-byte DLC NSP next to the complete (1).nsp)
            say(f'skipping an empty file: {p.name}'); continue
        if not is_package(p):   # a failed download saved under the game's name, a file cut off before its header: nx_extract would stop on it
            say(f'skipping {p.name}: neither an NSP (PFS0) nor an XCI (HEAD at 0x100)'); continue
        beside = next((q for q in listing if q.suffix.lower() == COMPRESSED.get(p.suffix.lower()) and q.stem.lower() == p.stem.lower()), None)   # from the listing: `Game.NSP` is beside `Game.nsz` on a case-sensitive volume too
        if beside: say(f'skipping {p.name}: {beside.name} is beside it'); continue
        packages.append(p)
    metas = contents_of(packages)
    kinds = {'Application': 'base game', 'Patch': 'update', 'AddOnContent': 'DLC'}
    for p in packages:
        m = metas.get(p, [])
        cs = [] if isinstance(m, str) else [(t, i, v) for t, i, v in m if t in kinds]
        if not cs:   # nx_extract could not read it (a damaged file), or it holds something else (a delta update): it cannot be packed either
            why = f'its content meta could not be read: {m}' if isinstance(m, str) else 'no base game, update or DLC content meta' + (f' ({", ".join(t for t, _, _ in m)})' if m else '')
            say(f'[RED] skipping {p.name}: {why}'); continue
        if len(cs) > 1: say(f'{p.name} holds several titles: ' + ', '.join(f'{kinds[t]} {i:016X} v{v}' for t, i, v in cs))
        elif (tag := name_version(p)) is not None and tag != cs[0][2]:
            say(f'{p.name}: its name says v{tag}, its content meta v{cs[0][2]}, which is the version it is taken for')
        for t, i, v in cs: add(p, i, v)
    if not fams: raise BuildError(f'no game package with a readable content meta in {nsp_dir}')
    games = ', '.join(f'{t:016X}' for t in fams)
    if want is not None and want not in fams: raise BuildError(f'no game {want:016X} (--title-id / TITLE_ID) in {nsp_dir}, which holds {games}')
    if want is None and len(fams) > 1: raise BuildError(f'more than one game in the folder: {games}; keep one game per folder, or name the one to pack with --title-id / TITLE_ID (a cartridge dump that holds two games)')
    app = want if want is not None else next(iter(fams))
    fam = fams[app]
    if fam['base'] is None: raise BuildError(f'no base game NSP found in {nsp_dir} (title ID {app:016X})')
    return app, fam


NSZ_MAIN = "import sys; sys.argv[0] = 'nsz'; from nsz import main; main()"   # the nsz tool of the Python running this file (Spyder does not have its bin folder on PATH)


NSZ_EXECUTABLES = ('nsz', 'nsz-cli-macos-arm64')   # a standalone nsz on PATH: an installed console script, or the macOS release binary


def nsz_command(env):
    """The nsz to run: the nsz package of the Python running this file, else a standalone nsz executable on the build's PATH that takes --keys (nsz 5.0 and later; a standalone
    nsz looks for its keys beside its own file, where this script cannot put them, and 4.6.1 has no --keys)"""
    if importlib.util.find_spec('nsz') is not None: return [sys.executable, '-c', NSZ_MAIN]
    failures = []
    for name in NSZ_EXECUTABLES:
        exe = shutil.which(name, path=env.get('PATH'))
        if not exe: continue
        exe = str(Path(exe).absolute())   # conversion runs in another cwd; PATH entries can be relative
        try: help_result = subprocess.run([exe, '--help'], capture_output=True, text=True, errors='replace', timeout=60, env=env)
        except (OSError, subprocess.SubprocessError) as e:
            failures.append(f'{exe}: --help failed ({e})'); continue
        if help_result.returncode:
            failures.append(f'{exe}: --help exited {help_result.returncode}'); continue
        if '--keys' in help_result.stdout: return [exe]
        failures.append(f'{exe}: no --keys option (update to nsz 5.0 or later)')
    detail = f'; checked: {"; ".join(failures)}' if failures else ''
    raise BuildError(f'NSZ / XCZ files need the nsz tool in the current Python ({sys.executable}: pip install nsz) or an nsz 5.0 or later executable on PATH ({" or ".join(NSZ_EXECUTABLES)}){detail}')


def nsz_source(p, tmp):
    """The path of p to give nsz -D. nsz takes only a lowercase extension (anything else it skips and exits 0 with no output) and resolves a symlink before looking, so a name in capitals
    goes as a hard link with a lowercase extension in tmp, or as a copy where there can be none (another volume, exFAT). The limit is nsz's own, in its PathTools.isCompressedGame"""
    src = p.resolve()
    if p.suffix == p.suffix.lower(): return src
    (tmp / 'source').mkdir()
    lower = tmp / 'source' / (p.stem + p.suffix.lower())
    try: os.link(src, lower)
    except OSError: shutil.copyfile(src, lower)
    return lower.resolve()


def decompress_nsz(nsp_dir, app, fam, keys, env, work=None):
    """NSZ / XCZ files of fam converted once with nsz -D into <build folder>/nsp/ (work, the --work-dir folder, else <title ID>_build in the NSP folder); returns fam with the converted files in their place. nsz writes into an empty temporary folder
    and the file is moved into nsp/ only when nsz exits 0, so a partial one is never taken for a whole one; a conversion is reused while its source has the same name, size and
    modification time. nsz -D prints an NCA hash mismatch but goes on and exits 0: nx_extract's hash check catches a damaged file (ALLOW_DAMAGED)"""
    paths = [fam['base']] + [p for _, p in fam['updates']] + [p for _, _, p in fam['dlc']]
    todo = list(dict.fromkeys(p for p in paths if p.suffix.lower() in COMPRESSED))   # a merged package is the base game, the update and DLC: converted once
    if not todo: return fam
    nsz, keys_opt = nsz_command(env), []
    if nsz[0] != sys.executable: keys_opt = ['--keys', keys.resolve()]   # a standalone nsz looks for keys beside itself, not in cwd: it takes them by --keys (nsz_command)
    out = (work or nsp_dir.absolute() / f'{app:016X}_build') / 'nsp'; logs, tmp = out.parent / 'logs', out / 'partial'
    PROG.plan(1); PROG.stage('decompress NSZ', done=0, total=len(todo))
    conv, made = {}, 0
    for i, p in enumerate(todo):
        dst = out / (p.stem + COMPRESSED[p.suffix.lower()]); keyfile = dst.with_name(dst.name + '.key')
        st = p.stat(); key = fingerprint(p.name, st.st_size, st.st_mtime_ns)
        if not (dst.is_file() and reused(keyfile, key) is not None):
            PROG.set(done=i, step=p.name)
            rmtree(tmp); tmp.mkdir(parents=True); keyfile.unlink(missing_ok=True)   # the stale output stays until the new one replaces it (swap_in)
            log = logs / f'nsz_{p.stem}.log'
            try:
                # nsz 4.6.1 loads keys.txt beside argv[0] at import time; with argv[0] = 'nsz' that is cwd. That release has no --keys option.
                (tmp / 'keys.txt').symlink_to(keys.resolve())
                run([*nsz, *keys_opt, '-D', '-o', tmp, nsz_source(p, tmp)], log, env=env, cwd=tmp)
                if not (tmp / dst.name).is_file(): raise BuildError(f'nsz made no {dst.name} (full output: {log})')
                swap_in(tmp / dst.name, dst)
            finally: rmtree(tmp)
            keyfile.write_text(key + '\n'); made += 1
        conv[p] = dst
    PROG.end(f'{made} converted, {len(todo) - made} reused ({out})')
    c = lambda p: conv.get(p, p)
    return {'base': c(fam['base']), 'base_version': fam['base_version'], 'updates': [(v, c(p)) for v, p in fam['updates']], 'dlc': [(t, v, c(p)) for t, v, p in fam['dlc']]}


def vnum(ver):   # 'v655360' → 655360
    return int(ver[1:])


def nsp_versions(fam):
    """The versions the folder holds: {version: its update package, or None for the base game alone}, each by the version its content meta gives (scan_nsps). The base game is
    v<its own version>: v0 for most, v65536 for a base game the eShop serves at 1.1.0 (Castlevania: Belmont's Curse), which has the 1.1.0 builds. A NUP or a dump folder gives its
    updates' versions itself and its base game as v0; a NUP may have no base game"""
    out = {f'v{fam.get("base_version", 0)}': None} if fam['base'] is not None else {}
    for v, p in fam['updates']: out[f'v{v}'] = p
    return out


def pick_nsps(fam, have, versions):
    """For each version to extract, (base game, update or None): the base game's own version (v0 for most) = the base game alone, v<N> = the base game + the update with version
    number N (nsp_versions). A NUP may have no base game: (None, update)"""
    if fam['base'] is not None: say(f'base: {fam["base"].name}' + (f' (v{fam["base_version"]})' if fam.get('base_version') else ''))
    out = {}
    for ver in versions:
        if ver not in have: raise BuildError(f'no NSP for update {ver} (the game profile needs these versions: {", ".join(versions)})')
        if have[ver]: say(f'update {ver}: {have[ver].name}')
        out[ver] = (fam['base'], have[ver])
    return out


def extract_bytes(picks, clones):
    """What the extraction writes, estimated from the NSPs' sizes. Later versions clone the base game's RomFS when the base is extracted alone and the volume supports clones:
    then each NSP counts once (by path: two NSPs can have the same size). Otherwise every version is a full copy of the base game with its update"""
    if clones and any(u is None for _, u in picks.values()): return sum(p.stat().st_size for p in {p for b, u in picks.values() for p in (b, u) if p})
    return sum(b.stat().st_size + (u.stat().st_size if u and u != b else 0) for b, u in picks.values())   # a merged package is both: counted once


def can_clone(d):   # whether this volume supports APFS clones (nx_extract uses them to save space; without them it writes full copies)
    a, b = d / '.clone_test', d / '.clone_test2'
    try:
        a.write_bytes(b'x'); rmtree(b)
        return ctypes.CDLL(None, use_errno=True).clonefile(str(a).encode(), str(b).encode(), 0) == 0
    finally:
        rmtree(a); rmtree(b)


# ---- Survey of a new game (when no game profile matches) ----
def same_build(nsp_dir, app, fam, keys, repo, env):
    """A title ID no profile has may be a known build under another title ID (a regional release: Advance Wars 1+2: Re-Boot Camp's main 320a1774… under 0100300012FFF000 as well as
    0100300012F2A000, GitHub issue #114): main's build ID of the base game with its newest update, then of the base game alone, against every profile's build rows (game.py by_build).
    Only the code modules are extracted (a RomFS manifest, no game files written). Returns (profile, version, main build ID) or None"""
    import game as gamemod   # dm/games (main put it on the path)
    work = nsp_dir / f'{app:016X}_survey'; logs = work / 'logs'
    files = fam.get('nup') or fam.get('dump')   # a NUP's code modules are read from its packages and a dump's are files already, not by nx_extract
    PROG.plan(1); PROG.stage('build ID lookup', step='reading the code modules')
    nx = None if files else find_nx_extract(repo, env)
    ups = sorted(fam['updates'], key=lambda u: u[0] or 0)
    for v, upd in ([ups[-1]] if ups else []) + ([(0, None)] if fam['base'] is not None else []):
        PROG.set(step=f'main of {"the base game" if upd is None else "update v" + str(v)}')
        tmp = work / 'exefs.partial'; rmtree(tmp)
        if files: code_modules(fam, upd or fam['base'], tmp / 'ExeFS')
        else:
            run([nx, keys, fam['base'], upd or '-', tmp, '--manifest'] + extract_flags(app), logs / 'exefs.log',
                ok=(4,))   # 4: a sparse base game (Minecraft Dungeons II), readable only with its update: no main of its own
        main = tmp / 'ExeFS/main'
        bid = build_id(main) if main.exists() else None
        g = gamemod.by_build(bid) if bid else None
        rmtree(tmp)
        if g:
            ver = next(ver for ver, b in g.builds().items() if b.get('main') == bid)
            PROG.end(f'main {bid} is game profile {g.id}\'s {ver}'); return g, ver, bid
    PROG.end('no profile registers its main build'); return None


def code_modules(fam, k, dst):   # the ExeFS of a NUP package or a dump's version folder into dst; nothing for a version folder with no code of its own
    if fam.get('dump'):
        if pack_dump.has_code(k): pack_dump.exefs(k / 'exefs', dst)
        return
    try: pack_nup.exefs(fam['nup'], k, dst)
    except pack_nup.Error as e: raise BuildError(str(e)) from e


def package_size(fam, k):   # (name, bytes) of a package for the survey report: an NSP file, a NUP package (its files' size), or a dump's folder (its code and RomFS: the folder of a flat dump also holds the build)
    if fam.get('nup'): return k.name, fam['nup'].tree_bytes(k.root)
    if fam.get('dump'): return k.name, sum(tree_size(d)[1] for d in (k / 'exefs', k / 'romfs')) or tree_size(k)[1]
    return k.name, k.stat().st_size


MANIFEST_HEAD = '# path\tbytes\tfirst 32 bytes (hex)\n'   # nx_extract --manifest's header


def survey_dump(src, out):
    """A dump's version folder as survey_nup leaves a NUP package's, with no control data (a dump has none). Returns the files listed"""
    if not pack_dump.is_version(src): raise BuildError(f'{src}: no exefs/main.npdm and romfs')
    out.mkdir(parents=True)
    pack_dump.exefs(src / 'exefs', out / 'ExeFS')
    rows = []
    for p, size, head in pg.romfs_files(src / 'romfs'):
        rows.append(f'{p}\t{size}\t{head.hex()}\n')
        if p.lower().endswith('.nro'):
            o = out / 'romfs_nro' / p.lstrip('/'); o.parent.mkdir(parents=True, exist_ok=True)
            pack_dump.clone(src / 'romfs' / p.lstrip('/'), o)
    (out / 'romfs_manifest.tsv').write_text(MANIFEST_HEAD + ''.join(rows))
    return len(rows)


def survey_nup(n, k, base, out):
    """A NUP package's files as nx_extract --manifest leaves an NSP's for the survey, without writing the RomFS: out/ExeFS, out/romfs_manifest.tsv (path, size and first 32 bytes of every
    RomFS file), out/romfs_nro (the NRO modules, whole: their imports decide the graphics API and the modules) and the package's control data, else its base game's. Returns the files listed"""
    out.mkdir(parents=True)
    try: pack_nup.exefs(n, k, out / 'ExeFS')
    except pack_nup.Error as e: raise BuildError(str(e)) from e
    rows = []
    def walk(d, pre):
        for name, isdir, i in n.children(d):
            p = pre + '/' + os.fsdecode(name)
            if isdir: walk(i, p); continue
            rows.append(f'{p}\t{n.files[i][1]}\t{n.head(i).hex()}\n')
            if p.lower().endswith('.nro'):
                o = out / 'romfs_nro' / p.lstrip('/'); o.parent.mkdir(parents=True, exist_ok=True)
                with open(o, 'wb') as f: n.copy(i, f)
    try:
        walk(pack_nup.romfs_of(k, base), '')
        pack_nup.control(k, out, pack_nup.app_package(n) if base is None else base)   # what an update lacks, from its base game (or the metadata record, as pack_version takes it)
    except pack_nup.Error as e: raise BuildError(str(e)) from e
    (out / 'romfs_manifest.tsv').write_text(MANIFEST_HEAD + ''.join(rows))
    return len(rows)


def check_aarch64(exefs, what, hint=''):   # main.npdm's 64-bit instruction flag (pg.aarch64): a clear one means A32 code and 32-bit dynamic tables, which the survey and the rewriter cannot read (LNPANDY/NX2Apple#116)
    if pg.aarch64(Path(exefs) / 'main.npdm'): return
    raise BuildError(f'{what} is a 32-bit ARM program (main.npdm: the 64-bit instruction flag is clear): '
                     f'Apple silicon cannot run 32-bit ARM code, and this repository runs a game\'s own AArch64 code, so it cannot be ported{hint}')


def check_versions_aarch64(dirs, logs, g, name):   # every extracted version of a game that has a profile; names the version and the registered ones that can be packed instead
    for ver, d in dirs.items():
        xlog = logs / f'extract_{ver}.log'
        dv = re.search(r'display version (\S+)', xlog.read_text(errors='replace')) if xlog.is_file() else None
        other = [f'{v} {b["display"]}' for v, b in g.builds().items() if v != ver]
        check_aarch64(d / 'ExeFS', f'{name} {ver}{f" ({dv.group(1)})" if dv else ""}',
                      f'; game profile {g.id} registers {", ".join(other)}: pack {"that version" if len(other) == 1 else "one of those versions"} (-p / PACKAGE) with its update in the folder' if other else '')


def survey(nsp_dir, app, fam, keys, repo, py, env):
    work = nsp_dir / f'{app:016X}_survey'; logs, ex = work / 'logs', work / 'extract'
    say(f'no game profile for {app:016X}: surveying first')
    nup, dump = fam.get('nup'), fam.get('dump')
    files = nup or dump   # a NUP's or a dump's newest version: its files are read as they are, with no nx_extract
    PROG.plan(3)   # the manifest, the code modules (reading them, then the rewrite-site census), the imports
    if not files: nx = find_nx_extract(repo, env)
    ups = sorted(fam['updates'], key=lambda u: u[0] or 0)
    upd = ups[-1][1] if ups else None
    tmp = work / 'extract.partial'; rmtree(tmp)
    def listed():   # files in the manifest so far
        try: return {'done': (tmp / 'romfs_manifest.tsv').read_bytes().count(b'\n'), 'unit': 'files'}
        except OSError: return None
    PROG.stage('survey: RomFS manifest', poll=None if files else listed)
    say(f'extracting the code modules and a RomFS manifest (no game files written) → {ex}')
    if files: say(f'  {survey_nup(nup, upd or fam["base"], fam["base"], tmp) if nup else survey_dump(upd or fam["base"], tmp):,} RomFS files listed')
    else: run([nx, keys, fam['base'], upd or '-', tmp, '--manifest'] + extract_flags(app), logs / 'extract.log')
    swap_in(tmp, ex)
    PROG.stage('survey: code modules', step='reading')
    dv = nacp.version(ex / 'control.nacp') if files else (m.group(1) if (m := re.search(r'display version (\S+)', (logs / 'extract.log').read_text(errors='replace'))) else '')
    title = nacp.name(ex / 'control.nacp') or (nsp_dir.name if dump else '')   # a dump has no control data: its folder names the game
    check_aarch64(ex / 'ExeFS', f'{title or "?"} ({app:016X})')
    F = pg.facts_of(ex / 'ExeFS', ex / 'romfs_manifest.tsv'); mods, rom = F['mods'], F['romfs']   # dm/games/profile_guess.py: what the code modules and the RomFS show
    def size(k): return package_size(fam, k)[1] / 2**30
    R = [f'# Survey: {title or "?"} ({app:016X})', '', f'- display version: {dv or "?"}']
    R += [f'- base: `{package_size(fam, fam["base"])[0]}` ({size(fam["base"]):.2f} GB)' if fam['base'] is not None else '- base: none (the update is the whole game)']
    R += [f'- update: `{package_size(fam, p)[0]}` (v{v}, {size(p):.2f} GB){" (used for this survey)" if p is upd else ""}' for v, p in ups]
    R += [f'- DLC: `{package_size(fam, p)[0]}` ({size(p) * 2**20:.0f} KB{", licence only, no content" if size(p) < 1 / 1024 else ""})' for _, _, p in fam['dlc']]

    # Code modules
    R += ['', '## Code modules', '', '| module | what | build ID | text / ro / data (MB) | bss (MB) |', '|---|---|---|---|---|']
    R += [f'| {n} | {m["kind"]} | `{m["bid"]}` | ' + ' / '.join(f'{z / 2**20:.1f}' for z in m['sizes']) + f' | {m["bss"] / 2**20:.1f} |' for n, m in mods.items()]
    if F['program'] is not None: R.append(f'\n- main.npdm program ID: `{F["program"]:016X}`')
    if (ex / 'ExeFS/main.npdm').exists(): R.append(f'- system resource: {F["sysres"] / 2**20:.0f} MB')
    R.append(f'- SDK: {", ".join(sorted(F["sdkver"])) or "?"} (Mario: 16_2_0 / 16_2_12)')
    R.append('- SDK components: ' + (', '.join(sorted(F['comps'])) or '—'))
    if F['unity']: R.append('- Unity: ' + ', '.join(sorted(F['unity'])))
    R.append('- engine and API markers: ' + ('; '.join(f'{k} ({", ".join(v)})' for k, v in F['markers'].items()) or '—'))
    extra = [n for n in mods if n not in ('main', 'sdk', 'rtld')]

    # RomFS manifest
    R += ['', f'## RomFS: {rom["n"]} files, {rom["bytes"] / 2**30:.2f} GB', '', '| top level | files | GB |', '|---|---|---|']
    R += [f'| {t} | {n} | {rom["topb"][t] / 2**30:.2f} |' for t, n in rom['tops'].most_common(20)]
    R += ['', '| header magic | files | GB |', '|---|---|---|'] + [f'| {m} | {n} | {rom["magb"][m] / 2**30:.2f} |' for m, n in rom['mags'].most_common()]
    R += ['', 'extensions: ' + ', '.join(f'`{e}` {n}' for e, n in rom['exts'].most_common(20))]

    # Rewrite-site census and SDK surface
    R += ['', '## Rewrite-site census (insn_census.py)', '', '```']
    census = [ex / 'ExeFS' / n for n in mods if n != 'rtld']   # rtld is replaced by the host
    for i, m in enumerate(census):
        PROG.set(step=f'rewrite sites in {m.name}', done=i, total=len(census), unit='modules')
        run([py, repo / 'dm/tools/insn_census.py', m], logs / f'census_{m.name}.log', env=env)
        R += (logs / f'census_{m.name}.log').read_text(errors='replace').rstrip().splitlines()
    R += ['```', '', '## Imports vs host hooks (nso_syms.py)', '', '```']
    PROG.stage('survey: imports vs host hooks')
    run([py, repo / 'dm/tools/nso_syms.py', repo / 'dm/host', '-', ex / 'ExeFS'], logs / 'nso_syms.log', env=env)
    PROG.end()
    R += (logs / 'nso_syms.log').read_text(errors='replace').rstrip().splitlines() + ['```']

    # What it means for a port
    G = pg.guess(F)   # the provisional profile's keys (write_profile writes the same)
    keep = G['modules'][0].split()   # how the build will get the shaders (decided again from the extraction at build time; the survey has only a RomFS manifest, so Unreal paks are guessed from main's markers)
    nros = {p.name: p for p in sorted((ex / 'romfs_nro').rglob('*.nro')) if p.is_file()}   # files only: a RomFS folder may end in .nro (The Binding of Isaac: Afterbirth+). The manifest pass writes the NROs (their imports decide the API and the modules: Portal 2's main is SDL with EGL, its Source engine NROs draw with NVN)
    nro_add = pg.nro_modules(ex / 'ExeFS', nros, keep) if nros else {}
    S = pg.shader_sources(mods['main']['imports'], F['glsl_src'], [m for m in sorted({*keep, *nro_add}, key=pg.load_order) if 'glslcCompile' in mods[m]['exports']], rom['mags']['SPIR-V'],
                          rom['exts'].get('.pak', 0) if 'Unreal' in F.get('main_markers', set()) else 0, rom['mags']['Trinity pack'], gl_names=F.get('gl_names', 0),
                          gl_src=F.get('gl_src', 0), gl_files=sum(rom['exts'].get(e, 0) for e in pg.GL_FILES), oodle_pkz=rom['mags']['PlatinumGames PKZ'],   # the manifest has no name tables: every PKZ counts
                          nro_imports=frozenset(pg.romfs_nro_imports(ex / 'romfs_nro')))
    vulkan, gl = S['graphics'][0] == 'vulkan', S['graphics'][0] == 'gl'
    notes = []
    if not vulkan and not gl and 'nvnBootstrapLoader' not in mods['main']['imports']: notes.append('main imports neither nvnBootstrapLoader nor Vulkan functions nor OpenGL through EGL: the host implements only NVN, Vulkan and OpenGL')
    if vulkan: notes.append(f'{S["graphics"][1]}; the NVN shader steps are skipped' + (', and its shaders are captured by booting it on the Mac (no SPIR-V file in the RomFS)' if S['vk_capture'][0] else ''))
    if gl: notes.append(f'{S["graphics"][1]}; the NVN shader steps are skipped: gl_shaders.py translates its GLSL sources (MVGL archives, main\'s strings'
                        + (', and the shaders it puts together at run time, captured by booting it on the Mac)' if S['gl_capture'][0] else ')'))
    notes += [S[k][1] for k in ('glslc_capture',) if S[k][0]]
    if S['ue_decode'][0]:   # the manifest has no pak indexes, so both kinds are named; the build tells them apart by the footer's version and the index (uepak.py inline_shaders)
        notes.append(f'{rom["exts"].get(".pak", 0)} Unreal pak(s): version 11 paks keep the shaders compressed in shader code archives (Octopath Traveler 0), decoded by booting the game on the Mac; '
                     'legacy paks (version 4, DRAGON QUEST XI S) and version 11 paks without shader code archives (OCTOPATH TRAVELER II) keep them inline in the cooked packages, which the shader split reads file by file')
    if extra: notes.append('modules besides main / sdk: {}; the provisional profile loads {} (load order main, subsdk*, sdk)'.format(", ".join(f'{n} ({mods[n]["kind"]})' for n in extra), G["modules"][0])
                           + ('; the build adds {}: the NROs import from {}'.format(', '.join(nro_add), 'it' if len(nro_add) == 1 else 'them') if nro_add else ''))
    if rom['mags']['UnityFS'] or 'Unity IL2CPP' in F['markers']: notes.append('Unity: the shaders are Shader objects in UnityFS bundles and data.unity3d, which shader_split.py reads through unityfs.py (written for Unity 2020.3\'s Switch format, also read for 2017.4, 2018.3, 2019.4, 2021.2, 2021.3, 2022.3, 6000.0 and 6000.3; check other Unity versions first)')
    if F['glslc']: notes.append(f'main has {F["glslc"]} GLSLC compiler output magics (0x19866891): shader_split.py takes those that are whole outputs (13 Sentinels and the KTGL games compile their shaders into main; Unity\'s player has 11 of its own)')
    if rom['mags']['Trinity pack']: notes.append('Trinity packs (Game Freak, arc/data.trpfs with its index data.trpfd): the shaders are BNSH inside Oodle Mermaid streams; shader_split.py decodes them with the game\'s own OodleLZ_Decompress in the Mac host, which dm/host/hle_oodle.c finds by the library\'s name string')
    elif rom['mags']['PlatinumGames PKZ']: notes.append('PlatinumGames PKZ archives (pkzl; Bayonetta 3): DAT archives inside, compressed per entry (OodleKraken, ZStandard or None); the shaders are shader objects (VSO / PSO / CSO …) in DATs, which shader_split.py reads (pkz_entries, pg_shader), Oodle Kraken through the game\'s own streaming decoder in the Mac host (dm/host/hle_oodle.c)')
    elif rom['mags']['RE Engine PAK']: notes.append('RE Engine PAK archives (KPKA; Capcom: Monster Hunter Rise, Ghost Trick, PRAGMATA): file names only as hashes, entries zstd / deflate, PRAGMATA\'s large ones in a block table; the shaders are SDF definitions inside them, which shader_split.py reads (kpka_jobs, sdf_shaders)')
    elif rom['mags']['MT Framework ARC']: notes.append('MT Framework ARC archives (Capcom, The Great Ace Attorney Chronicles): the shaders are in a shader package (SPK) inside one of them, which shader_split.py reads (mtarc_members, mtspk_shaders)')
    elif rom['mags']['Bezel Engine archive']: notes.append('NintendoWare Bezel Engine archives (.bea, magic SCNE; Big Brain Academy: Brain vs. Brain): every entry is a zstd frame, and the shaders are BNSH in their .bfsha entries, which shader_split.py reads (bea_members)')
    elif rom['mags']['CRI CPK'] and not F['glslc']: notes.append('CRI CPK archives (Bayonetta 2; Vanillaware\'s and Atlus\'s games keep their shaders elsewhere): files compressed one by one with CRILAYLA; shader_split.py decodes every entry (dm/gfx/tools/cpk.py, cpk_jobs) and scans it as a loose file (Bayonetta 2: BNSH files in PlatinumGames DAT archives)')
    elif rom['mags']['XNB'] and 'MonoGame' in F['markers']: notes.append('MonoGame (XNB content): the shaders are MonoGame effects (MGFX version 8, NVN control sections and code areas, or MonoGame 3.0\'s version 5, whole GLSLC outputs) in the XNB files EffectReader reads, in .mgfxo files and SpriteBatch\'s in main, which shader_split.py reads (mgfx_effect; Celeste, Bastion), a compressed XNB (MonoGame\'s LZX or LZ4, Stardew Valley) after decompressing it (xnb_decompress)')
    elif not (rom['mags']['BNSH'] or rom['mags']['BAHS']) and not rom['mags']['UnityFS'] and not vulkan and not gl and not F['glslc']:
        notes.append('no BNSH / BAHS at the top level: the shaders are inside compressed or other containers or in main, and must be located first')
    major = {v.split('_')[0] for v in F['sdkver']}
    if not major: notes.append('no SDK version string found')
    elif major != {'16'}: notes.append(f'SDK {"/".join(sorted(major))}, not Mario\'s 16: NVN object sizes, API version and service layouts differ by SDK (the host was converted from D2R\'s SDK 12 to SDK 16 sizes); check them one by one per docs/reference/nvn-objects.md')
    if any(l.split()[:1] == ['nn::ro'] for l in R):
        nro, nrr = rom['exts']['.nro'], rom['exts']['.nrr']
        # NRR files register NROs by hash; with none loose the NROs are inside an archive (Super Smash Bros. Ultimate: 14 NRRs, 98 NROs in data.arc, whose exports main imports)
        notes.append(f'imports nn::ro (loads NRO modules at run time): the RomFS has {nro} .nro files and {nrr} .nrr files; registered NROs are collected and rewritten beside the other modules'
                     + (' (look inside archives for the registered modules)' if nrr and not nro else '') + ' and embedded in the iOS app (gen_embed.py)')
    if fam['dlc']: notes.append('DLC present: it is extracted into the IPA and the host reports the indices as owned and mounts them; check that the host hooks every aoc / fs add-on content call main imports')
    R += ['', '## What it means for a port', ''] + [f'- {n}' for n in notes or ['—']]
    R += ['', '## Provisional profile keys (dm/games/profile_guess.py)', '', 'What the game files decide; mem_used follows the rewrite, and a game with virtual address memory that sets a heap gets a heap check on the Mac (scripts/build_ipa_from_nsp.py heap_check).', '', '```']
    R += [f'{k}\t{v}' + (f'\t# {why}' if why else '') for k, (v, why) in G.items()] + ['```']
    R += ['', 'Next steps: docs/agents (02 survey, 03 bring-up, symptoms.md for each failure).']
    (work / 'survey.md').write_text('\n'.join(R) + '\n')
    for n in notes: say(n)
    say(f'survey report: {work / "survey.md"}')
    upv = ups[-1][0] if ups else None
    return {'F': F, 'name': title, 'display': dv or '?', 'version': f'v{fam.get("base_version", 0)}' if upd is None else f'v{upv}'}   # the names nsp_versions gives


# ---- Provisional profile (a game with no profile in dm/games) ----
def ipa_name(name, app):   # "The Legend of Zelda: Link's Awakening" → TheLegendOfZeldaLinksAwakening.ipa: ASCII words joined, apostrophes dropped; the title ID when nothing ASCII is left
    words = re.findall(r'[A-Za-z0-9]+', name.replace("'", '').replace('’', ''))
    words = [w.capitalize() if w.isupper() and len(w) > 1 else w[:1].upper() + w[1:] for w in words]
    return (''.join(words)[:48] or f'{app:016X}') + '.ipa'


EMPTY_TABLES = {   # the shader tables before (or, for a Vulkan game, instead of) a shader split: the profile inputs' digest (dmsh_chain.py inputs) and build_ipa.sh read them
    'shader_map.tsv': '# kind: evidence  provisional profile: filled by the shader split (a Vulkan game\'s SPIR-V shaders go through the Vulkan layer and leave it empty)\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n',
    'shader_facts.tsv': '# Answer table for translation queries (generated by dm/gfx/tools/shader_split.py, do not edit): key <TAB> fact JSON. Provisional profile: filled by the shader split\n',
    'shader_pairs.tsv': '# kind: evidence  vertex / pixel shader pairs within BNSH variations (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n',
    'shader_map.tsv.conflicts.tsv': '# kind: evidence  control-section key conflict rulings: empty\ndm_ctl_sha16\tchosen_pc\tclass\tcandidates\trecords\n',
    'bf_want.tsv': '# generated\n',
    'pso_manifest.tsv': '# Prewarm seed list (shipped in the app, next to shader_map.tsv). Rows: S<TAB>shader key<TAB>stage / R<TAB>render pipeline key (pso_skey hex) / C<TAB>compute shader key / M<TAB>task shader key or -<TAB>native task / mesh pipeline key (pso_skey hex: the mesh shader in the vertex place).\n'
                        '# Provisional profile: empty until recorded on the Mac (DM_PSO_MANIFEST=off DM_PSO_RECORD).\n'}


def work_dir(args):   # the build folder --work-dir / WORK_DIR names, used as given; None = the default, <title ID>_build in the NSP folder
    w = args.work_dir or WORK_DIR
    return Path(os.path.expanduser(w)).resolve() if w else None


def build_folder(args, nsp_dir, gid):
    """The build folder of the game profile gid: --work-dir / WORK_DIR as given, else <NSP folder>/<gid>_build. A program packed with the application's first (in_programs, Super
    Mario 3D All-Stars' games) gets <work dir>/programs/<gid>_build under --work-dir, as it gets its own <gid>_build without: in the one folder each program's extract/<version>/RomFS.img
    replaced the one before, and the package took the last program's image for the first's ("missing RomFS marker", docs/reference/packing.md)"""
    work = work_dir(args)
    if not work: return nsp_dir / f'{gid}_build'
    return work / 'programs' / f'{gid}_build' if getattr(args, 'in_programs', None) is not None else work


def profile_folder(args, nsp_dir, app):
    """Where the provisional (or derived) profile goes: --profile-out / PROFILE_OUT, else <build folder>/profile, the build folder being --work-dir / WORK_DIR or <NSP folder>/<title ID>_build.
    A build folder of its own that has no profile yet gets a copy of the one in the default place (kept there), so a game surveyed before --work-dir is not surveyed again."""
    if args.profile_out or PROFILE_OUT: return Path(os.path.expanduser(args.profile_out or PROFILE_OUT)).resolve()
    old = nsp_dir / f'{app:016X}_build' / 'profile'
    work = work_dir(args)
    if not work: return old
    new = work / 'profile'
    if new != old.resolve() and old.is_dir() and not new.exists():
        say(f'copying the profile from {old} to {new} (the original stays)')
        new.parent.mkdir(parents=True, exist_ok=True); shutil.copytree(old, new, symlinks=True)
    return new


def write_profile(pdir, app, f):
    """A provisional game.tsv (+ the empty tables) from the survey, in pdir (outside the repository). An existing game.tsv is kept, so it can be edited by hand; delete it to regenerate.
    The keys the game files decide come from dm/games/profile_guess.py (each written under its reason): the modules to load, the memory partition, engine presets; the rest keeps game.py's
    defaults until the heap check (below) or a developer sets it. What the files show at every build is not written: the graphics API and the shader sources (shader_sources), the shared-font
    layout and the NVN API version (the host reads the SDK version)"""
    pdir.mkdir(parents=True, exist_ok=True)
    if (pdir / 'game.tsv').exists():
        say(f'reusing the provisional profile: {pdir / "game.tsv"} (delete it to regenerate from the survey)')
        return
    F = f['F']; G, R = pg.guess(F), F['romfs']
    groups, seen = [], {'main', 'other'}
    for d, _ in R['dirs'].most_common(24):   # shader package groups by RomFS top-level folder (this only names the package files; anything else goes to "other")
        g = re.sub(r'[^a-z0-9]', '', d.lower())[:16] or 'g'
        while g in seen: g += 'x'
        seen.add(g); groups.append(f'/{d}/={g}')
    # stored in the IPA uncompressed: file types that are mostly compressed already (Yaz0 / zstd / zip / media headers; Super Mario Odyssey's 3.8 GB of .szs took the zip 1 h 40 min to deflate again)
    # and big files (mostly compressed formats without a known header)
    store = sorted(e for e, b in R['extb'].items() if e and R['exts'][e] and (b / R['exts'][e] >= 1 << 20 or 2 * R['extc'][e] >= R['exts'][e]))
    name = f['name'] or f'{app:016X}'
    mods = G['modules'][0].split()
    T = '\t'
    def key(k, default=None):   # a key with the reason profile_guess.py gives for it above it; None when neither the guess nor a default has one
        v, why = G[k] if k in G else (default, '')
        return [] if v is None else ([f'# {why}'] if why else []) + [f'{k}{T}{v}']
    # shader_note is a shader input (dmsh_chain.profile_rows, and the shader map's header): it reads as a registered profile's does, so moving this profile into dm/games keeps every shader's cache key.
    # Super Mario RPG: "(provisional profile)" in this note scheduled all 82,032 libraries again, about 1.5 h, for identical shaders. The comment below says what this file is
    L = ['# Provisional game profile (generated by scripts/build_ipa_from_nsp.py from the survey with dm/games/profile_guess.py, not checked by hand)',
         f'# {name}, SDK {F["sdk"] or "?"}, survey {f["survey"]}', '',
         '# ---- Identity ----', f'title_id{T}{app:016X}', f'program_id{T}{(F["program"] or app):016X}', '',
         '# ---- App ----', f'name{T}{name}', f'ipa{T}{ipa_name(name, app)}', f'app_category{T}public.app-category.games', *key('pad_layout'), *key('op_mode'), '',
         '# ---- Versions ----', T.join(['build', f['version'], f['display']] + [f'{m}={F["mods"][m]["bid"]}' for m in mods]), f'package{T}{f["version"]}', f'shader_versions{T}{f["version"]}', '',
         '# ---- Files ----', *key('modules'), f'romfs_marker{T}{R["biggest"][1]}', f'zip_store{T}{":".join(store + [".metallib"])}', '',
         '# ---- Memory ----', *key('mem_reserve'), *key('mem_alias'), *key('mem_stack'), *key('app_pool'), *key('sys_resource'), '',
         '# ---- Shader split ----', *key('shader_groups', " ".join(groups) or "/=romfs"), f'shader_note{T}{name} shaders']
    (pdir / 'game.tsv').write_text('\n'.join(L) + '\n', encoding='utf-8')
    for n, text in EMPTY_TABLES.items(): (pdir / n).write_text(text)
    say(f'wrote a provisional game profile: {pdir / "game.tsv"}')
    for k in ('modules', 'mem_reserve', 'op_mode'):
        if k in G and G[k][1]: say(f'  {k} {G[k][0]}: {G[k][1]}')


# ---- Derived profile (a version the game's profile was not brought up on) ----
DERIVED_SKIP = {'game.tsv', 'shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'vk_capture.tsv', 'gl_capture.tsv', 'patches.py'}   # made again from the packed version's own files (patches.py: the rewriter's patches of the registered builds)


def derived_key(profile_dir):
    return profile_dir.with_name(profile_dir.name + '.derived.key')


def frame_step_builds(path, table='FRAME_STEP'):   # the keys of a table of a patches.py (FRAME_STEP: build IDs; MATCH_FRAME_STEP: module names), read without running the file (empty: no table)
    try:
        for n in ast.parse(path.read_text(encoding='utf-8')).body:
            if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == table for t in n.targets) and isinstance(n.value, ast.Dict):
                return {k.value for k in n.value.keys if isinstance(k, ast.Constant)}
    except (OSError, UnicodeError, SyntaxError): pass
    return set()


def derive_profile(g, pdir, ver, d, mods, app=None, shader_only=False):
    """The game's profile for a version it was not brought up on (the folder has no update, another update, files of other builds, or a registered build under another title ID: app),
    in pdir (outside the repository, where a provisional profile goes): its keys as they are, with this version's build row, package and shader versions, and app's title ID and
    main.npdm's program ID when app is not the profile's title. The shader tables, the chain header and the run-time capture list are
    left out: the build makes them again from this version's own files, as for a provisional profile. The hand-written tables (the prewarm list, shader_facts_manual.tsv) are copied:
    they name shaders by content key. A folder derived from the same inputs (the files copied and the game.tsv written, kept in <pdir>.derived.key) is kept as it is, with what later
    stages wrote into it (mem_used, the heap check, the split's tables), so their own reuse holds. shader_only preserves all registered game.tsv rows and links its existing cuts:
    only the shader tables change with installed DLC or newly decoded compute kernels. The modules are the profile's that this version ships and those main imports from that the list leaves out. A profile with frame step stubs (FRAME_STEP) and no function-matched ones (MATCH_FRAME_STEP) for other builds only gets fps_target 0 for this version:
    its main has no state page, so a cap of 30 or 60 would present every vsync while the game steps 1/30 s per frame (double speed). Returns the version's display version"""
    src = sorted(p for p in g.dir.iterdir() if p.is_file() and (p.name not in DERIVED_SKIP or shader_only and p.name == 'patches.py'))
    # The display version: the package's control data, else the one the profile registers for this main build. A NUP update or a dump carries no control data, and the game asks
    # the host for the version by its main's build ID (hle_sdk16.c GetDisplayVersion, the build rows): a row with ? is what the game then shows (Super Mario Bros. Wonder 1.2.1
    # from an update-only NUP: "Ver. ?" on its title screen, though the profile registers that build as 1.2.1)
    main = d / 'ExeFS/main'; bid = build_id(main) if main.exists() else None
    known = next((b['display'] for b in g.builds().values() if bid and b.get('main') == bid and b.get('display') not in (None, '', '?')), None)
    T, disp = '\t', nacp.version(d / 'control.nacp') or known or '?'
    add = pg.main_modules(d / 'ExeFS', mods) if not shader_only and (d / 'ExeFS/main').exists() else {}   # a module's slot can change between versions: what main imports and the profile's list leaves out (Absolum 1.2.1's movie module). The host loads what the row names
    mods = sorted({*mods, *add}, key=pg.load_order) if add else mods
    row = T.join(['build', ver, disp] + [f'{m}={build_id(d / "ExeFS" / m)}' for m in mods if (d / 'ExeFS' / m).exists()])
    # the header names the profile by its ID, not its folder: the text is part of the derived key, so the same profile in another checkout keeps the derived folder and the split
    out, put = [f'# Derived from game profile {g.id} by scripts/build_ipa_from_nsp.py for {ver} ({disp}), a version that profile was not brought up on:',
                '# its keys as they are, this version\'s build row, and shader tables made from this version\'s own files'], False
    if shader_only: out = [f'# Derived shader tables from game profile {g.id}; registered module builds and settings are preserved.']
    ids = {}
    if app is not None and app != g.title_id:   # the host reports the program ID and answers save data queries for it (hle_svc.c, hle_fs.c)
        ids = {'title_id': f'title_id{T}{app:016X}', 'program_id': f'program_id{T}{(pg.npdm(d / "ExeFS/main.npdm")[1] or app):016X}'}
        out.insert(1, f'# under title ID {app:016X}, not the profile\'s {g.title_id:016X}')
    stubs = frame_step_builds(g.dir / 'patches.py')
    # a frame step placed by its functions (MATCH_FRAME_STEP) may serve this main; whether it does is known only when the rewrite matches it, and where it does not the main
    # has no state page and the host keeps the game's own present interval anyway (fps_present_cap: a cap needs a frame step or an engine limit the host writes)
    matched = 'main' in frame_step_builds(g.dir / 'patches.py', 'MATCH_FRAME_STEP')
    own_step = shader_only or not stubs or matched or not (d / 'ExeFS/main').exists() or build_id(d / 'ExeFS/main') in stubs
    for line in (g.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
        if not own_step and line.split(T, 1)[0] == 'fps_target' and line.partition(T)[2].strip() != '0':
            out += ['# the profile has frame step stubs for other builds only: this version\'s main has none, so it keeps the game\'s own step', f'fps_target{T}0']; continue
        if line.split(T, 1)[0] in ids: out.append(ids[line.split(T, 1)[0]]); continue
        if not shader_only and line.split(T, 1)[0] == 'modules' and (d / 'ExeFS').is_dir():
            want = line.partition(T)[2].split(); have = [m for m in want if (d / 'ExeFS' / m).exists()]
            if add: have = sorted({*have, *add}, key=pg.load_order)
            if have != want:   # a module this version does not ship (Mario Kart World 1.0.0 has no subsdk1, the C library module of 1.8.0): left in, the rewrite stopped on the missing file
                if gone := [m for m in want if m not in have]: out.append(f'# this version ships no {" ".join(gone)}')
                if add: out.append('# main imports from ' + ', '.join(f'{m} ({k})' for m, k in add.items()) + ', which the profile does not list')
                out.append(f'modules{T}{" ".join(have)}'); continue
        if not shader_only and line.split(T, 1)[0] in ('build', 'package', 'shader_versions'):
            if not put: out += [row, f'package{T}{ver}', f'shader_versions{T}{ver}']; put = True
            continue
        out.append(line)
    text, kf = '\n'.join(out) + '\n', derived_key(pdir)   # beside the profile, not in it: no tool reads it as a table
    key = fingerprint(('files', src), text)
    if (pdir / 'game.tsv').is_file() and reused(kf, key) is not None:
        def rows(text):   # the same whitespace and comment rules as game.Game
            return [tuple(c.strip() for c in l.split(T)) for l in text.splitlines() if l.strip() and not l.lstrip().startswith('#')]
        expected = collections.Counter(rows(text)); keys = {r[0] for r in expected}
        try: kept = collections.Counter(r for r in rows((pdir / 'game.tsv').read_text(encoding='utf-8')) if r[0] in keys)
        except (OSError, UnicodeError): kept = None
        if kept == expected: return disp
    rmtree(kf); pdir.mkdir(parents=True, exist_ok=True)
    atomic_text(pdir / 'game.tsv', text)
    for p in pdir.iterdir():
        if p.name != 'game.tsv': rmtree(p)
    for p in src:
        # Registered cuts stay the same file: patch_nso.py deduplicates real paths, so an external profile must not register copied claims for the same builds.
        if shader_only and p.name == 'patches.py': (pdir / p.name).symlink_to(p.resolve())
        else: shutil.copyfile(p, pdir / p.name)
    kf.write_text(f'{key}\n')
    return disp


# ---- Stage reuse: a stage whose inputs are all as at its last successful run keeps its output ----
def files_under(d):   # (relative path, stat) of every file under d, sorted; [] when it does not exist
    out, todo = [], [str(d)]
    while todo:
        try: it = rfs.os.scandir(todo.pop())
        except OSError: continue
        with it:
            for e in it:
                if e.is_dir(follow_symlinks=False): todo.append(e.path)
                else: out.append((os.path.relpath(e.path, d), e.stat(follow_symlinks=False)))
    return sorted(out, key=lambda x: x[0])


def fingerprint(*items, env=None):
    """sha256 over a stage's inputs. Items: ('files', paths) = names and content (a missing file counts too); ('tree', folder) = relative path and size of every file under it
    (a RomFS: check_build pins its version, so names and sizes stand for its content); ('stat', folder) = relative path, size and mtime of every file under it (the translator's binaries);
    ('cmd', argv) = a tool's version output (run with env); anything else = its repr"""
    h = hashlib.sha256()
    for it in items:
        kind, v = it if isinstance(it, tuple) and len(it) == 2 and it[0] in ('files', 'tree', 'stat', 'cmd') else ('repr', it)
        h.update(f'\0{kind}\0'.encode())
        if kind == 'files':
            for p in map(rfs.Path, v):
                h.update(p.name.encode() + b'\0')
                try: h.update(hashlib.sha256(p.read_bytes()).digest())
                except OSError: h.update(b'missing')
        elif kind in ('tree', 'stat'):
            for rel, st in files_under(v): h.update(f'{rel}\0{st.st_size}\0{st.st_mtime_ns if kind == "stat" else ""}\n'.encode())
        elif kind == 'cmd':
            try: r = subprocess.run(v, env=env, capture_output=True, text=True, timeout=120); h.update((r.stdout + r.stderr).encode())
            except (OSError, subprocess.TimeoutExpired): h.update(b'missing')
        else: h.update(repr(v).encode())
    return h.hexdigest()


def reused(keyfile, key):   # the note the stage's last successful run wrote beside its key ('' when none), or None when the key differs or is missing
    try: k, _, note = keyfile.read_text().partition('\n')
    except OSError: return None
    return note.strip() if k == key else None


def tool_files(repo):   # what the shader stages run: dm/gfx/tools' scripts and the lock of the translator's fingerprints, the profile reader, the iOS compile convention, the RomFS reader, the code module loader
    return sorted((repo / 'dm/gfx/tools').glob('*.py')) + [repo / p for p in ('dm/gfx/tools/dmsh_chain.lock', 'dm/games/game.py', 'dm/ios/tools/ios_metal.py', 'dm/tools/romfs.py', 'dm/tools/nso.py')]


def python_tool_files(repo, entry, roots):   # transitive repository imports, including imports inside reader functions; unrelated tools are not inputs
    dirs, todo, seen = [repo / d for d in roots], [repo / entry], set()
    while todo:
        p = todo.pop()
        if p in seen or not p.is_file(): continue
        seen.add(p)
        for n in ast.walk(ast.parse(p.read_bytes())):
            mods = [a.name for a in n.names] if isinstance(n, ast.Import) else [n.module] if isinstance(n, ast.ImportFrom) and n.module else []
            todo += [d / f'{m.split(".")[0]}.py' for m in mods for d in dirs]
    return sorted(seen)


def chain_files(repo):   # what the iOS shaders run: not the split's readers (shader_split.py, unityfs.py, uepak.py)
    return python_tool_files(repo, 'dm/gfx/tools/dmsh_build.py', ('dm/gfx/tools', 'dm/ios/tools', 'dm/games')) + [repo / 'dm/gfx/tools/dmsh_chain.lock']


def split_profile_inputs(g):   # shader_split.py's GAME reads, plus the tables the packer checks or writes; renderer settings, notes and translation tables are not extraction inputs
    settings = [(name, g.get(name, '')) for name in ('modules', 'shader_groups', 'shader_note', 'shader_key_skip')]
    tables = [g.table(n) for n in ('shader_facts_manual.tsv', 'shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')]
    return [g.id, settings, ('files', tables)]


def split_output_key(pkg, g):   # detect deleted/edited packages and supplemental pairs before reusing a successful split
    return fingerprint(package_key(pkg), ('files', [pkg.with_name('pso_pairs.tsv')] if not g.table('pso_pairs.tsv').is_file() else []))


def package_key(pkg):   # the shader package's content (file names and bytes): what the iOS shaders are built from
    fs = [rel for rel, _ in files_under(pkg)]
    return fingerprint(fs, ('files', [pkg / f for f in fs]))


def profile_files(g):   # every file of the game profile (game.tsv and its tables) but the c1 / uniform extent tables, which the iOS shader step makes (chain_tables): no step reads them,
    out = {name for name, _, _ in CHAIN_TABLES} | {'patches.py'}   # and the rewriter's patches, which only the rewrite step reads (patch_files)
    return sorted(p for p in g.dir.iterdir() if p.is_file() and p.name not in out)


# ---- Steps ----
def check_capstone():   # patch_nso.py checks the same, but only at the rewrite step, after the extraction
    import capstone
    if capstone.cs_version()[0] != 5:   # the rewriter uses Capstone 5's names (CS_ARCH_ARM64, ARM64_OP_*), which Capstone 6 renamed
        cmd = shlex.join([sys.executable, '-m', 'pip', 'install', 'capstone>=5,<6'])
        raise BuildError(f'Capstone {capstone.__version__} is unsupported; the rewriter requires Capstone 5.x. Run: {cmd}')


def check_prereqs(env, xlate, mac=False):   # mac: a Mac-only build (PLATFORM 'mac'), which needs no xcodegen and compiles shaders only for macOS
    missing = [m for m in ('lz4.block', 'capstone', 'fontTools', 'zbic') if not _importable(m)] + ([] if _importable('compression.zstd') or _importable('backports.zstd') else ['backports.zstd'])   # zstd: built in from Python 3.14. zbic: a system 22.0.0+ module's segments, which only the rewrite reads, after the whole game is unpacked
    if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)} (pip install lz4 "capstone>=5,<6" fonttools zbic backports.zstd; backports.zstd only before Python 3.14)')
    check_capstone()
    tools = {'xcodebuild': 'Xcode', 'xcodegen': 'brew install xcodegen', 'zstd': 'brew install zstd', 'make': 'Xcode', 'glslangValidator': 'brew install glslang'}   # glslang: an OpenGL game's shaders, checked before its capture
    if mac: del tools['xcodegen']
    gone = [f'{t} ({how})' for t, how in tools.items() if not shutil.which(t, path=env['PATH'])]
    if gone: raise BuildError('tools not found: ' + ', '.join(gone))
    if not mac: check_ios_sdk(env)   # before the Metal check, whose message would blame the Metal toolchain
    mtl = subprocess.run(['xcrun', '-sdk', 'macosx' if mac else 'iphoneos', 'metal', '--version'], capture_output=True, text=True, env=env)   # Xcode 26 and later install the Metal toolchain separately
    if mtl.returncode: raise BuildError(f'the Metal compiler does not run ({(mtl.stderr.strip().splitlines() or ["no output"])[-1]}): install it with xcodebuild -downloadComponent MetalToolchain')
    if not xlate.exists(): raise BuildError(f'the shader translator is missing from the repository: {xlate}')
    check_translator(xlate, env)
    sc = env.get('SPIRV_CROSS', str(xlate.parent.parent / 'spirv_cross_bin/spirv-cross'))
    if not shutil.which(sc, path=env.get('PATH')): raise BuildError(f'spirv-cross is missing or not executable: {sc}')


def check_ios_sdk(env):   # the iOS platform is a separate download in Xcode's Components; xcodebuild, the Metal toolchain and the macOS SDK can be there without it
    if subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-version'], capture_output=True, text=True, env=env).returncode == 0: return
    dev = subprocess.run(['xcode-select', '-p'], capture_output=True, text=True, env=env).stdout.strip()
    if 'CommandLineTools' in dev: raise BuildError(f'the selected developer folder is the Command Line Tools ({dev}), which have no iOS SDK: select Xcode with sudo xcode-select -s /Applications/Xcode.app')
    raise BuildError('the iOS SDK is not installed: install the iOS platform in Xcode → Settings → Components, or with xcodebuild -downloadPlatform iOS')


def check_translator(xlate, env):
    """The shader translator is a committed native executable (dm/tools/native_tools.sh, universal): one short run of `--source-sha`, which translates nothing, shows before the extraction
    that it starts (not missing, executable, not held back by macOS), where dmsh_build.py would stop at the shader step (tool_lock.py xlate_embedded reports the same failure there)"""
    try: r = subprocess.run([str(xlate), '--source-sha'], capture_output=True, text=True, env=env, timeout=60, errors='replace')
    except (OSError, subprocess.SubprocessError) as e: raise BuildError(f'the shader translator does not start ({e}): {xlate}') from e
    if r.returncode == 0 and r.stdout.strip(): return
    why = (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or [f'exit code {r.returncode}'])[0]
    raise BuildError(f'the shader translator does not run ({why}): {xlate} is the committed native executable; check out the repository again if it is missing or damaged')


def check_native_python():
    # An x86_64 Python (an Intel Homebrew in /usr/local) runs under Rosetta on Apple silicon, and the tools it starts run as x86_64 too: make and clang build the Mac host, which runs
    # the game's own arm64 code, for x86_64, and every universal tool runs translated. sysctl.proc_translated is 1 under Rosetta, 0 or missing otherwise
    if sys.platform != 'darwin' or subprocess.run(['sysctl', '-n', 'sysctl.proc_translated'], capture_output=True, text=True).stdout.strip() != '1': return
    raise BuildError(f'this Python ({sys.executable}) is an Intel one running under Rosetta, and the tools it starts would run as Intel code too: run the script with an arm64 Python (/usr/bin/python3, or Homebrew\'s in /opt/homebrew)')


def _importable(m):
    try: __import__(m); return True
    except ImportError: return False


def extracted(log):   # (files, bytes) from the summary lines nx_extract wrote to log ("<dir>: N files X MB, ..." for ExeFS + RomFS; older logs have "| N files X MB"); None without them
    try: rows = re.findall(r'[:|] (\d+) files ([\d.]+) MB', log.read_text(errors='replace'))
    except OSError: return None
    return (sum(int(n) for n, _ in rows), int(sum(float(mb) for _, mb in rows) * 2**20)) if rows else None


def extract(nx, keys, ex, name, base, upd, cmp_dir, logs, g, app):   # the build IDs are checked by the caller (build_mismatch); app: the application read from the packages (--app)
    dst, marker, log = ex / name, g.get('romfs_marker'), logs / f'extract_{name}.log'
    try: reusable = (dst / 'ExeFS' / 'main').exists() and (dst / 'RomFS.img').is_file() and not (dst / 'RomFS').exists() and (rfs.source(dst) / marker).exists()
    except (OSError, ValueError, struct.error): reusable = False
    if reusable:
        PROG.stage(f'extract {name}')
        say(f'reusing the extracted {name}: {dst}')
        PROG.end('reused')
        return dst
    tmp = ex / (name + '.partial'); rmtree(tmp)
    last = extracted(log)   # the last extraction of this version (its log is about to be overwritten) has the exact size; without one, the NSPs' size is the estimate
    def poll():
        n, b = tree_size(tmp); return {'done': b, 'note': f'{n:,} files'}
    PROG.stage(f'extract {name}', poll=poll, unit='B', total=last[1] if last else base.stat().st_size + (upd.stat().st_size if upd and upd != base else 0), est=not last)
    run([nx, keys, base, upd or '-', tmp, '--romfs-image'] + extract_flags(app), log)
    swap_in(tmp, dst)
    n, b = extracted(log) or (0, 0)
    PROG.end(f'{n:,} files, {amount(b, "B")}')
    return dst


def extract_html(nx, keys, dst, base, upd, logs, app):
    """The offline HTML pages of the packed program (its HtmlDocument NCAs, which the web applet shows: the pause menu of Super Mario 3D All-Stars' games) → <dst>/html/html<n>/, beside the
    RomFS, where the host looks for them (hle_web.c). Reused; a program without any has an empty folder"""
    out, tmp = dst / 'html', dst / 'html.partial'
    if out.is_dir(): return
    rmtree(tmp); tmp.mkdir(parents=True)
    run([nx, keys, base, upd or '-', tmp, '--html'] + extract_flags(app), logs / f'extract_html_{dst.name}.log')
    swap_in(tmp, out)


def dump_folder(folder):   # a folder of the game's files (pack_dump.py), with no game package beside them: NSPs in the folder are what the player asked for
    return pack_dump.is_dump(folder) and not files_with_suffix(folder, *PACKAGE_SUFFIXES)


def nup_file(path):   # the NUP to pack: path when it is a .nup file, else the one .nup in the folder when no game package is beside it; None when there is none
    if path.is_file():
        if not pack_nup.is_nup(path): raise BuildError(f'not a folder or a NUP file: {path}')
        return path
    if files_with_suffix(path, *PACKAGE_SUFFIXES): return None
    nups = [p for p in files_with_suffix(path, '.nup') if pack_nup.is_nup(p)]
    if len(nups) > 1: raise BuildError(f'more than one NUP in {path}: {", ".join(p.name for p in nups)}; keep one game per folder, or give the .nup file to pack in place of the folder')
    return nups[0] if nups else None


def scan_nup(path, want=None):   # a NUP as scan_nsps gives a folder of NSPs, with its packages for the NSPs: (title ID, {'base', 'updates', 'dlc', 'nup': the open container})
    try:
        n = pack_nup.Nup(path)
        app, base, vers, dlc = pack_nup.scan(n)
    except (pack_nup.Error, OSError) as e: raise BuildError(str(e)) from e
    if want is not None and want != app: raise BuildError(f'no game {want:016X} (--title-id / TITLE_ID) in {path}, which holds {app:016X}')
    return app, {'base': base, 'updates': sorted(vers.items()), 'dlc': [(tid, None, k) for tid, k in sorted(dlc.items())], 'nup': n}


def extract_nup(ex, name, k, base, app, n):   # one version of a NUP (k: its base or an update package) into the layout extract makes; the code modules and the image are kept while unchanged
    dst = ex / name; part = dst / 'RomFS.img.part'
    def poll():
        try: return {'done': part.stat().st_size}
        except OSError: return None
    try:
        PROG.stage(f'unpack {name}', poll=poll, unit='B', total=n.tree_bytes(pack_nup.romfs_of(k, base)), est=True)
        how = pack_nup.pack_version(n, k, base, app, dst, say=lambda s: None, step=lambda s: PROG.set(step=s))
    except pack_nup.Error as e: raise BuildError(str(e)) from e
    PROG.end(amount((dst / 'RomFS.img').stat().st_size, 'B') + {'kept': ', reused (inputs unchanged)', 'verified': ', reused (image checked)'}.get(how, ''))
    return dst


def check_formats(dirs):
    """Each extracted image's files whose bytes do not start as their names promise (romfs_image.format_mismatches: a .zs file that is not a zstd stream) stop the build, as a damaged
    NSP does: a NUP or a dump folder carries no NCA hashes, so this is where its damage shows. ALLOW_DAMAGED lists them and goes on. A NUP of Ocarina of Time held three .zs
    textures beginning with 12,288 zero bytes; the game asked for one again on every pass of its background loader (412% CPU on the Mac; PR #1530). The result is kept beside
    the image (RomFS.img.formats) with its size and time, so an unchanged image is not read again (Ocarina of Time: 102,185 .zs files, 16 s from a USB SSD)"""
    import romfs_image
    for ver, d in dirs.items():
        img = d / 'RomFS.img'; stamp = d / 'RomFS.img.formats'
        st = img.stat(); ident = f'{st.st_size} {st.st_mtime_ns}'
        try: lines = stamp.read_text().splitlines()
        except OSError: lines = []
        if lines and lines[0] == ident: bad = [ln.split('\t') for ln in lines[1:]]
        else:
            PROG.add(1); PROG.stage(f'check {ver} files', step='reading file heads', unit='files')
            bad = [(p, str(size), head.hex()) for p, size, head in romfs_image.format_mismatches(img, lambda done, total: PROG.set(done=done, total=total))]
            stamp.write_text('\n'.join([ident] + ['\t'.join(b) for b in bad]) + '\n')
            PROG.end(f'{len(bad)} damaged' if bad else 'ok')
        if not bad: continue
        rows = '\n'.join(f'  {p} ({int(size):,} bytes, starts {head or "(nothing)"})' for p, size, head in bad[:20]) + (f'\n  … {len(bad) - 20} more' if len(bad) > 20 else '')
        msg = f'{ver}: {len(bad)} file(s) whose bytes do not start as their names promise (a .zs file that is not a zstd stream), so the game files are damaged:\n{rows}'
        if not ALLOW_DAMAGED: raise BuildError(f'{msg}\nPack again from a good copy of the game (--allow-damaged packs these files as they are)')
        say(f'[RED] {msg}\npacked as they are (--allow-damaged)')


def scan_dump(dump, want=None):   # a dump folder as scan_nsps gives a folder of NSPs, with folders for the packages: (title ID, {'base', 'updates', 'dlc', 'dump': True})
    try: app, vers, dlc = pack_dump.scan(dump)
    except pack_dump.Error as e: raise BuildError(str(e)) from e
    if want is not None and want != app: raise BuildError(f'no game {want:016X} (--title-id / TITLE_ID) in {dump}, which holds {app:016X}')
    return app, {'base': pack_dump.base_of(dump), 'updates': sorted(vers.items()), 'dlc': [(tid, None, roms) for tid, roms in dlc.items()], 'dump': True}


def extract_dump(ex, name, src, app):   # one version of a dump folder (src: base/ or updates/<version>/) into the layout extract makes; the code modules and the image are kept while unchanged
    dst = ex / name; part = dst / 'RomFS.img.part'
    def poll():
        try: return {'done': part.stat().st_size}
        except OSError: return None
    PROG.stage(f'pack {name}', poll=poll, unit='B', total=tree_size(src / 'romfs')[1], est=True)
    try: pack_dump.pack_version(src, app, dst, say=lambda s: None)
    except pack_dump.Error as e: raise BuildError(str(e)) from e
    PROG.end(amount((dst / 'RomFS.img').stat().st_size, 'B'))
    return dst


def extract_aoc(nx, keys, repo, work, app, fam, logs):
    """DLC NSPs in the folder → <id>_build/aoc/<index>.img (the RomFS of each one's data NCA; index = title ID − base − 0x1000, base the application's title ID even for one of its programs,
    app + N, whose DLC are the application's). The host reports these indices as owned and mounts them (DM_AOC_DIR).
    Returns None without DLC. Reused when the DLC NSPs (names, sizes and times) and nx_extract's sources are as at the last extraction (Monster Hunter Rise: 250 DLC, 30 s),
    and aoc/ holds the files that extraction wrote (names, sizes and times): a NUP or a dump folder packs its DLC into the same aoc/"""
    if not fam['dlc']: return None
    if fam.get('nup'):
        PROG.stage('unpack DLC', step='unpacking')
        try: pack_nup.pack_dlc(fam['nup'], app, {tid: k for tid, _, k in fam['dlc']}, work / 'aoc', say=lambda s: None)
        except pack_nup.Error as e: raise BuildError(str(e)) from e
        PROG.end(f'{len(fam["dlc"])} DLC')
        return work / 'aoc'
    if fam.get('dump'):
        PROG.stage('pack DLC', step='packing')
        try: pack_dump.pack_dlc(app, {tid: roms for tid, _, roms in fam['dlc']}, work / 'aoc', say=lambda s: None)
        except pack_dump.Error as e: raise BuildError(str(e)) from e
        PROG.end(f'{len(fam["dlc"])} DLC')
        return work / 'aoc'
    tmp, dst = work / 'aoc.partial', work / 'aoc'; rmtree(tmp)
    best = {}   # when one DLC comes in several copies, take the highest version, and the largest of equal versions
    for tid, ver, p in fam['dlc']:
        k = (ver or 0, p.stat().st_size)
        if tid not in best or k > best[tid][0]: best[tid] = (k, p)
    PROG.stage('extract DLC')
    kf = work / 'aoc.key'
    key = fingerprint(sorted((tid, p.name, p.stat().st_size, p.stat().st_mtime_ns) for tid, (_, p) in best.items()), ('files', [repo / 'dm/tools/nx_extract' / f for f in ('Program.cs', 'nx_extract.csproj')]))
    base = app & ~0xFFF   # a program (--program N, or one a launcher's build packs) is app + N: its DLC are numbered from the application, as the game asks for them
    if base != app: key = fingerprint(key, 'indices from the application')   # a program's aoc/ numbered from its own title ID before is made again
    if dst.is_dir() and (note := reused(kf, fingerprint(key, ('stat', dst)))) is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return dst
    rmtree(kf)
    for i, (tid, (_, p)) in enumerate(sorted(best.items())):
        idx = tid - base - 0x1000
        PROG.set(step='extracting', done=i, total=len(best), unit='DLC')
        say(f'DLC index {idx}: {p.name}')
        run([nx, keys, '--aoc', p, tmp / f'{idx}.img', '--romfs-image', f'{tid:016x}'], logs / f'aoc_{idx}.log')   # the DLC by its content meta's title ID: a merged package holds several
    swap_in(tmp, dst)
    kf.write_text(f'{fingerprint(key, ("stat", dst))}\n{len(best)} DLC\n')
    PROG.end(f'{len(best)} DLC')
    return dst


def build_mismatch(d, name, g):   # the modules of an extraction whose build ID is not the one the profile's build row of that version registers ([] = the registered build)
    want = g.builds().get(name)
    if want is None: return [f'{name} has no build row']
    return [f'{m} {build_id(d / "ExeFS" / m)} (registered {bid})' for m, bid in want.items() if m != 'display' and build_id(d / 'ExeFS' / m) != bid]


def patch_files(repo, exe_dir, mods, game_id=None):
    """The patch files that name one of the modules' builds (dm/tools/sdk_patches.py, dm/games/*/patches.py): patch_nso.py loads them all but applies only those of a module's
    build ID. The selected game's file also counts for unknown builds: it may supply MATCH_CUTS. Another game's rules do not change this game's rewrite. sdk_patches.py always counts:
    its MATCH_CUTS are SDK-wide and apply to builds it does not name, as engine_patches.py's (engine-wide) do. The patches.py of profiles in DM_GAMES_DIR (a provisional profile's) count too, as patch_nso.py loads them"""
    ids = {build_id(exe_dir / m) for m in mods}
    selected = Path(game_id).name if game_id else None
    extra = [p for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d for p in sorted(Path(d).glob('*/patches.py'))]
    shared = [repo / 'dm/tools/sdk_patches.py', repo / 'dm/tools/engine_patches.py']   # their MATCH_CUTS apply to every game's module of a kind
    files = {}
    for p in shared + sorted((repo / 'dm/games').glob('*/patches.py')) + extra: files.setdefault(p.resolve(), p)
    return [p for p in files.values()
            if p in shared or p.parent.name == selected or any(i in p.read_text(errors='replace') for i in ids)]


def collect_nros(py, repo, romfs, dst, env, logs):
    """The NROs a game registers in NRR files (RomFS *.nrr) and loads at run time with nn::ro, found by their SHA-256 as loose files or inside the RomFS's archives (dm/tools/nro_collect.py):
    {module name: NRO path}, {} for a game without NRRs. The host maps their rewritten copies at start and loads one when the game asks (loader.c "NRO modules"; Super Smash Bros. Ultimate:
    98 in data.arc). Reused while the NRRs, RomFS tree and collection tools are unchanged"""
    nrrs = sorted(p for p in romfs.rglob('*.nrr') if p.is_file())   # Super Smash Bros. Ultimate keeps them in a folder named .nrr
    kf = dst.with_name(dst.name + '.key')
    if not nrrs:
        rmtree(dst); rmtree(kf)   # an update can remove its NRRs: do not carry the previous manifest into the rewrite
        return {}
    key = fingerprint(('files', nrrs + [repo / f for f in ('dm/tools/nro_collect.py', 'dm/tools/nso_syms.py', 'dm/gfx/tools/smash_arc.py', 'dm/tools/romfs.py')]), ('tree', romfs))
    if not ((dst / 'nro.tsv').is_file() and reused(kf, key) is not None):
        tmp = dst.with_name(dst.name + '.partial'); rmtree(tmp)
        PROG.set(step=f'finding the NROs {len(nrrs)} NRR files register')
        run([py, repo / 'dm/tools/nro_collect.py', romfs, tmp, '--missing-ok'], logs / 'nro_collect.log', env=env)
        rmtree(kf); swap_in(tmp, dst); kf.write_text(f'{key}\n')
    # rows named 'refused' are online SDKs the host never loads (Epic Online Services): listed for the host's log, not rewritten
    return {l.split('\t')[0]: dst / (l.split('\t')[0] + '.nro') for l in (dst / 'nro.tsv').read_text().splitlines() if l and not l.startswith('#') and not l.startswith('refused\t')}


def nro_modules(g, exe_dir, nros):
    """modules of a provisional profile once the NROs are collected: the subsdks the NROs import from, which the survey could not see (dm/games/profile_guess.py nro_modules; Portal's movie
    module and GLSLC). Returns the profile, reloaded when it changed"""
    add = pg.nro_modules(exe_dir, nros, g.words('modules'))
    if not add: return g
    mods = sorted(set(g.words('modules')) | set(add), key=pg.load_order)
    why = ', '.join(f'{m} ({k})' for m, k in add.items())
    say(f'modules {" ".join(mods)}: the NROs the game loads import from {why}')
    return set_keys(g, {'modules': (' '.join(mods), f'the NROs the game loads at run time import from {why}, which main does not')}, 'After the NRO collection')


def rewrite(py, repo, exe_dir, out, env, logs, g, provisional=False):
    """patch_nso.py on every code module (each writes its own <module>.* files, all the same build_src.sha256 line), each with its log, as many at once as the Mac has cores.
    The modules are the profile's and the NROs the game registers (collect_nros; nro.tsv beside them lists those); a provisional profile first gets the subsdks those NROs import from (nro_modules).
    Reused when the modules, patch_nso.py (with nso.py, nso_code.py, nso_asm.py and the selected profile's function rules / the patch files of their builds), its switches and capstone are as at the
    last successful rewrite (Monster Hunter Rise's main alone takes 5.5 min). Returns the profile"""
    import capstone
    try: workers = int(env['DM_REWRITE_J']) if env.get('DM_REWRITE_J') else None
    except ValueError: raise BuildError('DM_REWRITE_J must be a positive integer') from None
    if workers is not None and workers < 1: raise BuildError('DM_REWRITE_J must be a positive integer')
    PROG.stage('rewrite', step='checking the inputs')
    nro_dir = out.with_name('nro_' + out.name.removeprefix('patched_'))
    nros = collect_nros(py, repo, rfs.source(exe_dir.parent), nro_dir, env, logs)
    if provisional and nros: g = nro_modules(g, exe_dir, nros)
    mods = g.modules_in(exe_dir)   # a version may lack a module of the profile another version has (game.py build_modules)
    manifest = nro_dir / 'nro.tsv'
    src = {m: exe_dir / m for m in mods} | nros
    names = list(src)
    kf = out.with_name(out.name + '.key')
    # the repository profile's build rows decide which builds the rewriter treats as the game's own (SDK-wide function patches skip those), and its snowdrop_pools the sizes of those cuts, so they are inputs too
    try: own = [l for l in (repo / 'dm/games' / Path(env.get('DM_GAME') or '-').name / 'game.tsv').read_text(encoding='utf-8').splitlines() if l.startswith(('build\t', 'snowdrop_pools\t'))]
    except OSError: own = []
    key = fingerprint(('files', [repo / 'dm/tools' / f for f in ('patch_nso.py', 'nso.py', 'nso_code.py', 'nso_asm.py', 'nso_match.py', 'ue_cvar.py', 'ue_malloc.py', 'ue_region.py', 'ue_ring.py', 'snowdrop_pools.py', 'loader_census.py', 'fps_target.py', 'ue_fixed_rate.py', 'unity_fps.py', 'unity_bucket.py')] + patch_files(repo, exe_dir, mods, env.get('DM_GAME')) + list(src.values()) + ([manifest] if manifest.is_file() else [])), names, capstone.__version__,
                      {k: env.get(k) for k in ('DM_PATCH_FRONT', 'DM_GAME')}, own)
    if all((out / f'{m}.patched.bin').is_file() for m in names) and reused(kf, key) is not None:
        PROG.end(f'{", ".join(mods)}{f" and {len(nros)} NROs" if nros else ""}, reused (inputs unchanged)')
        return capture_nro(py, repo, exe_dir, out, nro_dir, env, logs, g, provisional)
    tmp = out.with_name(out.name + '.partial'); rmtree(tmp); tmp.mkdir(parents=True); logs.mkdir(parents=True, exist_ok=True)
    if manifest.is_file(): shutil.copyfile(manifest, tmp / 'nro.tsv')   # refused-only manifests preserve the game's offline registration path too
    PROG.set(step=', '.join(mods) + (f' and {len(nros)} NROs' if nros else ''), done=0, total=len(names), unit='modules')
    procs, logf, todo, cores = {}, {}, list(names), max(2, os.cpu_count() or 4)
    jobs = max(1, min(cores, 2 * cores // max(1, len(names))))   # patch_nso.py workers per module, so parallel rewrites don't oversubscribe
    rewrite_env = dict(env, DM_REWRITE_J=str(workers if workers is not None else jobs))
    try:
        while todo or procs:
            while todo and len(procs) < cores:
                m = todo.pop(0); logf[m] = open(logs / f'rewrite_{m}.log', 'w')
                procs[m] = subprocess.Popen([str(py), str(repo / 'dm/tools/patch_nso.py'), str(src[m]), str(tmp), m], env=rewrite_env, stdout=logf[m], stderr=subprocess.STDOUT)
            for m in [m for m in procs if procs[m].poll() is not None]:
                p = procs.pop(m); logf[m].close(); PROG.set(done=len(names) - len(todo) - len(procs))
                if p.returncode:
                    log = logs / f'rewrite_{m}.log'
                    PROG.end(ok=False)
                    PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=40)).rstrip('\n'))
                    raise BuildError(f'patch_nso.py exit code {p.returncode} on {m} (full output: {log})')
            time.sleep(0.2)
    finally:   # an error or an interrupt stops the other rewrites
        for p in procs.values():
            if p.poll() is None: p.terminate(); p.wait()
        for f in logf.values(): f.close()
    rmtree(kf); swap_in(tmp, out); kf.write_text(f'{key}\n')
    PROG.end(', '.join(mods) + (f' and {len(nros)} NROs' if nros else ''))
    return capture_nro(py, repo, exe_dir, out, nro_dir, env, logs, g, provisional)


NRO_CAPTURE_SECS = 120   # Mario Tennis Fever's boot stub decodes its 24.7 MB NRO in under a second, with a cold file cache far less than this


def capture_nro(py, repo, exe_dir, out, nro_dir, env, logs, g, provisional):
    """An NRO its NRR registers and no file holds (nro_collect.py's missing.tsv): the game decodes it at run time, so boot the rewritten modules with DM_NRO_CAPTURE (dm/host/hle_ro.c), which writes the image
    of the first LoadModule the host has no rewritten copy of, by its SHA-256, and ends the run. That image joins the collected NROs, kept for the next build as they are, and the rewrite runs again with it.
    Mario Tennis Fever: main is a boot stub, and the game's code is one NRO in main's data segment. Returns the profile"""
    missing = nro_dir / 'missing.tsv'
    if not missing.is_file(): return g
    want = dict(l.split('\t') for l in missing.read_text().splitlines() if l)
    if not want: return g
    PROG.add(2); PROG.stage('NRO capture', step='building the Mac host')   # and the rewrite again
    work = out.parent
    host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
    cap, rd, log = nro_dir / 'capture.partial', work / 'tmp/nro_run', logs / 'nro_capture.log'
    rmtree(cap); rmtree(rd); cap.mkdir(parents=True)
    for d in ('save', 'temp'): (rd / d).mkdir(parents=True)
    PROG.set(step='booting the game')
    say(f'{len(want)} registered NRO(s) in no file: booting the game in the Mac host to capture what it decodes at run time')
    henv = dict(env, DM_ROMFS=str(rfs.source(exe_dir.parent)), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), DM_NRO_CAPTURE=str(cap))
    boot_mac(host, out, henv, rd, log, NRO_CAPTURE_SECS, lambda: any(cap.glob('*.nro')))
    got = sorted(cap.glob('*.nro'))
    if not got or got[0].stem not in want:
        PROG.end(ok=False)
        with open(log, errors='replace') as f: tail = ''.join(collections.deque(f, maxlen=40)).rstrip('\n')
        PROG.say(f'---- last lines of {log} ----\n' + tail)
        raise BuildError(f'the game loaded none of the {len(want)} NRO(s) its NRR registers and no file holds, first {next(iter(want))} (host output: {log})')
    sha = got[0].stem; manifest = nro_dir / 'nro.tsv'
    rows = [l for l in manifest.read_text().splitlines() if l]
    name = f'nro{sum(1 for l in rows if not l.startswith(("#", "refused")))}'
    shutil.move(got[0], nro_dir / f'{name}.nro')
    manifest.write_text('\n'.join(rows + [f'{name}\t{sha}\t{(nro_dir / f"{name}.nro").stat().st_size}\t{want.pop(sha)}\tcapture']) + '\n')
    missing.write_text(''.join(f'{h}\t{n}\n' for h, n in want.items()))
    rmtree(cap); rmtree(rd)
    PROG.end(f'{name}, {(nro_dir / f"{name}.nro").stat().st_size / 2**20:.1f} MB')
    return rewrite(py, repo, exe_dir, out, env, logs, g, provisional)


def captured_nros(patched):   # the folder of the NROs of a build that captured some (capture_nro): the shader split reads them, as it reads main, and the file table of Camelot's archive is in one
    d = patched.with_name('nro_' + patched.name.removeprefix('patched_'))
    try: return d if any(l.endswith('\tcapture') for l in (d / 'nro.tsv').read_text().splitlines()) else None
    except OSError: return None


GLSLC_SECS = 30   # the longest capture run: Three Houses compiles its 9 shaders within 1.2 s of boot, and the run stops as soon as every file the profile names is there
GLSLC_KEYS = '5:A*7/4'   # A every 4 s from 5 s on during the capture run (DM_KEYS): Crash Team Racing Nitro-Fueled compiles its two run-time programs only once its title screen has had a
# button press (0 in 200 s without input, both 3 s after the first A); the run keeps its own save and temporary storage, so whatever the presses choose is thrown away with them


def glslc_wanted(g):   # the capture files the profile's shader map was built from (column 5: /glslc_capture/<name>#GLSLC@…); None when the map names no captures
    try: t = g.table('shader_map.tsv').read_text(errors='replace')
    except OSError: return None
    return set(re.findall(r'/glslc_capture/([0-9a-f]{16}\.glslc)#', t)) or None


def glslc_done(d):   # capture files written whole: a GLSLC output (magic 0x19866891) holds its own length at +0x44
    out = set()
    try: names = [n for n in os.listdir(d) if n.endswith('.glslc')]
    except OSError: return out
    for n in names:
        try: b = (d / n).read_bytes()
        except OSError: continue
        if len(b) >= 0x48 and b[:4] == b'\x91\x68\x86\x19' and struct.unpack_from('<I', b, 0x44)[0] == len(b): out.add(n)
    return out


def apple_silicon():   # hw.optional.arm64 is 1 on Apple silicon (for a Python under Rosetta too); an Intel Mac has no such key
    return subprocess.run(['sysctl', '-n', 'hw.optional.arm64'], capture_output=True, text=True).stdout.strip() == '1'


HOST_DIRS = ('dm/host', 'dm/gfx', 'dm/gfx/nvn', 'dm/gfx/nvn/include', 'dm/gfx/tex', 'dm/gfx/vk', 'dm/gfx/gl')   # the folders the host's compile line takes its sources and headers from (dm/host/Makefile SRC, GFX, GFX_H)


def decoder_host_key(host):
    """A managed host's decoder build identity, only while its recorded executable bytes still match. Unknown/replaced hosts use their binary identity instead."""
    host = Path(host)
    try: record = json.loads(host.with_name(host.name + '.decoder.json').read_text())
    except (OSError, ValueError): return None
    if (isinstance(record, dict) and record.get('format') == 1 and isinstance(record.get('inputs'), str)
            and re.fullmatch(r'[0-9a-f]{64}', record['inputs']) and host.is_file() and record.get('binary') == fingerprint(('files', [host]))):
        return record['inputs']
    return None


def build_mac_host(repo, out, env, logs, g):
    """The Mac host for the profile, built into out: dm/host's own compile line (make -n) with the output moved, since make takes no target path with spaces and dm/host/dmhost may be another build's.
    Reused when the compile line and its inputs (the source files of HOST_DIRS, the chain header dm/gfx/nvn/dmsh_chain.h among them, the profile header make generates, libvpx, dav1d and the third-party headers, the compiler
    and the macOS SDK) are as at the last build into out. The host runs the game's own arm64 code natively, so an Intel Mac stops here (GitHub issue #54)"""
    if not apple_silicon(): raise BuildError('this step boots the game in the Mac host, which runs the game\'s own arm64 code natively: it needs a Mac with Apple silicon, and this Mac has an Intel processor')
    host = repo / 'dm/host'
    run(['make', '-s', f'GAME={g.id}', f'gen/{g.id}/game_profile.h', 'gen/vpx/libvpx.a', 'gen/dav1d/libdav1d.a', 'host_msl'], logs / 'mac_host_make.log', env=env, cwd=host)
    dry = subprocess.run(['make', '-n', '-B', f'GAME={g.id}', 'dmhost'], env=env, cwd=host, capture_output=True, text=True)
    line = next((l for l in dry.stdout.splitlines() if ' -o dmhost ' in l), None)
    if not line: raise BuildError(f'make -n gave no compile line for the Mac host: {dry.stderr.strip()[-500:]}')
    cmd = shlex.split(line); cmd[cmd.index('-o') + 1] = out
    kf, tp = out.with_name(out.name + '.key'), repo / 'dm/third_party'
    srcs = [p for d in HOST_DIRS for p in sorted((repo / d).iterdir()) if p.is_file() and p.suffix in ('.c', '.m', '.h', '.S', '.inc')]
    common = fingerprint(cmd, ('files', [host / f'gen/{g.id}/game_profile.h', host / 'gen/msl-macosx/host_msl_gen.h', host / 'gen/vpx/libvpx.a', host / 'gen/dav1d/libdav1d.a', tp / 'stb_vorbis/stb_vorbis.c']),
                         ('tree', tp / 'vulkan-headers/include'), ('tree', tp / 'libvpx/vpx'), ('tree', tp / 'dav1d/include'), ('cmd', ['clang', '--version']), ('cmd', ['xcrun', '--show-sdk-build-version']), env=env)
    key = fingerprint(common, ('files', srcs))
    chain = repo / CHAIN_H
    # The decoder serves game bytes before rendering. DMSH_CHAIN selects translated shader libraries and appears in the startup log; it cannot change decompression.
    # Retain every other host build input, including any other header content. Bind the identity to the built binary so a replaced/custom host cannot inherit it.
    chain_text = re.sub(r'(?m)^#define DMSH_CHAIN "[^"]*"$', '#define DMSH_CHAIN "<chain>"', chain.read_text()) if chain.is_file() else None
    decoder = fingerprint('oodle-host-v1', common, ('files', [p for p in srcs if p != chain]), chain_text)
    if out.is_file() and reused(kf, key) is not None and decoder_host_key(out) == decoder: return out
    rmtree(kf); out.parent.mkdir(parents=True, exist_ok=True)
    run(cmd, logs / 'mac_host_build.log', env=env, cwd=host)
    record = out.with_name(out.name + '.decoder.json')
    part = record.with_suffix('.json.partial')
    part.write_text(json.dumps({'format': 1, 'binary': fingerprint(('files', [out])), 'inputs': decoder}) + '\n')
    os.replace(part, record)
    kf.write_text(f'{key}\n')
    return out


def boot_mac(host, patched, henv, rd, log, secs, done=lambda: False):
    """Boot the game in the Mac host (headless and silent: no DM_WINDOW, no DM_AUDIO) with its output in log, until done() says so, the host exits, or secs pass;
    then SIGTERM (the host's graceful exit), a kill 5 s later. Returns the host's exit code, None when it was still running"""
    t0 = time.time()
    with open(log, 'w') as f:
        p = subprocess.Popen([str(host), str(patched)], env=henv, cwd=rd, stdout=f, stderr=subprocess.STDOUT)
        try:
            while not done() and p.poll() is None and time.time() - t0 < secs: time.sleep(0.25)
        finally:
            rc = p.poll()
            if rc is None:
                p.terminate()
                try: p.wait(5)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
    return rc


def host_stopped(log, rc, secs):   # a capture run whose host stopped by itself before the capture was complete: the game crashed or gave up, and the IPA will too (a provisional profile's IPA is still built)
    with open(log, errors='replace') as f: tail = ''.join(collections.deque(f, maxlen=12)).rstrip('\n')
    PROG.say(f'[RED] the game stopped after {secs:.0f} s in the Mac host (exit {rc}): its shaders are incomplete and the IPA will not get further; ---- last lines of {log} ----\n'
             + tail)


def capture_glslc(repo, dirs, pkg_ver, patched, work, env, logs, g):
    """A game that compiles some shaders at run time with its own GLSLC (Fire Emblem: Three Houses; profile_guess.py shader_sources), so no game file holds them. The game boots in the Mac host
    (headless and silent) with DM_GLSLC_CAPTURE (dm/host/hle_glslc.c), which writes each output to <version dir>/glslc_capture, where shader_split.py reads them; the outputs are the same on every run.
    dirs: {shader version: its extraction}; a folder that already holds every file the profile's shader map names is reused.
    The source/import heuristic can also detect a compiler used only on cache misses. With no output captured in a surviving boot, return the submitted shader keys for
    verification against the offline split (check_glslc_coverage). Known capture files remain required. This proves coverage of the observed boot only, not later gameplay"""
    want = glslc_wanted(g)
    todo = [v for v, d in dirs.items() if not (glslc_done(d / 'glslc_capture') >= want if want else glslc_done(d / 'glslc_capture'))]
    PROG.stage('GLSLC capture')
    if not todo:
        say(f'reusing the captured run-time shaders: {", ".join(str(d / "glslc_capture") for d in dirs.values())}')
        PROG.end('reused'); return
    if todo != [pkg_ver]: raise BuildError(f'the run-time shader capture runs the packed version {pkg_ver}\'s code; {", ".join(v for v in todo if v != pkg_ver)} needs a capture of its own')
    PROG.set(step='building the Mac host')
    host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
    d = dirs[pkg_ver]; cap, rd, log = d / 'glslc_capture.partial', work / 'tmp/glslc_run', logs / 'glslc_capture.log'
    rmtree(cap); rmtree(rd)
    for s in ('save', 'temp'): (rd / s).mkdir(parents=True)
    # Only builds with no required captures can prove offline coverage. An empty map exposes submitted keys even if a previous split knows them.
    shmap = g.table('shader_map.tsv')
    if not want:
        shmap = rd / 'shader_map.tsv'
        shmap.write_text('# GLSLC capture: observe submitted shaders\n')
    # everything the run writes stays in <id>_build (DM_ROOT is already there): a fresh save and temporary storage, no shader libraries yet, the observation map (the host looks beside itself by default)
    henv = dict(env, DM_ROMFS=str(rfs.source(d)), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), DM_NVN_SHMAP=str(shmap), DM_GLSLC_CAPTURE=str(cap), DM_KEYS=GLSLC_KEYS)
    PROG.set(step='booting the game', done=0, total=len(want) if want else None, unit='shaders')
    say(f'booting the game in the Mac host to capture the shaders it compiles at run time → {d / "glslc_capture"}')
    t0 = time.time()
    def done():   # every file the profile names is there (Three Houses: within its first frames)
        got = glslc_done(cap); PROG.set(done=len(got & want if want else got))
        return bool(want and got >= want)
    rc = boot_mac(host, patched, henv, rd, log, GLSLC_SECS, done)
    got = glslc_done(cap)
    if rc is not None and not (want and got >= want): host_stopped(log, rc, time.time() - t0)
    if not want and not got and rc is None and not any(cap.glob('*.glslc')):
        text = log.read_text(errors='replace')
        observed = set(re.findall(r'\[RED\] shader ([0-9a-f]{16}) is not in the shader map', text))
        if observed and '[RED] glslc:' not in text:
            rmtree(cap); rmtree(rd)
            PROG.end(f'no run-time outputs captured, {len(observed)} submitted shader keys to check against the offline split')
            say('the GLSL/import heuristic can also select a compiler used only on cache misses. The build continues only if the offline split covers every shader observed in this boot. Later gameplay is unverified')
            return observed
    if not got or (want and not got >= want):
        PROG.end(ok=False)
        with open(log, errors='replace') as f: tail = ''.join(collections.deque(f, maxlen=40)).rstrip('\n')
        PROG.say(f'---- last lines of {log} ----\n' + tail)
        raise BuildError(f'captured only {len(got)} run-time shaders'
                         + (f', the profile needs {len(want)}' if want else '') + f' (host output: {log})')
    if want and got - want: say(f'[RED] captured files the profile\'s shader map does not name: {", ".join(sorted(got - want))}')
    swap_in(cap, d / 'glslc_capture'); rmtree(rd)
    PROG.end(f'{len(got)} shaders in {hms(time.time() - t0)}')


def check_glslc_coverage(observed, g):
    """Require the offline split to supply every shader observed during a surviving boot with no runtime outputs."""
    if not observed: raise BuildError('no submitted shader keys to validate after the empty GLSLC capture')
    missing = observed - shader_keys(g)
    if missing: raise BuildError(f'no run-time GLSLC outputs and {len(missing)} observed shaders absent from the offline split: {", ".join(sorted(missing))}')
    say(f'the offline split covers all {len(observed)} shaders observed during the GLSLC capture boot (later gameplay unverified)')


UE_DECODE_SECS = 300   # the longest Oodle decode run: Octopath Traveler 0's 10,596 shaders take about 10 s, the boot up to nnMain included


def ue_stamp(d):   # what an extraction's decoded shader archives were made from: its paks and IoStore TOCs (paths and sizes)
    return ''.join(f'{p.relative_to(rfs.source(d))}\t{p.stat().st_size}\n' for p in sorted([*(rfs.source(d)).rglob('*.pak'), *(rfs.source(d)).rglob('*.utoc')]))


def decode_ue_shaders(repo, dirs, pkg_ver, patched, work, env, logs, g):
    """A game with Unreal paks (Octopath Traveler 0): Unreal Engine keeps its shaders in the shader code archives of its paks, each shader Oodle-compressed, and only the game's own Oodle
    decodes them. dm/gfx/tools/uepak.py takes the archives out of the paks (the pak index is AES-encrypted with a key from main) and writes one Oodle job; the game's code decodes it in the Mac host
    (DM_OODLE_JOB, dm/host/hle_oodle.c: right before nnMain, then the host exits), and the decoded archives go to <version dir>/ue_shaders, where shader_split.py reads them.
    The job runs the packed version's code (Oodle's format does not change between builds). Unreal Engine 4.26 and later compress shaders with LZ4 by default (DRAGON QUEST VII Reimagined),
    which uepak.py decodes itself, so such a game needs no host run here. Unreal Engine 5.6 (FINAL FANTASY RESONANCE) keeps its shader libraries in the IoStore container instead, two layers
    of Oodle (container blocks, then each shader group): uepak.py reads them through the game's Oodle in the host's tool mode (uepak.OodleHost), with no boot. A folder whose stamp matches its paks is reused"""
    sys.path.insert(0, str(repo / 'dm/gfx/tools')); import uepak
    todo = {v: d for v, d in dirs.items() if not ((d / 'ue_shaders/stamp').exists() and (d / 'ue_shaders/stamp').read_text() == ue_stamp(d))}
    PROG.stage('UE shader decode')
    if not todo:
        say(f'reusing the decoded shader archives: {", ".join(str(d / "ue_shaders") for d in dirs.values())}')
        PROG.end('reused'); return
    t0, total, host = time.time(), 0, None
    for v, d in todo.items():
        jd, rd, log = work / f'tmp/ue_job_{v}', work / 'tmp/ue_run', logs / f'ue_decode_{v}.log'
        rmtree(jd); rmtree(rd)
        for s in ('save', 'temp'): (rd / s).mkdir(parents=True)
        dec = None
        if uepak.io_shader_tocs(str(rfs.source(d))):
            if not host:
                PROG.set(step='building the Mac host')
                host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
            dec = uepak.OodleHost(host, patched, env)
        PROG.set(step=f'{v}: shader archives out of the paks' + (' and IoStore containers' if dec else ''))
        try: listing, n = uepak.job(str(rfs.source(d)), str(d / 'ExeFS/main'), str(jd), decode=dec)
        except ValueError as e: raise BuildError(str(e)) from e   # a damaged pak, archive or main (uepak's errors name it)
        finally:
            if dec: dec.close()
        if not listing: raise BuildError(f'the RomFS has Unreal paks, but none under {rfs.source(d)} holds a shader code archive: where the shaders are has to be found first')
        say(f'{v}: ' + '; '.join(f'{x["name"]} {x["shaders"]:,} shaders' + (f' ({x["compression"]})' if x.get('compression') else '') for x in listing)
            + (f'; decoding {n:,} with the game\'s Oodle in the Mac host' if n else ''))
        if n:   # Oodle-compressed shaders (Octopath Traveler 0); LZ4 ones (Unreal Engine 4.26 and later by default) are decoded by uepak.py already
            if not host:
                PROG.set(step='building the Mac host')
                host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
            PROG.set(step=f'{v}: decoding {n:,} shaders in the Mac host')
            henv = dict(env, DM_ROMFS=str(rfs.source(dirs[pkg_ver])), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), DM_OODLE_JOB=str(jd / 'oodle.job'))
            rc = boot_mac(host, patched, henv, rd, log, UE_DECODE_SECS)
            if rc != 0 or not (jd / 'oodle.job.out').exists():
                PROG.end(ok=False)
                PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=20)).rstrip('\n'))
                raise BuildError(f'the Oodle decode run stopped with exit {rc} (host output: {log})')
        out = d / 'ue_shaders.partial'; rmtree(out)
        uepak.finish(str(jd), str(out)); (out / 'stamp').write_text(ue_stamp(d))
        swap_in(out, d / 'ue_shaders'); rmtree(jd); rmtree(rd); total += n + sum(x['shaders'] for x in listing if x['compression'] == 'IoStore, decoded')
    PROG.end(f'{total:,} shaders in {hms(time.time() - t0)}')


CAPTURE_SECS = 120   # the longest run-time shader capture: Super Mario Galaxy creates its 3,089 Vulkan modules within 6 s of its first one (about 15 s after boot on an M4 Pro)
# Run-time shader captures, by graphics API: the host's switch, the profile's table of the keys a capture must hold, and a capture file's name → its key (None: not a whole file;
# the host writes each under a temporary name and renames it). Vulkan: <16 hex>.spv (dm/gfx/vk/hle_vk.m); GL: <16 hex>.vert / .frag (dm/gfx/gl/hle_gl.m sh_capture)
CAPTURES = {'vk': dict(name='Vulkan', env='DM_VK_CAPTURE', table='vk_capture.tsv', what='SPIR-V shaders it generates', key=lambda n: n[:-4] if n.endswith('.spv') and len(n) == 20 else None),
            'gl': dict(name='GL', env='DM_GL_CAPTURE', table='gl_capture.tsv', what='GLSL shaders it puts together',
                       key=lambda n: n if re.fullmatch(r'[0-9a-f]{16}\.(vert|frag)', n) else None)}


def capture_wanted(g, api):   # the keys the profile's capture table lists; None without the table (a provisional profile)
    try: keys = {l.strip() for l in g.table(CAPTURES[api]['table']).read_text().splitlines() if l.strip() and not l.startswith('#')}
    except OSError: return None
    return keys or None


def capture_done(d, api):   # keys of the whole files in a capture folder
    try: names = os.listdir(d)
    except OSError: return set()
    return {k for k in map(CAPTURES[api]['key'], names) if k}


JIT_ROUTE_SECS = 130   # how long the JIT capture's route (the profile's jit_route) runs
JIT_SECS = 90   # the longest JIT boot capture: Super Mario 64 asks for its 14.2 MB of boot code 3 s after start and for its first run-time blocks within 10 s


def uses_jit(exefs):   # main imports nn::jit (Super Mario 64 of Super Mario 3D All-Stars: its N64 code through the game's JIT plugin, dm/host/hle_jit.c)
    if not (exefs / 'main').is_file(): return False
    return any(n.startswith('_ZN2nn3jit20CreateJitEnvironment') and not defined for n, defined, _ in pg.nso_syms.dynsyms(pg.nso(exefs / 'main')[0]))


def jit_stamp(patched, g):   # what captures depend on: the size of every rewritten module (the image's layout) and the profile's request keys
    sizes = sorted((f.name, f.stat().st_size) for f in Path(patched).glob('*.patched.bin'))
    return json.dumps({'modules': sizes, 'jit_key_data': g.get('jit_key_data', ''), 'jit_out_code': g.get('jit_out_code', ''), 'jit_route': g.get('jit_route', '')})


def jit_boot_done(part):
    """boot_mac's done() for a JIT capture into the folder part: the boot code (the largest request) and nothing new for 10 s"""
    last = {'n': 0, 't': time.time()}
    def done():
        fs = list(part.glob('*.jit')); n = len(fs)
        if n != last['n']: last['n'], last['t'] = n, time.time()
        return bool(fs and max(f.stat().st_size for f in fs) > 1 << 20 and time.time() - last['t'] > 10)
    return done


def capture_jit(py, repo, d, patched, work, env, logs, g):
    """A game that generates code at run time through nn::jit (uses_jit): the iPhone runs no such code, so it serves the plugin's output from an image built here (dm/tools/jit_capture.py,
    hle_jit.c). The game boots twice in the Mac host (headless and silent) with DM_JIT_CAPTURE, each run into a folder of its own under <version dir>/jit_capture (boot1, boot2): two runs in
    different layouts tell which output words are host addresses. Every other folder there is added too: the captures of play on the Mac (the Mac run command writes them to
    jit_capture/play), since some code is made only when the game reaches it, and a request the image lacks stops the iPhone there. A capture folder made with other rewritten modules or
    request keys is cleared (jit_capture/stamp.json). The image goes beside the RomFS (<version dir>/jit), where build_ipa.sh takes it"""
    PROG.stage('JIT capture')
    caps, out = d / 'jit_capture', d / 'jit'
    stamp = jit_stamp(patched, g); sf = caps / 'stamp.json'
    if caps.exists() and (not sf.exists() or sf.read_text() != stamp):
        say(f'[RED] {caps}: made with other rewritten modules or request keys; cleared (play captures included: play the game on the Mac again for them)'); rmtree(caps)
    caps.mkdir(parents=True, exist_ok=True); sf.write_text(stamp)
    host = None
    route = g.get('jit_route', '')   # a third run plays this DM_KEYS route from a fresh save: what the game compiles past its boot (Super Mario 64: its file select's overlay blocks); a request the image lacks stops the iPhone
    for name in ('boot1', 'boot2') + (('route',) if route else ()):
        c = caps / name
        if any(c.glob('*.jit')): continue
        if host is None: PROG.set(step='building the Mac host'); host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
        part, rd, log = caps / f'{name}.partial', work / 'tmp/jit_run', logs / f'jit_{name}.log'
        rmtree(part); rmtree(rd); part.mkdir(parents=True)
        for x in ('save', 'temp', 'msl'): (rd / x).mkdir(parents=True)
        henv = dict(env, DM_ROMFS=str(rfs.source(d)), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(d.parent.parent / 'shader'), DM_JIT_CAPTURE=str(part))
        if name == 'route': henv['DM_KEYS'] = route
        PROG.set(step=f'booting the game ({name})')
        say(f'booting the game in the Mac host to capture the code its JIT plugin makes ({name}) → {c}')
        t0 = time.time(); rc = boot_mac(host, patched, henv, rd, log, *((JIT_ROUTE_SECS, lambda: False) if name == 'route' else (JIT_SECS, jit_boot_done(part))))
        if not any(f.stat().st_size > 1 << 20 for f in part.glob('*.jit')):
            if rc is not None: host_stopped(log, rc, time.time() - t0)
            raise BuildError(f'the JIT capture ({name}) holds no boot code (host output: {log})')
        swap_in(part, c); rmtree(rd)
    PROG.set(step='image')
    tmp = d / 'jit.partial'; rmtree(tmp)
    run([py, repo / 'dm/tools/jit_capture.py', '--profile', g.id] + sorted(str(x) for x in caps.iterdir() if x.is_dir() and not x.name.endswith('.partial')) + [tmp], logs / 'jit_capture.log', env=env)
    swap_in(tmp, out)
    PROG.end((logs / 'jit_capture.log').read_text(errors='replace').strip().splitlines()[-1][:120])
    return out


def capture_shaders(repo, d, patched, work, env, logs, g, api):
    """Shaders no game file holds, made at run time: a Vulkan game without SPIR-V files (Super Mario Galaxy: Hagi's GX shaders, from its pipeline database) or an OpenGL game whose GLSL is put
    together at run time (Balatro: LÖVE adds its header and its built-in code to each of the game's shader files, in Lua). The game boots in the Mac host (headless and silent, no shader
    library) with DM_VK_CAPTURE (dm/gfx/vk/hle_vk.m) or DM_GL_CAPTURE (dm/gfx/gl/hle_gl.m), which writes each shader to <version dir>/vk_capture or gl_capture, where vk_shaders.py /
    gl_shaders.py read them with the RomFS; the shaders are the same on every run. The run stops once every key the profile's capture table lists is there; without the table, once none has
    been added for 20 s. A folder that already holds every listed key is reused"""
    C = CAPTURES[api]; want, out = capture_wanted(g, api), d / f'{api}_capture'
    PROG.stage(f'{C["name"]} shader capture')
    if want and capture_done(out, api) >= want:
        say(f'reusing the captured run-time shaders: {out}'); PROG.end('reused'); return out
    PROG.set(step='building the Mac host')
    host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
    cap, rd, log = d / f'{api}_capture.partial', work / f'tmp/{api}_run', logs / f'{api}_capture.log'
    rmtree(cap); rmtree(rd); cap.mkdir(parents=True)
    for x in ('save', 'temp', 'msl'): (rd / x).mkdir(parents=True)
    # everything the run writes stays in <id>_build (DM_ROOT is already there): a fresh save and temporary storage, an empty shader library folder (pipelines fail, the game still creates every shader)
    henv = dict(env, DM_ROMFS=str(rfs.source(d)), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), **{C['env']: str(cap)})
    PROG.set(step='booting the game', done=0, total=len(want) if want else None, unit='shaders')
    say(f'booting the game in the Mac host to capture the {C["what"]} at run time → {out}')
    t0 = time.time(); last = {'n': 0, 't': t0}
    def done():   # every listed key is there; without the table, none has been added for 20 s
        got = capture_done(cap, api); n = len(got & want if want else got); PROG.set(done=n)
        if n != last['n']: last['n'], last['t'] = n, time.time()
        return bool((want and got >= want) or (not want and n and time.time() - last['t'] > 20))
    rc = boot_mac(host, patched, henv, rd, log, CAPTURE_SECS, done)
    got = capture_done(cap, api)
    if rc is not None and not (want and got >= want): host_stopped(log, rc, time.time() - t0)
    if not got or (want and not got >= want):
        PROG.end(ok=False)
        PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=40)).rstrip('\n'))
        raise BuildError(f'captured only {len(got)} run-time shaders'
                         + (f', the profile needs {len(want)}' if want else '') + f' (host output: {log})')
    if want and got - want: say(f'[RED] captured shaders the profile\'s {C["table"]} does not list: {len(got - want)}')
    swap_in(cap, out); rmtree(rd)
    PROG.end(f'{len(got)} shaders in {hms(time.time() - t0)}')
    return out


# ---- A provisional profile after the rewrite: mem_used and the heap check ----
def atomic_text(path, text):
    f = tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, prefix=f'.{path.name}.', delete=False)
    try:
        with f: f.write(text)
        os.replace(f.name, path)
    finally: Path(f.name).unlink(missing_ok=True)


def set_keys(g, changes, section):
    """Write keys into a provisional game.tsv, each under its reason, in a section of their own at the end; earlier rows of the same keys (and the reason line right above one) go,
    and a key whose value is None only goes. Returns the profile reloaded"""
    p = g.dir / 'game.tsv'; old = p.read_text(encoding='utf-8').splitlines(); out = []
    for line in old:
        if line.split('\t', 1)[0] in changes:
            if out and out[-1].startswith('# ') and not out[-1].startswith('# ----'): out.pop()
            continue
        out.append(line)
    put = {k: vw for k, vw in changes.items() if vw[0] is not None}
    if put: out += ['', f'# ---- {section} ----'] + [x for k, (v, why) in put.items() for x in ([f'# {why}'] if why else []) + [f'{k}\t{v}']]
    atomic_text(p, '\n'.join(out) + '\n')
    return type(g)(g.dir)


def provisional_prep(py, repo, env, logs, g):
    """What a provisional profile needs before a Mac host can be built for it: the shader tables (empty until a shader split fills them; an older provisional profile may lack some),
    which the host and build_ipa.sh read"""
    for n, text in EMPTY_TABLES.items():
        if not g.table(n).exists(): g.table(n).write_text(text)


CHAIN_H = 'dm/gfx/nvn/dmsh_chain.h'   # the chain version the host is compiled with, one for every game


def chain_of(repo):   # the chain version dm/gfx/nvn/dmsh_chain.h holds (None when it has none)
    try: m = re.search(r'#define DMSH_CHAIN "(.*)"', (repo / CHAIN_H).read_text())
    except OSError: return None
    return m.group(1) if m else None


def chain_header(py, repo, env, logs):
    """The chain version header, written again when it no longer holds the version the source tree gives (a chain change committed without dmsh_chain.py --header): dmsh_build.py stops
    on a stale one, and the host and build_ipa.sh read it. The version follows from the source alone (dmsh_chain.py), one for every game (GitHub issue #78: Breath of the Wild after bb585cd)"""
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py')], env=env, capture_output=True, text=True)
    if r.returncode or not r.stdout.split(): raise BuildError(f'dmsh_chain.py exit code {r.returncode}: {(r.stderr.strip() or "no version")[-500:]}')
    want, have = r.stdout.split()[-1], chain_of(repo)
    if have == want: return
    run([py, repo / 'dm/gfx/tools/dmsh_chain.py', '--header'], logs / 'dmsh_chain.log', env=env)
    say(f'chain version header rewritten: {repo / CHAIN_H} held {have or "no version"}, the source tree gives {want}')


CHAIN_TABLES = (('c1_full.tsv', 'c1_guard.py', 'c1 guard'), ('cb_extent.tsv', 'cb_extent.py', 'uniform read extents'))   # made from the shader build's Mac outputs: the host takes each only when its header names its library's stamp


def lib_head(lib):
    """What c1_full.tsv / cb_extent.tsv start with to be taken beside the library directory lib (dmsh_chain.py table_head): its chain version and the profile inputs its stamp (inputs.tsv,
    written by a full dmsh_build.py run) names. None when it has no stamp"""
    stamp = shader_lib.read_stamp(lib)
    return f'# chain {shader_lib.name_of(lib)} inputs {stamp["inputs"]}' if stamp.get('inputs') else None


def chain_tables(py, repo, sh, env, logs, g, chain):
    """c1_full.tsv (c1_guard.py: shaders whose c1 reads are not proven bounded) and cb_extent.tsv (cb_extent.py: how far each shader reads into its uniform blocks) from the Mac outputs of the
    shader build (sh/cache/b-<chain>/), into the profile folder. They are not in the repository (git ignores them): they follow from the shaders this Mac built, so every pack makes its own.
    A full dmsh_build.py run makes both; this makes those missing or not naming the library's stamp again, which the host would ignore (every c1 its own 64 KiB, whole uniform bindings
    copied; GitHub issue #78). True when both name it; False when the library has no stamp (no full build's Mac outputs to make them from)"""
    lib = sh / 'cache' / f'b-{chain}'
    want = lib_head(lib)
    if not want: return False
    for name, tool, step in CHAIN_TABLES:
        p = g.table(name)
        try: head = p.read_text(errors='replace').partition('\n')[0]
        except OSError: head = ''
        if head.startswith(want + ' '): continue
        PROG.set(step=step)
        run([py, repo / 'dm/gfx/tools' / tool, lib, p], logs / f'{tool[:-3]}.log', env=env)
        say(f'{name} ' + (f'named {" ".join(head.split()[2:5])}' if head.startswith('# chain ') else 'names no library' if head else 'missing') + f': made for {want[2:]} ({p})')
    return True


def refine_after_rewrite(g, ex_dir, patched, reg=None):
    """Refresh computed mem_used for the current modules; explicit profile values always win."""
    for profile in (reg, g) if reg else (g,):
        section, reason = '', ''
        for line in (profile.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
            if line.startswith('# ----'): section = line
            if line.split('\t', 1)[0].strip() == 'mem_used':
                computed = section == '# ---- After the rewrite ----' and reason.startswith(('# used memory for the rewritten modules', '# used memory for the current rewritten modules', '# the rewritten modules are larger than the default 64 MB'))
                if not computed: return g
            reason = line
    if pg.npdm(ex_dir / 'ExeFS/main.npdm')[0]: return g
    used = pg.mem_used(patched, g.modules_in(patched)) or 0x4000000
    if int(g.get('mem_used', '0x4000000'), 0) == used: return g
    say(f'mem_used {used:#x}: from the current rewritten modules (64 MB when the default covers them)')
    return set_keys(g, {'mem_used': (f'{used:#x}', 'used memory for the current rewritten modules, at least the default 64 MB')}, 'After the rewrite')


def heap_checked(g):   # the heap check's stamp in the profile ("# heap check: ..."): its result is kept, later runs skip it
    return any(l.startswith('# heap check: ') for l in (g.dir / 'game.tsv').read_text(encoding='utf-8').splitlines())


def heap_check(repo, d, patched, work, env, logs, g):
    """The heap check of a provisional profile whose game has virtual address memory (an NPDM system resource) and sets a heap (dm/games/profile_guess.py heap_check_needed):
    its heap request decides the alias / heap split of the guest block, and only a run shows it (Wonder asks for 1.44 GB, Tears of the Kingdom for 2.09 GB, which Engage's split
    has no room for). The game boots in the Mac host (headless and silent, no shader libraries) until its first heap request is answered or fails, or until it has run
    HEAP_CHECK_NNMAIN_SECS in nnMain without one (Engage sets none): a few seconds. A boot that resized the heap region (mem_alias / mem_reserve) is followed by another on the new
    layout, up to HEAP_CHECK_ROUNDS, since the checks behind the request (the app pool's, mem_used) run only once it fits. The keys go into the profile (profile_guess.py heap_signals), which is stamped"""
    PROG.stage('heap check')
    host, rd, log = work / 'tools/dmhost', work / 'tmp/heap_run', logs / 'heap_check.log'
    # Up to HEAP_CHECK_ROUNDS boots: a heap request that did not fit the heap region stops the first one there (mem_alias / mem_reserve set), and only the next boot reaches the app pool
    # check behind it (Pokémon FireRed / LeafGreen 2026: a 3.15 GB heap plus 102 MB of modules over the 3.2 GB pool, which mem_used then covers)
    set_all, hows, t0 = {}, [], time.time()
    for rnd in range(HEAP_CHECK_ROUNDS):
        PROG.set(step='building the Mac host'); build_mac_host(repo, host, env, logs, g)
        rmtree(rd)
        for x in ('save', 'temp', 'msl'): (rd / x).mkdir(parents=True)
        # a fresh save and temporary storage, no shader libraries (only the heap request matters), the profile's shader map, and the profile's alias region: the keys are for that layout, which is the iPhone's, not the Mac's 64 GB
        henv = dict(env, DM_ROMFS=str(rfs.source(d)), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), DM_NVN_SHMAP=str(g.table('shader_map.tsv')), DM_ALIAS_SIZE=g.get('mem_alias'))
        PROG.set(step='booting the game')
        seen = {}
        def done(seen=seen):   # the first heap request answered (or refused for good), or nnMain running for a while without one
            try: text = log.read_text(errors='replace')
            except OSError: return False
            if 'SetHeapSize OOM' in text or '[mem] heap ' in text: return True
            if '>>> nnMain' in text: seen.setdefault('nnmain', time.time())
            return time.time() - seen.get('nnmain', time.time()) > HEAP_CHECK_NNMAIN_SECS
        rc = boot_mac(host, patched, henv, rd, log, HEAP_CHECK_SECS, done)
        text = log.read_text(errors='replace'); rmtree(rd)
        sig = {k: v for k, v in pg.heap_signals(text, g).items() if g.get(k, '') != v[0]} | pg.depth_signal(text, g)   # the boot often passes the game's depth mode call too
        for k, (v, why) in sig.items(): say(f'heap check: {k} {v}: {why}')
        m = re.search(r'\[mem\] heap ([0-9.]+ GB)', text)
        how = ('the heap request did not fit' if 'SetHeapSize OOM' in text else 'the heap and the modules did not fit the app pool' if re.search(r'\[mem\] SetHeapSize \S+ \+ modules \S+ > app pool', text)
               else f'heap {m.group(1)}' if m else 'no heap set' if '>>> nnMain' in text
               else f'the host stopped before nnMain (exit {rc})' if rc is not None else 'no nnMain within the time limit')
        hows.append(how)
        if sig: g = set_keys(g, sig, 'Heap check on the Mac'); set_all.update(sig)
        if not sig.keys() & {'mem_alias', 'mem_reserve'}: break   # the heap region as it was: another boot would stop where this one did
    how = ', then '.join(hows)
    p = g.dir / 'game.tsv'; atomic_text(p, p.read_text(encoding='utf-8') + f'\n# heap check: {time.strftime("%Y-%m-%d")}, {how}' + (f'; set {", ".join(set_all)}' if set_all else '') + '\n')
    if not m and '>>> nnMain' not in text and 'SetHeapSize OOM' not in text and 'mem_used' not in set_all:   # stopped before any heap request: worth a look before the IPA is tried
        PROG.say(f'[RED] heap check: {how}; ---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=20)).rstrip('\n'))
    PROG.end(f'{how} in {hms(time.time() - t0)}' + (f'; set {", ".join(set_all)}' if set_all else ''))
    return type(g)(g.dir)


def rewrite_id(patched):   # the rewrite's key (rewrite: what the rewritten modules were made from), short; '' before a rewrite
    try: return patched.with_name(patched.name + '.key').read_text().partition('\n')[0][:12]
    except OSError: return ''


def region_checked(g, patched):   # the region check's stamp in the profile ("# address regions: ... (rewrite <id>)"): later runs of the same rewrite skip it (a new cut checks again)
    return any(l.startswith('# address regions: ') and l.endswith(f'(rewrite {rewrite_id(patched)})') for l in (g.dir / 'game.tsv').read_text(encoding='utf-8').splitlines())


CHECKED_CAP = 'from the address region check'   # the reason line of a stdalloc_cap the region check set (checked_cap): a later check works it out again


def checked_cap(g):   # the profile's stdalloc_cap row is the one an earlier region check set
    lines = (g.dir / 'game.tsv').read_text(encoding='utf-8').splitlines()
    return any(l.startswith('stdalloc_cap\t') and i and lines[i - 1].startswith(f'# {CHECKED_CAP}') for i, l in enumerate(lines))


def region_check(repo, d, patched, work, env, logs, g):
    """The address region check of a provisional or derived profile whose game has virtual address memory: the game boots in the Mac host (headless and silent, no shader libraries) with a
    Switch-sized alias region (DM_ALIAS_SIZE), so whatever it reserves fits, until it has run REGION_CHECK_NNMAIN_SECS in nnMain. The host's region table (DM_REGIONS, from the
    observation gates the rewriter places in the sdk) says which regions the game took, where each request returns to in the game, through which SDK API and how much of it it used, and where they reach
    against the profile's alias region (profile_guess.py region_findings). A red line when they reach past it: on an iPhone a region would fail. The one fix it makes: when the
    regions made after a StandardAllocator region need more room than the host's cap leaves them, it sets stdalloc_cap (profile_guess.py stdalloc_room), unless the profile sets one
    by hand, which it checks instead (Absolum: 2.56 GB from its first seconds; 2.5 and 2.72 GB both play). Others are reported, not changed: the fix depends on who lays the region out. A derived profile gets it too: its build has none of the registered builds' fixed-address cuts, only the function-matched ones
    (Super Smash Bros. Ultimate 13.0.1, GitHub issue #375), so a region those cuts shrank shows here before the IPA aborts on an iPhone. The profile is stamped"""
    PROG.stage('address regions')
    host, rd, log, table = work / 'tools/dmhost', work / 'tmp/region_run', logs / 'region_check.log', logs / 'regions.tsv'
    PROG.set(step='building the Mac host'); build_mac_host(repo, host, env, logs, g)
    rmtree(rd); table.unlink(missing_ok=True)
    for x in ('save', 'temp', 'msl'): (rd / x).mkdir(parents=True)
    henv = dict(env, DM_ROMFS=str(rfs.source(d)), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(rd / 'msl'), DM_NVN_SHMAP=str(g.table('shader_map.tsv')),
                DM_ALIAS_SIZE=f'{SWITCH_ALIAS:#x}', DM_REGIONS=str(table))
    PROG.set(step='booting the game')
    t0, seen = time.time(), {}
    def done():
        try: text = log.read_text(errors='replace')
        except OSError: return False
        if '>>> nnMain' in text: seen.setdefault('nnmain', time.time())
        return time.time() - seen.get('nnmain', time.time()) > REGION_CHECK_NNMAIN_SECS
    rc = boot_mac(host, patched, henv, rd, log, HEAP_CHECK_SECS + REGION_CHECK_NNMAIN_SECS, done)
    rmtree(rd)
    try: ran = log.read_text(errors='replace')
    except OSError: ran = ''
    depth = pg.depth_signal(ran, g)   # before the shader steps and the prebuild, which take the mode from the profile
    if depth:
        g = set_keys(g, depth, 'Address region check')
        for k, (v, why) in depth.items(): say(f'address regions: {k} {v}: {why}')
    try: rows = table.read_text(errors='replace')
    except OSError: rows = ''
    import nso_syms
    alias, syms = int(g.get('mem_alias'), 0), nso_syms.defined(nso_syms.load(d / 'ExeFS/sdk')) if (d / 'ExeFS/sdk').exists() else {}
    have = 0 if checked_cap(g) else int(g.get('stdalloc_cap', '0'), 0)   # a value set by hand (or the registered profile's) stays; one an earlier check wrote is worked out again
    if not have and rows:
        need = pg.stdalloc_cap_needed(pg.region_rows(rows, syms), alias)
        if need != (int(g.get('stdalloc_cap', '0'), 0) or None):
            g = set_keys(g, {'stdalloc_cap': (f'{need:#x}' if need else None, f'{CHECKED_CAP}: the regions made after the StandardAllocator region need more room than the 256 MB the host leaves them')}, 'Address region check')
            say(f'address regions: stdalloc_cap {need:#x} ({need / 2**30:.2f} GB) set in {g.dir / "game.tsv"}' if need else 'address regions: the stdalloc_cap an earlier check set is no longer needed: removed')
        have = need or 0
    found = pg.region_findings(rows, alias, syms, REGION_CHECK_NNMAIN_SECS, have)
    if not rows: how = 'no region table (the sdk has no region gates)' if rc is None or '>>> nnMain' in log.read_text(errors='replace') else f'the host stopped before nnMain (exit {rc})'
    elif not found: how = 'no address regions'
    else: how = found[-1][1]
    for red, text in found: say(('[RED] ' if red else '') + 'address regions: ' + text)
    p = g.dir / 'game.tsv'; old = [l for l in p.read_text(encoding='utf-8').rstrip('\n').split('\n') if not l.startswith('# address regions: ')]
    atomic_text(p, '\n'.join(old).rstrip('\n') + f'\n\n# address regions: {time.strftime("%Y-%m-%d")}, {how} (rewrite {rewrite_id(patched)})\n')
    PROG.end(f'{how} in {hms(time.time() - t0)}', ok=not any(red for red, _ in found))
    return type(g)(g.dir)


def data_rows(b):   # a table's data rows (without # comment lines: the header changes with the shader_split.py version, not with the game files)
    return [l for l in b.splitlines() if not l.startswith(b'#')]


def table_difference(name, profile, split):
    """How a split table differs from the profile's, for the build error: the row counts on each side and the first such row of each. Rows are compared as sets of lines, so one row added early does not read as every later row changed."""
    a, b = collections.Counter(data_rows(profile)), collections.Counter(data_rows(split))
    only_profile, only_split = list((a - b).elements()), list((b - a).elements())
    if not only_profile and not only_split: return f'{name}: the same rows in another order'
    out = [f'{name}: {len(only_profile):,} row(s) only in the profile, {len(only_split):,} only in the split']
    if only_profile: out.append('  profile: ' + only_profile[0].decode(errors='replace'))
    if only_split: out.append('  split:   ' + only_split[0].decode(errors='replace'))
    return '\n'.join(out)


def dlc_only_shader_change(g, tables):
    """A different DLC shader union, with the registered non-DLC shaders and facts unchanged. Package indices may be re-numbered when an add-on is removed.
    Source ownership is the split's /aoc/<index>/ prefix; a module or base/update shader difference never qualifies for this shader-only derivation."""
    def rows(path):
        out = {}
        for line in path.read_text().splitlines():
            if not line.strip() or line.startswith(('#', 'package\t')): continue
            c = line.split('\t')
            if len(c) != 5 or not c[0].endswith('.bin') or c[3] in out: return None
            out[c[3]] = (c[0], c[2], c[4])
        return out
    try:
        old, new = rows(g.table('shader_map.tsv')), rows(tables / 'shader_map.tsv')
        if old is None or new is None: return False
        old_dlc = {k: v for k, v in old.items() if v[2].startswith('/aoc/')}
        new_dlc = {k: v for k, v in new.items() if v[2].startswith('/aoc/')}
        if old_dlc == new_dlc or {k: v for k, v in old.items() if k not in old_dlc} != {k: v for k, v in new.items() if k not in new_dlc}: return False
        dlc = old_dlc.keys() | new_dlc.keys()
        def facts(path):
            out, seen = [], set()
            for line in path.read_text().splitlines():
                if not line.strip() or line.startswith('#'): continue
                c = line.split('\t', 1)
                if len(c) != 2 or c[0] in seen: raise ValueError('invalid facts')
                seen.add(c[0])
                if c[0] not in dlc: out.append(line)
            return out
        return facts(g.table('shader_facts.tsv')) == facts(tables / 'shader_facts.tsv')
    except (OSError, UnicodeError, ValueError): return False


def compute_reader_additions(g, tables, split_log):
    """Only newly decoded Unity ComputeShader kernels, preserving every registered shader, fact and evidence pair.
    Called for registered module builds only. Package indices may move when the new kernels sort among old entries;
    package/stage/source identity may not. Actual reader key/source records in the split log prove each addition's origin."""
    def rows(path):
        out = {}
        for line in path.read_text().splitlines():
            if not line.strip() or line.startswith(('#', 'package\t')): continue
            c = line.split('\t')
            if len(c) != 5 or not c[0].endswith('.bin') or not c[1].isdigit() or c[3] in out: raise ValueError('invalid shader map')
            out[c[3]] = (c[0], c[2], c[4])
        return out
    def facts(path):
        out = {}
        for line in path.read_text().splitlines():
            if not line.strip() or line.startswith('#'): continue
            c = line.split('\t', 1)
            if len(c) != 2 or c[0] in out: raise ValueError('invalid shader facts')
            out[c[0]] = c[1]
        return out
    try:
        old, new = rows(g.table('shader_map.tsv')), rows(tables / 'shader_map.tsv')
        added = new.keys() - old.keys()
        if not added or any(new.get(k) != v for k, v in old.items()): return False
        evidence = set()
        for line in split_log.splitlines():
            if line.startswith('unity compute source: '):
                record = json.loads(line.removeprefix('unity compute source: '))
                if not isinstance(record, list) or len(record) != 2 or not all(isinstance(v, str) for v in record): return False
                evidence.add(tuple(record))
        if any(new[k][1] != 'compute' or (k, new[k][2]) not in evidence for k in added): return False
        before, after = facts(g.table('shader_facts.tsv')), facts(tables / 'shader_facts.tsv')
        if after.keys() - before.keys() != added or any(after.get(k) != v for k, v in before.items()): return False
        if data_rows(g.table('shader_pairs.tsv').read_bytes()) != data_rows((tables / 'shader_pairs.tsv').read_bytes()): return False
        pso = g.table('pso_pairs.tsv')
        if pso.is_file():
            # New supplemental pass covers may accompany the reader, but no registered pair can disappear.
            old_pairs, new_pairs = set(data_rows(pso.read_bytes())), set(data_rows((tables / 'pso_pairs.tsv').read_bytes()))
            if not old_pairs <= new_pairs: return False
            for line in new_pairs - old_pairs:
                if not line.strip(): continue
                pair = line.decode().split('\t')
                if len(pair) != 2 or old.get(pair[0], ('', '', ''))[1] != 'vertex' or old.get(pair[1], ('', '', ''))[1] != 'pixel': return False
        return True
    except (OSError, UnicodeError, ValueError): return False


def split_shaders(py, repo, roms, mains, pkg, env, logs, g, provisional=False, oodle=None, dlc_roms=(), derive_dlc=None, derive_compute=None):
    """The shader split into pkg, checked against the profile's tables. The package is reused when the extractor and the modules it imports, the profile settings and tables it reads or is checked against,
    the DLC RomFS roots, each version's modules, RomFS (paths and sizes) and run-time capture, and the package's own contents are as at the last successful split. Returns the package's content key, the shader build's input (None for damaged files, which never reuse them): a split that runs
    again because a reader of another engine changed writes the same package, and the iOS shaders stay reused.
    oodle: for decoder-backed packs, builds or reuses the Mac host and returns DM_OODLE_HOST / DM_OODLE_MODULES. Verified host provenance ignores only its chain label;
    unknown hosts use their bytes, and rewritten modules are always content inputs. Both macOS and iOS builds share this extraction cache."""
    names = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv')
    PROG.stage('shader split', step='checking the inputs')   # then the count of files and archive ranges scanned (shader_split.py's progress lines); the package and tables come at the end
    kf, outputkf = pkg.with_name(pkg.name + '.key'), pkg.with_name(pkg.name + '.outputs.key')
    inputs = [('files', python_tool_files(repo, 'dm/gfx/tools/shader_split.py', ('dm/gfx/tools', 'dm/tools', 'dm/games'))), bool(provisional), 'split-v2']
    for r, m in zip(roms, mains):
        inputs += [('files', [m] + [m.parent / n for n in g.modules_in(m.parent) if n != 'main']), ('stat' if provisional else 'tree', r)]
        for setting, folder, pattern in (('DM_GLSLC_CAPTURE', 'glslc_capture', '*.glslc'), ('DM_UE_SHADERS', 'ue_shaders', '*.ushaderbytecode')):
            capture = Path(env.get(setting) or r.parent / folder)
            inputs += [('stat', capture) if provisional else ('files', sorted(capture.glob(pattern)))]   # its files, not where it is: the same build reached through a moved folder or a link keeps its split
    if env.get('DM_NRO_DIR'): inputs += [('files', sorted(Path(env['DM_NRO_DIR']).glob('*.nro')))]   # NROs captured from a boot (capture_nro)
    inputs += [item for r in dlc_roms for item in (('dlc', Path(r).name), ('stat', r))]   # DLC has no registered module build to pin its contents: include its index and re-extraction times, even when file sizes stay the same
    if oodle:   # a host or rewriter fix can change what the game's decoder produces
        PROG.set(step='building the Mac host for the game\'s Oodle decoder'); env = dict(env, **oodle())
        host = Path(env['DM_OODLE_HOST'])
        inputs += [('decoder', decoder_host_key(host) or fingerprint(('files', [host]))), ('modules', package_key(Path(env['DM_OODLE_MODULES'])))]
    key = None if ALLOW_DAMAGED else fingerprint(*inputs, *split_profile_inputs(g))
    dlckf, dlckey = pkg.with_name(pkg.name + '.dlc.key'), None
    cskf, cskey, promoted = pkg.with_name(pkg.name + '.compute.key'), None, None
    def promote(derive, kind):
        nonlocal g, provisional, env, promoted
        g, provisional, promoted = derive(), True, kind; inputs[1] = True
        env = dict(env, DM_GAMES_DIR=os.pathsep.join([str(g.dir.parent), env.get('DM_GAMES_DIR', '')]), DM_GAMES_FIRST='1')
    if not provisional and derive_dlc:
        dlckey = fingerprint(key, ('files', [Path(__file__)])) if key else None   # proof pins the comparison code as well as the registered tables and every split input
        if dlckey and reused(dlckf, dlckey) is not None:
            promote(derive_dlc, 'dlc')
            key = fingerprint(*inputs, *split_profile_inputs(g))
    if not provisional and derive_compute:
        cskey = fingerprint(key, ('files', [Path(__file__)])) if key else None
        if cskey and reused(cskf, cskey) is not None:
            promote(derive_compute, 'compute')
            key = fingerprint(*inputs, *split_profile_inputs(g))
    have_pairs = g.table('pso_pairs.tsv').is_file() or pkg.with_name('pso_pairs.tsv').is_file()
    if key and pkg.is_dir() and have_pairs and (note := reused(kf, key)) is not None and reused(outputkf, split_output_key(pkg, g)) is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return package_key(pkg)
    PROG.set(step='splitting')
    tmp = pkg.with_name(pkg.name + '.partial'); rmtree(tmp); (tmp / 'tables').mkdir(parents=True)
    def counted(ln):   # shader_split.py's 'progress: <done>/<total>'
        if m := re.fullmatch(r'progress: (\d+)/(\d+)', ln): PROG.set(done=int(m[1]), total=int(m[2]), unit='files')
    run([py, repo / 'dm/gfx/tools/shader_split.py', ','.join(map(str, roms)), ','.join(map(str, mains)), tmp / 'pkg'] + [tmp / 'tables' / n for n in names + ('pso_pairs.tsv',)] + ([','.join(map(str, dlc_roms))] if dlc_roms else []),
        logs / 'shader_split.log', env=env, on_line=counted)
    text = (logs / 'shader_split.log').read_text(errors='replace')
    m = re.search(r'shaders: (\d+) \(', text)
    bad = re.search(r'^\[RED\] (\d+) file\(s\) could not be read.*$', text, re.M)   # the split leaves out a file no reader reads (its own red line per file in the log) and goes on
    if bad and not provisional:   # the profile's tables were split from every file: one that does not read now is a reader change
        raise BuildError(f'{bad.group(0).removeprefix("[RED] ")}; the profile\'s tables hold every file\'s shaders (each file in {logs / "shader_split.log"})')
    if bad: say(f'{bad.group(0)}; the IPA goes without them (each file in {logs / "shader_split.log"})')
    if m and m.group(1) == '0':   # dmsh_build.py would make no library, and the iOS shader step stop with no word of why
        raise BuildError('the shader split found no shaders in main or the RomFS: the game keeps them in a form shader_split.py does not read; locate them (docs/agents) and add a reader')
    # pso_pairs.tsv (supplemental BAHS variations and legacy Unity pass-input covers): checked like the others where the profile has one; else kept beside the
    # package, where dmsh_build.py reads it too, so padding and the pipeline prebuild use the same pairs
    if not provisional and derive_dlc and dlc_only_shader_change(g, tmp / 'tables'):
        promote(derive_dlc, 'dlc')
    if not provisional and derive_compute and compute_reader_additions(g, tmp / 'tables', text):
        promote(derive_compute, 'compute')
    if provisional or g.table('pso_pairs.tsv').is_file(): names += ('pso_pairs.tsv',)
    else: shutil.copyfile(tmp / 'tables' / 'pso_pairs.tsv', pkg.with_name('pso_pairs.tsv'))
    if provisional:   # a provisional profile takes the tables as they come
        for n in names: shutil.copyfile(tmp / 'tables' / n, g.table(n))
    else:
        # Data rows byte-identical to the profile's = the same game files the tables were built from; if not, stop (the profile's tables are left alone). Only data rows are compared: the header of Engage's profile tables was written by an older shader_split
        diff = [n for n in names if data_rows((tmp / 'tables' / n).read_bytes()) != data_rows(g.table(n).read_bytes())]
        if diff:
            rows = '\n'.join(table_difference(n, g.table(n).read_bytes(), (tmp / 'tables' / n).read_bytes()) for n in diff)
            raise BuildError(f'the split shader tables differ from game profile {g.id}\'s:\n{rows}\nEither these game files are not the versions the profile registers, or shader_split.py now reads them differently and the profile\'s tables were not split again (the split\'s tables: {tmp / "tables"})')
    rmtree(kf); rmtree(outputkf); swap_in(tmp / 'pkg', pkg); rmtree(tmp)
    note = (f'{int(m.group(1)):,} shaders' if m else '') + (f', {bad.group(1)} file(s) unreadable' if bad else '')
    if key:   # an own profile's tables are outputs too; the next split checks the profile after they were written
        key = fingerprint(*inputs, *split_profile_inputs(g))
        outputkf.write_text(split_output_key(pkg, g) + '\n')
        kf.write_text(f'{key}\n{note}\n')
    if promoted == 'dlc' and dlckey: dlckf.write_text(f'{dlckey}\nDLC-only shader content\n')
    else: rmtree(dlckf)
    if promoted == 'compute' and cskey: cskf.write_text(f'{cskey}\nNewly decoded compute kernels\n')
    else: rmtree(cskf)
    PROG.end(note)
    return key and package_key(pkg)


def shader_keys(g):   # the base shaders dmsh_build.py builds, shared by progress and the stale-library event
    keys = set()
    for ln in g.table('shader_map.tsv').read_text(errors='replace').splitlines():
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and c[0].endswith('.bin'): keys.add(c[3])
    return keys


SHADER_MISSING = re.compile(r'\[RED\] (?:shader ([0-9a-f]{16}) \([^)]*\) failed to load:|failed to load shader ([0-9a-f]{16})_b:)')   # what the host logs for a shader its library lacks (nvn_mtl.m)


def seen_listed(g):   # the keys of the profile's shader_seen.tsv
    return set(re.findall(r'^[0-9a-f]{16}\b', g.table('shader_seen.tsv').read_text(errors='replace'), re.M))


def seen_logs(work, app):   # the logs a --seen-shaders build reads: the run command's, the app's and the one before it (only opening the app and quitting in its settings window starts a new log)
    logs = Path(os.path.expanduser('~/Library/Logs/nxnative'))
    return work / 'run/host.log', logs / f'{app:016X}.log', logs / f'{app:016X}.prev.log'


def seen_keys(g, sh, logs):
    """The keys of a partial Mac library: the profile's shader_seen.tsv, those of the last partial build (sh/partial-keys.txt) and those the logs report missing. The file keeps what earlier runs added, as a
    shader that loads no longer shows in the next log. Only keys of the shader map go in: a list or a log of another game version may name others, and the builder knows only the map's. No key at all stops
    the build, as the builder takes no keys for the whole map. Returns the keys, sorted"""
    listed, known, done = seen_listed(g), shader_keys(g), sh / 'partial-keys.txt'
    try: earlier = set(done.read_text().split())
    except OSError: earlier = set()
    found = set()
    for log in logs:
        try: found.update(a or b for a, b in SHADER_MISSING.findall(log.read_text(errors='replace')))
        except OSError: pass
    keys = sorted((listed | earlier | found) & known)
    if not keys: raise BuildError(f'none of the {len(listed):,} shaders in {g.table("shader_seen.tsv")} is in this version\'s shader map: build the whole map, without --seen-shaders')
    sh.mkdir(parents=True, exist_ok=True); atomic_text(done, ''.join(k + '\n' for k in keys))
    extra = len(set(keys) - listed)
    say(f'partial shader library: {len(keys):,} of {len(known):,} shaders ({len(listed & known):,} from the profile\'s playtests' + (f', {extra:,} that earlier runs reported missing' if extra else '')
        + '); the rest will not draw, so areas the list does not cover draw incomplete until this script runs again')
    return keys


class ShaderWatch:
    """Progress of dmsh_build.py, from the small JSON file it rewrites at most every half second (DMSH_PROGRESS): the cache check over every key, then the compile, which finishes the keys
    in batches (cache hits and misses alike) and counts them. It used to be read from the files dmsh_build.py wrote, one per shader output, which it no longer leaves on disk"""
    def __init__(self, path): self.path = path

    def __call__(self):
        try: p = json.loads(self.path.read_text())
        except (OSError, ValueError): return None
        cached, misses = p.get('cached', 0), p.get('misses')
        if p.get('step') == 'cache check':
            return {'step': 'cache check', 'done': p['done'], 'total': p['total'], 'unit': 'shaders', 'note': f'{cached:,} cached'}
        return {'step': 'compiling', 'done': p.get('done', 0), 'total': p.get('total', 0), 'unit': 'shaders', 'note': f'{misses:,} not cached' if misses else ''}


def spirv_cross(repo, env=None):   # resolve the same selection the shader tools run, including an executable name on PATH
    env = os.environ if env is None else env
    selected = env.get('SPIRV_CROSS', str(repo / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'))
    return Path(shutil.which(selected, path=env.get('PATH')) or selected)


def toolchain(repo, env=None):   # Metal is a separate Xcode download: its version and SDK must invalidate reuse too
    return (('cmd', ['xcodebuild', '-version']), ('files', [spirv_cross(repo, env)]),
            *(('cmd', ['xcrun', '-sdk', sdk, *args]) for sdk in ('macosx', 'iphoneos')
              for args in (['metal', '--version'], ['metallib', '--version'], ['--show-sdk-version'], ['--show-sdk-build-version'])))


def prune_stale_chains(sh, chain):
    """Remove the shader libraries of other chain versions, and their keys, once the build for this one succeeded: sh/cache/b-<old> (Mac), sh/ios/b-<old> (iOS), sh/ios/b-<old>.key and sh/mac-b-<old>.key.
    A chain change translates every shader again into a new b-<chain>/, so each upstream update left another 2 GB for Breath of the Wild (two old folders: 4.4 GB). The host and the packers read only
    b-<chain>/, shader_regressions.py compares this build with the one before it (already done, and the next chain change compares with this one), and the Metal compile cache (cas/metallib, keyed by the MSL)
    holds what the old libraries compiled. Called only after the libraries, their tables and the key are in place, so a stopped or failed build keeps the old ones. Only names of the exact form
    b-<12 hex digits> go (the key files with .key, the Mac's with a mac- prefix), never another entry. A removal that fails is a [RED] line, not an error: the build is done, and the next one tries
    again. Returns the chain versions removed"""
    def listing(d, pat): return [d / n for n in (os.listdir(d) if d.is_dir() else []) if re.fullmatch(pat, n) and n not in (f'b-{chain}', f'b-{chain}.img', f'b-{chain}.key', f'mac-b-{chain}.key')]
    old = [p for d in (sh / 'cache', sh / 'ios') for p in listing(d, r'b-[0-9a-f]{12}') if p.is_dir()]
    old += [p for d, pat in ((sh / 'ios', r'b-[0-9a-f]{12}\.key'), (sh, r'mac-b-[0-9a-f]{12}\.key'), (sh / 'cache', r'b-[0-9a-f]{12}\.img(\.part)?'), (sh / 'ios', r'b-[0-9a-f]{12}\.img(\.part)?')) for p in listing(d, pat) if p.is_file()]
    gone, size = [], 0
    for p in old:
        try: n = tree_size(p)[1] if p.is_dir() else p.stat().st_size; rmtree(p)
        except OSError as e: say(f'[RED] could not remove old shader libraries {p}: {e}'); continue
        gone.append(p); size += n
    if not gone: return []
    names = sorted({re.sub(r'\.(key|img(\.part)?)$', '', p.name).removeprefix('mac-') for p in gone})
    say(f'removed old shader libraries {", ".join(names)} ({amount(size, "B")})')
    return names


def prune_shader_cache(sh, chain):
    """Remove the per-shader cache entries (shader/<digest> in sh/cache/cas.db) the current chain's last full build did not use, with prune_stale_chains and under the same conditions (issue #1033).
    A shader's digest covers the chain's code, so each chain change translated every shader again into a new entry and left the old ones: Tears of the Kingdom's held six sets for 28,669 shaders,
    11.85 GB of sources and reflection nothing read. The digests in use are listed by dmsh_build.py's full build in b-<chain>/_b_cas.txt beside the Mac outputs (every full build writes them);
    index/ cannot tell, since a cache hit does not rewrite it. No list (libraries from before the list): nothing goes. An entry newer than the list stays (regress.py's chain builds share this
    cache and may publish meanwhile). The digests also cover the iOS compile convention, so a build of the other target translates again, as switching targets did before; the Metal compile cache
    (metallib/) and the fetch variants (variant/) are keyed by the MSL and stay. Only names of 64 hex digits go. Removals that fail are one [RED] line, as for the libraries.
    Logs the space of the sources and reflection: an entry's libraries are clones of the compile cache's. Returns the number removed"""
    used = shader_lib.view(sh / 'cache' / f'b-{chain}') / '_b_cas.txt'
    try: keep, since = set(used.read_text().split()), used.stat().st_mtime
    except OSError: return 0
    if not shader_cache.exists(sh / 'cache/cas.db'): return 0
    store = shader_cache.Store(sh / 'cache/cas.db', say)
    try: n, size = store.prune_shaders(keep, since)
    finally: store.close()
    if n: say(f'removed old shader cache entries: {n:,} ({amount(size, "B")})')
    return n


def stale_shaders(py, repo, env, lib_dirs, chain, g):
    """Why cached shader libraries cannot be reused by the build that starts: (reason, old, new, count) or None. 'chain': library folders b-<V> of another translation chain (lib_dirs: the
    selected targets' cache folders) have libraries without a completed current build; old is those folders' names, new b-<chain>. A full build writes its stamp only at the end,
    so a partial current-chain output must not hide the old libraries. 'inputs': the current chain's stamp names different profile inputs (dmsh_chain.py --inputs).
    count: base libraries scheduled, once per current shader key and selected target, including cache restores, never copies retained in older chains"""
    cur = [d / f'b-{chain}' for d in lib_dirs]
    cached = [d for d in cur if shader_lib.has_library(d)]
    other = []
    for root, d in zip(lib_dirs, cur):
        if d in cached and lib_head(d): continue
        other.extend(old for old in sorted({Path(shader_lib.folder_of(p)) for p in root.glob('b-*')}) if old.name != d.name and shader_lib.has_library(old))
    if other: return ('chain', ', '.join(sorted({d.name for d in other})), f'b-{chain}', len(shader_keys(g)) * len(lib_dirs))
    if not cached: return None
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py'), '--inputs'], env=env, capture_output=True, text=True)
    new = r.stdout.split()[-1] if r.returncode == 0 and r.stdout.split() else None
    for d in cached:
        have = lib_head(d)   # '# chain b-<V> inputs <digest>', None without a stamp
        if new and have and have.split()[-1] != new: return ('inputs', have.split()[-1], new, len(shader_keys(g)) * len(lib_dirs))
    return None


def pack_libraries(sh, chain, pull=None):
    """Each finished library folder as one file, which the host and the tools read in place (shader_lib.py). pull False: nothing reads the iPhone's pulling libraries (ios_wants_pulling), which stay out of its
    image, so the IPA packs that file as it is and reuses it. A build that wants them again compiles them"""
    shader_lib.pack(sh / 'cache' / f'b-{chain}', say)
    shader_lib.pack(sh / 'ios' / f'b-{chain}', say, leave_out=pull is False)


def ios_gpus(gpu_option, g):
    """The GPU architectures of the iPhone's pipeline archive, as build_ipa.sh gets them (DM_PSO_GPUS): --gpu / PSO_GPUS, else by the profile's pso_gpu (for --gpu: the tokens joined with commas), else g18p for
    pso_mtl4 on, as build_ipa.sh decides without DM_PSO_GPUS: only the Metal 4 path reads the archive on iOS"""
    return gpu_option or ((','.join(g.get('pso_gpu', '').split()) or 'g18p') if g.get('pso_mtl4', 'off') == 'on' else 'none')


def ios_wants_pulling(g, gpus, prepare_xcode, env):
    """Keep the libraries the device's prebuild and hybrid pipelines need, even when pso_mtl4 defaults off: the launcher can enable them at run time.
    IOS_PULL=0 opts out, except for an offline archive or Xcode project, which always takes them (as build_ipa.sh does)."""
    if prepare_xcode or (gpus != 'none' and g.table('shader_pairs.tsv').is_file()): return True
    return (env.get('IOS_PULL') or '1') == '1'


def build_shaders(py, repo, sh, xlate, env, logs, g, pkg_key=None, mac=False, seen=None, pull=None):
    """The iOS shader libraries (dmsh_build.py, which also builds the Mac's into sh/cache when the build wants them), or with mac only the Mac's (a Mac-only build: no IOSOUT). Returns the folder the host takes
    them from. Reused when the shader package (pkg_key, its content; None never reuses) and the chain's tools, translator binaries, profile and compilers are as at the last successful build:
    dmsh_build.py would only restore every shader from its cache (63,000 for Breath of the Wild: about 100 s). The key of the iOS libraries is sh/ios/b-<chain>.key, that of the Mac's
    sh/mac-b-<chain>.key (not in sh/cache, whose b-* entries dmsh_build.py reads as library folders). Each target writes only its own key (an iOS build that keeps a Mac build's libraries, both); either is taken only
    while the Mac sources and reflection carry a full build's stamp (inputs.tsv) for c1_full.tsv and cb_extent.tsv to name (chain_tables).
    Switching targets translates again (the per-shader keys include the iOS compile convention); the Metal compile cache (dmsh_mtlcache.py) reuses previously compiled MSL for that target.
    After success the libraries of other chain versions and the cache entries this chain's build did not use are removed (prune_stale_chains, prune_shader_cache), the libraries become one file each,
    b-<chain>.img beside the folder's name, read in place (shader_lib.py), and the compile cache is one file, sh/cache/cas.db (shader_cache.py), read in place and into which a cache from before it moves: a reused step touches none of their files.
    seen (--seen-shaders, a Mac-only build): only those keys, into sh/partial/b-<chain>, emptied first, with its key inside; the full build's libraries, its key and the cache are left as they are, and the
    per-shader cache (DMSH_CAS) is the full build's, so a later full build reuses this work. It gets no stamp and no c1_full.tsv or cb_extent.tsv, on purpose: beside a stamp the host would take the tables'
    silence on a shader the partial build never saw as proof it is safe (no chain_tables, no shader_regressions, no pruning; the host logs the missing stamp in red at each start)
    pull: an iPhone build's ios_wants_pulling. False neither compiles the pulling libraries nor keeps them in its image; True does not reuse an image without them"""
    ios, log, cas = sh / 'ios', logs / 'dmsh_build.log', sh / 'cache/cas.db'
    shader_cache.adopt(sh / 'cache/cas', cas, say)
    PROG.stage('Mac shaders' if mac else 'iOS shaders')
    chain = chain_of(repo)
    out = sh / 'partial' if seen else sh / 'cache' if mac else ios
    libs, kf, kmac = out / f'b-{chain}', ios / f'b-{chain}.key', out / 'key' if seen else sh / f'mac-b-{chain}.key'
    pso_pairs = g.table('pso_pairs.tsv')
    if not pso_pairs.is_file(): pso_pairs = sh / 'pso_pairs.tsv'   # the split's copy is a shader input too (vs_pad)
    if pkg_key:
        PROG.set(step='checking the inputs')
        key = fingerprint(pkg_key, ('files', chain_files(repo)), ('stat', xlate.parent), ('files', profile_files(g)), ('files', [pso_pairs]), *toolchain(repo, env), str(xlate),
                          {k: env.get(k) for k in ('XLATE_DEBUG', 'XLATE_REDUCED')}, *((seen,) if seen else ()), env=env)
        note = next((n for k in ((kmac,) if seen else (kmac, kf) if mac else (kf,)) if (n := reused(k, key)) is not None), None)
        # the libraries are the last build's, so are tables that name their stamp; a missing or stale one is made from that build's Mac outputs, and without them the step runs again
        lost_pull = pull and not mac and not seen and shader_lib.left_out_pulling(libs)   # includes an image built before pulling libraries were kept by default
        if lost_pull: say('the app now needs the pulling vertex libraries, which this image left out: building them')
        if shader_lib.has_library(libs) and note is not None and not lost_pull and (seen or chain_tables(py, repo, sh, env, logs, g, chain)):
            if not seen: prune_stale_chains(sh, chain); prune_shader_cache(sh, chain); pack_libraries(sh, chain, None if mac else pull)   # folders from before this become the one file here
            PROG.end(f'{note}, reused (inputs unchanged)'); return out
    else: key = None
    rmtree(kmac)
    if not mac: rmtree(kf)
    if seen: rmtree(out)   # what a run leaves is what it built; its keys are in partial-keys.txt
    # The Metal work follows from what the build produces: an iPhone build needs only the iOS libraries (the macOS
    # metallibs are about half of the compile and never enter the IPA), a Mac build only the Mac's, and a build that
    # wants both compiles both. The Mac sources and reflection are written either way (c1_full / cb_extent come from them).
    # An iPhone build of a folder that holds this chain's Mac libraries (a --platform mac build, whose run command takes
    # them from there) builds them again too: a full dmsh_build.py run removes every output of the folder first, so that
    # a stale key keeps no old library, and restores only the targets it builds. The Metal compile cache gives back every
    # library whose MSL was compiled before.
    keep_mac = not mac and shader_lib.has_library(sh / 'cache' / f'b-{chain}')
    benv = dict(env, XLATE=str(xlate), IOSOUT=str(ios), DMSH_TARGETS='mac' if mac else 'mac,ios' if keep_mac else 'ios')
    if ALLOW_OTHER_METAL: benv['DM_ALLOW_OTHER_METAL'] = '1'
    if mac: del benv['IOSOUT']   # dmsh_build.py builds the iOS libraries only with IOSOUT
    elif pull is False: benv['DMSH_IOS_PULL'] = '0'   # nothing in this app reads them
    if seen: benv['DMSH_CAS'] = str(cas)
    prog = sh / 'progress.json'; rmtree(prog); benv['DMSH_PROGRESS'] = str(prog)   # dmsh_build.py's progress (ShaderWatch)
    try: PROG.watch(ShaderWatch(prog))
    except Exception: pass   # progress never stops the build
    try: stale = None if seen else stale_shaders(py, repo, env, [sh / 'cache'] if mac else [sh / 'cache', ios] if keep_mac else [ios], chain, g)
    except Exception: stale = None   # a note never stops the build
    if stale:
        reason, old, new, n = stale
        PROG.event('shaders_stale', reason=reason, old=old, new=new, count=n)
        say(f'[NOTE] cached shader libraries cannot be reused ({old} -> {new}): ' + ('the translation chain changed' if reason == 'chain' else 'the game profile\'s shader inputs changed')
            + f'; {n:,} base libraries scheduled (including cache restores; later builds reuse as before)')
    if mac and not seen and g.table('shader_seen.tsv').is_file():
        say(f'building the whole shader map, {len(shader_keys(g)):,} shaders: hours for a large one the first time. --seen-shaders builds only the {len(seen_listed(g)):,} the profile\'s playtests used, in minutes; areas they do not cover then draw incomplete')
    if not seen:   # the run's folder replaces the library: a stopped build keeps no old shaders under the same chain, as dmsh_build.py removes a folder's
        shader_lib.remove_image(sh / 'cache' / f'b-{chain}')
        if not mac: shader_lib.remove_image(ios / f'b-{chain}')
    run([py, repo / 'dm/gfx/tools/own_compilers.py', sh / 'pkg', out if seen else sh / 'cache', *(seen or ())], log, env=benv)
    PROG.watch(None)
    if not seen and not chain_tables(py, repo, sh, env, logs, g, chain): raise BuildError(f'no stamped Mac shader outputs in {sh / "cache" / f"b-{chain}"} (inputs.tsv): dmsh_build.py did not finish a full build')
    what = 'Mac' if mac else 'iOS'
    n = sum(1 for _ in shader_lib.view(libs).glob('*_b.metallib')) if shader_lib.exists(libs) else 0   # the image dmsh_build.py writes (a folder from an older chain)
    if n == 0: raise BuildError(f'no {what} shader libraries in {libs}')
    say(f'{n} {what} shader libraries ({libs})')
    if keep_mac: say(f'{sum(1 for _ in shader_lib.view(sh / "cache" / f"b-{chain}").glob("*_b.metallib"))} Mac shader libraries kept for the Mac run command ({sh / "cache" / f"b-{chain}"})')
    t = log.read_text(errors='replace')
    if own := re.search(r'^note: (built with this Mac\'s compilers.*)$', t, re.M): say(own.group(1))
    if lost := re.search(r'^\[RED\] lost without failing: .*$', t, re.M): say(f'{lost.group(0)}; each is named in {log}, and the build goes on: a shader the game never reaches costs nothing')
    m, c = re.search(r'shaders (\d+): ok (\d+)', t), re.search(r'shader cache: hit (\d+)', t)
    note = ''
    if m:
        total, ok = int(m.group(1)), int(m.group(2))
        failed = f'; {total - ok:,} failed, reasons at the end of {log}' if ok < total else ''
        note = f'{ok:,} of {total:,} shaders ready{failed}'
        PROG.end(f'{ok:,} of {total:,} shaders ready' + (f' ({int(c.group(1)):,} from the cache)' if c else '') + failed)
    else: PROG.end()
    if not seen: shader_regressions(py, repo, sh / 'cache', chain, logs)   # before the key: a stopped build is checked again on the next run
    if key:
        for k in (kmac,) if mac else (kf, kmac) if keep_mac else (kf,): k.write_text(f'{key}\n{note}\n')
    if not seen:   # last: a build that stopped before here keeps the old libraries and cache entries
        prune_stale_chains(sh, chain); prune_shader_cache(sh, chain); pack_libraries(sh, chain, None if mac else pull)
        if mac: rmtree(sh / 'partial')   # the whole library replaces a partial one; partial-keys.txt stays
    return out


def check_metal(py, repo, env):
    """An NVN game's shaders are built only with the Metal compiler they were tested with (dmsh_chain.lock), unless ALLOW_OTHER_METAL: checked before the shader steps, so a Mac with
    another Xcode stops in seconds instead of after the shader split. Issue #242 reported corruption with an untested compiler; its role remains unconfirmed"""
    if ALLOW_OTHER_METAL: return   # the shader step says which compiler it used (own_compilers.py's note line)
    r = subprocess.run([py, str(repo / 'dm/gfx/tools/own_compilers.py'), '--check-metal'], capture_output=True, text=True, env=env, errors='replace')
    if r.returncode == 0: return
    msg = (r.stdout.strip() or r.stderr.strip() or f'own_compilers.py --check-metal exit code {r.returncode}').removeprefix('[RED] ')
    raise BuildError(msg.replace('set DM_ALLOW_OTHER_METAL=1 (the NSP script\'s --allow-other-metal)', 'ALLOW_OTHER_METAL = True (--allow-other-metal)'))


def shader_regressions(py, repo, lib_dir, chain, logs):
    """dm/gfx/tools/shader_regressions.py on the Mac libraries (dmsh_build.py writes its build report there in an iOS build too): shaders that translated in this game's previous build
    and fail with this chain stop the build, unless ALLOW_SHADER_REGRESSIONS. The chain is one for every game, so a change can break a game its author never ran (GitHub issue #272)"""
    log = logs / 'shader_regressions.log'
    if not (shader_lib.view(lib_dir / f'b-{chain}') / '_b_build.json').is_file(): return   # a full dmsh_build.py run always writes it
    r = subprocess.run([py, str(repo / 'dm/gfx/tools/shader_regressions.py'), str(lib_dir), chain], capture_output=True, text=True, errors='replace')
    log.write_text(r.stdout + r.stderr)
    if r.returncode == 0: return
    if r.returncode != 1: raise BuildError(f'shader_regressions.py exit code {r.returncode} (full output: {log})')
    lines = r.stdout.splitlines()
    prev = re.search(r'against (b-\w+)', r.stdout)
    msg = f'{len(lines) - 1} shaders that translated in {prev.group(1) if prev else "the previous build"} fail in b-{chain}: a change to the translation chain broke them'
    PROG.say('\n'.join(lines[1:11]) + (f'\n... {len(lines) - 11} more in {log}' if len(lines) > 11 else ''))
    if ALLOW_SHADER_REGRESSIONS: say(f'[RED] {msg}; going on (ALLOW_SHADER_REGRESSIONS)'); return
    raise BuildError(f'{msg} (list: {log}). Please report it at https://github.com/seedds/nxnative/issues with that list; '
                     'ALLOW_SHADER_REGRESSIONS = True (--allow-shader-regressions) packs the app without those shaders')


def layer_shaders(py, repo, api, name, roots, sh, key, logs, env, mac):
    """What build_vk_shaders and build_gl_shaders share: dm/gfx/tools/<api>_shaders.py on the roots → sh/<api>/ for the Mac and, unless mac (a Mac-only build), sh/ios/<api>/ for the iPhone.
    Returns the folder the host (DM_MSL_DIR) or build_ipa.sh (MSL_IOS) takes the libraries from. key: the inputs' fingerprint (None never reuses). The iOS libraries' key is sh/ios/<api>.key,
    the Mac's sh/<api>.key, which an iOS build writes too (it builds both), so a Mac-only build is reused after either; an iOS build after a Mac-only one translates again.
    Otherwise the old output is deleted first, so a failed run leaves nothing for build_ipa.sh or the host to pick up; the translator's temporary files go to WORK (on the NSP folder's volume)"""
    ios = sh / 'ios'; out = sh if mac else ios
    kf, kmac, lib, log = ios / f'{api}.key', sh / f'{api}.key', out / api / f'{api}_shaders.metallib', logs / f'{api}_shaders.log'
    note = next((n for k in ((kmac, kf) if mac else (kf,)) if (n := reused(k, key)) is not None), None) if key else None
    if lib.is_file() and note is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return out
    PROG.set(step='translating')
    rmtree(kmac); rmtree(sh / api)
    if not mac: rmtree(kf); rmtree(ios / api)
    run([py, repo / f'dm/gfx/tools/{api}_shaders.py', *roots, sh] + ([] if mac else ['--ios', ios]), log, env=dict(env, TMPDIR=env['WORK']))
    if not lib.is_file(): raise BuildError(f'no {"Mac" if mac else "iOS"} {name} shader library in {lib.parent}')
    m = re.search(r'macosx: (\d+)' if mac else r'iphoneos: (\d+)', log.read_text(errors='replace'))
    note = f'{int(m.group(1)):,} shaders for the Mac' + ('' if mac else ' and iOS') if m else ''
    if key:
        for k in (kmac,) if mac else (kf, kmac): k.write_text(f'{key}\n{note}\n')
    PROG.end(note)
    return out


def build_vk_shaders(py, repo, roots, sh, env, logs, g, reuse=True, mac=False, provisional=False):
    """A Vulkan game's SPIR-V shaders (roots: the RomFS, and a run-time capture folder) → one Metal library each for the Mac (sh/vk/) and the iPhone (sh/ios/vk/, what build_ipa.sh takes from MSL_IOS;
    not with mac), replacing the NVN split and chain build (layer_shaders). Reused when the roots (paths and sizes), the tools, the profile and the compilers are as at the last successful run (reuse False: never)"""
    PROG.stage('Vulkan shaders', step='checking the inputs')
    key = fingerprint(*[('stat' if provisional else 'tree', r) for r in roots], ('files', tool_files(repo)), ('files', profile_files(g)), *toolchain(repo, env), env=env) if reuse else None
    return layer_shaders(py, repo, 'vk', 'Vulkan', roots, sh, key, logs, env, mac)


def build_gl_shaders(py, repo, roots, sh, env, logs, g, reuse=True, mac=False, provisional=False):
    """An OpenGL game's GLSL sources (roots: the RomFS, main, a run-time capture folder) → one Metal library each for the Mac (sh/gl/) and the iPhone (sh/ios/gl/, what build_ipa.sh takes from MSL_IOS;
    not with mac), replacing the NVN split and chain build, as build_vk_shaders does for a Vulkan game; glslang (glslangValidator) compiles the GLSL to SPIR-V for spirv-cross"""
    PROG.stage('GL shaders', step='checking the inputs')
    if not shutil.which('glslangValidator', path=env['PATH']): raise BuildError('glslangValidator not found (brew install glslang): it compiles an OpenGL game\'s GLSL shaders')
    key = fingerprint(*[('stat' if provisional else 'tree', r) if Path(r).is_dir() else ('files', [r]) for r in roots], ('files', tool_files(repo)), ('files', profile_files(g)), *toolchain(repo, env),
                      ('cmd', ['glslangValidator', '--version']), env=env) if reuse else None
    return layer_shaders(py, repo, 'gl', 'GL', roots, sh, key, logs, env, mac)


class IpaWatch:
    """Progress of build_ipa.sh, from its step lines ("[time] generate Gen/ …", "xcodebuild …", "pack IPA …", then pack_ipa.py's "pack: N entries, X bytes") and the files it writes:
    the CompileC lines in xcodebuild's build.log (one per source file: Gen/src, Gen/main/main.c, Gen/embed.S and the app's Sources), then the bytes of the archive pack_ipa.py is
    writing (<IPA>.partial, renamed at the end). Its two halves are the stages "app build" and "IPA pack" """
    XCODE = {'CompileC': 'compiling', 'Ld': 'linking', 'CpResource': 'copying resources', 'CompileAssetCatalogVariant': 'asset catalog', 'ProcessInfoPlistFile': 'Info.plist'}

    def __init__(self, repo, w, out_ipa):
        self.ios, self.o, self.part = repo / 'dm/ios', w / 'build/ios/pkg', out_ipa.with_name(out_ipa.name + '.partial')
        self.last = out_ipa.stat().st_size if out_ipa.is_file() else None   # the last IPA's size: the expected size of the new archive
        self.kept = 0
        self.state, self.t, self.off, self.compiled, self.what, self.total, self.most, self.lock = None, 0, 0, 0, '', None, 0, threading.Lock()

    def line(self, line):   # a line of build_ipa.sh's output
        s = re.sub(r'^\[[\d:]+\] ', '', line)
        with self.lock:   # a poll sees a step change whole
            if s.startswith('generate Gen/'): self.state = 'gen'; PROG.set(step='generating sources')
            elif s.startswith('[RED] pipeline archive:'): say(s)   # the build goes on without one, and only this says so
            elif s.startswith('pipeline archive for'): PROG.set(step='pipeline archive (metal-tt)')
            elif s.startswith('xcodebuild'):
                units = len(os.listdir(self.ios / 'Gen/src')) + 2 + sum(1 for p in (self.ios / 'Sources').iterdir() if p.suffix in ('.c', '.m'))   # + Gen/main/main.c and Gen/embed.S
                self.state, self.t = 'xcode', time.time(); PROG.set(step='xcodebuild', done=0, total=units, unit='files', est=True)   # an incremental build compiles fewer
            elif s.startswith('pack IPA'):
                try: self._poll()   # the rest of build.log (xcodebuild writes it in big blocks)
                except OSError: pass
                PROG.end(f'{self.compiled:,} file{"" if self.compiled == 1 else "s"} compiled')
                self.state, self.total, self.most = 'list', None, 0; PROG.stage('IPA pack', poll=self.poll, step='listing the files')
            elif self.state == 'list' and (m := re.match(r'pack: (\d+) entries, (\d+) bytes', s)):   # the files' bytes: the archive's size when nothing deflates, else the last IPA's size is closer
                self.state, self.total = 'keep', self.last or int(m.group(2)); PROG.set(step='packing', unit='B', note=f'{int(m.group(1)):,} entries')
            elif self.state == 'keep' and (m := re.match(r'pack: keeping (\d+) bytes', s)):
                self.kept = int(m.group(1)); self.state = 'pack'; self.most = 0; self.total = max(0, self.total - self.kept)

    def poll(self):   # from the poll thread
        with self.lock: return self._poll()

    def _poll(self):
        state = self.state
        if state == 'xcode':
            log = self.o / 'build.log'
            if log.stat().st_mtime < self.t - 1: return None   # still the last build's log
            if log.stat().st_size < self.off: self.off = self.compiled = 0
            with open(log, 'rb') as f: f.seek(self.off); chunk = f.read()
            chunk = chunk[:chunk.rfind(b'\n') + 1]; self.off += len(chunk)
            for ln in chunk.split(b'\n'):
                w = ln.split(b' ', 1)[0].decode(errors='replace')
                if w == 'CompileC': self.compiled += 1
                self.what = self.XCODE.get(w, self.what)
            r = {'done': self.compiled, 'note': self.what}
        elif state == 'pack':
            try: self.most = max(self.most, self.part.stat().st_size - self.kept)   # never backwards: the archive is renamed at the end
            except OSError: pass
            r = {'done': self.most, 'total': self.total, 'est': True}
        else: return None
        return r


def env_file_pairs(path):   # an environment file's switches (one KEY=VALUE per line, # starts a comment), read as the iOS app reads them (dm/ios/Sources/main.m)
    out = {}
    try: text = Path(os.path.expanduser(path)).read_text(errors='replace')
    except OSError as e: raise BuildError(f'cannot read the environment file (--env-file / DM_ENV_FILE): {e}') from e
    for ln in text.splitlines():
        k, eq, v = ln.strip().partition('=')
        if k.strip() and eq and not k.startswith('#'): out[k.strip()] = v
    return out


def build_env_file(work, env_file, motion):
    """The environment file the build bakes in (the iOS app's bundled dm/env.txt, the Mac app's, the Mac run command's switches): the --env-file / DM_ENV_FILE one as it is, or, for a build
    without motion (MOTION / --no-motion), its switches plus DM_MOTION=0 in <id>_build/build_env.txt"""
    if motion: return env_file
    pairs = env_file_pairs(env_file) if env_file else {}
    pairs['DM_MOTION'] = '0'
    f = work / 'build_env.txt'
    f.write_text(''.join(f'{k}={v}\n' for k, v in pairs.items()))
    return str(f)


def mac_host(py, repo, work, env, logs, g):
    """The stage "Mac host" of a Mac-only build or a Mac app (PLATFORM 'mac', 'mac-app'): the Mac host for the profile (build_mac_host, into <id>_build/tools/dmhost, where the run-time captures
    build it too; DM_GAMES_DIR / DM_GAMES_FIRST in env pick a provisional or derived profile) and the button-icon font the host gives a game for the Nintendo extension shared font
    (make_ext_font.py, into <id>_build/font, made again when the tool changed). Returns (host, font folder)"""
    PROG.stage('Mac host', step='building')
    host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
    PROG.set(step='button-icon font')
    font = work / 'font'; ttf, kf = font / 'nintendo_ext.ttf', font / 'nintendo_ext.key'
    key = fingerprint(('files', [repo / 'dm/tools/make_ext_font.py']))
    if not (ttf.is_file() and reused(kf, key) is not None):
        rmtree(kf); font.mkdir(parents=True, exist_ok=True)
        run([py, repo / 'dm/tools/make_ext_font.py', ttf], logs / 'make_ext_font.log', env=env)
        kf.write_text(f'{key}\n')
    PROG.end(str(host))
    return host, font


def mac_run(work, game, patched, host, font, msl, aoc, gfx, lang, env, logs, g, env_file='', partial=False, parts=None, jit=False):
    """The command that runs the game in a window with sound after a Mac-only build (PLATFORM 'mac'; docs/development.md, Build by hand (Mac), step 5): its save and temporary storage in
    <id>_build/run (kept from run to run), the extracted RomFS, the Mac shader libraries (msl), the DLC, the profile's shader map (a host outside dm/host does not find it beside itself;
    c1_full.tsv and cb_extent.tsv are read beside it), the game language, and the switches of an environment file (DM_ENV_FILE / --env-file). partial (--seen-shaders): the pipeline bases go into an
    archive of their own, as the prebuild's stamp does not name the library (the whole library would find its bases done), and the command keeps its log, which the next partial build reads.
    parts: a dict that gets the command's parts (a multi-program application's programs.tsv, mac_programs). jit: what the game's nn::jit plugin makes in play goes to jit_capture/play, for the iPhone's image (capture_jit)"""
    rd = work / 'run'
    for d in ('save', 'temp'): (rd / d).mkdir(parents=True, exist_ok=True)
    renv = {'DM_ROOT': work, 'DM_ROMFS': rfs.source(game), 'DM_SAVE': rd / 'save', 'DM_TEMP': rd / 'temp', 'DM_MSL_DIR': msl, 'DM_FONT_DIR': font}
    if aoc: renv['DM_AOC_DIR'] = aoc
    if gfx == 'nvn': renv['DM_NVN_SHMAP'] = g.table('shader_map.tsv')
    if partial:
        renv['DM_PSO_ARCHIVE'] = rd / 'pso_archive_partial'
        (rd / 'pso_archive_partial/prebuilt.tsv').unlink(missing_ok=True)   # the keys may have grown: the prebuild walks the pairs again, and skips what the archive holds
    if jit: (game / 'jit_capture/play').mkdir(parents=True, exist_ok=True); renv['DM_JIT_CAPTURE'] = game / 'jit_capture/play'
    if PROGRAM_INDEX and g.get('program_of', ''): renv['DM_PROGRAM'] = PROGRAM_INDEX   # the host over the application's programs runs this one (main.c dm_game_cur)
    renv.update(DM_LANG=lang, DM_WINDOW=1, DM_AUDIO=1)
    if env_file: renv.update(env_file_pairs(env_file))
    if gfx == 'nvn' and g.table('shader_pairs.tsv').is_file():
        if not g.table('pso_pairs.tsv').is_file() and (work / 'shader/pso_pairs.tsv').is_file(): renv['DM_PSO_PAIRS'] = work / 'shader/pso_pairs.tsv'   # split_shaders' copy
        # The pipeline prebuild (docs/development.md, Hybrid pipelines): the pulling base of every shader pair into run/pso_archive, so the first run's draws have stand-ins instead
        # of waiting for the Metal compiler. The host's tool mode skips it when pso_archive/prebuilt.tsv matches (chain version, depth mode, pair list): Breath of the Wild's
        # 12,755 pairs took about 3 minutes on an M2 Max the first time. It takes the depth mode from the profile's depth_mode key (default gl, NVN's); with a wrong one the game builds its bases again in the app
        PROG.add(1); PROG.stage('Pipeline prebuild', step='building')
        penv = run_saves.cloud_off(dict(env, **{k: str(v) for k, v in renv.items()}, DM_PSO_PREBUILD='tool'))   # the environment file's DM_SAVE_SYNC is for the player's runs
        if g.get('depth_mode', 'gl') == 'zero': penv['DM_PSO_PREBUILD_DEPTH'] = 'zero'   # a game that sets the near = 0 depth mode (the profile says so: the host learns it only at run time, which for a provisional or derived profile includes its check boots)
        run([host], logs / 'pso_prebuild.log', env=penv, cwd=rd)
        PROG.end()
    q = lambda v: shlex.quote(str(v))
    if parts is not None: parts.update(env=renv, host=host, arg=patched, cwd=rd)
    return f'cd {q(rd)} && ' + ' '.join(f'{k}={q(v)}' for k, v in renv.items()) + f' {q(host)} {q(patched)}' + (f' 2>&1 | tee {q(rd / "host.log")}' if partial else '')


def other_programs(args, g, first=None):
    """A game of several programs (the profile's key programs, Super Mario 3D All-Stars) is one package: its first program's build packs each of the others, each from its own profile into its own
    build folder (main with in_programs: it stops where the package would be made, and returns what the package takes). {program index: that}, empty for a game of one program"""
    global PROGRAM_INDEX
    runs = {}
    for n, _ in g.pairs('programs') if g.get('programs', '') and not getattr(args, 'in_programs', None) else ():
        say(f'==== program {n}')
        sub = types.SimpleNamespace(**vars(args)); sub.program, sub.in_programs = int(n), first or {}
        runs[int(n)] = main(sub)
    PROGRAM_INDEX = 0
    return runs


def mac_programs(args, g, first):
    """A Mac build of a game of several programs writes programs.tsv, which DM_PROGRAMS names: one host over all of them (game.py header_multi), and oe::ExecuteProgram ends the running program and
    starts the host again on the named one (hle_oe.c). As on Horizon the programs share one save. Returns the file, or None for a game of one program"""
    runs = other_programs(args, g, first)
    if not runs: return None
    runs = {0: first, **runs}
    tsv = Path(first['cwd']).parent / 'programs.tsv'
    rows = ['# The programs of this multi-program application on the Mac (build_ipa_from_nsp.py): index, the host (exe), rewritten modules (arg), run folder (cwd) and environment; DM_PROGRAMS names this file (hle_oe.c)']
    for n, r in runs.items():
        kv = {k: v for k, v in r['env'].items() if k not in ('DM_WINDOW', 'DM_AUDIO')}   # the run command's own switches, which every program inherits
        kv['DM_SAVE'] = first['env']['DM_SAVE']
        rows.append('\t'.join([str(n), f'exe={first["host"]}', f'arg={r["arg"]}', f'cwd={r["cwd"]}'] + [f'{k}={v}' for k, v in kv.items()]))
    tsv.write_text('\n'.join(rows) + '\n')
    return tsv


MTL_ARCH_M = '''#import <Metal/Metal.h>
int main(void) { @autoreleasepool { id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) return 1; printf("%s\\n", d.architecture.name.UTF8String); return 0; } }
'''


def mac_gpu_arch(work, env, logs):
    """This Mac's GPU architecture as Metal names it (MTLDevice.architecture, e.g. applegpu_g14s for an M2 Max): the archive file the host opens in an app's pso/ (pso_bundle_open)"""
    src, exe = work / 'tools/mtl_arch.m', work / 'tools/mtl_arch'
    if not (exe.is_file() and src.is_file() and src.read_text() == MTL_ARCH_M):
        src.parent.mkdir(parents=True, exist_ok=True); src.write_text(MTL_ARCH_M)
        run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', src, '-o', exe], logs / 'mtl_arch.log', env=env)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    arch = r.stdout.strip()
    if r.returncode or not re.fullmatch(r'applegpu_\w+', arch): raise BuildError(f'cannot tell this Mac\'s GPU architecture ({(r.stderr or r.stdout).strip() or r.returncode}); --gpu names it, none builds the app without a pipeline archive')
    return arch


def mac_pso(py, repo, work, lib, g, gpus, env, logs):
    """PLATFORM 'mac-app', an NVN game with shader pairs: its pipeline archive made offline for this Mac's GPU (dm/gfx/tools/pso_offline.py --platform macos, metal-tt), which the app carries as
    Resources/pso and the launcher hands to the host as DM_PSO_BUNDLE, as the iOS app does (docs/development.md, The pipeline archive made offline): the app's first start builds no base,
    and the bases (about 615 MB for Breath of the Wild) do not go into the player's ~/Library/Application Support. gpus: --gpu / PSO_GPUS (empty: this Mac's GPU; none: no archive, the app
    builds the bases on its first start). Made again only when its inputs change (pso_offline.py's key). Returns the folder, or None"""
    if gpus == 'none': say('pipeline archive: none (--gpu none): the app builds the pipeline bases on its first start'); return None
    if gpus and not re.fullmatch(r'(applegpu_)?g\w+(,(applegpu_)?g\w+)*', gpus): raise BuildError(f'--gpu / PSO_GPUS takes GPU architectures such as g14s, or none: {gpus}')
    try: here = mac_gpu_arch(work, env, logs)
    except BuildError:
        if not gpus: raise
        here = None
    if gpus and here and here not in {x if x.startswith('applegpu_') else 'applegpu_' + x for x in gpus.split(',')}:   # PSO_GPUS is shared with the IPA (g18p is an iPhone's)
        say(f'pipeline archive: --gpu / PSO_GPUS ({gpus}) does not name this Mac\'s GPU ({here}): on this Mac the app builds its pipeline bases on its first start (empty = this Mac\'s GPU)')
    arch = gpus or here
    PROG.add(1); PROG.stage('Pipeline archive', step=f'{arch} (metal-tt, minutes when its inputs changed)')
    out = work / 'pso/mac'
    cmd = [py, repo / 'dm/gfx/tools/pso_offline.py', '--game', g.id, '--msl', lib, '--out', out, '--work', work / 'pso/mac-work', '--gpu', arch, '--platform', 'macos']
    if not g.table('pso_pairs.tsv').is_file() and (work / 'shader/pso_pairs.tsv').is_file():   # the split's copy, as the app gets it (--pso-pairs)
        cmd += ['--pairs', g.table('shader_pairs.tsv'), '--pairs', work / 'shader/pso_pairs.tsv']
    if run(cmd, logs / 'pso_offline_mac.log', env=env, fatal=False):   # an optimization: the app builds the bases on its first start without it
        shutil.rmtree(out, ignore_errors=True)
        PROG.end('failed, the app is packed without one', warn=True)
        say(f'[RED] pipeline archive: pso_offline.py failed (see {logs / "pso_offline_mac.log"}); the app is packed without one and builds its pipeline bases on its first start (--gpu none skips this step)')
        return None
    if not any(out.glob('*.mtl4a')):   # no pair (a header-only table, #338), or every eligible pair declares function constants (the host specializes them on the device, rest_pairs.tsv)
        PROG.end('no base to bundle'); say('pipeline archive: none (no shader pair, or every eligible one has function constants): the app builds what it needs on its first start'); return None
    PROG.end(f'{arch}, {amount(tree_size(out)[1], "B")}')
    return out


def mac_app_name(name):   # the app's file name: the game name with the characters Finder shows otherwise (':' and '/') as ' - '
    return re.sub(r'\s*[:/]\s*', ' - ', name).strip(' .-') or 'nxnative'


def mac_app(py, repo, out, app, work, game, patched, host, font, msl, aoc, gfx, lang, g, name, version, bundle_id, icon, sign, env, logs, env_file='', gpus='', programs=None, control=None):
    """The last stage of PLATFORM 'mac-app': pack the Mac app (dm/mac/pack_app.py) from what a Mac-only build made: the Mac host and font (mac_host), the packed version's RomFS and rewritten
    modules, the Mac shader libraries (msl: <shader>/cache, whose b-<chain> goes in, or a Vulkan or GL game's <shader>, whose vk / gl goes in), the DLC, the profile's tables (with the shader pairs
    the pipeline prebuild reads), the pipeline archive made offline for this Mac (mac_pso; gpus: --gpu / PSO_GPUS), the launcher settings and an environment file.
    The save and the log go to the player's Library, keyed by the folder's title ID (app); a new app replaces the old one only when it is complete. programs: a game of several programs, the others' files
    (other_programs), which go in beside the first's as one app. control: the control.nacp that lists the game's languages (languages_control), by default the packed version's"""
    lib = msl / (f'b-{chain_of(repo)}' if gfx == 'nvn' else 'vk' if gfx == 'vulkan' else 'gl')
    pso = mac_pso(py, repo, work, lib, g, gpus, env, logs) if gfx == 'nvn' and g.table('shader_pairs.tsv').is_file() else None   # a stage of its own, before the app's
    PROG.stage('Mac app', step='starting')
    # The launcher settings the IPA carries (dm/games/settings.py), without the iPhone's own switches: the app's settings window and Mac front ends show them (docs/frontends.md)
    settings = work / 'app/launcher.json'; settings.parent.mkdir(parents=True, exist_ok=True)
    run([py, repo / 'dm/games/settings.py', g.id, patched, control or game / 'control.nacp', settings], logs / 'mac_settings.log', env=dict(env, GFXAPI=gfx, SETTINGS_PLATFORM='mac', DM_LANG=lang))
    cmd = [py, repo / 'dm/mac/pack_app.py', out, '--host', host, '--patched', patched, '--romfs', rfs.source(game), '--msl', lib, '--tables', g.table('shader_map.tsv').parent,
           '--font', font, '--title-id', f'{app:016X}', '--name', name, '--bundle-id', bundle_id, '--lang', lang, '--sign', sign or '-']
    split_pairs = work / 'shader/pso_pairs.tsv'   # the split's copy of pso_pairs.tsv, for a profile without the table (as mac_run's DM_PSO_PAIRS)
    for opt, v in (('--version', version), ('--category', g.get('app_category')), ('--marker', g.get('romfs_marker')), ('--aoc', aoc), ('--icon', icon),
                   ('--pso', pso), ('--pso-pairs', split_pairs if gfx == 'nvn' and not g.table('pso_pairs.tsv').is_file() and split_pairs.is_file() else None),
                   ('--env-file', Path(os.path.expanduser(env_file)).resolve() if env_file else None), ('--settings', settings)):
        if v: cmd += [opt, v]
    for n, r in (programs or {}).items(): cmd += ['--program', f'{n}\t{r["patched"]}\t{r["romfs"]}\t{r["msl"]}\t{r["tables"]}\t{r["html"]}']
    if (game / 'html').is_dir(): cmd += ['--html', game / 'html']
    unchanged = False
    def line(ln):
        nonlocal unchanged
        if ln.startswith('step: '): PROG.set(step=ln[6:].strip())
        if ln.startswith('app: ') and ln.endswith(', reused (inputs unchanged)'): unchanged = True
    run(cmd, logs / 'mac_app.log', env=env, on_line=line)
    if not (out / 'Contents/MacOS/dmhost').is_file(): raise BuildError(f'no app at {out}')
    PROG.end(f'{out.name}, ' + ('reused (inputs unchanged)' if unchanged else amount(tree_size(out)[1], 'B')))


def find_repo(here):   # this file lives in the repository's scripts/; walk up to the level that has dm/tools/nx_extract/Program.cs
    for d in here.parents:
        if (d / 'dm/tools/nx_extract/Program.cs').exists(): return d
    raise BuildError(f'cannot find the repository root above {here} (no dm/tools/nx_extract)')


def parse_args(argv):
    """Command-line options: when given they override "Settings"; F5 in Spyder passes none, so "Settings" is used as is. -h shows the help"""
    import argparse
    ap = argparse.ArgumentParser(prog='build_ipa_from_nsp.py',
        description='Build an unsigned IPA or prepare an Xcode project from a game\'s NSPs; a game with no profile is surveyed and gets a provisional profile.',
        epilog='example: python3 scripts/build_ipa_from_nsp.py ~/nsp/"Super Mario Bros Wonder" -k ~/keys/prod.keys')
    ap.add_argument('nsp_dir', nargs='?', help='folder holding one game\'s NSPs, or its files as a dump (scripts/pack_dump.py lists the layout), or one NUP container (a .nup file; the file itself may be given) (default: the NSP_DIR setting, else asked)')
    ap.add_argument('-k', '--keys', help='path of prod.keys, not needed for a dump folder or a NUP (default: the PROD_KEYS setting, else asked)')
    ap.add_argument('--title-id', help='the game to pack when the folder holds several (a cartridge dump with two games): its title ID, 16 hex digits (default: the TITLE_ID setting)')
    ap.add_argument('-p', '--package', help='version packed into the IPA, e.g. v327680 / v0 (default: the newest in the folder)')
    ap.add_argument('-o', '--out', help='output IPA, or app with --platform mac-app (default: <the profile\'s ipa>, or <game name>.app, in the NSP folder)')
    ap.add_argument('--app-name', help='app name on the home screen (default: the game name in the base NSP)')
    ap.add_argument('--bundle-id', help='the app\'s bundle ID (default: saved Xcode ID when preparing again, else org.nxnative.<base game title ID in lowercase>)')
    ap.add_argument('--romfs', choices=('bundle', 'separate'), help='bundle = the game files go into the IPA (default: the ROMFS setting, bundle); separate = they stay out of the IPA and the Xcode app: the build is small and fast, and the RomFS image '
                    'is copied to the phone once per game version with the scripts/push_romfs.sh command printed at the end (the app refuses an image of another version)')
    ap.add_argument('--shader-files', action=argparse.BooleanOptionalAction, help='an NVN game\'s shader library goes into the IPA as its separate files, not as one file, for install tools that cannot copy '
                    'a large file (AltServer: 2 GiB; the game files then need --romfs separate too) (default: the SHADER_FILES setting, off)')
    ap.add_argument('--prepare-xcode', action='store_true', help='prepare a per-game Xcode project and add it to the shared workspace instead of packing an IPA (requires keeping extracted files)')
    ap.add_argument('--platform', choices=('ios', 'mac', 'mac-app'), help='ios = the IPA (or the Xcode project); mac = for development: build only what running the game on this Mac needs (the Mac shaders and the Mac host, '
                    'no iOS shaders, Xcode project or IPA), keep the extracted files and print the command that runs the game; mac-app = what mac builds, then a self-contained, double-clickable '
                    'Mac app of the game (default: <game name>.app in the NSP folder, -o names another) (default: the PLATFORM setting, ios)')
    ap.add_argument('--icon', help='the app icon, a JPEG or PNG of any size (default: the ICON setting, else the game\'s icon from its control data; a dump has none, a NUP may have none)')
    ap.add_argument('--fallback-icon', help='the app icon only when --icon / ICON is not given and the game has none of its own (a dump, a NUP without one), a JPEG or PNG of any size: a game with its own icon '
                    'keeps it, so a front end can pass a stand-in without knowing what the game carries (default: the FALLBACK_ICON setting, else such an app\'s icon is blank)')
    ap.add_argument('--mac-sign', help='--platform mac-app: the codesign identity for the app, e.g. "Developer ID Application: <name> (<team>)" (default: the MAC_SIGN setting, else ad-hoc)')
    ap.add_argument('--team-id', help='Apple developer team ID (default: TEAM_ID setting, DM_TEAM_ID environment, or saved Xcode selection)')
    ap.add_argument('--xcode-workspace', help='workspace collecting prepared games (default: NxNative.xcworkspace beside the NSP folder; use with --prepare-xcode)')
    ap.add_argument('-l', '--language', help='game language, a code or a name: ja, fr, de, es, it, nl, pt, ru, ko, zh-Hans, zh-Hant, en-GB, fr-CA, es-419, pt-BR, Japanese, ... (default: the LANGUAGE setting, else English); '
                    'it must be one the game supports, which the console lists')
    ap.add_argument('--keep-extracted', action=argparse.BooleanOptionalAction, help='keep the extracted game files after the IPA is built, for the next build (default: the KEEP_EXTRACTED setting, on)')
    ap.add_argument('--work-dir', help='the game\'s build folder, used as it is (default: the WORK_DIR setting, else <title ID>_build in the NSP folder); the extracted files, shader cache and temporary files live there, and the IPA or Mac app defaults to the folder holding it')
    ap.add_argument('--env-file', help='environment switches for a diagnostic build (one KEY=VALUE per line)')
    ap.add_argument('--gpu', help='NVN games: GPU architectures the app gets its pipeline archive for, made on this Mac with metal-tt, comma-separated: g18p (A19 / A19 Pro), or the name a device\'s log gives when the app has no archive for its GPU; '
                    'none = no archive, the device builds the bases on its first start (default: the PSO_GPUS setting; when that is empty, a profile with pso_mtl4 on uses its pso_gpu key or g18p, and other profiles use none). '
                    'With --platform mac-app: the Mac app\'s archive, by default for this Mac\'s GPU (none: the app builds the bases on its first start)')
    ap.add_argument('--seen-shaders', action=argparse.BooleanOptionalAction, help='--platform mac or mac-app: build only the shaders the profile\'s playtests used (its shader_seen.tsv) and those the last runs\' logs reported missing, not the whole map: '
                    'minutes instead of hours, but areas the list does not cover draw without the rest until this runs again (default: the SEEN_SHADERS setting, off)')
    ap.add_argument('--show-fps', action=argparse.BooleanOptionalAction, help='show the game\'s frame rate on the iPhone, frames on screen per second at the top left (default: the SHOW_FPS setting, on)')
    ap.add_argument('--motion', action=argparse.BooleanOptionalAction, help='pass a gyro controller\'s motion (on iOS also the phone\'s own) to games that read it; --no-motion builds with DM_MOTION=0, for when motion misbehaves (default: the MOTION setting, on)')
    ap.add_argument('--survey-only', action='store_true', help='a game with no profile: only the survey, no provisional profile and no IPA')
    ap.add_argument('--program', type=int, default=None, help='a multi-program application (Super Mario 3D All-Stars): pack program N of the NSPs as a game of its own, title ID = the application\'s + N (default: the PROGRAM_INDEX setting, 0; a game whose profile lists programs packs them all as one from program 0: this is for bring-up)')
    ap.add_argument('--allow-damaged', action='store_true', help='a damaged NSP (files failing the NCA hash check), or files whose bytes do not start as their names promise (a .zs file that is no zstd stream): write them as read and go on instead of stopping')
    ap.add_argument('--allow-shader-regressions', action='store_true', help='shaders that translated in the game\'s previous build and fail with the current translation chain: '
                    'pack the app without them instead of stopping')
    ap.add_argument('--allow-other-metal', action='store_true', help='an NVN game: build its shaders with this Mac\'s Metal compiler when it is not the one they were tested with '
                    '(dm/gfx/tools/dmsh_chain.lock, Xcode 27.2) instead of stopping; such shaders can render wrong pictures')
    ap.add_argument('--profile-out', help='where the provisional profile goes (default: <build folder>/profile: <NSP folder>/<title ID>_build/profile, or under --work-dir)')
    ap.add_argument('--provisional', action='store_true', help='build from a provisional profile even when dm/games has one for the game (tests the guesses)')
    ap.add_argument('--no-heap-check', action='store_true', help='a provisional profile: skip the heap check and the address region check on the Mac (the alias / heap split stays a guess); a derived profile: skip the address region check')
    ap.add_argument('--progress', choices=('auto', 'bar', 'lines', 'json', 'off'), help='stage lines on the console: bar = one line per stage, redrawn in place; lines = a plain line every BEAT seconds; json = newline-delimited JSON events (no prompts); off = only each stage\'s start and end '
                    '(default: the PROGRESS setting; auto = bar in a terminal or Spyder, lines when the output goes to a file or a pipe)')
    return ap.parse_args(argv)


def icon_file(value, option):
    """The image an icon option or setting names, None when it is empty. Checked when the build starts, not after the extraction: build_ipa.sh would only log a missing one"""
    if not value: return None
    p = Path(os.path.expanduser(value)).resolve()
    if not p.is_file(): raise BuildError(f'no such icon file ({option}): {p}')
    return p


def app_icon(given, spare, *dirs):
    """The image the app is given, and whether it is the fallback: --icon / ICON (given), else the game icon nx_extract saved from the control data (English, icon.jpg; in dirs, the packed version's
    first, then the base game's), which a dump never has and a NUP may lack, else --fallback-icon / FALLBACK_ICON (spare), which never replaces a game's own. None without any"""
    own = next((d / 'icon.jpg' for d in dirs if (d / 'icon.jpg').exists()), None)
    return given or own or spare, bool(spare and not (given or own))


def say_game(app, name, version, bundle_id, out, icon):
    """Which game the app is, once its name, version and bundle ID are settled (before the shader stages): the event game for a front end (json mode; the base game's title ID as DMTitleID has it,
    out = the IPA or Mac app, None for an Xcode project; icon = the image the app is given: --icon / ICON, else the extracted icon.jpg, else --fallback-icon / FALLBACK_ICON; None without one), then the message for people"""
    PROG.event('game', title_id=f'{app:016X}', name=name, version=version, bundle_id=bundle_id, out=str(out) if out else None, icon=str(icon) if icon else None)
    say(f'app name: {name}; version: {version}; bundle ID: {bundle_id}' + (f'; app: {out}' if out and out.suffix == '.app' else ''))


def say_push_romfs(image, bundle_id):
    """The command that puts a --romfs separate build's game files on the phone. The image is the extraction's RomFS.img, which stays where it is while the game files are unchanged (the extraction is reused),
    so the command is needed once per game and version, and again only when the image changed: the app compares its identity (size and a hash of the index) with the one it was built for and says so when it differs"""
    say('the game files are not in this build. Copy them to the phone once, with the app installed (it is needed again only when the image changed, a new game version or another extraction):')
    PROG.say(f'  scripts/push_romfs.sh <device UDID> {bundle_id} "{image}"')
    # the file on disk, as build_ipa.sh's [ -f "$ROMFS" ] decides whether romfs.id is written: rfs.source() is a romfs.Path, whose is_dir() is True for an image's root (GitHub issue #530)
    if not os.path.isfile(image): say('[RED] the extraction holds a RomFS folder, not an image: this build carries no identity for it, and push_romfs.sh packs the folder first (delete extract/ and build again for an image)')


def report_fix_coverage(patched, mods, reg):
    """What the modules of builds the profile does not register kept of their registered builds' cuts: the rewriter's <module>.match.tsv (patch_nso.py) lists the cuts of those
    builds and each function patch's result; a game patch not applied, or registered cuts with no function patch at all, is a red line (the game may stop where they were needed).
    SDK-wide patches (sdk_patches.py) are listed as applied or not, without red: a module of a game without virtual address memory needs none"""
    for m in mods:
        try: rows = [l.split('\t') for l in (patched / f'{m}.match.tsv').read_text().splitlines() if l and not l.startswith('#')]
        except OSError: continue
        known = [r[1:] for r in rows if r[0] == 'registered' and len(r) == 4]
        tried = [r[1:] for r in rows if r[0] == 'patch' and len(r) == 4]
        sdk = [r[1:] for r in rows if r[0] == 'sdk' and len(r) == 4]
        lost = '; '.join(f'{disp} ({bid[:8]}…) has {n} cuts' for bid, disp, n in known)
        if sdk:
            applied, absent = [n for n, offs, _ in sdk if offs != '-'], [n for n, offs, _ in sdk if offs == '-']
            say(f'{m}: SDK-wide patches' + (f' applied: {", ".join(map(repr, applied))}' if applied else '') + ('; ' if applied and absent else '')
                + (f' with no reviewed implementation in this build, so not applied: {", ".join(map(repr, absent))}' if absent else ''))
        if not tried:
            if known and not any(offs != '-' for _, offs, _ in sdk):
                say(f'[RED] {m}: no function patch carries the registered builds\' fixes to this build ({lost}), and the game may stop where they were needed; see {reg.dir / "README.md"}')
            continue
        missing = [(name, why) for name, offs, why in tried if offs == '-']
        if missing: say(f'[RED] {m}: function patches not applied, and the game may stop where they were needed: ' + '; '.join(f'{name!r} ({why})' for name, why in missing)
                        + (f' (registered: {lost})' if lost else ''))
        if len(missing) < len(tried): say(f'{m}: function patches applied: ' + ', '.join(repr(name) for name, offs, _ in tried if offs != '-'))


def report_profile_status(g, reg, pkg_ver, derived, provisional, artifact='IPA', patched=None):
    """Keep build-specific warnings visible for the IPA, the Xcode project and a Mac-only build"""
    disp = g.builds()[pkg_ver]['display'] if derived else ''
    if derived:   # unknown builds do not inherit fixed addresses; opt-in function matches are reported by the rewriter
        known = {(m, b) for row in reg.builds().values() for m, b in row.items() if m != 'display'}
        other = [m for m, b in g.builds()[pkg_ver].items() if m != 'display' and (m, b) not in known]
        registered_builds = ", ".join(f'{v} {b["display"]}' for v, b in reg.builds().items())
        if g.title_id == reg.title_id and g.builds() == reg.builds():
            say(f'this {artifact} uses the registered {pkg_ver} ({disp}) builds and every fix for them, with derived shader tables ({derived})')
        elif g.title_id != reg.title_id and not other and pkg_ver in reg.builds():   # a registered build under another title ID (same_build): every fix applies
            say(f'this {artifact} is game profile {reg.id}\'s {pkg_ver} ({disp}) under title ID {g.title_id:016X}: its settings and every fix for its builds, with shaders split from this title\'s own files')
        else: say(f'[RED] this {artifact} uses {pkg_ver} ({disp}), which game profile {reg.id} was not brought up on ({registered_builds}): '
            'its settings, with this version\'s own shaders' + (f'; {", ".join(other)} {"is a build" if len(other) == 1 else "are builds"} the profile does not register, so the fixes made for '
            'the registered builds\' fixed addresses do not apply there; opt-in function patches apply only on a verified code match (see rewrite logs), and the game may not run as well' if other else '; every module is a registered build'))
        if other and patched: report_fix_coverage(patched, other, reg)
    if provisional:
        say(f'[RED] this {artifact} was built from a provisional game profile and the game may not boot: {g.dir}')
        say(f'  bring-up on the Mac: DM_GAMES_DIR="{g.dir.parent}" make -C dm/host GAME={g.id} dmhost, see docs/agents')


def provisional_version(have, builds, requested, profile_root):
    """Select from actual inputs; a stale survey fails without deleting user data."""
    version = requested or max(have, key=vnum)
    if version not in have:
        raise BuildError(f"requested version {version} is not in the input folder; available: {', '.join(sorted(have, key=vnum))}")
    if version not in builds:
        raise BuildError(f"cached provisional profile describes {', '.join(builds)}, but inputs select {version}. "
                         f"Preserving the existing profile at {profile_root}. Use --profile-out with a new empty folder "
                         "to survey this update, or explicitly select an available registered version with --package. "
                         "Refusing to silently pack an older version")
    return version


def main(args=None):
    args = args or parse_args([])
    PROG.mode = progress_mode(args.progress or PROGRESS)
    global ALLOW_DAMAGED; ALLOW_DAMAGED = args.allow_damaged or ALLOW_DAMAGED
    global PROGRAM_INDEX; PROGRAM_INDEX = args.program if args.program is not None else PROGRAM_INDEX
    global ALLOW_SHADER_REGRESSIONS; ALLOW_SHADER_REGRESSIONS = args.allow_shader_regressions or ALLOW_SHADER_REGRESSIONS
    global ALLOW_OTHER_METAL; ALLOW_OTHER_METAL = args.allow_other_metal or ALLOW_OTHER_METAL
    here = Path(globals().get('__file__') or sys.argv[0]).resolve()
    if sys.version_info < (3, 12): raise BuildError('Direct RomFS image builds require Python 3.12 or newer')
    repo = find_repo(here)
    prepare_xcode = args.prepare_xcode or PREPARE_XCODE
    platform = args.platform or PLATFORM
    if platform not in ('ios', 'mac', 'mac-app'): raise BuildError(f'PLATFORM / --platform must be ios, mac or mac-app: {platform}')
    mac = platform in ('mac', 'mac-app')   # the Mac's shaders and host only: no iOS shaders, Xcode project or IPA (mac_run prints the command; mac-app packs a Mac app, mac_app)
    mac_pack = platform == 'mac-app'
    seen_shaders = SEEN_SHADERS if args.seen_shaders is None else args.seen_shaders
    if seen_shaders and not mac: raise BuildError('--seen-shaders / SEEN_SHADERS builds a partial library of Mac shaders: it needs --platform mac or mac-app')
    if args.mac_sign and not mac_pack: raise BuildError('--mac-sign signs the Mac app: it needs --platform mac-app')
    if mac and prepare_xcode: raise BuildError(f'--prepare-xcode makes an iPhone project: it cannot be combined with --platform {platform}')
    if mac and not apple_silicon(): raise BuildError(f'--platform {platform} runs the game in the Mac host, which runs the game\'s own arm64 code natively: it needs a Mac with Apple silicon, and this Mac has an Intel processor')
    check_native_python()
    keep = KEEP_EXTRACTED if args.keep_extracted is None else args.keep_extracted
    if mac and not keep: say('keeping the extracted game files all the same: the game runs from them on the Mac' + (' and the app is made of clones of them' if mac_pack else '')); keep = True
    if prepare_xcode and not keep: raise BuildError('--prepare-xcode requires --keep-extracted: Xcode builds from the extracted files')
    if args.xcode_workspace and not prepare_xcode: raise BuildError('--xcode-workspace requires --prepare-xcode')
    romfs_mode = args.romfs or ROMFS
    if romfs_mode not in ('bundle', 'separate'): raise BuildError(f'ROMFS / --romfs must be bundle or separate: {romfs_mode}')
    if romfs_mode == 'separate' and mac: raise BuildError(f'--romfs separate keeps the game files off the iPhone app: it cannot be combined with --platform {platform}, which runs from the extracted files')
    if romfs_mode == 'separate' and not keep: raise BuildError('--romfs separate requires --keep-extracted: the image must remain available for the one-time transfer to the phone')
    shader_files = SHADER_FILES if args.shader_files is None else args.shader_files
    if shader_files and (mac or prepare_xcode): raise BuildError('SHADER_FILES / --shader-files shapes the IPA for install tools that cannot copy a large file: it cannot be combined with ' + (f'--platform {platform}' if mac else '--prepare-xcode, which Xcode installs'))
    nsp_dir = Path(os.path.expanduser(args.nsp_dir or NSP_DIR or ask('NSP folder: ', 'the NSP folder'))).resolve()
    if not nsp_dir.exists(): raise BuildError(f'not a folder: {nsp_dir}')
    nup = nup_file(nsp_dir)   # a NUP file given in place of the folder: its folder is the NSP folder (the build folder and the IPA go there)
    if nsp_dir.is_file(): nsp_dir = nsp_dir.parent
    global NSP_FOLDER; NSP_FOLDER = nsp_dir
    dump = 'nup' if nup else 'folder' if dump_folder(nsp_dir) else None   # their files are not encrypted, so they need no keys
    keys = None if dump else Path(os.path.expanduser(args.keys or PROD_KEYS or ask('path of prod.keys: ', 'prod.keys (-k)'))).resolve()
    if keys and not keys.is_file(): raise BuildError(f'no such file: {keys}')
    user_icon = icon_file(args.icon or ICON, '--icon / ICON'); spare_icon = icon_file(args.fallback_icon or FALLBACK_ICON, '--fallback-icon / FALLBACK_ICON')
    sys.path.insert(0, str(repo / 'dm/games')); import game as gamemod   # game profiles
    global pg; import profile_guess as pg   # dm/games: a provisional profile's keys from the game files and from the heap check
    sys.path.insert(0, str(repo / 'dm/tools')); global nacp; import nacp   # the game name in the NSP's control data
    lang_in = args.language or LANGUAGE   # checked against the game's languages once its control data is extracted
    want_lang = language_code(lang_in) if lang_in else 'en-US'
    if not want_lang: raise BuildError(f'unknown game language {lang_in!r}; give one of {language_list([c for c, _ in nacp.LANGUAGES])}, by code or by name')
    env = tool_env()
    env.setdefault('USER', getpass.getuser())   # xcodegen fails without USER (Couldn't find current username)
    env.setdefault('J', str(os.cpu_count() or 8))   # shader_split.py's and dmsh_build.py's workers (their default is 8)
    py = sys.executable
    want = args.title_id or TITLE_ID
    if want and not re.fullmatch(r'[0-9A-Fa-f]{16}', want): raise BuildError(f'--title-id / TITLE_ID takes a title ID, 16 hex digits: {want}')
    if PROGRAM_INDEX and dump: raise BuildError('--program / PROGRAM_INDEX picks a program from a multi-program application\'s NSPs; '
                                                     + ('a NUP' if nup else 'a dump folder') + ' is read as one program\'s files')
    if nup:
        app, fam = scan_nup(nup, int(want, 16) if want else None)
        say(f'a NUP: {nup.name}, {app:016X}, {len(fam["updates"])} update(s), {len(fam["dlc"])} DLC')
        if fam['base'] is None: say(pack_nup.NO_BASE)
    elif dump:
        app, fam = scan_dump(nsp_dir, int(want, 16) if want else None)
        say(f'a dump folder: {app:016X}, {len(fam["updates"])} update(s), {len(fam["dlc"])} DLC')
    else:
        with package_reader(repo, keys, env) as packages:
            app, fam = scan_nsps(nsp_dir, packages.contents, int(want, 16) if want else None)
            packages.check_keys(fam)
        if PROGRAM_INDEX:   # one program of a multi-program application, packed as a game of its own: its title ID names the profile and the build folder
            app += PROGRAM_INDEX; say(f'program {PROGRAM_INDEX} of the application: title ID {app:016X}')
        fam = decompress_nsz(nsp_dir, app, fam, keys, env, work_dir(args))
    force =args.provisional or PROVISIONAL   # a provisional profile even when dm/games has one (testing the guesses)
    g, provisional, shadowed = None if force else gamemod.by_title(app), False, False
    alias = None if force or g else same_build(nsp_dir, app, fam, keys, repo, env)   # a known build under another title ID: packed from a profile derived from that build's
    if alias:
        g = alias[0]; say(f'no game profile for {app:016X}, but its main build {alias[2]} is game profile {g.id}\'s {alias[1]} ({g.get("name", "")}) under another title ID')
    if g is None:   # no game profile: survey, then (unless --survey-only) a provisional profile outside the repository and a best-effort IPA
        missing = [m for m in ('lz4.block', 'capstone') if not _importable(m)]
        if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)}')
        check_capstone()
        pdir = profile_folder(args, nsp_dir, app)
        if not (args.survey_only or SURVEY_ONLY): rmtree(derived_key(pdir / f'{app:016X}'))
        if (pdir / f'{app:016X}' / 'game.tsv').exists() and not (args.survey_only or SURVEY_ONLY):
            say(f'no game profile for {app:016X}: reusing the provisional profile from the last run (delete it to survey again)')
        else:
            facts = survey(nsp_dir, app, fam, keys, repo, py, env)
            if args.survey_only or SURVEY_ONLY: return
            facts['survey'] = str(nsp_dir / f'{app:016X}_survey' / 'survey.md')
            write_profile(pdir / f'{app:016X}', app, facts)
        # the profile folder joins the search path of game.py, which the shader tools, make and build_ipa.sh all use (DM_GAMES_DIR)
        os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
        if force and gamemod.by_title(app):   # dm/games has the game too: every tool (game.py, make, build_ipa.sh) takes the provisional profile first (DM_GAMES_FIRST)
            os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'; shadowed = True
        g, provisional = gamemod.load(f'{app:016X}'), True
        say('[RED] provisional game profile: values guessed from the survey, the game may not boot; bring it up per docs/agents after packing')
    say(f'game profile: {g.id}{" (provisional: " + str(g.dir) + ")" if provisional else ""}')
    if seen_shaders and not g.table('shader_seen.tsv').is_file(): raise BuildError(f'--seen-shaders: game profile {g.id} has no list of the shaders its playtests used ({g.table("shader_seen.tsv")}): build the whole map, without this option')
    builds, need = g.builds(), g.words('shader_versions')
    have, derived = nsp_versions(fam), False
    if provisional:   # Do not silently pin a cached survey to an older input version.
        pkg_ver = provisional_version(have, builds, args.package or PACKAGE, g.dir.parent)
    else:   # the newest version in the folder, or the one asked for. The profile is used as it is for a version it registers when every version its shader tables were made from is there;
        # any other version (no update, another update) is packed from a profile derived from it (derive_profile), with its own shaders
        pkg_ver = args.package or PACKAGE or max(have, key=vnum)
        if pkg_ver not in have: raise BuildError(f'the version to pack (-p / PACKAGE) is not in the folder, which holds {", ".join(sorted(have, key=vnum))}')
        if pkg_ver not in builds:
            registered_builds = ", ".join(f'{v} {b["display"]}' for v, b in builds.items())
            derived = f'{pkg_ver} is not a version game profile {g.id} registers ({registered_builds})'
        elif not all(v in have for v in need): derived = f'the folder has no {", ".join(v for v in need if v not in have)}, whose files game profile {g.id}\'s shader tables were made from'
        if alias and not derived: derived = f'title {app:016X} is not game profile {g.id}\'s title {g.title_id:016X}'
        if derived: say(f'{derived}: packing {pkg_ver} from a profile derived from it')
    versions = [pkg_ver] if derived else need + ([pkg_ver] if pkg_ver not in need else [])
    work = build_folder(args, nsp_dir, g.id)
    out_ipa = Path(os.path.expanduser(args.out or OUT_IPA)).resolve() if (args.out or OUT_IPA) else work.parent / g.get('ipa')   # by default next to the build folder (the NSP folder, beside the NSPs and <id>_build/, unless --work-dir moved it)
    if shadowed and not (args.out or OUT_IPA): out_ipa = out_ipa.with_name(out_ipa.stem + '-provisional.ipa')   # never over the IPA of the game's real profile
    if mac_pack and (args.out or OUT_IPA) and out_ipa.suffix != '.app': raise BuildError(f'the Mac app (-o / OUT_IPA) must end in .app: {out_ipa}')
    project = work / 'xcode/DmIOS.xcodeproj'
    workspace = Path(os.path.expanduser(args.xcode_workspace or XCODE_WORKSPACE)).resolve() if (args.xcode_workspace or XCODE_WORKSPACE) else nsp_dir.parent / 'NxNative.xcworkspace'
    if prepare_xcode and workspace.suffix != '.xcworkspace': raise BuildError('--xcode-workspace must end in .xcworkspace')
    saved_signing = {}
    if prepare_xcode:
        sys.path.insert(0, str(repo / 'dm/ios/tools')); import xcode_projects
        try:
            saved_signing = xcode_projects.signing_settings(project)
            # Preserve the user's selections when migrating the earlier single generated project.
            legacy_config = repo / 'dm/ios/Gen/dm.xcconfig'
            if not saved_signing and legacy_config.exists() and f'/gen/{g.id}/' in legacy_config.read_text():
                saved_signing = xcode_projects.signing_settings(repo / 'dm/ios/DmIOS.xcodeproj')
        except ValueError as e: raise BuildError(str(e)) from e
    say('Mac app: the Mac shaders and the Mac host, packed into a Mac app; no iOS shaders, Xcode project or IPA' if mac_pack else 'Mac-only build for development: the Mac shaders and the Mac host, no iOS shaders, Xcode project or IPA' if mac else f'Xcode project: {project}' if prepare_xcode else f'output IPA: {out_ipa}')
    say(f'work folder (extracted game files in extract/, {"kept for the next build" if keep else "deleted after the IPA is built"}): {work}')
    xlate = repo / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
    # DM_GAME picks the profile; DM_ROOT / WORK point at <id>_build: the tools' default paths and temporary directories all stay inside it
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'))
    logs = work / 'logs'
    check_prereqs(env, xlate, mac)
    picks = pick_nsps(fam, have, versions)

    ex = work / 'extract'; ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    gb = (sum(fam['nup'].tree_bytes(pack_nup.romfs_of(u or b, b)) for b, u in picks.values()) if nup else sum(tree_size((u or b) / 'romfs')[1] for b, u in picks.values()) if dump else extract_bytes(picks, False)) / 2**30 + 6   # every merged image is complete, including on APFS; shader cache + slack
    if free_gb(work) < gb and not all((ex / v).exists() for v in versions): raise BuildError(f'less than {gb:.0f} GB free on the volume of {work}')
    heapck = provisional and HEAP_CHECK and not args.no_heap_check and not heap_checked(g)   # the heap check, when the game needs one (once per provisional profile: it stamps the profile)
    PROG.plan(len(versions) + bool(fam['dlc']) + 4 - (prepare_xcode or (mac and not mac_pack)))   # prepare Xcode (a Mac-only build: the Mac host) replaces app build + IPA pack with one stage (a Mac app: Mac host + Mac app)
    nx = None if dump else find_nx_extract(repo, env)
    dirs = {}
    for ver in versions:   # each merged version streams directly into its own image
        b, u = picks[ver]
        dirs[ver] = extract_nup(ex, ver, u or b, b, app, fam['nup']) if nup else extract_dump(ex, ver, u or b, app) if dump else extract(nx, keys, ex, ver, b, u, None, logs, g, app)
        if not nup and not dump: extract_html(nx, keys, dirs[ver], b, u, logs, app)
    check_formats(dirs)
    check_versions_aarch64(dirs, logs, g, nacp.name(dirs[versions[0]] / 'control.nacp') or f'{app:016X}')   # before build_mismatch, derive_profile and build_sources read the modules
    bad = [] if derived else [f'{v}: {x}' for v in versions for x in build_mismatch(dirs[v], v, g)]   # every module's build ID must be the one the profile registers for its version
    if bad and provisional: raise BuildError(f'not the builds provisional game profile {g.id} registers: {"; ".join(bad)}')
    if bad:   # files of other builds under a registered version's tag: packed like any other version
        derived = f'not the builds game profile {g.id} registers: {"; ".join(bad)}'
        say(f'{derived}: packing {pkg_ver} from a profile derived from it')
        versions, dirs = [pkg_ver], {pkg_ver: dirs[pkg_ver]}
    reg = g   # the profile as registered (a derived build reports what it lacks against it)
    if derived:   # the derived profile goes where a provisional one would, and every tool (game.py, make, build_ipa.sh) takes it first (DM_GAMES_FIRST)
        pdir = profile_folder(args, nsp_dir, app)
        disp = derive_profile(g, pdir / g.id, pkg_ver, dirs[pkg_ver], g.words('modules'), app)
        os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
        os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'
        g = gamemod.load(g.id)
        say(f'derived game profile for {pkg_ver} ({disp}): {g.dir}')
    sv = versions if derived else need   # the versions whose files the shaders come from
    game = dirs[pkg_ver]
    # how the shaders are got follows from the packed version's files (profile_guess.py shader_sources): the graphics API, and the shaders made only at run time
    src = pg.build_sources(game / 'ExeFS', rfs.source(game), g.modules_in(game / 'ExeFS'))
    gfx = src['graphics'][0]   # nvn, vulkan (Hades: the host's Vulkan layer, SPIR-V shaders) or gl (Digimon Story Cyber Sleuth: the host's GL layer, GLSL sources)
    glslc = gfx == 'nvn' and src['glslc_capture'][0]   # shaders the game compiles at run time, captured in the Mac host (capture_glslc)
    vkcap = gfx == 'vulkan' and src['vk_capture'][0]   # SPIR-V shaders the game generates at run time, captured in the Mac host (capture_shaders)
    glcap = gfx == 'gl' and src['gl_capture'][0]   # GLSL the game puts together at run time (LÖVE), captured in the Mac host (capture_shaders)
    uedec = gfx == 'nvn' and src['ue_decode'][0]   # Unreal Engine shaders, Oodle-compressed in the paks' shader code archives, decoded in the Mac host (decode_ue_shaders)
    if gfx == 'vulkan': say(f'Vulkan game ({src["graphics"][1]}): vk_shaders.py translates the shaders instead of the NVN shader chain')
    if gfx == 'gl': say(f'OpenGL game ({src["graphics"][1]}): gl_shaders.py translates the shaders instead of the NVN shader chain')
    if seen_shaders and gfx != 'nvn': raise BuildError(f'--seen-shaders is for NVN games: this one is {gfx}, whose shaders build whole in minutes')
    if gfx == 'nvn': check_metal(py, repo, env)   # before the captures and the shader split, which take minutes
    for on, k in ((glslc, 'glslc_capture'), (vkcap, 'vk_capture'), (glcap, 'gl_capture'), (uedec, 'ue_decode'), (gfx == 'nvn' and src['oodle_packs'][0], 'oodle_packs'), (gfx == 'nvn' and src['ue_legacy'][0], 'ue_legacy')):
        if on: say(src[k][1])
    PROG.add(glslc + vkcap + glcap + uedec + (gfx == 'nvn'))
    aoc = extract_aoc(nx, keys, repo, work, app, fam, logs)
    dlc_roms = sorted(p for p in aoc.iterdir() if p.is_dir() or p.suffix == '.img') if aoc else []
    base = next((dirs[v] for v in versions if picks[v][1] is None), dirs[versions[0]])   # the folder the base game was extracted to alone
    # the app name and bundle ID both come from the base NSP: the game name in its control data, and a bundle ID from its title ID; a setting / the command line overrides them
    app_name = args.app_name or APP_NAME or nacp.name(base / 'control.nacp') or (g.get('name') if dump else '')   # a dump has no control data, a NUP may have no NACP
    if not app_name and not (mac and not mac_pack): raise BuildError(f'no game name in the base extraction ({base / "control.nacp"}; an older extraction did not save it: delete extract/ and run again, or give --app-name)')
    bundle_id = args.bundle_id or BUNDLE_ID or saved_signing.get('PRODUCT_BUNDLE_IDENTIFIER') or f'org.nxnative.{app:016x}'
    team_id = args.team_id or TEAM_ID or env.get('DM_TEAM_ID') or saved_signing.get('DEVELOPMENT_TEAM') or '0000000000'
    if prepare_xcode:
        try: xcode_projects.check_bundle_id(workspace, project, bundle_id)
        except ValueError as e: raise BuildError(str(e)) from e
    # the app version is the packed game's display version: the update's control data when an update is packed (nx_extract saves it next to the RomFS), else the base game's
    app_version = nacp.version(game / 'control.nacp') or nacp.version(base / 'control.nacp')
    if mac_pack and not (args.out or OUT_IPA): out_ipa = work.parent / f'{mac_app_name(app_name)}{"-provisional" if shadowed else ""}.app'
    icon, stand_in = app_icon(user_icon, spare_icon, game, base)
    if (not mac or mac_pack) and getattr(args, 'in_programs', None) is None:   # an app is made, once: a program packed with the application's first (in_programs) is part of its app
        if stand_in: say(f'no game icon: the app takes the fallback icon {spare_icon} (--fallback-icon / FALLBACK_ICON)')
        say_game(app, app_name, app_version or '1.0', bundle_id, None if prepare_xcode else out_ipa, icon)
    # the game language: Horizon gives the game one of the languages its control data lists (the packed version's, which an update may extend), so the one packed must be among them
    control = languages_control(game, base)
    have = nacp.languages(control)
    lang = game_language(want_lang, bool(lang_in), have)
    say(f'game language: {lang} ({dict(nacp.LANGUAGES)[lang]})' + (f', the game has no {want_lang}' if lang != want_lang else '')
        + (f'; the game\'s languages: {" ".join(have)}' if have else '; the control data lists no languages (not checked)') + (' (DM_LANG=<code> in the run command changes it)' if mac else ' (DM_LANG=<code> in the app\'s Documents/env.txt changes it)'))

    patched = work / f'patched_{pkg_ver}'
    g = rewrite(py, repo, game / 'ExeFS', patched, env, logs, g, provisional)
    if nd := captured_nros(patched): env = dict(env, DM_NRO_DIR=str(nd))
    chain_header(py, repo, env, logs)   # before any host is built
    if provisional or derived:   # the keys that need the rewritten modules, then (provisional) the heap check (both only change keys the host is built with, not the rewrite or the shader steps)
        provisional_prep(py, repo, env, logs, g)
        g = refine_after_rewrite(g, game, patched, reg if derived else None)
        if provisional and heapck and pg.heap_check_needed(game / 'ExeFS'):
            if apple_silicon(): PROG.add(1); g = heap_check(repo, game, patched, work, env, logs, g)
            else: say('no heap check: it boots the game in the Mac host, which needs a Mac with Apple silicon; the profile keeps its guessed heap keys')
        if HEAP_CHECK and not args.no_heap_check and pg.has_va(game / 'ExeFS') and not region_checked(g, patched) and apple_silicon():   # where its address regions reach (a derived build too: the registered builds' cuts do not reach it)
            PROG.add(1); g = region_check(repo, game, patched, work, env, logs, g)
    observed_glslc = capture_glslc(repo, {v: dirs[v] for v in sv}, pkg_ver, patched, work, env, logs, g) if glslc else None
    if uedec: decode_ue_shaders(repo, {v: dirs[v] for v in sv}, pkg_ver, patched, work, env, logs, g)
    sh = work / 'shader'
    if gfx == 'vulkan':   # the packed version's merged RomFS holds the SPIR-V files the game loads; a capture holds those it generates
        cap = capture_shaders(repo, game, patched, work, env, logs, g, 'vk') if vkcap else None
        ios = build_vk_shaders(py, repo, [rfs.source(game)] + dlc_roms + ([cap] if cap else []), sh, env, logs, g, reuse=not ALLOW_DAMAGED, mac=mac, provisional=bool(provisional or derived))
    elif gfx == 'gl':   # the GLSL sources in the packed version's archives and in its main (the game compiles them at run time; the host finds each by the hash of its text), and a capture of those it puts together
        cap = capture_shaders(repo, game, patched, work, env, logs, g, 'gl') if glcap else None
        ios = build_gl_shaders(py, repo, [rfs.source(game), game / 'ExeFS/main'] + dlc_roms + ([cap] if cap else []), sh, env, logs, g, reuse=not ALLOW_DAMAGED, mac=mac, provisional=bool(provisional or derived))
    else:
        # Trinity packs and legacy Unreal paks: the split decodes them with the packed version's own decoder (Oodle, LZ5), in the Mac host built for the profile
        oodle = (lambda: {'DM_OODLE_HOST': str(build_mac_host(repo, work / 'tools/dmhost', env, logs, g)), 'DM_OODLE_MODULES': str(patched)}) if src['oodle_packs'][0] or src['ue_legacy'][0] else None
        own = bool(provisional or derived)   # the shader tables come from this build's split (the profile's are checked otherwise)
        def derive_shader_tables(reason):   # called only after the strict DLC-only or additive-compute comparison passed
            nonlocal g, derived
            assert g is not None
            derived = reason
            pdir = Path(os.path.expanduser(args.profile_out or PROFILE_OUT)).resolve() if (args.profile_out or PROFILE_OUT) else work / 'profile'
            derive_profile(g, pdir / g.id, pkg_ver, game, g.words('modules'), app, shader_only=True)
            os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
            os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'
            g = gamemod.load(g.id)
            say(f'{derived}: using a shader-only derived profile at {g.dir}; registered module fixes are preserved')
            return g
        derive_dlc = (lambda: derive_shader_tables('the installed DLC shader content differs from the registered tables')) if not own else None
        derive_compute = (lambda: derive_shader_tables('the reader decoded additional compute kernels from this registered build')) if not own else None
        pkey = split_shaders(py, repo, [rfs.source(dirs[v]) for v in sv], [dirs[v] / 'ExeFS/main' for v in sv], sh / 'pkg', env, logs, g, own, oodle, dlc_roms, derive_dlc, derive_compute)
        if observed_glslc is not None: check_glslc_coverage(observed_glslc, g)
        seen = seen_keys(g, sh, seen_logs(work, app)) if seen_shaders else None
        ios = build_shaders(py, repo, sh, xlate, env, logs, g, pkey, mac, seen, None if mac else ios_wants_pulling(g, ios_gpus((args.gpu or PSO_GPUS).strip(), g), prepare_xcode, env))
    motion = MOTION if args.motion is None else args.motion
    jit = uses_jit(game / 'ExeFS')
    if jit and not mac and not mac_pack: capture_jit(py, repo, game, patched, work, env, logs, g)   # the iPhone runs no code made at run time: it serves the plugin's output from an image
    env_file = build_env_file(work, args.env_file or DM_ENV_FILE, motion)
    if not motion: say('motion (gyro): off, DM_MOTION=0 is built in (on the iPhone, DM_MOTION=1 in the app\'s Documents/env.txt turns it back on)')
    if mac_pack:   # a Mac app: the Mac host, then the app packed from it and the build's files
        if getattr(args, 'in_programs', None) is not None:   # a program of the game: the app its first program's build packs takes these
            rmtree(work / 'tmp'); return {'patched': patched, 'romfs': rfs.source(game), 'msl': ios / (f'b-{chain_of(repo)}' if gfx == 'nvn' else 'vk' if gfx == 'vulkan' else 'gl'), 'tables': g.table('shader_map.tsv').parent, 'html': game / 'html', 'gfx': gfx}
        host, font = mac_host(py, repo, work, env, logs, g)
        programs = other_programs(args, g)
        if not icon: say(('no game icon: the NUP carries none' if nup else 'no game icon: a dump folder has no control data') + '; the app\'s icon will be blank; --icon / ICON or --fallback-icon / FALLBACK_ICON gives it one' if dump else '[RED] no game icon in the extraction (an older extraction did not save it: delete extract/ and run again); the app\'s icon will be blank')
        mac_app(py, repo, out_ipa, app, work, game, patched, host, font, ios, aoc, gfx, lang, g, app_name, app_version, bundle_id, icon, args.mac_sign or MAC_SIGN, env, logs, env_file,
                (args.gpu or PSO_GPUS).strip(), programs, control)
        rmtree(work / 'tmp')
        say(f'done: {out_ipa} ({tree_size(out_ipa)[1] / 2**30:.1f} GB); '
            f'open it from Finder. Its save is in ~/Library/Application Support/nxnative/{app:016X}/, its log in ~/Library/Logs/nxnative/{app:016X}.log')
        if seen_shaders: say('a partial shader library: where a shader is missing, the log says so in red and the draw goes without it. Run this script again with --seen-shaders to add what the log names, or without it for the whole map')
        report_profile_status(g, reg, pkg_ver, derived, provisional, 'Mac app', patched)
        return
    if mac:   # a Mac-only build ends with the Mac host and the command that runs the game
        first = getattr(args, 'in_programs', None)   # a program packed with the application's first: its host, built over all of them, and its fonts
        host, font = (first['host'], first['font']) if first else mac_host(py, repo, work, env, logs, g)
        parts = {'font': font}
        cmd = mac_run(work, game, patched, host, font, ios, aoc, gfx, lang, env, logs, g, env_file, partial=seen_shaders, parts=parts, jit=jit)
        rmtree(work / 'tmp')
        if getattr(args, 'in_programs', False): return parts
        programs = None if PROGRAM_INDEX else mac_programs(args, g, parts)
        if programs: cmd = cmd.replace(' && ', f' && DM_PROGRAMS={shlex.quote(str(programs))} ', 1)
        say(f'done: the game runs on the Mac with this command (its save in {work / "run/save"}, kept from run to run; DM_WINDOW=1 opens a window, DM_AUDIO=1 plays sound):')
        PROG.say(cmd)
        if seen_shaders: say(f'a partial shader library: where a shader is missing, the log says so in red and the draw goes without it. Run this script again with --seen-shaders to add what {work / "run/host.log"} names, or without it for the whole map')
        report_profile_status(g, reg, pkg_ver, derived, provisional, 'Mac build', patched)
        return

    if getattr(args, 'in_programs', None) is not None:   # a program of the game: the app its first program's build packs takes these
        rmtree(work / 'tmp'); return {'row': (PROGRAM_INDEX, g.id, patched, rfs.source(game), ios, gfx), 'ex': ex, 'keep': keep}
    programs = other_programs(args, g)
    if programs and romfs_mode == 'separate': raise BuildError('a game of several programs keeps its files in the app (ROMFS / --romfs bundle): Documents holds one RomFS image, one program\'s')
    if not prepare_xcode and free_gb(out_ipa.parent) < 5: raise BuildError(f'less than 5 GB free on the volume of {out_ipa.parent}')
    say('preparing the Xcode project' if prepare_xcode else f'building the IPA → {out_ipa}')
    ipa_env = dict(env, GAME=g.id, W=str(work / 'ipa'), PATCHED=str(patched), ROMFS=str(rfs.source(game)), MSL_IOS=str(ios), GFXAPI=gfx, FONT_PY=py, DM_DISPLAY_NAME=app_name, DM_BUNDLE_ID=bundle_id, AOC=str(aoc or ''), DM_XCODE_DERIVED_DATA=str(work / 'xcode-derived-data'), DM_TEAM_ID=team_id, DM_LANG=lang, CONTROL=str(control))
    if prepare_xcode: ipa_env['DM_XCODE_PROJECT_DIR'] = str(project.parent)
    separate = romfs_mode == 'separate'
    if separate: ipa_env['DM_COPY_ROMFS'] = 'NO'; say('game files: separate (DM_COPY_ROMFS=NO), left out of the app; the image stays at ' + str(rfs.source(game)))
    if shader_files: ipa_env['DM_SHADER_FILES'] = '1'; say('shader library: its separate files (DM_SHADER_FILES=1), for install tools that cannot copy a large file')
    if app_version: ipa_env['DM_APP_VERSION'] = app_version
    if icon: ipa_env['DM_ICON'] = str(icon); say(f'app icon: {icon}')   # app icon (found above)
    else: say(('no game icon: the NUP carries none' if nup else 'no game icon: a dump folder has no control data') + '; the home-screen icon will be blank; --icon / ICON or --fallback-icon / FALLBACK_ICON gives it one' if dump else '[RED] no game icon in the extraction (an older extraction did not save it: delete extract/ and run again); the home-screen icon will be blank')
    if env_file: ipa_env['DM_ENV_FILE'] = str(Path(os.path.expanduser(env_file)).resolve())
    show_fps = SHOW_FPS if args.show_fps is None else args.show_fps
    ipa_env['DM_SHOW_FPS'] = '1' if show_fps else '0'
    gpus = (args.gpu or PSO_GPUS).strip()
    if gpus and not re.fullmatch(r'none|(applegpu_)?g\w+(,(applegpu_)?g\w+)*', gpus): raise BuildError(f'--gpu / PSO_GPUS takes GPU architectures such as g18p,g17p, or none: {gpus}')
    gpus = ios_gpus(gpus, g)
    ipa_env['DM_PSO_GPUS'] = gpus
    if gfx == 'nvn': say(f'pipeline archive: {"none, the device builds the bases on its first start if it takes the Metal 4 path (--gpu g18p builds one)" if gpus == "none" else gpus + " (made on this Mac with metal-tt; --gpu none leaves it out)"}')
    if show_fps: say('frame-rate counter: on (DM_SHOW_FPS=0 in the app\'s Documents/env.txt turns it off)')
    if programs:   # one app holding every program of the game (build_ipa.sh PROGRAMS_TSV)
        tsv = work / 'ipa_programs.tsv'; ipa_env['PROGRAMS_TSV'] = str(tsv)
        rows = [(0, g.id, patched, rfs.source(game), ios, gfx)] + [programs[n]['row'] for n in sorted(programs)]
        tsv.write_text('# program\tprofile\trewritten modules\tRomFS\tiOS shaders\tgraphics API (build_ipa.sh PROGRAMS_TSV)\n' + ''.join('\t'.join(map(str, r)) + '\n' for r in rows))
        say(f'one app holding programs {", ".join(str(r[0]) for r in rows)}')
    if prepare_xcode:
        PROG.stage('prepare Xcode')
        run(['bash', repo / 'dm/ios/tools/build_ipa.sh', '--prepare-xcode'], logs / 'prepare_xcode.log', env=ipa_env)
        try: xcode_projects.register_project(workspace, project, app_name, workspace.with_name(workspace.stem + '-derived-data'))
        except ValueError as e: raise BuildError(str(e)) from e
        PROG.end()
        rmtree(work / 'tmp')
        say(f'ready: {workspace}')
        say(f'game project: {project}')
        say(f'Xcode derived data: {work / "xcode-derived-data"}')
        if separate: say_push_romfs(rfs.source(game), bundle_id)
        say('choose the game scheme in Xcode, select your Team and iPhone or iPad, then Run (Release)')
        report_profile_status(g, reg, pkg_ver, derived, provisional, 'Xcode project', patched)
        return
    watch = IpaWatch(repo, work / 'ipa', out_ipa)
    PROG.stage('app build', poll=watch.poll, step='starting')   # build_ipa.sh's "pack IPA" line starts the stage "IPA pack"
    xcode_data = work / 'ipa/build/ios/pkg/dd'   # one file between builds (tree_tar.py); the app in it goes into the IPA, so it is packed after
    tree_tar.unpack(xcode_data, say)
    run(['bash', repo / 'dm/ios/tools/build_ipa.sh', out_ipa], logs / 'build_ipa.log', env=ipa_env, on_line=watch.line)
    if not out_ipa.is_file(): raise BuildError(f'no IPA at {out_ipa}')
    tree_tar.pack(xcode_data, say)
    PROG.end(f'{out_ipa.name}, {amount(out_ipa.stat().st_size, "B")}')
    rmtree(work / 'tmp')
    if not keep:
        for d in [ex] + [r['ex'] for r in programs.values()]: say(f'deleting the extracted game files: {d}'); rmtree(d)
    say(f'done: {out_ipa} ({out_ipa.stat().st_size / 2**30:.1f} GB)')
    if separate: say_push_romfs(rfs.source(game), bundle_id)
    report_profile_status(g, reg, pkg_ver, derived, provisional, patched=patched)


NSP_FOLDER = None   # main's: the folder whose work folders cli tidies after a build that succeeded


class Stopped(BaseException):   # a SIGTERM or SIGHUP, raised by cli's handler: not an Exception, so no `except Exception` on the way keeps it from cli
    def __init__(self, sig=signal.SIGTERM): super().__init__(sig); self.sig = sig


def _sigterm(sig, frame): raise Stopped(sig)


STOPS = (signal.SIGTERM, signal.SIGHUP)   # the signals cli turns into Stopped


def cli(argv):   # returns the exit code: 0 done, 1 error, 130 interrupted, 143 stopped by SIGTERM, 129 by SIGHUP
    # main points DM_GAMES_DIR (and DM_GAMES_FIRST) at this run's provisional or derived profile for its tools. Spyder runs the next F5 in the same process, where the game lookup
    # (game.py by_title) then found that profile and took it for a registered one: ASTRAL CHAIN's second run skipped every provisional step and stopped with no dmsh_chain.h
    environ = dict(os.environ)
    # A SIGTERM (a front end's cancel) ends the build as an interrupt does: the running tool's process group stopped (run), the stage line ended, exit code 143. So does a SIGHUP
    # (the terminal closed): the shell sends it to its job, which the tool's group, a session of its own (proc_group), is not in, and its default action ended this script
    # with no cleanup and left the tool running. The handlers are cli's only while it runs, and in the main thread only (Python's rule): Spyder's kernel, which runs the next
    # F5 in the same process, gets its own back
    main_thread = threading.current_thread() is threading.main_thread()
    saved = {s: signal.signal(s, _sigterm) for s in STOPS} if main_thread else {}
    try:
        main(parse_args(argv))
        if NSP_FOLDER:   # a developer's work folder, which a player's NSP folder does not have (tree_tar.py)
            for w in NSP_FOLDER.glob('*-work'):
                if re.fullmatch(r'[0-9A-Fa-f]{16}-work', w.name): tree_tar.pack_quiet(w, say)
    except (BuildError, nso.Error, pack_nup.Error) as e:   # a damaged module, read in this process (the survey, the heap and region checks), or a NUP's version it cannot pack (no RomFS, read for the disk check before extraction) stops the build the same way
        PROG.close(ok=False); PROG.say(f'[RED] {e}'); return 1
    except KeyboardInterrupt:
        PROG.close(ok=False); PROG.say('[RED] interrupted'); return 130
    except Stopped as e:
        PROG.close(ok=False); PROG.say(f'[RED] stopped ({signal.Signals(e.sig).name})'); return 128 + int(e.sig)
    except Exception as e:   # an unexpected error: end the stage line before the traceback (which goes to stderr; json mode also sends the error as a message)
        PROG.close(ok=False)
        if PROG.mode == 'json': PROG.say(f'[RED] {type(e).__name__}: {e}')
        raise
    finally:
        for s, h in saved.items(): signal.signal(s, signal.SIG_DFL if h is None else h)   # first: a SIGTERM or SIGHUP in this cleanup is the caller's again (None: one not set from Python)
        PROG.close(ok=False)   # stop the ticker
        for k in os.environ.keys() - environ.keys(): del os.environ[k]
        os.environ.update(environ)
    return 0


if __name__ == '__main__':
    code = cli(sys.argv[1:])
    if not in_ipython(): sys.exit(code)   # give an exit code in a terminal; do not exit in Spyder (keeps the console clean)
