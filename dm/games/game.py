#!/usr/bin/env python3
# Game profiles: dm/games/<base game title ID>/game.tsv (key <TAB> values..., lines starting with # are comments) + the shader tables (and patches.py, the rewriter's patches of the game's builds) in the same folder. The shader tools, the host build, iOS packaging and the NSP script take each game's constants from here.
# Usage: game.py list (list all profiles); game.py <id> <key> [<default>] (print that key's value, for shell; without a default a missing key is an error); game.py header <id> <out.h> (generate the host's game_profile.h);
#   game.py dir <id> (print the profile folder); game.py link <id> <link> (point a symlink at the profile folder: make cannot take paths with spaces)
#   The port's host program nxhost-run is one program for every profile (RUNTIME_FIELDS below): game.py header --runtime <out.h> (its game_profile.h, the same for every profile);
#   game.py table <out.c> <default id> (the table of every profile in dm/games compiled into it, and the default it starts with: cmake -DGAME); game.py runtime <id> <out.tsv>
#   (one profile's constants as the file nxhost-run --profile <file> reads: a derived or provisional profile, found through DM_GAMES_DIR, runs without compiling anything);
#   game.py runtime --all <folder> (every profile in dm/games, <folder>/<id>.tsv)
#   From Python: import game; g = game.load() (takes DM_GAME; there is no default game: unset is an error that lists the profiles); game.by_title(title ID); g.get / g.words / g.int / g.builds() / g.table(file name)
# DM_GAMES_DIR (folders separated by os.pathsep: ':' on Linux and macOS, ';' on Windows, whose paths hold a ':' after the drive letter; the NSP script joins it so) adds profile
#   folders outside the repository, searched after dm/games: the NSP script writes a provisional profile for a game without one into
#   <NSP folder>/<id>_build/profile/<id>/ and points DM_GAMES_DIR at <id>_build/profile, so the shader tools, the host build and build_ipa.sh all find it.
#   DM_GAMES_FIRST=1 searches DM_GAMES_DIR before dm/games (the NSP script's --provisional: a provisional profile for a game that has one in dm/games, to test the guesses)
import os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


class Game:
    def __init__(self, d):
        self.dir = Path(d).resolve(); self.id = self.dir.name; self.rows = []
        for line in (self.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.lstrip().startswith('#'): continue
            c = line.split('\t'); self.rows.append((c[0].strip(), [v.strip() for v in c[1:]]))

    def values(self, key):   # every row of this key (keys such as build repeat)
        return [v for k, v in self.rows if k == key]

    def get(self, key, default=None):
        r = self.values(key)
        if r: return '\t'.join(r[0])
        if default is None: raise KeyError(f'{self.dir / "game.tsv"} has no key {key}')
        return default

    def words(self, key, default=None): return self.get(key, default).split()
    def int(self, key): return int(self.get(key), 0)
    def table(self, name): return self.dir / name

    @property
    def title_id(self): return int(self.get('title_id'), 16)

    def builds(self):
        """{version: {'display': display version, module name: build ID, ...}}"""
        out = {}
        for ver, disp, *mods in self.values('build'):
            out[ver] = dict([('display', disp)] + [tuple(m.split('=', 1)) for m in mods])
        return out

    def pairs(self, key):   # "a=b c=d" → [(a, b), (c, d)]
        return [tuple(w.split('=', 1)) for w in self.words(key)]


def extra_dirs():   # profile folders outside the repository (DM_GAMES_DIR, os.pathsep between them as the NSP script joins them), searched after dm/games
    return [Path(p) for p in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if p]


def search_dirs():   # dm/games, then DM_GAMES_DIR; DM_GAMES_FIRST=1 the other way round
    return extra_dirs() + [HERE] if os.environ.get('DM_GAMES_FIRST') else [HERE] + extra_dirs()


def find_dir(name):   # the profile folder of a title ID (the first of search_dirs that has it); a path is taken as is
    if os.sep in str(name): return Path(name)
    return next((d / name for d in search_dirs() if (d / name / 'game.tsv').exists()), HERE / name)


def load(name=None):
    name = name or os.environ.get('DM_GAME')
    if not name:   # there is no default game
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'[RED] DM_GAME is not set: pick a game profile by base game title ID: {have}')
    d = find_dir(name)
    if not (d / 'game.tsv').exists():   # profile folders are named after the base game's title ID (the old names smbw / fe_engage are gone)
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'[RED] no game profile {d / "game.tsv"} (DM_GAME / GAME take the base game title ID: {have})')
    return Game(d)


def all_games():
    out, seen = [], set()
    for root in search_dirs():
        if not root.is_dir(): continue
        for d in sorted(root.iterdir()):
            if (d / 'game.tsv').exists() and d.name not in seen: seen.add(d.name); out.append(Game(d))
    return out


def by_title(tid):
    return next((g for g in all_games() if g.title_id == tid), None)


def by_build(main_id):   # the profile whose build rows register this main build ID (a release of the same build under another title ID: the NSP script's same_build)
    return next((g for g in all_games() if any(b.get('main') == main_id for b in g.builds().values())), None)


def constants(g):
    """The profile constants the host uses, checked: {key: value} in game_profile.h's order. The key is the macro's name after DM_GAME_ in lower case, the value an int,
    a str, a list (modules: the names; display_versions: (main build ID, display version) pairs) or None (shared_font_ttf, nvn_api_major / nvn_api_minor: the
    profile has no such key, so the host decides by the SDK version). header() writes them as macros (Apple: one host per game), runtime() and table() as the port's
    profile file and compiled-in table (one host for every game: RUNTIME_FIELDS)"""
    mods = g.words('modules')
    vers = [(b['main'], b['display']) for b in g.builds().values() if 'main' in b]
    pi = g.get('present_interval', 'game')   # present interval: game = as the game sets it (as on the Switch), display = one frame per display tick
    if pi not in ('game', 'display'): sys.exit(f'{g.id}: present_interval must be game or display: {pi}')
    ft = g.get('fps_target', '0')   # frame rate cap of the rewriter's frame step (FRAME_STEP in the profile's patches.py: Tears of the Kingdom 1.4.3, Breath of the Wild 1.9.0) or of the engine's own frame limit (hle_fpslock.c): 0 = the game's own step, 30 / 60
    if ft not in ('0', '30', '60'): sys.exit(f'{g.id}: fps_target must be 0, 30 or 60: {ft}')
    fh = g.get('fps_hold', 'on')   # whether the frame step may hold a frame longer than the cap's vsyncs when frames come late (hle_nvn.c fstep_tick): on / off
    if fh not in ('on', 'off'): sys.exit(f'{g.id}: fps_hold must be on or off: {fh}')
    ps = g.get('present_snap', 'off')   # present snapshots by default (nvn_mtl.m mtl_snap_on; DM_PRESENT_SNAP and the iOS launcher's Frame buffering override it): on / off
    if ps not in ('on', 'off'): sys.exit(f'{g.id}: present_snap must be on or off: {ps}')
    p4 = g.get('pso_mtl4', 'off')   # render pipelines through MTL4Compiler on iOS by default, with the pipeline archive and hybrid pipelines (on by default on the Mac; DM_PSO_MTL4 overrides): on / off
    if p4 not in ('on', 'off'): sys.exit(f'{g.id}: pso_mtl4 must be on or off: {p4}')
    cp = g.get('core_preempt', 'off')   # same-core preemption approximation (hle_thread.c): on / off
    if cp not in ('on', 'off'): sys.exit(f'{g.id}: core_preempt must be on or off: {cp}')
    aba = g.get('alloc_big_align', 'off')   # nn::mem::StandardAllocator::Allocate aligns blocks of 64 KB or more to 64 KB (hle_os.c; Vanillaware's memory pool storage): on / off
    if aba not in ('on', 'off'): sys.exit(f'{g.id}: alloc_big_align must be on or off: {aba}')
    sf = g.get('shared_font', 'bfttf')   # what the shared-font address points at (hle_sdk16.c): bfttf = 8-byte header + TTF, ttf = the TTF alone
    if sf not in ('bfttf', 'ttf'): sys.exit(f'{g.id}: shared_font must be bfttf or ttf: {sf}')
    pl = g.get('pad_layout', 'simple')   # the virtual gamepad's first layout (DmGamepad.m; afterwards whatever the player switched to in game is kept): simple = 2D, full = 3D (D-pad, right stick, L3 / R3),
    if pl not in ('simple', 'full', 'touch'): sys.exit(f'{g.id}: pad_layout must be simple, full or touch: {pl}')   # touch = Tap (no control: every touch goes to the game's touch screen)
    fbs = g.words('feedback_snap', '')   # fragment shader key prefixes whose same-pass feedback sampling gets a snapshot first (nvn_mtl.m fb_prepare); none by default
    if any(not re.fullmatch(r'[0-9a-f]{8,16}', k) for k in fbs): sys.exit(f'{g.id}: feedback_snap takes fragment shader keys (8-16 lowercase hex digits): {fbs}')
    rcd = g.get('render_cond', 'off')   # NVN conditional rendering evaluated at replay, timestamp reports with counter 0 (nvn_mtl.m OP_RENDER_COND): on / off
    if rcd not in ('on', 'off'): sys.exit(f'{g.id}: render_cond must be on or off: {rcd}')
    tfu = g.get('tex_flush_upload', 'off')   # textures written by the CPU into a pool and published with nvnMemoryPoolFlushMappedRange are uploaded like packaged ones (hle_nvn.c): on / off
    if tfu not in ('on', 'off'): sys.exit(f'{g.id}: tex_flush_upload must be on or off: {tfu}')
    tdu = g.get('tex_dirty_upload', 'off')   # with tex_flush_upload: a registered texture the CPU writes after init without any flush is uploaded too (dirty pages, hle_nvn.c nvn_ft_written): on / off
    if tdu not in ('on', 'off') or (tdu == 'on' and tfu != 'on'): sys.exit(f'{g.id}: tex_dirty_upload must be on or off, and needs tex_flush_upload on: {tdu}')
    tds = g.get('tex_dyn_slots', 'off')   # shaders whose texture handle the translator could not trace to one slot: slot indices in c2, the bound slots in the fallback arrays (nvn_mtl.m): on / off
    if tds not in ('on', 'off'): sys.exit(f'{g.id}: tex_dyn_slots must be on or off: {tds}')
    vcr = g.get('vconv_range', 'off')   # vertex attributes converted on the CPU only over the vertices each draw reads (nvn_mtl.m vc_draw_range): on / off
    if vcr not in ('on', 'off'): sys.exit(f'{g.id}: vconv_range must be on or off: {vcr}')
    mml = g.get('mem_map_limit', 'off')   # MapPhysicalMemory refused beyond the application pool, as on Horizon (hle_svc.c): on / off
    if mml not in ('on', 'off'): sys.exit(f'{g.id}: mem_map_limit must be on or off: {mml}')
    gts = g.get('gpu_timestamps', 'submission')   # counter timestamps (nvn_mtl.m rep_ticks): submission = one time per submission, segment = the GPU start / end of the segment holding the report
    if gts not in ('submission', 'segment'): sys.exit(f'{g.id}: gpu_timestamps must be submission or segment: {gts}')
    om = g.get('op_mode', 'console')   # the operation mode reported at start (hle_oe.c dm_opmode; DM_OPMODE overrides): console = docked, handheld = touch screen in use (Super Mario Galaxy's pointer)
    if om not in ('console', 'handheld'): sys.exit(f'{g.id}: op_mode must be console or handheld: {om}')
    if pl == 'touch' and om != 'handheld': sys.exit(f'{g.id}: pad_layout touch needs op_mode handheld (the touch screen reaches the game only in handheld mode)')
    pm = g.get('perf_mode', 'follow')   # the performance mode reported (hle_oe.c): follow = the operation mode's (Boost docked, Normal handheld), boost = Boost always
    if pm not in ('follow', 'boost'): sys.exit(f'{g.id}: perf_mode must be follow or boost: {pm}')
    dmo = g.get('depth_mode', 'gl')   # the NVN depth mode the game sets, for the pipeline archive made offline (dm/gfx/tools/pso_offline.py; the host follows the game at run time): gl / zero
    if dmo not in ('gl', 'zero'): sys.exit(f'{g.id}: depth_mode must be gl or zero: {dmo}')
    api = g.get('device_api', '55.15')   # the NVN API major.minor version nvnDeviceGetInteger reports (hle_nvn.c)
    if not re.fullmatch(r'\d+\.\d+', api): sys.exit(f'{g.id}: device_api must be major.minor: {api}')
    return {'id': g.id, 'name': g.get('name'), 'program_id': int(g.get('program_id'), 16), 'modules': mods, 'romfs_marker': g.get('romfs_marker'),
            'mem_reserve': g.int('mem_reserve'), 'mem_alias': g.int('mem_alias'), 'mem_stack': g.int('mem_stack'), 'app_pool': g.int('app_pool'),
            'mem_used': int(g.get('mem_used', '0x4000000'), 0),   # used memory reported beside the heap (hle_svc.c GetInfo UsedMemorySize)
            'sys_resource': int(g.get('sys_resource', '0x2000000'), 0),   # the NPDM's system resource size (hle_svc.c GetInfo TotalSystemResourceSize; 0 = no virtual address memory)
            'args': g.get('game_args', ''),   # game arguments when the command line gives none (main.c; Octopath Traveler 0: Unreal's -SwitchVirtualAddressSpaceSize=)
            'env': g.get('game_env', ''),   # KEY=VALUE pairs getenv answers (hle_os.c; Absolum: .NET's DOTNET_GCRegionRange)
            'mem_map_limit': int(mml == 'on'),
            'stdalloc_cap': int(g.get('stdalloc_cap', '0'), 0),   # StandardAllocator's address space with virtual address memory (hle_svc.c); 0 = what the alias region has room for
            'display_versions': vers, 'present_display': int(pi == 'display'), 'core_preempt': int(cp == 'on'), 'alloc_big_align': int(aba == 'on'), 'fps_target': int(ft),
            'fps_hold': int(fh == 'on'), 'present_snap': int(ps == 'on'), 'pso_mtl4': int(p4 == 'on'),
            'shared_font_ttf': int(sf == 'ttf') if g.get('shared_font', '') else None,   # without the key the host decides by the SDK version (hle_sdk16.c)
            'pad_layout': ('simple', 'full', 'touch').index(pl), 'opmode': int(om == 'console'), 'perf_boost': int(pm == 'boost'), 'feedback_snap': ' '.join(fbs),
            'tex_flush_upload': int(tfu == 'on'), 'render_cond': int(rcd == 'on'), 'tex_dirty_upload': int(tdu == 'on'), 'tex_dyn_slots': int(tds == 'on'),
            'gpu_ts_segment': int(gts == 'segment'), 'vconv_range': int(vcr == 'on'),
            'nvn_api_major': int(api.split('.')[0]) if g.get('device_api', '') else None,   # without the key: by the SDK version (hle_nvn.c nvn_api)
            'nvn_api_minor': int(api.split('.')[1]) if g.get('device_api', '') else None}


def source(g):   # the profile folder as the generated files name it
    return f'dm/games/{g.id}' if g.dir.parent == HERE else str(g.dir)


def cq(s):   # a C string literal
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def header(g):
    """the host's game_profile.h: the profile constants the host uses, as macros (Apple's form: the host is compiled for one profile)"""
    c, q = constants(g), cq
    vers = ', '.join('{' + q(b) + ', ' + q(d) + '}' for b, d in c['display_versions'])
    L = [f'// Generated by dm/games/game.py from {source(g)}/game.tsv; do not edit',
         '#pragma once',
         f'#define DM_GAME_ID {q(c["id"])}',
         f'#define DM_GAME_NAME {q(c["name"])}',
         f'#define DM_GAME_PROGRAM_ID 0x{c["program_id"]:016X}ull',
         f'#define DM_GAME_MODULES {{{", ".join(map(q, c["modules"]))}}}',
         f'#define DM_GAME_NMOD {len(c["modules"])}',
         f'#define DM_GAME_ROMFS_MARKER {q(c["romfs_marker"])}',
         f'#define DM_GAME_MEM_RESERVE {c["mem_reserve"]:#x}ull',
         f'#define DM_GAME_MEM_ALIAS {c["mem_alias"]:#x}ull',
         f'#define DM_GAME_MEM_STACK {c["mem_stack"]:#x}ull',
         f'#define DM_GAME_APP_POOL {c["app_pool"]:#x}ull',
         f'#define DM_GAME_MEM_USED {c["mem_used"]:#x}ull',
         f'#define DM_GAME_SYS_RESOURCE {c["sys_resource"]:#x}ull',
         f'#define DM_GAME_ARGS {q(c["args"])}',
         f'#define DM_GAME_ENV {q(c["env"])}',
         f'#define DM_GAME_MEM_MAP_LIMIT {c["mem_map_limit"]}',
         f'#define DM_GAME_STDALLOC_CAP {c["stdalloc_cap"]:#x}ull',
         f'#define DM_GAME_DISPLAY_VERSIONS {vers}',
         f'#define DM_GAME_PRESENT_DISPLAY {c["present_display"]}',
         f'#define DM_GAME_CORE_PREEMPT {c["core_preempt"]}',
         f'#define DM_GAME_ALLOC_BIG_ALIGN {c["alloc_big_align"]}',
         f'#define DM_GAME_FPS_TARGET {c["fps_target"]}',
         f'#define DM_GAME_FPS_HOLD {c["fps_hold"]}',
         f'#define DM_GAME_PRESENT_SNAP {c["present_snap"]}',
         f'#define DM_GAME_PSO_MTL4 {c["pso_mtl4"]}',
         *([f'#define DM_GAME_SHARED_FONT_TTF {c["shared_font_ttf"]}'] if c['shared_font_ttf'] is not None else []),
         f'#define DM_GAME_PAD_LAYOUT {c["pad_layout"]}',
         f'#define DM_GAME_OPMODE {c["opmode"]}',
         f'#define DM_GAME_PERF_BOOST {c["perf_boost"]}',
         f'#define DM_GAME_FEEDBACK_SNAP {q(c["feedback_snap"])}',
         f'#define DM_GAME_TEX_FLUSH_UPLOAD {c["tex_flush_upload"]}',
         f'#define DM_GAME_RENDER_COND {c["render_cond"]}',
         f'#define DM_GAME_TEX_DIRTY_UPLOAD {c["tex_dirty_upload"]}',
         f'#define DM_GAME_TEX_DYN_SLOTS {c["tex_dyn_slots"]}',
         f'#define DM_GAME_GPU_TS_SEGMENT {c["gpu_ts_segment"]}',
         f'#define DM_GAME_VCONV_RANGE {c["vconv_range"]}',
         *([f'#define DM_GAME_NVN_API_MAJOR {c["nvn_api_major"]}', f'#define DM_GAME_NVN_API_MINOR {c["nvn_api_minor"]}'] if c['nvn_api_major'] is not None else []),
         '']
    return '\n'.join(L)


# ---- The port's form: one host program for every profile (nxhost-run, dm/host/CMakeLists.txt; dm/host/game_rt.c reads it) ----
# Each constant is a field of struct dm_game_profile, which the host fills at start from a profile file (runtime(): key<TAB>value rows, --profile / DM_GAME_PROFILE), from
# the table of every profile in dm/games compiled into it (table(), --game <title ID>) or from the profile it was configured with (cmake -DGAME, the default). Kinds:
#   str      any text without a tab or a line break, empty allowed          u64   0x<hex> (or decimal) up to 2^64 - 1
#   flag     0 or 1        fps   0, 30 or 60        pad   0, 1 or 2 (pad_layout: simple, full, touch)
#   optflag  0, 1 or - (not set: the host decides by the SDK version)      optint   a decimal number below 65536, or - (not set)
#   modules  the module names, separated by spaces: 1 to MAX_MODULES       versions   <main build ID>=<display version> pairs, separated by spaces: 0 to MAX_VERSIONS
# The lists' bounds: Horizon's ExeFS holds at most 13 code modules (rtld, main, subsdk0 to subsdk9, sdk); a profile registers each build it was brought up on
# (89 profiles: at most 4 modules and 3 builds today). A profile beyond either bound is refused here and by the host, never cut short
MAX_MODULES, MAX_VERSIONS = 16, 32
RUNTIME_FIELDS = (('id', 'str'), ('name', 'str'), ('program_id', 'u64'), ('modules', 'modules'), ('romfs_marker', 'str'), ('mem_reserve', 'u64'), ('mem_alias', 'u64'),
                  ('mem_stack', 'u64'), ('app_pool', 'u64'), ('mem_used', 'u64'), ('sys_resource', 'u64'), ('args', 'str'), ('env', 'str'), ('mem_map_limit', 'flag'),
                  ('stdalloc_cap', 'u64'), ('display_versions', 'versions'), ('present_display', 'flag'), ('core_preempt', 'flag'), ('alloc_big_align', 'flag'),
                  ('fps_target', 'fps'), ('fps_hold', 'flag'), ('present_snap', 'flag'), ('pso_mtl4', 'flag'), ('shared_font_ttf', 'optflag'), ('pad_layout', 'pad'),
                  ('opmode', 'flag'), ('perf_boost', 'flag'), ('feedback_snap', 'str'), ('tex_flush_upload', 'flag'), ('render_cond', 'flag'), ('tex_dirty_upload', 'flag'),
                  ('tex_dyn_slots', 'flag'), ('gpu_ts_segment', 'flag'), ('vconv_range', 'flag'), ('nvn_api_major', 'optint'), ('nvn_api_minor', 'optint'))


def runtime_constants(g):
    """constants() checked for the port's form: the lists within their bounds, no tab or line break in a value (the profile file's rows could not hold it)"""
    c = constants(g)
    assert [k for k, _ in RUNTIME_FIELDS] == list(c), 'RUNTIME_FIELDS and constants() list different keys'
    if len(c['modules']) > MAX_MODULES: sys.exit(f'{g.id}: {len(c["modules"])} modules, more than the port\'s host takes ({MAX_MODULES}: game.py MAX_MODULES)')
    if len(c['display_versions']) > MAX_VERSIONS: sys.exit(f'{g.id}: {len(c["display_versions"])} build rows with a main build ID, more than the port\'s host takes ({MAX_VERSIONS}: game.py MAX_VERSIONS)')
    for b, d in c['display_versions']:
        if not b or not d or re.search(r'[\s=]', b) or re.search(r'\s', d): sys.exit(f'{g.id}: build {b} display version {d!r}: the port\'s profile file needs a main build ID without "=" or blanks and a display version without blanks')
    for k, v in c.items():
        if isinstance(v, str) and re.search(r'[\t\r\n]', v): sys.exit(f'{g.id}: {k} holds a tab or a line break, which the port\'s profile file cannot carry: {v!r}')
    return c


def runtime_value(kind, v):   # a constant as the profile file writes it
    if v is None: return '-'
    if kind == 'u64': return f'{v:#x}'
    if kind == 'modules': return ' '.join(v)
    if kind == 'versions': return ' '.join(f'{b}={d}' for b, d in v)
    return str(v)


def runtime(g):
    """the port's profile file: the constants as key<TAB>value rows, which nxhost-run takes with --profile <file> or DM_GAME_PROFILE=<file> (dm/host/game_rt.c)"""
    c = runtime_constants(g)
    return '\n'.join([f'# Generated by dm/games/game.py runtime from {source(g)}/game.tsv; do not edit. The game profile\'s constants for the port\'s host program: nxhost-run --profile <this file>',
                      '# (or DM_GAME_PROFILE=<this file>; dm/host/game_rt.c). key<TAB>value; "-" = not set (the host decides by the SDK version)'] +
                     [f'{k}\t{runtime_value(kind, c[k])}' for k, kind in RUNTIME_FIELDS] + [''])


CTYPE = {'str': 'const char *{}', 'u64': 'uint64_t {}', 'flag': 'int {}', 'fps': 'int {}', 'pad': 'int {}', 'optflag': 'int {}', 'optint': 'int {}',
         'modules': 'int nmod; const char *{}[DM_GAME_MAX_MODULES]', 'versions': 'int nversions; struct dm_game_version {}[DM_GAME_MAX_VERSIONS]'}


def runtime_header():
    """the port's game_profile.h (game.py header --runtime): the same DM_GAME_* names as reads of the profile chosen at start, so the host is one program for every profile"""
    L = ['// Generated by dm/games/game.py header --runtime; do not edit. The port\'s form of the game profile (dm/host/CMakeLists.txt): one host program for every profile. The',
         '// constants are the fields of dm_game, which dm/host/game_rt.c fills at start (a profile file, the compiled-in table by title ID, or the default, cmake -DGAME), and each',
         '// DM_GAME_* name of Apple\'s form (game.py header <id>, the host compiled for one profile) reads its field. Apple never includes this file, so DM_GAME_RUNTIME marks',
         '// the port\'s code. Not macros here: shared_font_ttf and nvn_api_major / nvn_api_minor, -1 when the profile does not set them (Apple\'s form leaves those undefined)',
         '#pragma once',
         '#include <stdint.h>',
         '#define DM_GAME_RUNTIME 1',
         f'#define DM_GAME_MAX_MODULES {MAX_MODULES}   // ExeFS holds at most 13 code modules (game.py MAX_MODULES)',
         f'#define DM_GAME_MAX_VERSIONS {MAX_VERSIONS}   // build rows with a main build ID (game.py MAX_VERSIONS)',
         'struct dm_game_version { const char *build, *ver; };   // a main build ID and its display version (the build rows)',
         'struct dm_game_profile {']
    L += [f'    {CTYPE[kind].format(k)};' + ('   // -1: not set' if kind.startswith('opt') else '') for k, kind in RUNTIME_FIELDS]
    L += ['};',
          'extern struct dm_game_profile dm_game;   // the profile of this run (game_rt.c); the default\'s until main picks another',
          '// The fields in order, with their kinds (game.py RUNTIME_FIELDS): game_rt.c reads a profile file by this list',
          '#define DM_GAME_FIELDS(X) \\']
    L += [f'    X({k}, {kind.upper()}) \\' for k, kind in RUNTIME_FIELDS] + ['']
    for k, kind in RUNTIME_FIELDS:
        if kind.startswith('opt'): continue
        L.append(f'#define DM_GAME_{k.upper()} (dm_game.{k})')
        if kind == 'modules': L.append('#define DM_GAME_NMOD (dm_game.nmod)')
        if kind == 'versions': L.append('#define DM_GAME_NVERSIONS (dm_game.nversions)   // the port\'s form only: DM_GAME_DISPLAY_VERSIONS is an array here, not an initializer list')
    return '\n'.join(L + [''])


def table_row(g):   # one profile as a designated initializer of struct dm_game_profile
    c, out = runtime_constants(g), []
    for k, kind in RUNTIME_FIELDS:
        v = c[k]
        if kind == 'str': out.append(f'.{k} = {cq(v)}')
        elif kind == 'u64': out.append(f'.{k} = {v:#x}ull')
        elif kind == 'modules': out.append(f'.nmod = {len(v)}, .{k} = {{{", ".join(map(cq, v))}}}')
        elif kind == 'versions': out.append(f'.nversions = {len(v)}, .{k} = {{{", ".join("{" + cq(b) + ", " + cq(d) + "}" for b, d in v)}}}')
        else: out.append(f'.{k} = {-1 if v is None else v}')
    return '{' + ', '.join(out) + '}'


def table(default):
    """the port's compiled-in table (game.py table): every profile in dm/games (not DM_GAMES_DIR's: the host must not depend on the folders of the machine that built it),
    and dm_game initialised to the default's row, the profile cmake -DGAME named. A default from outside dm/games (DM_GAMES_DIR at configure time) is a row of its own
    after the registered ones, which --game does not search"""
    reg = [Game(d) for d in sorted(HERE.iterdir()) if (d / 'game.tsv').exists()]
    rows = [table_row(r) for r in reg]
    k = next((i for i, r in enumerate(reg) if r.dir == default.dir), None)
    outside = k is None
    if outside: k = len(rows); rows.append(table_row(default))
    L = ['// Generated by dm/games/game.py table from dm/games/*/game.tsv' + (f' and {source(default)}/game.tsv' if outside else '') + '; do not edit. The port\'s compiled-in game',
         '// profiles (dm/host/game_rt.c: nxhost-run --game <title ID>) and the default, cmake -DGAME',
         '#include "game_profile.h"']
    L += [f'#define DM_GAME_ROW_{i} {r}' for i, r in enumerate(rows)]
    L += ['const struct dm_game_profile dm_game_profiles[] = {' + ', '.join(f'DM_GAME_ROW_{i}' for i in range(len(rows))) + '};',
          f'const int dm_game_nprofiles = {len(reg)};   // the registered ones (dm/games), which --game searches',
          f'const int dm_game_default = {k};   // {default.id} ({source(default)})',
          f'const char dm_game_default_from[] = {cq("" if not outside else str(default.dir))};   // the default\'s folder when it is not in dm/games',
          f'struct dm_game_profile dm_game = DM_GAME_ROW_{k};',
          '']
    return '\n'.join(L)


def write_if_changed(out, text):   # make and CMake run the generators every time; an unchanged file must not rebuild the host
    out = Path(out); out.parent.mkdir(parents=True, exist_ok=True)
    if not (out.exists() and out.read_text(encoding='utf-8') == text):
        tmp = out.with_name(out.name + '.tmp'); tmp.write_text(text, encoding='utf-8'); tmp.replace(out)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['list']:
        for g in all_games(): print(f'{g.id}\t{g.title_id:016X}\t{g.get("name", "")}')
    elif len(a) == 3 and a[0] == 'header' and a[1] == '--runtime':   # the port's form, the same for every profile
        write_if_changed(a[2], runtime_header())
    elif len(a) == 3 and a[0] == 'header':
        out, text = Path(a[2]), header(load(a[1])); out.parent.mkdir(parents=True, exist_ok=True)
        if not (out.exists() and out.read_text() == text):   # written only when it changes: make runs this every time (the profile folder can change under the same GAME), and an unchanged header must not rebuild the host
            tmp = out.with_name(out.name + '.tmp'); tmp.write_text(text); tmp.replace(out)
    elif len(a) == 3 and a[0] == 'runtime' and a[1] == '--all':   # every profile in dm/games, <folder>/<id>.tsv (t_game_profile reads them back against the table)
        for d in sorted(HERE.iterdir()):
            if (d / 'game.tsv').exists(): write_if_changed(Path(a[2]) / f'{d.name}.tsv', runtime(Game(d)))
    elif len(a) == 3 and a[0] == 'runtime':
        write_if_changed(a[2], runtime(load(a[1])))
    elif len(a) == 3 and a[0] == 'table':
        write_if_changed(a[1], table(load(a[2])))
    elif len(a) == 2 and a[0] == 'dir':
        print(load(a[1]).dir)
    elif len(a) == 3 and a[0] == 'link':   # link → the profile folder (replaced when it points elsewhere)
        d, ln = load(a[1]).dir, Path(a[2]); ln.parent.mkdir(parents=True, exist_ok=True)
        if not (ln.is_symlink() and Path(os.readlink(ln)) == d):
            if ln.is_symlink() or ln.exists(): ln.unlink()
            ln.symlink_to(d)
    elif len(a) in (2, 3):
        print(load(a[0]).get(*a[1:]))
    else:
        sys.exit(__doc__ or 'usage: game.py list | game.py <id> <key> [<default>] | game.py header <id> <out.h> | game.py header --runtime <out.h> | game.py runtime <id> <out.tsv> | '
                 'game.py runtime --all <folder> | game.py table <out.c> <default id> | game.py dir <id> | game.py link <id> <link>')
