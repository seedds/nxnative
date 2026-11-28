# Generic NSO rewriter: Switch aarch64 module → an image that can execute natively on Apple systems.
# Usage: patch_nso.py <NSO file> <output dir> <module name>
# Rewrites (the Apple kernel modifies x18 and tpidr_el0; Horizon's thread area, svc and timers have no counterpart on Apple):
#   x18 read/write      → thread slot K18 (TSD, via tpidrro_el0)
#   mrs tpidr_el0       → read thread slot KTLS; msr tpidr_el0 → write KTLS
#   mrs tpidrro_el0     → read thread slot KTLR (Horizon ThreadLocalRegion pointer)
#   any of these with x18 as its register → a copy between thread slots, K18 among them (site_slot_copy; the host gate writes K18 for mrs x18, cntpct_el0)
#   svc #n, mrs cntpct_el0 → host gate (gate address taken from thread slot KGATE; once in C, gates.tsv is looked up by return point)
#   sub sp, sp, xN      → xN rounded up to 16 first (Apple's kernel faults on a misaligned sp, Horizon does not; site_sp_align)
#   sub sp, sp, #imm … add sp, sp, #imm with imm not a multiple of 16 → both rounded up to 16, when nothing between them reaches past the frame (odd_sp_frames)
#   ldxr* / ldxp → ldaxr* / ldaxp, stxr* / stxp → stlxr* / stlxp: every exclusive access gets acquire or release ordering (exclusive_order_sites; the games ran on the Switch's Cortex-A57 without it, Apple cores need it)
#   the completer of Trinity's per-frame rendezvous: dmb ish between its flag store and its load of the state word (trinity_completer_sites; exactly one such function in a module, or no change)
#   spin wait loops     → a copy whose back-edges pass a spin gate (the host yields there; a busy poll costs nothing on the Switch, see "Spin waits")
#   Registered cuts (CUTS of the patch files, restricted by NSO build ID, see "Registered patches"): before rewriting, the original bytes are asserted one by one and any mismatch aborts; results are listed in <name>.cuts.tsv
#   Function-matched cuts (MATCH_CUTS of the selected game's patches.py): unknown builds only, unique complete-function and direct-callee fingerprints, then original-byte assertions
#   Registered observation gates (OBS of the patch files, restricted by build ID): one instruction at the entry is replaced with "enter the host observation callback → execute the original instruction → jump back", so diagnostics modify zero code at runtime; listed in <name>.obs.tsv
# The key page only stores "actual key number × 8" at the end of the data segment, and patch code fetches thread slots indirectly through the key page: the loader may use any key number.
# Output: <name>.patched.bin (key page and patch area appended after the image at PATCH_OFF; when that would start beyond 112 MB the patch area sits before the base, see FAR; a text longer than a B reaches adds a second one after the image, see TWO), <name>.fixups.tsv (layout and key page index header), <name>.gates.tsv, <name>.sites.tsv, <name>.cuts.tsv, <name>.obs.tsv, <name>.changed.tsv (the text words whose instructions no longer run as the file has them)
import struct, re, sys, os, glob, shlex, types, importlib.util, multiprocessing, signal
from itertools import combinations
import capstone
if capstone.cs_version()[0] != 5:   # the rewriter uses Capstone 5's names (CS_ARCH_ARM64, ARM64_OP_*), which Capstone 6 renamed: see "Capstone API version" in docs/reference/rewriter.md
    sys.exit(f'[RED] Capstone {capstone.__version__} is unsupported; the rewriter requires Capstone 5.x. Run: {shlex.join([sys.executable, "-m", "pip", "install", "capstone>=5,<6"])}')
from capstone import *
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG
import mod_layout, nso, nso_asm, nso_syms
from nso_asm import *   # the instruction encoders (B, ADRP, LDRX, ...), shared with the per-build patch files

# The NSP script terminates a failed or cancelled rewrite with SIGTERM. Exit through the pool's context manager so it reaps the workers too.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
try: WORKERS = int(os.environ.get('DM_REWRITE_J') or os.cpu_count() or 1)
except ValueError: sys.exit('[RED] DM_REWRITE_J must be a positive integer')
if WORKERS < 1: sys.exit('[RED] DM_REWRITE_J must be a positive integer')

src, outdir, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
# An NSO, or an NRO (a module the game loads at run time with nn::ro, Super Smash Bros. Ultimate's fighter code), rewritten as an NSO of the same layout would be. nso.py checks the file
# against its header before anything is allocated (a damaged module stops here with a [RED] line)
MOD = nso.cli_read(src)
ORIG_TEXT = bytes(MOD.image[:MOD.text_end])   # the text as the file has it, before any relayout, cut or site: <name>.changed.tsv compares against it
# Horizon maps modules at 4 KB, Apple hosts at 16 KB (W^X / signed __TEXT): move writable data up by whole 4 KB pages when it would share the code's 16 KB page (mod_layout.py)
try: MOD, _relayout_D, _relayout_notes = mod_layout.relayout_module(MOD)
except mod_layout.Error as e: sys.exit(f'[RED] {NAME}: {e}')
if _relayout_D: print(f'{NAME}: ' + '; '.join(_relayout_notes))
bss = MOD.bss
# Every jump between the patch area and the code is a B (±128 MB). A patch area appended after the image (with bss) cannot reach the code when it would start beyond 112 MB (leaving 16 MB for the patch area): Metroid Dread's main has about 1.8 GB of bss (the engine's static heap); Engage's main starts its patch area at about 103 MB and keeps it after the image.
# The patch area then sits FRONT bytes before the module base (PATCH_OFF is negative), and the output file holds only text + ro + data (FILE_IMAGE_LEN) followed by the patch area; the loader supplies zero pages for bss and the key page (IMAGE_SIZE is the whole image).
# The key page stays after bss (the patch area reaches it through ADRP, ±4 GB). Modules below the limit produce output byte-identical to before
NEAR_PATCH_OFF = ((((len(MOD.image) + bss + 0x3FFF) & ~0x3FFF) + 0x4000 + 0xFFFF) & ~0xFFFF)   # where an appended patch area would start (as in the else branch below)
FAR, FRONT = NEAR_PATCH_OFF > 0x7000000, 0x2000000
# A text segment that ends within 32 MB of the ±128 MB reach (Monster Hunter Rise: 125.3 MB of text) puts the patch area right before the base instead. Its stubs follow the sites in address
# order, so the last site's stub ends up about as far from it as the text is long. The first pass tries the gap that keeps every site in reach; when the patch area does not fit there
# (Monster Hunter Rise: 3.6 MB), the rewriter runs again with exactly the size it needs (DM_PATCH_FRONT, set only by that rerun) and B() checks every branch
BIG_TEXT = FAR and MOD.text_end + FRONT > 0x8000000 - 0x10000
# A text segment longer than the reach itself (EA SPORTS FC 27: 185.6 MB of text in main) leaves no gap before the base from which a B reaches its end, so the patch area is split in
# two (TWO): the front area before the base takes the sites below SPLIT, the back area right after the image (after bss, the key page and the frame step page, at BACK_OFF) takes
# the others. A site's stubs, and their branches back to it, stay in the area it belongs to; SPLIT keeps 1 MB of margin for a stub's exits past its site (a window's targets, a
# switch's cases). The front area starts at 16 MB; when its sites need more, the rewriter runs again with exactly that size (DM_PATCH_FRONT), and B() checks every branch.
# SPLIT is also no lower than the lowest site the back area reaches with the same margin (BACK_MIN): when ro, data and bss after the text are large (Hello Kitty Party Land: 151.8 MB
# of text, 98 MB more before BACK_OFF), a 16 MB front area would hand the back area sites more than 128 MB below it, so the front area's first pass takes the gap up to BACK_MIN instead
TWO = BIG_TEXT and MOD.text_end > 0x8000000 - 0x10000
# When BACK_MIN is beyond the front area's reach too (GAP: Resident Evil 4 v131072), branches that cannot reach their area go through a hop in function alignment padding.
# The gap's sites are split between the two areas according to how many hop locations each area can reach. Putting them all at the back exhausts its padding on Minecraft 1.26.62.
BACK_MIN, GAP = 0, False   # TWO: BACK_OFF (set below) less the reach, plus the margin
if TWO:
    BACK_MIN = ((((len(MOD.image) + bss + 0x3FFF) & ~0x3FFF) + 0x8000 + 0xFFFF) & ~0xFFFF) - 0x8000000 + 0x100000
    GAP = BACK_MIN >= 0x8000000 - 0x200000
    FRONT = int(os.environ.get('DM_PATCH_FRONT', '0'), 0) or (0x1000000 if GAP else min(0x1000000, (0x8000000 - 0x100000 - BACK_MIN) & ~0xFFFF))
elif BIG_TEXT: FRONT = int(os.environ.get('DM_PATCH_FRONT', '0'), 0) or (0x8000000 - 0x10000 - MOD.text_end) & ~0xFFFF
SPLIT = (0x8000000 - FRONT - 0x100000 if GAP else max(0x8000000 - FRONT - 0x100000, BACK_MIN)) if TWO else None
IMG = MOD.image
if not FAR: IMG.extend(bytes(bss))
TEXT_END = MOD.text_end
RO_OFF, DATA_OFF = MOD.segments[1][0], MOD.segments[2][0]
if FAR:
    FILE_IMAGE_LEN = (len(IMG) + 0x3FFF) & ~0x3FFF
    KEY_PAGE_OFF = (len(IMG) + bss + 0x3FFF) & ~0x3FFF
    IMAGE_SIZE, PATCH_OFF = KEY_PAGE_OFF + 0x4000, -FRONT
    BACK_OFF = (KEY_PAGE_OFF + 0x8000 + 0xFFFF) & ~0xFFFF if TWO else 0   # after the key page and the frame step page FRAME_STEP may take
    assert not TWO or BACK_OFF - 0x8000000 + 0x100000 == BACK_MIN, 'BACK_MIN and BACK_OFF disagree'
else:
    KEY_PAGE_OFF = (len(IMG) + 0x3FFF) & ~0x3FFF
    IMG.extend(bytes(0x4000))
    PATCH_OFF = (len(IMG) + 0xFFFF) & ~0xFFFF
    IMAGE_SIZE = BACK_OFF = 0   # only the layout with the patch area before the base has them

# ---- Registered patches of particular builds, keyed by NSO build ID: dm/tools/sdk_patches.py (Nintendo SDK module builds, which several games can ship) and dm/games/<id>/patches.py
# (a game's own modules: its profile's builds). Every file is loaded, whatever the game, so a module gets the patches of its build wherever it comes from; a build ID claimed twice stops the rewrite.
# A file may define:
#   CUTS = {build ID: [(offset, original bytes, new bytes, reason)]}: before rewriting, the original bytes are asserted one by one and any mismatch aborts (listed in <name>.cuts.tsv)
#   ALIAS_CUTS = {build ID: [offset, ...]}: those of the build's CUTS that exist only to fit an iPhone's small alias region (a MATCH_CUTS rule marks the same with alias_only=True).
#     The rewriter records each applied one as an ALIAS_CUT row in <name>.fixups.tsv, and the Mac loader restores its original bytes at load when the alias region is at least the
#     Switch's 64 GB (loader.c undo_alias_cuts; a smaller DM_ALIAS_SIZE or DM_KEEP_ALIAS_CUTS=1 keeps them)
#   OBS = {build ID: [(offset, number, original instruction word, reason)]}: observation gates. Numbers match OBS_* in the host's hle.h; the original instruction must be position
#     independent (no PC-relative / branch / anything involving x18 or tpidr), asserted offline; when the host has no callback attached the gate block goes in and out once and returns
#   FRAME_STEP = {build ID: function(ctx)}: the frame step stubs of a main ("Frame step in whole vsyncs" below)
#   STUBS = {build ID: function(ctx)}: code a fix needs in the patch area, branched to from its sites ("Stubs" below)
#   MATCH_STUBS = {module: [rule, ...]}: STUBS placed by the same function matcher as MATCH_FRAME_STEP, without a frame-step state page. A rule with fps_required=True disables
#     native engine frame targets unless its stub sets ctx.fps_ready=True after installing every required correction; an unmatched future build keeps its original speed.
#   MATCH_FRAME_STEP = {'main': [{name, size, fingerprints, stub}, ...]}: the frame step stubs of any main without FRAME_STEP, placed by the function they patch (nso_match.locate_stub:
#     one complete unwind function of that size and fingerprint, or no stubs and a red line); stub(ctx) writes its sites at ctx.base + offsets inside that function. Rules are
#     one patch's reviewed implementations (one name). Only the profile selected by DM_GAME participates, as for MATCH_CUTS. A rule's others = {label: {size, fingerprints}}
#     are more functions its stub patches or depends on, found the same way: ctx.others[label] is the start of each, or None (a red line) when it does not match, and the stub leaves that part out
#     An anchored rule {name, calls, find, stub, reviewed} is found instead as the one function that calls the imported symbol calls; find(fn) (nso_match.Function) checks the code there
#     and returns what the stub needs (ctx.found: its sites, registers), or raises MatchError (a red line, no stubs); reviewed lists what find returned on the builds checked by hand. It holds over updates that change the function elsewhere
#   MATCH_CUTS = {module name: [nso_match rule, ...]}: fallback for modules without registered CUTS. Only the profile selected by DM_GAME participates; a derived profile uses
#     the repository's rules, an external-only provisional profile its own. Missing or ambiguous matches are reported and skipped; known builds retain their exact CUTS.
#     A rule of several functions applies whole or not at all. For such a module <name>.match.tsv lists the cuts of the profile's registered builds of it and each patch's result,
#     which the NSP script reports with the build (what an unregistered build lost)
#     A function rule may also check relocated data through check(module, start), raising MatchError when the data differs from the patch's assumptions.
#   sdk_patches.py's MATCH_CUTS are SDK-wide: every game's module of that name whose build has no CUTS, whatever the profile (a build that must keep Nintendo's code registers an
#     empty CUTS list). They are fixes of Nintendo's code that hold for any game on a platform with a small address space; a build without a reviewed implementation of one keeps
#     the original code, logged but not red (games without virtual address memory never reach it). engine_patches.py's MATCH_CUTS are engine-wide in the same way: fixes of
#     middleware code (Unity, Unreal, ...) for every game's module that ships it, logged but not red where nothing matches
#   CAPABILITIES = {build ID: (name,...)}: verified main-build features exposed by fixups metadata to the launcher
#   BINDING = [{what, now, goal, purpose, why[, blocked or kept][, checked]}]: not read here; each patch's binding now and the loosest it could have (dm/tools/docs_patches.py "Binding", docs/agents/game-patches.md)
SHARED = ('sdk_patches.py', 'engine_patches.py')   # rules for every game's module of a kind: Nintendo's SDK, and engine code (their MATCH_CUTS, keyed by module name)
PATCH_FILES = [os.path.join(os.path.dirname(os.path.abspath(__file__)), f) for f in SHARED] + sorted(glob.glob(os.path.join(glob.escape(os.path.dirname(os.path.abspath(__file__))), '../games/*/patches.py')))
# Every folder given to glob is escaped: brackets in a parent folder (`Game [0400486024DEC000][v131072]`) are a character class to glob, and the search would find nothing
# Profiles outside the repository (DM_GAMES_DIR, folders separated by os.pathsep, which is ':' as in dm/games/game.py: the NSP script's provisional profile of a game being brought up) may have a
# patches.py of their own builds too; they are loaded after the repository's, under the same rule (a build ID claimed twice stops the rewrite)
PATCH_FILES += [p for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d for p in sorted(glob.glob(os.path.join(glob.escape(d), '*', 'patches.py')))]
patch_paths = {}
for path in PATCH_FILES: patch_paths.setdefault(os.path.realpath(path), path)   # repeated roots and symlinks name the same file, not competing build claims
PATCH_FILES = list(patch_paths.values())
def load_patches():
    tables, where = {'CUTS': {}, 'ALIAS_CUTS': {}, 'OBS': {}, 'FRAME_STEP': {}, 'STUBS': {}, 'MATCH_CUTS': {}, 'MATCH_FRAME_STEP': {}, 'MATCH_STUBS': {}, 'SDK_MATCH_CUTS': {}, 'CAPABILITIES': {}}, {}
    for n, path in enumerate(PATCH_FILES):
        spec = importlib.util.spec_from_file_location(f'patches_{n}', path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        for kind, table in tables.items():
            if kind == 'SDK_MATCH_CUTS': continue
            if kind == 'MATCH_CUTS' and os.path.basename(path) in SHARED:   # SDK-wide and engine-wide
                for mod, rules in getattr(m, kind, {}).items(): tables['SDK_MATCH_CUTS'].setdefault(mod, []).extend(rules)
                continue
            if kind in ('MATCH_CUTS', 'MATCH_FRAME_STEP', 'MATCH_STUBS'):
                if os.path.basename(path) in SHARED: continue
                profile = os.path.basename(os.path.dirname(path))
                roots = [os.path.join(os.path.dirname(os.path.abspath(__file__)), '../games')] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d]
                profile_dir = next((os.path.join(root, profile) for root in roots if os.path.isdir(os.path.join(root, profile))), None)
                if profile_dir and os.path.realpath(os.path.dirname(path)) != os.path.realpath(profile_dir): continue
                if getattr(m, kind, {}): table[profile] = getattr(m, kind)   # repository rules win for derived profiles; otherwise use the first external root
                continue
            for bid, v in getattr(m, kind, {}).items():
                assert bid not in table, f'{kind} of build {bid} in both {where[kind, bid]} and {path}'
                table[bid], where[kind, bid] = v, path
    return tables, where
PATCHES, PATCH_WHERE = load_patches()
BUILD_ID = MOD.build_id[:0x10].hex()
cuts = list(PATCHES['CUTS'].get(BUILD_ID, []))
ALIAS_OFFS = set(PATCHES['ALIAS_CUTS'].get(BUILD_ID, ()))
assert ALIAS_OFFS <= {c[0] for c in PATCHES['CUTS'].get(BUILD_ID, [])}, f'ALIAS_CUTS of build {BUILD_ID} name offsets that are not among its CUTS: {sorted(ALIAS_OFFS - {c[0] for c in PATCHES["CUTS"].get(BUILD_ID, [])})}'
if BUILD_ID not in PATCHES['CUTS']:
    from nso_match import resolve
    profile = os.path.basename(os.environ.get('DM_GAME', '').rstrip('/'))
    # the repository profile's build rows (a derived profile replaces them with its own version's): the builds of this module the game was brought up on, and what they had cut
    known, rows, own = [], [], set()
    try:
        if profile:
            with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../games', profile, 'game.tsv'), encoding='utf-8') as f: rows = f.read().splitlines()
    except OSError: pass
    for row in rows:
        c = [x.strip() for x in row.split('\t')]
        if c[0] == 'build' and len(c) > 3:
            bid = dict(x.split('=', 1) for x in c[3:] if '=' in x).get(NAME)
            if bid: own.add(bid)
            if bid and bid != BUILD_ID and PATCHES['CUTS'].get(bid): known.append((bid, c[2], len(PATCHES['CUTS'][bid])))
    outcomes, sdk_outcomes = [], []
    cuts += resolve(IMG, TEXT_END, PATCHES['MATCH_CUTS'].get(profile, {}).get(NAME, []), outcomes=outcomes, alias_only=ALIAS_OFFS)
    cuts += resolve(IMG, TEXT_END, PATCHES['SDK_MATCH_CUTS'].get(NAME, []), outcomes=sdk_outcomes, red=False, alias_only=ALIAS_OFFS)
    if BUILD_ID not in own and (known or outcomes or sdk_outcomes):   # a build the game was brought up on lost nothing
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, NAME + '.match.tsv'), 'w') as f:
            f.write(f'# {NAME} {BUILD_ID}: not a registered build. registered: build ID, display version, cuts; patch (the game profile\'s) or sdk (SDK-wide): name, cut offsets or -, implementation or why not\n')
            for bid, disp, n in known: f.write(f'registered\t{bid}\t{disp}\t{n}\n')
            for kind, got in (('patch', outcomes), ('sdk', sdk_outcomes)):
                for name, offs, why in got: f.write(f'{kind}\t{name}\t{",".join(f"{o:#x}" for o in offs) if offs else "-"}\t{why}\n')
# the frame step of a main with no FRAME_STEP of its build, placed by the function it patches (MATCH_FRAME_STEP), found before any cut changes the image
FS_MATCH = None
if NAME == 'main' and BUILD_ID not in PATCHES['FRAME_STEP']:
    from nso_match import locate_stub
    FS_MATCH = locate_stub(IMG, TEXT_END, PATCHES['MATCH_FRAME_STEP'].get(os.path.basename(os.environ.get('DM_GAME', '').rstrip('/')), {}).get(NAME, []))
# Ordinary matched stubs do not declare a frame step. Keep their FPS dependency
# separate: finding the engine interval alone does not prove every consumer scales.
STUB_RULES = PATCHES['MATCH_STUBS'].get(os.path.basename(os.environ.get('DM_GAME', '').rstrip('/')), {}).get(NAME, [])
assert all(isinstance(r.get('fps_required', False), bool) for r in STUB_RULES), 'fps_required must be a bool'
NATIVE_FPS_BLOCKED = any(r.get('fps_required', False) for r in STUB_RULES)
STUB_MATCH = None
if STUB_RULES and BUILD_ID not in PATCHES['STUBS']:
    from nso_match import locate_stub
    STUB_MATCH = locate_stub(IMG, TEXT_END, [{k: v for k, v in r.items() if k != 'fps_required'} for r in STUB_RULES])
# The frame step state page is the page after the key page. An appended patch area starts on the first 64 KB boundary after the key page, which is that page itself when the key
# page ends on such a boundary (Hades II's v0 main): a main that gets the state page then starts its patch area 64 KB later. Nothing has been written to the patch area yet, and
# every other module keeps its layout
if (BUILD_ID in PATCHES['FRAME_STEP'] or FS_MATCH) and NAME == 'main' and not FAR and PATCH_OFF < KEY_PAGE_OFF + 0x4000 + 0x1000:
    PATCH_OFF += 0x10000
ALIAS_APPLIED = [c for c in cuts if c[0] in ALIAS_OFFS]
assert len(ALIAS_APPLIED) <= 32, f'too many ALIAS_CUTS ({len(ALIAS_APPLIED)} > 32)'
for off, ob, nb, why in cuts:
    ob, nb = bytes.fromhex(ob), bytes.fromhex(nb)
    assert len(ob) == len(nb) and IMG[off:off + len(ob)] == ob, f'cut original bytes mismatch {off:#x}: {bytes(IMG[off:off + len(ob)]).hex()}'
    if off in ALIAS_OFFS: assert len(ob) <= 16 and off + len(ob) <= TEXT_END, f'ALIAS_CUT at {off:#x} exceeds loader bounds'
    IMG[off:off + len(nb)] = nb
# Key page indices are fixed; the key page content is written by the loader as "actual key number × 8".
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511}
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE'))}

# ---- Unreal console variables in main (dm/tools/ue_cvar.py finds them by their registered names, without a build ID) ----
# The fixups table names each one's TAutoConsoleVariable (UE_CVAR:<name>); the host writes those the profile decides on (hle_fpslock.c, hle_uecvar.c)
import ue_cvar
UE_CVARS, ue_cvar_why = ue_cvar.find_all(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF) if NAME == 'main' else ({}, [])
if UE_CVARS: print(f'{NAME}: Unreal console variables (ue_cvar.py): ' + ', '.join(f'{n} at {o:#x}' for n, o in UE_CVARS.items()))
for w in ue_cvar_why:
    if w.startswith('[RED] '): print(f'[RED] {NAME}: Unreal console variable {w[6:]}: the host cannot write it')

# ---- A target frame rate the game steps by (main: dm/tools/fps_target.py finds it by the code that reads and sets it, without a build ID) ----
# The fixups table names the global pointer and the field (FPS_TARGET_PTR, FPS_TARGET_FIELD); the host writes the profile's fps_target there (hle_fpslock.c)
import fps_target
FPS_TARGET_PTR, FPS_TARGET_FIELD, fps_target_why = fps_target.find_re_engine(bytes(IMG), TEXT_END, DATA_OFF) if NAME == 'main' else (None, None, '')
FPS_TARGET_F32 = FPS_TARGET_PTR is not None
if NAME == 'main' and not fps_target_why:
    FPS_TARGET_PTR, FPS_TARGET_FIELD, fps_target_why = fps_target.find(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF)
if FPS_TARGET_PTR is not None: print(f'{NAME}: {fps_target_why} (fps_target.py)')
elif fps_target_why.startswith('[RED] '): print(f'[RED] {NAME}: {fps_target_why[6:]}: the host cannot write it')

# ---- Monolith's native interval (the wait and simulation step share it; no game/build offsets) ----
import monolith_fps
MONOLITH_INTERVAL, monolith_why = monolith_fps.find(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF) if NAME == 'main' else (None, '')
if MONOLITH_INTERVAL is not None: print(f'{NAME}: {monolith_why} (monolith_fps.py)')
elif monolith_why.startswith('[RED] '): print(f'[RED] {NAME}: {monolith_why[6:]}: the host cannot write it')

# ---- Unreal's fixed frame rate (main: dm/tools/ue_fixed_rate.py finds UEngine::FixedFrameRate and bUseFixedFrameRate by their reflection entries and GEngine where the engine is made, without a build ID) ----
# The fixups table names GEngine's offset, the property's and the flag's (UE_FIXED_RATE_PTR, UE_FIXED_RATE_FIELD, UE_FIXED_RATE_FLAG); the host writes the profile's fps_target there while the flag is set (hle_fpslock.c)
import ue_fixed_rate
UE_FIXED_PTR, UE_FIXED_FIELD, UE_FIXED_FLAG, ue_fixed_why = ue_fixed_rate.find(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF) if NAME == 'main' and UE_CVARS else (None, None, None, '')
if UE_FIXED_PTR is not None: print(f'{NAME}: {ue_fixed_why} (ue_fixed_rate.py)')
elif ue_fixed_why.startswith('[RED] '): print(f'[RED] {NAME}: {ue_fixed_why[6:]}: the host cannot write it')

# ---- Unity's frame rate (main: dm/tools/unity_fps.py finds Application.targetFrameRate and QualitySettings' vSyncCount through the engine's internal-call table, without a build ID) ----
# The fixups table names their places (UNITY_TARGET_FPS; UNITY_QS_PTR, _IDX, _ARR, _SIZE, _VSYNC); the host writes the profile's fps_target there (hle_fpslock.c)
import unity_fps
UNITY_FPS, unity_why = unity_fps.find(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF) if NAME == 'main' else ({}, '')
if UNITY_FPS: print(f'{NAME}: {unity_why} (unity_fps.py)')
if '[RED] ' in unity_why: print(f'[RED] {NAME}: {unity_why}: the host cannot write what is not read')

# ---- Unreal's FMallocBinned3 (any module: dm/tools/ue_malloc.py finds it by the engine's own name string, getter and vtable, without a build ID) ----
# Its constructor reserves 37 GB of address space for its pools. Its calls are cut, and observation gate OBS_UE_MALLOC goes on each (below), where x0 holds the object: the host gives
# the object its own allocator's vtable there (dm/host/hle_uemalloc.c), whose place and slot layout the fixups table names (UE_MALLOC_VT, UE_MALLOC_SLOT)
import ue_malloc
UE_CALLS, UE_VT, UE_SLOT, ue_why = ue_malloc.find(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF)
for _c in UE_CALLS or ():
    cuts.append((_c, bytes(IMG[_c:_c + 4]).hex(), struct.pack('<I', NOP).hex(), f'{ue_why}: the constructor call → nop, gate OBS_UE_MALLOC on it (ue_malloc.py)'))
    struct.pack_into('<I', IMG, _c, NOP)
if UE_CALLS: print(f'{NAME}: {ue_why}')
elif ue_why.startswith('[RED] '): print(f'[RED] {NAME}: {ue_why[6:]}: Unreal\'s own allocator runs')

# ---- Unity's ALLOC_BUCKET BucketAllocator (main: dm/tools/unity_bucket.py finds its construction by the allocator's name, without a build ID) ----
# Unity 2019 reserves (255 + 1) x 4 MB for it at once; its block limit is cut to what 256 MB holds (a platform limit: an iPhone's alias region is 4.375 GB)
import unity_bucket
UB_CUT, ub_why = unity_bucket.find(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF) if NAME == 'main' else (None, '')
if UB_CUT:
    cuts.append(UB_CUT); IMG[UB_CUT[0]:UB_CUT[0] + 4] = bytes.fromhex(UB_CUT[2])
    print(f'{NAME}: {ub_why}')
elif ub_why.startswith('[RED] '): print(f'[RED] {NAME}: {ub_why[6:]}: its whole reservation stays')

# ---- Snowdrop's three-pool page allocator (main: dm/tools/snowdrop_pools.py finds its constructor by its structure, without a build ID) ----
# Its 12 GB of address ranges become the sizes the profile decides on (snowdrop_pools, measured against the game's regions)
import snowdrop_pools
sd_sites, sd_why = snowdrop_pools.find(bytes(IMG), TEXT_END) if NAME == 'main' else (None, '')
if sd_sites is not None:
    sd_sizes, sd_from = snowdrop_pools.profile_sizes()
    for off, ob, nb, why in snowdrop_pools.cuts(IMG, sd_sites, sd_sizes):
        cuts.append((off, ob, nb, why))
        IMG[off:off + 4] = bytes.fromhex(nb)
    print(f'{NAME}: {sd_why}: large {sd_sizes[0]} MB, 512 KB pages {sd_sizes[1]} MB, 64 KB pages {sd_sizes[2]} MB ({sd_from})')
elif sd_why.startswith('[RED] '): print(f'[RED] {NAME}: {sd_why[6:]}: its 12 GB reservation stays and fails on an iPhone')

# ---- Unreal's engine address region (main: dm/tools/ue_region.py finds the store of its size right before nninitStartup reserves it, without a build ID) ----
# Observation gate OBS_UE_REGION goes on that store (below), the size register in the gate number's upper bits: the host gives the region what the alias region has room for there
# (dm/host/hle_svc.c "Unreal's engine region"), where the engine would ask for 62 GB (16 GB in Unreal Engine 4.18)
import ue_region
UE_REGION, UE_REGION_REG, UE_REGION_X1, ue_region_why = ue_region.find(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF) if NAME == 'main' else (None, None, None, '')
if UE_REGION is not None: print(f'{NAME}: {ue_region_why}')
elif ue_region_why.startswith('[RED] '): print(f'[RED] {NAME}: {ue_region_why[6:]}: the engine asks for its whole default region')

# ---- Unreal's NVN dynamic buffers (main: dm/tools/ue_ring.py finds the load of their copy count, without a build ID) ----
# Each write lock moves to the next copy with no fence, and a ring of 3 can come round to a copy that queued draws still read: one more copy widens the margin (a workaround: GitHub issue #1040)
import ue_ring
UE_RING, UE_RING_OLD, UE_RING_NEW, ue_ring_why = ue_ring.find(bytes(IMG), TEXT_END, RO_OFF, DATA_OFF) if NAME == 'main' else (None, None, None, '')
if UE_RING is not None:
    cuts.append((UE_RING, struct.pack('<I', UE_RING_OLD).hex(), struct.pack('<I', UE_RING_NEW).hex(), f'{ue_ring_why} (ue_ring.py)'))
    struct.pack_into('<I', IMG, UE_RING, UE_RING_NEW)
    print(f'{NAME}: {ue_ring_why}')
elif ue_ring_why.startswith('[RED] '): print(f'[RED] {NAME}: {ue_ring_why[6:]}: its dynamic buffers keep 3 copies')

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
patch = bytearray(); fix = []; gates = []; stats = {}
patch_front, patch_back, PATCH_BASE = patch, bytearray(), PATCH_OFF   # the area emit() writes into and its offset: the front one unless TWO puts a site in the back one
def use_area(a):
    """Emit what follows into the patch area that reaches the code at offset a (TWO: the back area from SPLIT on)"""
    global patch, PATCH_BASE
    patch, PATCH_BASE = (patch_back, BACK_OFF) if TWO and a >= SPLIT else (patch_front, PATCH_OFF)
def stat(k): stats[k] = stats.get(k, 0) + 1
def here(): return PATCH_BASE + len(patch)
def emit(w, kind=None):
    if kind: fix.append((len(patch), kind))
    patch.extend(struct.pack('<I', w & 0xFFFFFFFF))

def emit_key_addr(reg, kind):
    fix.append((len(patch), kind)); emit(ADRP(reg, here(), KEY_PAGE_OFF)); emit(LDRX(reg, reg, KEY_INDEX[kind]))

def emit_slot_addr(base, key_reg, kind):
    emit_key_addr(key_reg, kind); emit(ADDR(base, base, key_reg))

# ---- Encoders: dm/tools/nso_asm.py (imported above) ----

def dis(w, addr):
    l = list(md.disasm(struct.pack('<I', w), addr)); assert len(l) == 1, hex(w)
    return l[0]
def regset(ins):
    r, w = ins.regs_access()
    return {ins.reg_name(x) for x in r}, {ins.reg_name(x) for x in w}
def num(name):
    m = re.fullmatch(r'[xw](\d+)', name or ''); return int(m.group(1)) if m else None
def written_w(p):
    """The general-purpose registers p writes, as wN names, since a write of wN or xN changes the same register. Capstone's reg_name calls x30 lr and x29 fp; s0, d0, q0 and the like are not w0.
    Capstone lists a compare's first operand as written too, so a caller that only wants real writes leaves compares out"""
    names = ({'lr': 'x30', 'fp': 'x29'}.get(n, n) for n in map(p.reg_name, p.regs_access()[1]))
    return {f'w{num(n)}' for n in names if num(n) is not None}
def writes_reg(p, reg):
    """Whether p changes the general-purpose register reg (xN or wN): a write of wN clears the upper half of xN too, and a compare only sets the flags"""
    return p.mnemonic not in ('cmp', 'cmn', 'tst') and 'w' + reg[1:] in written_w(p)
def has18(ins): return re.search(r'\b[xw]18\b', ins.op_str) is not None
def regs_in(ins):
    s = {int(n) for n in re.findall(r'\b[xw](\d+)\b', ins.op_str)}
    try:
        rd, wr = regset(ins); s |= {num(n) for n in rd | wr} - {None}
    except Exception: pass
    return s
def pick(avoid, n=2):
    out = [r for r in range(0, 16) if r not in avoid and r != 18][:n]; assert len(out) == n; return out

def subst(ins, S):
    """x18/w18 → xS/wS: only the 4 register field positions are changed, verified against the disassembly text."""
    w = struct.unpack('<I', ins.bytes)[0]
    want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{S}', ins.op_str)
    pos = [p for p in (0, 5, 10, 16) if (w >> p) & 31 == 18]
    for k in range(len(pos), 0, -1):
        for c in combinations(pos, k):
            nw = w
            for p in c: nw = (nw & ~(31 << p)) | (S << p)
            try: dd = dis(nw, ins.address)
            except AssertionError: continue
            if dd.mnemonic == ins.mnemonic and dd.op_str == want: return nw
    raise RuntimeError(f'subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}')

# ---- Rewrite blocks of each kind ----
def sp_delta(ins):
    """The amount an instruction writes back to sp (pre-index [sp, #i]! / post-index [sp], #i); returns 0 if sp is not written back."""
    m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-f]+)\]!', ins.op_str)
    if m: return int(m.group(1), 0)
    m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-f]+)$', ins.op_str)
    if m: return int(m.group(1), 0)
    assert not re.match(r'(sp|wsp),', ins.op_str), f'writes sp: {ins.address:x} {ins.op_str}'
    return 0

def site_plain(ins):
    S, T = pick(regs_in(ins))
    dlt = sp_delta(ins)
    save = -16 if dlt == 0 else -112          # Instructions that modify sp write below sp, so the scratch save is placed deep in the reserved area
    assert dlt == 0 or dlt >= -96, f'sp delta {dlt} at {ins.address:x}'
    start = here()
    emit(STP(S, T, 31, save)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(subst(ins, S))
    emit(STRX(S, T))                 # Always write back: capstone can miss reporting a write (the adds w18 instance)
    emit(LDP(S, T, 31, save - dlt)); emit(BX(here(), ins.address + 4))
    return start

def select_x18(ins):
    """A CSEL with one guest x18 source and a normal register destination."""
    word = struct.unpack('<I', ins.bytes)[0]
    rd, rn, rm, cond = word & 31, (word >> 5) & 31, (word >> 16) & 31, (word >> 12) & 15
    if word & 0x7FE00C00 != 0x1A800000 or rd in (18, 31) or (rn == 18) == (rm == 18) or cond >= 14: return None
    return rd, (rm if rn == 18 else rn), (cond ^ (rn == 18)), bool(word >> 31)


# Only these instructions can carry a proof of a dead register across them. The first operand of a writer is a full xN/wN definition unless the instruction
# also reads it (including as an address register). A wN definition zeros the high half of xN. Stores, compares and NOP can never end a register's lifetime.
SELECT_WRITES = set('adr adrp add adds and ands asr bic csel eor ldr ldrb ldrh ldrsb ldrsh ldrsw ldur lsl lsr mov movn movz mul orn orr sbfx sub subs ubfx'.split())
SELECT_READS = {'cmp', 'cmn', 'tst', 'nop', 'str', 'strb', 'strh', 'stur', 'sturb', 'sturh'}


def select_scratch(at, by_address, function_end, rd):
    """One dead AAPCS64 temporary for a select, or None. The old value must be overwritten before a read on every path, or reach a normal return
    inside the same unwind function. Keep x0–x8, callee-saved registers and LR out of the search. Calls, unknown instructions, outgoing branches and
    an exhausted search budget give no proof."""
    budget = 64

    def dead_after(pc, pending):
        nonlocal budget
        if not pending or budget == 0 or pc >= function_end or (ins := by_address.get(pc)) is None: return set()
        budget -= 1
        m = ins.mnemonic
        if m == 'ret' and not ins.op_str: return pending
        if m == 'b': return dead_after(ins.operands[-1].imm, pending)
        if m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            pending = pending - regs_in(ins)
            return dead_after(pc + 4, pending) & dead_after(ins.operands[-1].imm, pending)
        if m not in SELECT_WRITES and m not in SELECT_READS: return set()
        dead = set()
        if m in SELECT_WRITES and not ins.writeback:
            dst, _, sources = ins.op_str.partition(',')
            dst_n = num(dst)
            if dst_n in pending:
                reads, _ = regset(ins)
                if not any(num(name) == dst_n for name in reads | set(re.findall(r'\b[xw]\d+\b', sources))):
                    dead.add(dst_n)
        return dead | dead_after(pc + 4, pending - regs_in(ins))

    return min(dead_after(at + 4, set(range(9, 18)) - {rd}), default=None)


def site_select(ins, args, temp):
    """Split CSEL on its original flags. Only the arm selecting x18 needs the key page. In that arm the destination's old value is dead,
    so the destination and one dead temporary can load K18 without touching the guest stack."""
    rd, other, other_cond, wide = args
    start, skip = here(), len(patch)
    emit(NOP)  # filled with the conditional branch once the other arm's address is known
    emit(MRS_TPIDRRO(temp)); emit_slot_addr(temp, rd, 'K18')
    emit(LDRX(rd, temp) if wide else 0xB9400000 | (temp << 5) | rd)  # ldr wD,[xT] zero-extends the result
    emit(BX(here(), ins.address + 4))
    struct.pack_into('<I', patch, skip, BEQ(start, here()) | other_cond)
    if not wide or rd != other: emit(MOVX(rd, other) if wide else MOVX(rd, other) & 0x7FFFFFFF)
    emit(BX(here(), ins.address + 4))
    return start


def site_sp(ins):
    """Destination is sp and a source is x18 (Engage's alloca: mov x18, sp; sub x18, x18, x0; mov sp, x18). site_plain saves its scratch relative to the old sp and would restore it relative to the new one:
    copy the old sp into T first, and after the instruction restore S / T relative to T (without writeback, ldp may use the base register as a destination). x18 is not written, so the slot is not written back"""
    S, T = pick(regs_in(ins))
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(ADDI(T, 31, 0))             # mov T, sp (in add immediate, 31 is sp)
    emit(subst(ins, S))
    emit(LDP(S, T, T, -16)); emit(BX(here(), ins.address + 4))
    return start

def casp_pair18(ins):
    """Whether ins is a compare and swap of a register pair (casp, caspa, caspl, caspal; x or w) whose compare pair (Rs) or new-value pair (Rt) is x18, x19"""
    w = struct.unpack('<I', ins.bytes)[0]
    return w & 0xBFA07C00 == 0x08207C00 and 18 in ((w >> 16) & 31, w & 31)

def site_casp(ins):
    """A compare and swap whose pair is x18, x19. A pair is an even register and the next one, so subst cannot rename x18 alone (xS, x19 is no pair): the pair moves to a free even
    pair P, P + 1, P loaded from the slot K18 and P + 1 copied from x19. The instruction writes the compare pair with the memory's old value, so after it P goes back to the slot and
    P + 1 to x19; a new-value pair is only read. The memory access is still the one instruction, so it stays atomic. P, P + 1 and T (the slot's address) are saved below sp as in
    site_plain (casp has no writeback: sp does not move). Sonic Frontiers Definitive Edition main+0x19ce5c4: caspal x18, x19, x4, x5, [x16]"""
    w = struct.unpack('<I', ins.bytes)[0]
    rs, rn, rt = (w >> 16) & 31, (w >> 5) & 31, w & 31
    assert rn != 18, f'casp with x18 as its base and in a pair {ins.address:x} {ins.op_str}'
    used = regs_in(ins) | {18, 19}
    P = next(r for r in range(0, 16, 2) if not {r, r + 1} & used)
    T = next(r for r in range(16) if r not in used | {P, P + 1})
    nw = w
    if rs == 18: nw = (nw & ~(31 << 16)) | (P << 16)
    if rt == 18: nw = (nw & ~31) | P
    want = re.sub(r'\b([xw])(18|19)\b', lambda m: f'{m.group(1)}{P + (m.group(2) == "19")}', ins.op_str)
    dd = dis(nw, ins.address)
    if (dd.mnemonic, dd.op_str) != (ins.mnemonic, want): raise RuntimeError(f'casp subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}: {dd.mnemonic} {dd.op_str}')
    start = here()
    emit(STP(P, P + 1, 31, -16)); emit(STUR(T, 31, -24))
    emit(MRS_TPIDRRO(T)); emit_slot_addr(T, P, 'K18'); emit(LDRX(P, T)); emit(MOVX(P + 1, 19))
    emit(nw)
    if rs == 18: emit(STRX(P, T)); emit(MOVX(19, P + 1))
    emit(LDUR(T, 31, -24)); emit(LDP(P, P + 1, 31, -16)); emit(BX(here(), ins.address + 4))
    return start

def site_sp_align(ins, n):
    """sub sp, sp, xN (no shift): Horizon leaves stack alignment checking off for user code, Apple's kernel does not, so an sp that is not 16-byte aligned faults (SIGBUS) at the next sp-based access.
    Paper Mario: The Thousand-Year Door's script call bridges (main+0x17c140 and five more beside it, hand-written) reserve the callee's stack arguments as their sizes add up (4 for one int)
    and give the area back with add sp, sp, xN: the callee's first stp faulted. xN is rounded up to 16 first, which the add then gives back too. Compiled code keeps such sizes 16-aligned, so
    there it changes nothing; of the 22 games' modules on hand only Paper Mario's main has the form"""
    start = here()
    emit(ADDI(n, n, 15)); emit(0x927CEC00 | n << 5 | n)   # add xN, xN, #15; and xN, xN, #~15
    emit(struct.unpack('<I', ins.bytes)[0]); emit(BX(here(), ins.address + 4))
    return start

def site_cond(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    S, T = pick(regs_in(ins))
    tgt = ins.operands[-1].imm
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    m = ins.mnemonic
    taken = here() + 4 * 3
    off = (taken - here()) >> 2
    nw = ((w & 0xFF000000) | ((off & 0x7FFFF) << 5) | S) if m in ('cbz', 'cbnz') else ((w & 0xFFF80000) | ((off & 0x3FFF) << 5) | S)
    pc = here(); emit(nw)
    dd = dis(nw, pc); assert dd.mnemonic == m and dd.operands[-1].imm == taken
    emit(LDP(S, T, 31, -16)); emit(BX(here(), ins.address + 4))
    assert here() == taken
    emit(LDP(S, T, 31, -16)); emit(BX(here(), tgt))
    return start

def site_pcrel(ins):
    tgt = ins.operands[1].imm
    S, T = pick(set())
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18')   # S first serves as a temporary for the key page offset; the next instruction then loads the target value
    pc = here(); emit(ADRP(S, pc, tgt))
    if ins.mnemonic == 'adr': emit(ADDI(S, S, tgt & 0xFFF))
    emit(STRX(S, T))
    emit(LDP(S, T, 31, -16)); emit(BX(here(), ins.address + 4))
    return start

def site_br(ins, base, count):
    """Jump table br x18: target = base + 4i. Stash x16 in the red zone → equally spaced landing pads → restore x16 → original target."""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # x16 / x17 are temporary registers that can stay live within a function; save them to the red zone before borrowing
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(LDUR(17, 31, -24))
    padbase = start + 4 * 24
    delta = padbase - base
    op = ADDI if delta >= 0 else SUBI; dl = abs(delta)
    ops = []; hi, lo = dl >> 12, dl & 0xFFF
    while hi: h = min(hi, 0xFFF); ops.append(op(16, 16, h, 1)); hi -= h
    if lo: ops.append(op(16, 16, lo, 0))
    assert len(ops) <= 12
    for o in ops: emit(o)
    emit(BR(16))
    while here() < padbase: emit(NOP)
    tails = padbase + 4 * count
    for i in range(count): emit(BX(here(), tails + 8 * i))
    for i in range(count):
        emit(0xF85F0000 | (31 << 5) | 16)      # ldur x16,[sp,#-16]
        emit(BX(here(), base + 4 * i))
    return start

def site_br_tab(ins, targets):
    """Offset-table br x18 (ldrsw x18,[xB,x18,lsl #2]; add x18,x18,xB): target = table base + signed entry; the table is constant, so all targets are read at rewrite time.
    x16 = guest x18, compared with each target in turn (adrp + add put the target address in x17); on a match restore x16 / x17 and jump to the original target; no match → brk (the original bounds check already stops out-of-range indices)"""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # as in site_br: save x16 / x17 to the red zone before borrowing
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    uniq = sorted(set(targets))
    pads = here() + 4 * (4 * len(uniq) + 1)
    for k, t in enumerate(uniq):
        emit(ADRP(17, here(), t)); emit(ADDI(17, 17, t & 0xFFF)); emit(CMPR(16, 17)); emit(BEQ(here(), pads + 12 * k))
    emit(BRK(0x18))
    for t in uniq:
        emit(LDUR(16, 31, -16)); emit(LDUR(17, 31, -24)); emit(BX(here(), t))
    return start

def site_blr(ins):
    """blr x18 (Engage main+0x9ff324): x16 = guest x18, x30 = the instruction after the original blr, br x16. At a call site x16 / x17 are intra-procedure-call temporaries under the calling convention, so they need no saving;
    the return address is the original one, so the callee returns straight there (IL2CPP's C++ exceptions look up unwind tables by return address and must not stop in the patch area)"""
    start = here()
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    ret = ins.address + 4
    emit(ADRP(30, here(), ret)); emit(ADDI(30, 30, ret & 0xFFF)); emit(BR(16))
    return start

def site_br_call(ins):
    """br x18 as an indirect tail call (tail_call): x16 = guest x18, br x16. As at site_blr, x16 / x17 are intra-procedure-call temporaries at a call, and x30 keeps the caller's return address"""
    start = here()
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(BR(16))
    return start

def site_slot_copy(ins, src, dst):
    """A system register access whose general-purpose register is x18: the guest's x18 is the thread slot K18, so the access becomes a copy from one thread slot to another
    (mrs x18, tpidr_el0: KTLS → K18; mrs x18, tpidrro_el0: KTLR → K18; msr tpidr_el0, x18: K18 → KTLS), or src is a word that puts the value in x15 (mrs x18, ctr_el0).
    The Apple kernel may change x18 at any instruction, so the value never passes through it: x15 carries it, and x16 / x17 form the slot addresses, all three saved in the red zone"""
    start = here()
    emit(STP(16, 17, 31, -16)); emit(STUR(15, 31, -24))
    if isinstance(src, str): emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, src); emit(LDRX(15, 16))
    else: emit(src)
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, dst); emit(STRX(15, 16))
    emit(LDUR(15, 31, -24)); emit(LDP(16, 17, 31, -16))
    emit(BX(here(), ins.address + 4))
    return start

def site_sysreg_read(ins, kind):
    """mrs xN, tpidr_el0 / tpidrro_el0 → xN = thread slot value."""
    N = ins.bytes[0] & 31
    if N == 18: return site_slot_copy(ins, kind, 'K18')
    tmp = 16 if N != 16 else 17
    start = here()
    emit(STUR(tmp, 31, -16)); emit(MRS_TPIDRRO(N)); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N)); emit(LDUR(tmp, 31, -16))   # tmp is saved by borrowing the red zone
    emit(BX(here(), ins.address + 4))
    return start

def site_tpidr_write(ins):
    N = struct.unpack('<I', ins.bytes)[0] & 31
    if N == 18: return site_slot_copy(ins, 'K18', 'KTLS')
    (T,) = pick({N}, 1)
    start = here()
    emit(0xF81F0000 | (31 << 5) | T)           # stur xT,[sp,#-16]
    tmp = next(x for x in (16, 17) if x not in (N, T))
    emit(STUR(tmp, 31, -24)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, tmp, 'KTLS'); emit(STRX(N, T)); emit(LDUR(tmp, 31, -24))
    emit(0xF85F0000 | (31 << 5) | T)           # ldur xT,[sp,#-16]
    emit(BX(here(), ins.address + 4))
    return start

def emit_gate_call():
    """Common section of a gate block (right after stp x29,x30,[sp,#-16]! and before blr x30): x30 = the gate address in thread slot KGATE.
        Fetching the slot through the key page needs a second register to be borrowed; x29 is borrowed and then immediately restored from the stack: the x29 saved by _dm_gate must be
        the guest's original value, since the crash backtrace walks the x29 chain."""
    emit(MRS_TPIDRRO(30)); emit_slot_addr(30, 29, 'KGATE'); emit(LDRX(30, 30)); emit(LDRX(29, 31))

def site_gate(ins, kind, arg):
    """svc / mrs cntpct_el0 → host gate. The gate address is taken from thread slot KGATE; the gate looks up gates.tsv by return point (the address of this block's ldp)."""
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(BX(here(), ins.address + 4))
    gates.append((ret, kind, arg, ins.address))
    return start

def site_obs(off, oid, word):
    """Observation gate: original entry instruction → enter the host gate (arg = observation number) → restore x29/x30 → execute the original instruction → jump back to off+4.
        The first half of the gate block is word-for-word isomorphic with site_gate; dm_gate saves x0–x28 and the flags in its frame and restores them, so a register keeps its value unless the callback writes it
        (OBS_STDALLOC_INIT's x2, OBS_UE_REGION's size). Any instruction that can be relocated takes one"""
    ins = dis(word, off)
    assert ins.mnemonic not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') and not ins.mnemonic.startswith('b.') \
        and not (ins.mnemonic.startswith('ldr') and '[' not in ins.op_str) and not has18(ins), f'observation gate original instruction cannot be relocated {off:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(word)
    emit(BX(here(), off + 4))
    gates.append((ret, 'obs', oid, off))
    return start

# ---- Exclusive windows ----
EXCL_LD = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
EXCL_ST = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')
def find_windows(insl):
    out = []
    for i, ins in enumerate(insl):
        if ins.mnemonic not in EXCL_LD: continue
        j = i
        while j < len(insl) and insl[j].mnemonic not in EXCL_ST: j += 1
        if j >= len(insl) or j - i >= 16: continue
        # Another exclusive load before the store: this one has no store of its own (a retry block, see retry_block); spanning both would merge two sequences
        # (Tears of the Kingdom main+0x5a4674: ldxr w18; cmp; b.eq <stxr of 0x5a427c>; ...; 0x5a469c: ldxr w13; and; stxr; cbnz, a loop other code branches to)
        if any(x.mnemonic in EXCL_LD for x in insl[i + 1:j]): continue
        if j + 1 < len(insl) and insl[j + 1].mnemonic in ('cbz', 'cbnz'): j += 1
        seg = insl[i:j + 1]
        if any(has18(x) for x in seg): out.append(seg)
    return out

def window_adrp(x, S, pc):
    """An adrp inside a window, copied to pc: the same page from the copy's address (x18 as its stand-in S). Pikmin 4 1.1.1 main+0x43da140 (Unreal Engine 4.26.2): ldaxr x9,[x9]; cmp; b.ne;
    adrp x10 + add x10 (the same address again for the store); mov x18,x26; stlxr w9,x8,[x10]; …; cbnz w9"""
    rd = num(x.reg_name(x.operands[0].reg))
    return ADRP(S if rd == 18 else rd, pc, x.operands[1].imm)

def check_window_branches(seg):
    """For windows with a B, reject exits that carry a reservation through a stub's store."""
    if not any(x.mnemonic == 'b' for x in seg): return
    assert all(b.address == a.address + 4 for a, b in zip(seg, seg[1:])), f'noncontiguous window {seg[0].address:x}'
    by_addr = {x.address: x for x in seg}
    pending, seen = [(seg[0].address, False)], set()
    while pending:
        addr, live = pending.pop()
        if addr not in by_addr:
            assert not live, f'branch leaves window with live reservation {addr:x}'
            continue
        if (addr, live) in seen: continue
        seen.add((addr, live))
        x = by_addr[addr]; m = x.mnemonic
        if m in EXCL_LD: live = True
        elif m in EXCL_ST or m == 'clrex': live = False
        if m == 'b':
            tgt = x.operands[-1].imm
            pending.append((tgt, live))
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            pending.extend(((x.operands[-1].imm, live), (addr + 4, live)))
        else: pending.append((addr + 4, live))

def site_window(seg):
    check_window_branches(seg)
    used = set()
    for x in seg: used |= regs_in(x)
    S, T = pick(used)
    lo, hi = seg[0].address, seg[-1].address + 4
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    copy0 = here(); n = len(seg)
    # Exit stubs: the first stub right after the window must be the fall-through one (after the last window instruction, hi); targets of branches leaving the window come after it.
    # Previously the fall-through stub came last: as soon as a window contains a conditional branch out (e.g. ldxr; cmp; b.ne out; stxr; cbnz), a successful stxr fell into the
    # first branch-out stub, i.e. took the "compare not equal" path (SMBW main 0x81c40: the enqueue retried forever, the tail index filled up without storing a single task, and the job system hung). D2R's windows had no branch out, so it never hit this.
    exits = copy0 + 4 * n + 16
    ex_targets = []; words = []
    for k, x in enumerate(seg):
        w = struct.unpack('<I', x.bytes)[0]
        if has18(x): w = subst(x, S)
        m = x.mnemonic
        if m in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            tgt = x.operands[-1].imm
            if lo <= tgt < hi: newt = copy0 + (tgt - lo)
            else: ex_targets.append(tgt); newt = exits + 16 * (len(ex_targets) - 1)
            pc = copy0 + 4 * k; off = (newt - pc) >> 2
            if m == 'b': w = B(pc, newt)
            elif m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
            else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
            dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
        elif m == 'adrp': w = window_adrp(x, S, copy0 + 4 * k)
        else:
            assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
        words.append(w)
    for w in words: emit(w)
    for t in [hi] + ex_targets:   # fall-through first
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(BX(here(), t)); emit(NOP)
    return start, lo, hi

def retry_block(insl, k, wseg):
    """The branch insl[k] lands in the middle of window wseg. Accepted only when it ends an out-of-line retry block that re-enters the window at its store-exclusive:
    an exclusive load, then instructions that do not branch, then this branch. Tears of the Kingdom main+0x18598: ldxr w17,[x10]; cmp w15,w17; b.eq 0x181fc,
    where 0x181fc is the stxr w17,w18,[x10] of the window 0x181ec..0x18200, whose failed stxr branches back to 0x18598 (same shape at 0x185e8 → 0x18244).
    Returns (the retry block, the window from that store on), or None"""
    t = insl[k].operands[-1].imm
    tail = [y for y in wseg if y.address >= t]
    if not tail or tail[0].mnemonic not in EXCL_ST: return None
    for p in range(k - 1, max(-1, k - 8), -1):
        q = insl[p]
        if q.mnemonic in EXCL_LD: return insl[p:k + 1], tail
        if q.mnemonic in ('b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') or q.mnemonic.startswith('b.') or q.mnemonic in EXCL_ST: return None
    return None

def site_retry_window(parts):
    """A retry block (see retry_block) gets a patch copy of its own: it cannot jump into the window's copy, because the x18 stand-in would have to be loaded
    between its exclusive load and the store. parts = [(retry block, its fall-through), (window from the store on, the window's fall-through)] are copied one
    after the other under one prologue, each followed by its fall-through stub; branches to any copied instruction (between the parts, and the failed store's
    branch back to the retry block's head) stay inside the copy, every other target leaves through an exit stub as in site_window"""
    used = set()
    for seg, _ in parts:
        for x in seg: used |= regs_in(x)
    S, T = pick(used)
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    pos, at = {}, here()
    for seg, _ in parts:
        for x in seg: pos[x.address] = at; at += 4
        at += 16
    exits, ex_targets = at, []
    for seg, ft in parts:
        for x in seg:
            w = struct.unpack('<I', x.bytes)[0]
            if has18(x): w = subst(x, S)
            m, pc = x.mnemonic, here()
            if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
                tgt = x.operands[-1].imm
                if tgt not in pos and tgt not in ex_targets: ex_targets.append(tgt)
                newt = pos[tgt] if tgt in pos else exits + 16 * ex_targets.index(tgt)
                off = (newt - pc) >> 2
                if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
                else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
                dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
            elif m == 'adrp': w = window_adrp(x, S, pc)
            else:
                assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
            emit(w)
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(BX(here(), ft)); emit(NOP)
    for t in ex_targets:
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(BX(here(), t)); emit(NOP)
    return start

# ---- Function boundaries (eh_frame_hdr, or .eh_frame when a module has no .eh_frame_hdr) ----
m0 = struct.unpack_from('<I', IMG, 4)[0]
assert IMG[m0:m0 + 4] == b'MOD0'
ehs_rel, ehe_rel = struct.unpack_from('<ii', IMG, m0 + 16)
ehs, ehe = m0 + ehs_rel, m0 + ehe_rel
def rd(o, enc):
    fmt, app = enc & 0xF, enc & 0x70
    spec, sz = {0x0: ('<Q', 8), 0x2: ('<H', 2), 0x3: ('<I', 4), 0x4: ('<Q', 8),
                0xA: ('<h', 2), 0xB: ('<i', 4), 0xC: ('<q', 8)}[fmt]
    v = struct.unpack_from(spec, IMG, o)[0]
    if app == 0x10: v += o
    if app == 0x30: v += ehs
    return v, sz
starts = set()
if ehe > ehs:
    _, penc, cenc, tenc = IMG[ehs:ehs + 4]
    o = ehs + 4
    _, s = rd(o, penc); o += s
    nfde, s = rd(o, cenc); o += s
    for _ in range(nfde):
        a, s = rd(o, tenc); o += s
        _, s = rd(o, tenc); o += s
        starts.add(a)
elif ehs_rel != 0:
    # No .eh_frame_hdr (empty range in MOD0): devkitA64-linked modules (exlaunch plugins) point both ends at .eh_frame itself.
    # Walk CIE/FDE records up to the zero terminator, indexing each CIE's 'R' pointer encoding by its offset.
    cies, o = {}, ehs
    while o + 8 <= len(IMG):
        ln = struct.unpack_from('<I', IMG, o)[0]
        if ln in (0, 0xFFFFFFFF) or ln < 4 or o + 4 + ln > len(IMG): break
        cid = struct.unpack_from('<I', IMG, o + 4)[0]
        if cid == 0:
            ver = IMG[o + 8]
            p = o + 9; aug = IMG[p:IMG.index(b'\0', p)].decode('latin1'); p += len(aug) + 1
            if ver >= 4: p += 2   # address_size, segment_selector_size
            for _ in range(2):    # code_alignment_factor (uleb), data_alignment_factor (sleb)
                while IMG[p] & 0x80: p += 1
                p += 1
            if ver == 1: p += 1   # return_address_register
            else:
                while IMG[p] & 0x80: p += 1
                p += 1
            fenc = 0x00
            if aug.startswith('z'):
                while IMG[p] & 0x80: p += 1
                p += 1
                for c in aug[1:]:
                    if c == 'R': fenc = IMG[p]; p += 1
                    elif c == 'P': pe = IMG[p]; p += 1 + rd(p + 1, pe)[1]
                    elif c == 'L': p += 1
            cies[o] = fenc
        else:
            fenc = cies.get(o + 4 - cid)
            if fenc is not None and fenc != 0xFF: starts.add(rd(o + 8, fenc)[0])
        o += 4 + ln
starts = sorted(x for x in starts if x < TEXT_END)

# ---- Linear scan to find all rewrite points (not relying on register read/write reports) ----
# Data blocks in the text (nso_code.py: Absolum's main starts with 1.7 MB of them) are not code: words there that read as rewrite sites are left alone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_code
DATA_BLOCKS = nso_code.data_blocks(IMG, TEXT_END, md)
CODE_RANGES = list(zip([0] + [e for _, e in DATA_BLOCKS], [s for s, _ in DATA_BLOCKS] + [TEXT_END]))

# ---- Hops: branches beyond a b's reach (GAP above) ----
# A branch between the code and a patch area that a b does not reach goes through a hop: a b written into a word of the text that no path runs, within reach of both ends, like a
# linker's range extension thunk. Those words are alignment padding: the nop or zero words right before an unwind entry's start, after an instruction that ends every path through it
# (b, br, ret, brk), and fewer bytes of them than the start's alignment, so they are only the padding that aligns it. One hop serves every branch to its target that it reaches.
# Only a module with GAP needs one; any other module's branches all reach, so its output is unchanged
REACH = (1 << 27) - 4
_holes, _hops = None, {}
def hop_holes():
    """The padding words that may hold a hop, in address order (the text as the file has it)"""
    out, data = [], [(s, e) for s, e in DATA_BLOCKS]
    for s in starts:
        if s < 8 or s & 15: continue
        align, k = s & -s, s
        while k - 4 >= 0 and struct.unpack_from('<I', ORIG_TEXT, k - 4)[0] in (0, 0xD503201F): k -= 4
        if k == s or s - k >= align or k < 4: continue
        p = struct.unpack_from('<I', ORIG_TEXT, k - 4)[0]
        if not (p & 0xFC000000 == 0x14000000 or p & 0xFFFFFC1F in (0xD61F0000, 0xD65F0000) or p & 0xFFE0001F == 0xD4200000): continue
        if any(a < s and k < b for a, b in data): continue
        out.extend(range(k, s, 4))
    return out
def reach_via(frm, to):
    """to when a b at frm reaches it, else a hop that does (and that reaches to)"""
    global _holes
    if -REACH - 4 <= to - frm <= REACH: return to
    for h in _hops.get(to, ()):
        if abs(h - frm) <= REACH: return h
    if _holes is None: _holes = hop_holes()
    lo, hi = max(frm, to) - REACH, min(frm, to) + REACH
    i = bisect.bisect_left(_holes, lo)
    assert i < len(_holes) and _holes[i] <= hi, f'no padding word for a hop from {frm:#x} to {to:#x} (between {lo:#x} and {hi:#x})'
    h = _holes.pop(i)
    assert IMG[h:h + 4] == ORIG_TEXT[h:h + 4], f'hop {h:#x} was written by something else'
    struct.pack_into('<I', IMG, h, B(h, to)); _hops.setdefault(to, []).append(h); stat('hop')
    return h
def BX(frm, to): return B(frm, reach_via(frm, to))
def exclusive_order_sites(img, ranges):
    """Offsets of the exclusive loads and stores without ordering: ldxr / ldxrb / ldxrh / ldxp and stxr / stxrb / stxrh / stxp, any size (each with its fixed fields exact; not CAS or CASP).
    Setting bit 15 makes each the same instruction with ordering: ldaxr* / ldaxp (acquire) and stlxr* / stlxp (release). Armv8 lets the accesses after an exclusive load without acquire
    be satisfied before it, and those before an exclusive store without release be seen after it. The games ran on the Switch's Cortex-A57 without that showing, Apple cores do both:
    a lock taken with ldxr / stxr let two threads take the same queue item (Breath of the Wild 1.6.0's job queue pop at main+0x13a0784), and on an Apple M6 a plain store published
    with stxr or stxp was read stale 12,858 and 10,961 times in 100 million rounds, never with stlxr or stlxp (docs/reference/threads.md). Every one is upgraded, with no shape list:
    ordering only removes outcomes, and an atomic increment cost the same with stlxr as with stxr there. Nintendo's own SDK atomics already use the ordered forms"""
    out = []
    for s, e in ranges:
        for m in re.finditer(rb'[\x08\x48\x88\xC8]', img[s:e]):   # the top byte of every form: size << 6 | 0x08
            o = s + m.start() - 3
            if o & 3 or o < s: continue
            w = struct.unpack_from('<I', img, o)[0]
            if (w & 0x3FFFFC00 == 0x085F7C00 or w & 0xBFFF8000 == 0x887F0000           # ldxr* (Rs and Rt2 all ones), ldxp (Rs all ones)
                    or w & 0x3FE0FC00 == 0x08007C00 or w & 0xBFE08000 == 0x88200000):   # stxr* (Rt2 all ones), stxp
                out.append(o)
    return out
ORDERED = set(exclusive_order_sites(IMG, CODE_RANGES))   # changed_runs leaves these out: a cheat that writes over one runs its own instruction, which no site depends on
for o in ORDERED:
    struct.pack_into('<I', IMG, o, struct.unpack_from('<I', IMG, o)[0] | 0x8000)   # ldxr → ldaxr, stxr → stlxr, ldxp → ldaxp, stxp → stlxp
if ORDERED: stats['exclusive_ordered'] = len(ORDERED)
def odd_sp_frames(img, ranges, md):
    """(sub offset, add offset, size) of stack frames whose size is not a multiple of 16: sub sp, sp, #imm, then within 64 instructions add sp, sp, #imm with the same imm. Horizon leaves stack
    alignment checking off for user code, Apple's kernel does not, so the first sp-based access in such a frame faults (SIGBUS, addr=0). Civilization VII's main has four hand-written blocks
    (main+0x4e8a14, +0x556914, +0x57e914, +0x57ec18) that save x1–x17 in a 0x98-byte frame around an svc GetInfo; the first of them runs on the UI's second screen change. Rounding both
    immediates up to 16 leaves the code as it was only when nothing between them reaches past the frame, so a frame is taken only when every instruction between is: one that does not use sp,
    a call or an svc (no other branch: the add must close every path), or a load / store with sp as its only base, no writeback, at an offset whose bytes all lie inside the frame. Any
    other use of sp (a copy of it, another adjustment) leaves the frame alone. Compiled code keeps frames 16-aligned, so there it finds nothing"""
    from capstone import CS_GRP_CALL, CS_GRP_INT, CS_GRP_JUMP, CS_GRP_RET
    from capstone.arm64 import ARM64_OP_MEM, ARM64_OP_REG, ARM64_REG_SP
    WIN = 64
    NARROW = {'ldrb': 1, 'strb': 1, 'ldrsb': 1, 'ldurb': 1, 'sturb': 1, 'ldursb': 1, 'ldrh': 2, 'strh': 2, 'ldrsh': 2, 'ldurh': 2, 'sturh': 2, 'ldursh': 2, 'ldrsw': 4, 'ldursw': 4}
    REG_BYTES = {'x': 8, 'w': 4, 'q': 16, 'd': 8, 's': 4, 'h': 2, 'b': 1}
    def inside(ins, size):   # True when the instruction leaves sp alone, or reads the frame within its first size bytes
        if ins.group(CS_GRP_RET) or ins.group(CS_GRP_JUMP) and not ins.group(CS_GRP_CALL): return False
        if ins.group(CS_GRP_CALL) or ins.group(CS_GRP_INT): return True
        r, w = ins.regs_access()
        if ARM64_REG_SP not in r and ARM64_REG_SP not in w: return True
        mems = [op.mem for op in ins.operands if op.type == ARM64_OP_MEM]
        regs = [ins.reg_name(op.reg) for op in ins.operands if op.type == ARM64_OP_REG]
        if ARM64_REG_SP in w or ins.writeback or len(mems) != 1 or mems[0].base != ARM64_REG_SP or mems[0].index or not regs or len(regs) > 2: return False
        width = NARROW.get(ins.mnemonic) or REG_BYTES.get(regs[0][0])
        return width is not None and 0 <= mems[0].disp and mems[0].disp + width * len(regs) <= size
    out = []
    for s, e in ranges:
        for m in re.finditer(rb'\xD1', img[s:e]):   # the top byte of sub (immediate, 64-bit): a word-by-word walk took 2.7 s on a 134 MiB main
            o = s + m.start() - 3
            if o & 3 or o < s or o + 4 > e: continue
            w = struct.unpack_from('<I', img, o)[0]
            if w & 0xFFC003FF != 0xD10003FF or not w >> 10 & 15: continue   # sub sp, sp, #imm (64-bit, no shift), imm not a multiple of 16
            size = w >> 10 & 0xFFF
            add = 0x910003FF | size << 10
            for q in range(o + 4, min(e, o + 4 + 4 * WIN), 4):
                if struct.unpack_from('<I', img, q)[0] == add:
                    out.append((o, q, size)); break
                ins = next(md.disasm(bytes(img[q:q + 4]), q), None)
                if ins is None or not inside(ins, size): break
    return out
def sp_adjust(op, n):
    """sub (op 0xD10003FF) or add (op 0x910003FF) sp, sp, #n. The immediate is 12 bits, optionally shifted left by 12: a frame of 0xff1–0xfff rounds up to 0x1000, which
    only the shifted form holds (#1, lsl #12); written unshifted, its bit 12 would land in the shift bit and make the instruction sub sp, sp, #0, lsl #12, a frame of nothing"""
    if n <= 0xFFF: return op | n << 10
    assert not n & 0xFFF and n >> 12 <= 0xFFF, hex(n)
    return op | 1 << 22 | n >> 12 << 10
for s, a, size in odd_sp_frames(IMG, CODE_RANGES, md):   # both immediates rounded up to 16 (odd_sp_frames)
    r = (size + 15) & ~15
    struct.pack_into('<I', IMG, s, sp_adjust(0xD10003FF, r)); struct.pack_into('<I', IMG, a, sp_adjust(0x910003FF, r))
    stat('sp_frame')
def trinity_completer_sites(img, ranges):
    """Offsets of the completer of Trinity's per-frame rendezvous (Game Freak; Pokémon Scarlet 1.0.0 main+0x28f872c, 4.0.0 main+0x2d0010c):
      stp x29,x30,[sp,#-0x20]!; stp x20,x19,[sp,#0x10]; mov x29,sp; ldr x8,[x0,#imm]; mov w9,#1; strb w9,[x8,#A]; ldrb w9,[x8,#A+1]; cbz w9,..;
      ldr x20,[x8,#B]; add x9,x20,#C; ldar w8,[x9]; cmp w8,#2; b.ne ..
    It sets the release flag with the plain strb and returns without a wake if the state word (ldar) is 2. A load-acquire does not order an earlier plain store, so the flag can stay
    invisible while main resets the state to 0, reads the flag as 0 and parks for good (GitHub issue #1250, docs/reference/threads.md). The frame holds only the saved registers, so nothing
    in the function reads x29"""
    out, needle = [], struct.pack('<II', 0x88DFFD28, 0x7100091F)   # ldar w8,[x9]; cmp w8,#2
    for s, e in ranges:
        o = img.find(needle, s, e)
        while o >= 0:
            f = o - 40
            if o % 4 == 0 and f >= s and o + 12 <= e:
                w = struct.unpack_from('<13I', img, f)
                if w[:3] == (0xA9BE7BFD, 0xA9014FF4, 0x910003FD) and w[3] & 0xFFC003FF == 0xF9400008 and w[4] == 0x52800029 and w[5] & 0xFFC003FF == 0x39000109 and \
                        w[6] & 0xFFC003FF == 0x39400109 and w[6] >> 10 & 0xFFF == (w[5] >> 10 & 0xFFF) + 1 and w[7] & 0xFF00001F == 0x34000009 and w[8] & 0xFFC003FF == 0xF9400114 and \
                        w[9] & 0xFFC003FF == 0x91000289 and w[12] & 0xFF00001F == 0x54000001:
                    out.append(f)
            o = img.find(needle, o + 4, e)
    return out
def trinity_completer_barrier(img, f):
    """The frame pointer's mov gives up its word: the ldr, the mov and the strb move up one and dmb ish follows the strb. Backtraces skip this frame, and nothing in it reads x29"""
    struct.pack_into('<4I', img, f + 8, *struct.unpack_from('<3I', img, f + 12), 0xD5033BBF)
trinity = trinity_completer_sites(IMG, CODE_RANGES)
if len(trinity) > 1: print(f'[RED] {NAME}: {len(trinity)} functions have the shape of Trinity\'s rendezvous completer, so none gets its barrier: ' + ', '.join(hex(o) for o in trinity))
elif trinity:
    trinity_completer_barrier(IMG, trinity[0])
    stat('trinity_completer')
md_lite = Cs(CS_ARCH_ARM64, CS_MODE_ARM)   # no details: the x18 scan reads only the operand text
R18 = re.compile(r'\b[xw]18\b')
def x18_words(lo, hi):
    """offsets in [lo, hi) of the instructions whose operands name x18 / w18 (as has18). Decoded 16 KB at a time without details, going on after a word capstone cannot decode:
    a third of the time of decoding one word at a time with details (Paper Mario: The Thousand-Year Door main, 437 sites in 300,000 words either way)"""
    out, a = set(), lo
    while a < hi:
        end = min(hi, a + 0x4000); last = a - 4
        for addr, size, mn, op in md_lite.disasm_lite(bytes(IMG[a:end]), a):
            last = addr
            if R18.search(op): out.add(addr)
        a = last + 4 if last + 4 >= end else last + 8   # the chunk is done, or the word after the last decoded one is no instruction
    return out
# The x18 scan and the spin wait search only read the original image, so they can run in worker processes.
def in_parallel(fn, jobs):
    """Returns [fn(*job) for job in jobs], computed in forked worker processes.
    The workers are forked on each call, so they see the module's state as it is at that point.
    Pool.terminate takes the job queue's lock for good, sends each worker one SIGTERM and joins it with no timeout, so that SIGTERM must end a worker wherever it lands. Python's handler
    only sets a flag, which Python clears in a new child, so a worker just forked could lose the signal, wait for that lock forever and hang the rewrite (jobs too small for every worker to
    start first). So SIGTERM stays blocked across the fork, which keeps it pending in the kernel, until a worker has the default action back and that action can end it.
    In this process it acts only while waiting for the results: the handler's sys.exit raised during the pool's teardown could stop a terminate halfway and leave workers behind, or be discarded
    in a destructor."""
    if WORKERS < 2 or len(jobs) < 2:
        return [fn(*job) for job in jobs]
    def worker_signals():
        signal.signal(signal.SIGTERM, signal.SIG_DFL)
        signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGTERM})
    mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM})   # the pool's threads inherit it: a worker that replaces a lost one starts the same way
    try:
        with multiprocessing.get_context('fork').Pool(min(WORKERS, len(jobs)), initializer=worker_signals) as pool:
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)   # a SIGTERM that came meanwhile acts here, inside the with, which reaps the workers
            try: results = pool.starmap(fn, jobs)
            finally: signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM})
            pool.close(); pool.join()   # finished: the workers leave by the pool's own sentinels, so a finished rewrite sends no SIGTERM
            return results
    finally:
        pool = None   # its destructors run here, still blocked
        signal.pthread_sigmask(signal.SIG_SETMASK, mask)

def find_x18_words():
    """x18_words over all the code, in 1 MB pieces"""
    pieces = []
    for start, end in CODE_RANGES:
        for lo in range(start, end, 0x100000):
            pieces.append((lo, min(lo + 0x100000, end)))
    words = set()
    for found in in_parallel(x18_words, pieces):
        words |= found
    return words

x18 = find_x18_words()
kinds = {}
# Spin wait candidates (see "Spin waits" below): backward direct branches (b, b.cond, cbz / cbnz, tbz / tbnz) at most SPIN_WIN bytes back, by target
SPIN_WIN, SPIN_TOPS = 512, {0x16, 0x17, 0x54, 0x34, 0x35, 0xB4, 0xB5, 0x36, 0x37, 0xB6, 0xB7}
spin_cand = {}
for o in (o for s, e in CODE_RANGES for o in range(s, e, 4)):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w >> 24 in SPIN_TOPS:
        t = w >> 24
        if t in (0x16, 0x17): d = ((w & 0x3FFFFFF) - (1 << 26)) * 4
        elif t & 0x7E == 0x36: d = (((w >> 5) & 0x3FFF) - (1 << 14)) * 4 if w & 0x40000 else 4
        elif t == 0x54: d = (((w >> 5) & 0x7FFFF) - (1 << 19)) * 4 if w & 0x800000 and not w & 0x10 else 4
        else: d = (((w >> 5) & 0x7FFFF) - (1 << 19)) * 4 if w & 0x800000 else 4
        if -SPIN_WIN <= d <= 0: spin_cand.setdefault(o + d, []).append(o)
    if w & 0xFFE0001F == 0xD4000001: kinds[o] = ('svc', (w >> 5) & 0xFFFF); continue
    if w & 0xFFFFFFE0 == 0xD53BD040: kinds[o] = ('tpidr_r', None); continue
    if w & 0xFFFFFFE0 == 0xD51BD040: kinds[o] = ('tpidr_w', None); continue
    if w & 0xFFFFFFE0 == 0xD53BD060: kinds[o] = ('tpidrro_r', None); continue
    if w & 0xFFFFFFE0 == 0xD53BE020: kinds[o] = ('cntpct', w & 31); continue
    if w & 0xFFFFFFE0 == 0xD53BE040: kinds[o] = ('cntvct', w & 31); continue
    # mrs xN, ctr_el0 (cache type): the Apple kernel does not expose it to user mode (SIGILL); SDK 16's nn::os::FlushDataCache reads it for the cache line size
    if w & 0xFFFFFFE0 == 0xD53B0020: kinds[o] = ('ctr', w & 31); continue
    # sub sp, sp, xN (extended register, LSL #0): sp may end up misaligned, which Horizon allows and Apple's kernel does not (site_sp_align); with x18 it is an x18 site (site_sp)
    if w & 0xFFE0FFFF == 0xCB2063FF and (w >> 16) & 31 != 18: kinds[o] = ('sp_align', (w >> 16) & 31); continue
    if o in x18: kinds[o] = ('x18', None)

# Take the instruction table per function, find the windows and jump tables
import bisect
def func_of(a):
    """The function holding a: from the FDE start at or before it to the next one. Code before the first FDE is one function as well, from the end of the data block before it (or the text's
    start) to the first FDE: FINAL FANTASY RESONANCE 1.0.1 main (Unreal Engine 5.6.1) has no unwind entry before main+0x6a75d4, and each of its sites there used to be a function of one
    instruction, so the byte jump table at main+0x219234 (adr x18; ldrb w0,[x17,x16]; add x18,x18,x0,lsl #2; br x18) was never seen. Of the 120 modules of 33 games on hand, only one other has x18
    sites before its first FDE (DRAGON QUEST VII Reimagined main, 110 before +0xb310, none in an exclusive window or a br x18), so every other rewrite is byte-identical"""
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0: return (starts[i], starts[i + 1] if i + 1 < len(starts) else TEXT_END)
    return (max((e for s, e in DATA_BLOCKS if e <= a), default=0), starts[0]) if starts else (a, a + 4)
done = set(); windows = []; jumps = {}; tabjumps = {}; tailcalls = set(); retries = []
def tail_call_at_end(insl, j):
    """The br x18 is an indirect tail call, not a jump table: the last write of x18 before it is a plain pointer load and the br ends the function. Ace Attorney Investigations Collection
    (Unity 2022.3 IL2CPP) main+0x1d271bc: ldr x18,[x0,#0x18]; ...; ldr x16,[sp]; ...; br x18, a thunk that moves stack arguments and jumps to the method pointer at +0x18"""
    if j + 1 != len(insl): return False
    for p in reversed(insl[:j]):
        if 'x18' not in [p.reg_name(r) for r in p.regs_access()[1]]: continue
        return p.mnemonic == 'ldr' and bool(re.fullmatch(r'x18, \[(x\d+|sp)(, #(0x[0-9a-f]+|\d+))?\]', p.op_str))
    return False
def tail_call_padded(insl, j):
    """An unframed, straight-line pointer thunk ending in br x18 and unreachable NOP or zero trap padding."""
    if j + 1 >= len(insl) or any(not (p.mnemonic == 'nop' or p.mnemonic == 'udf' and p.op_str == '#0') for p in insl[j + 1:]): return False
    if any(b.address != a.address + 4 for a, b in zip(insl, insl[1:])): return False
    for p in insl[:j]:
        if p.mnemonic not in ('mov', 'ldr', 'ldrh', 'ldp', 'add', 'str', 'stp'): return False
        if {p.reg_name(r) for r in p.regs_access()[1]} & {'sp', 'wsp', 'fp', 'lr', 'x29', 'w29', 'x30', 'w30'}: return False
    for p in reversed(insl[:j]):
        if not {'x18', 'w18'} & {p.reg_name(r) for r in p.regs_access()[1]}: continue
        return p.mnemonic in ('ldr', 'ldp') and bool(re.fullmatch(r'x18, (x\d+, )?\[(x\d+|sp)(, #(0x[0-9a-f]+|\d+))?\]', p.op_str))
    return False
def tail_call(insl, j):
    """After terminal or padded unframed tails, a br x18 with other code after it is a tail call only when the frame is provably gone: a jump inside a function needs that function's frame.
    A wrong accept is silent, where a rejection stops at `jump table <addr>`, so anything not proven here is rejected. Two forms (Warm Snow and Ori and the Blind Forest, Unity IL2CPP):
    a 16-byte thunk, stp x29,x30,[sp,#-0x10]!; mov x29,sp, only guards and argument moves, its error blocks after the br; or an ordinary function, sub sp,sp,#N first and add sp,sp,#N right
    before the br, the pair saved to and restored from one slot, whose body may branch and call (lazy init, loops) and whose other paths may follow the br. Either way nothing writes sp, x29
    or x30 on the way, no stack store touches the pair, and from the restore to the br every path runs straight"""
    if j + 1 == len(insl): return tail_call_at_end(insl, j)
    if tail_call_padded(insl, j): return True
    if j < 4 or any(b.address != a.address + 4 for a, b in zip(insl, insl[1:])): return False
    writes = lambda p: {p.reg_name(r) for r in p.regs_access()[1]}
    cond = lambda p: p.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz') or p.mnemonic.startswith('b.')
    # The last write of x18 is a plain pointer load
    load = next((k for k in range(j - 1, -1, -1) if writes(insl[k]) & {'x18', 'w18'}), None)
    if load is None or insl[load].mnemonic not in ('ldr', 'ldp') or not re.fullmatch(r'x18, (x\d+, )?\[(x\d+|sp)(, #(0x[0-9a-f]+|\d+))?\]', insl[load].op_str): return False
    # The frame. disp is the saved pair's offset from sp
    thunk = insl[0].mnemonic == 'stp' and insl[0].op_str == 'x29, x30, [sp, #-0x10]!'
    if thunk:
        if insl[1].mnemonic != 'mov' or insl[1].op_str != 'x29, sp': return False
        if insl[j - 1].mnemonic != 'ldp' or insl[j - 1].op_str != 'x29, x30, [sp], #0x10': return False
        restore, start, disp, saved, fp_set = j - 1, 2, 0, True, True
    else:
        sub, add = (re.fullmatch(r'sp, sp, #(0x[0-9a-f]+|\d+)', p.op_str) if p.mnemonic == m else None for p, m in ((insl[0], 'sub'), (insl[j - 1], 'add')))
        if not sub or not add or int(sub[1], 0) != int(add[1], 0): return False
        restore = next((k for k in range(j - 2, 0, -1) if insl[k].mnemonic == 'ldp' and re.fullmatch(r'x29, x30, \[sp(, #(0x[0-9a-f]+|\d+))?\]', insl[k].op_str)), None)
        if restore is None: return False
        disp = insl[restore].operands[-1].mem.disp
        if disp < 0 or disp + 16 > int(sub[1], 0): return False
        start, saved, fp_set = 1, False, False
    tail = min(load, restore)   # no call or jump from here on
    after_br = insl[j].address
    setup_end = insl[start].address   # no branch re-enters the frame's setup: raised as the setup is walked
    targets = []
    guarded = False
    for k in range(start, j - 1):   # the body and the epilogue; j - 1 is the frame's last restore or add sp
        p = insl[k]
        if k == restore: continue
        if cond(p) or p.mnemonic == 'b':
            t = p.operands[-1].imm
            if not saved or k >= restore or p.mnemonic == 'b' and k >= tail: return False
            if t > after_br: guarded = True
            elif thunk: return False
            targets.append(t)
            continue
        if p.mnemonic in ('bl', 'blr'):
            # A saved x30, and x18 not live across the call
            if thunk or not saved or k >= tail: return False
            continue
        if p.mnemonic in ('br', 'ret', 'udf', 'brk', 'hlt', 'svc', 'hvc', 'smc', 'msr', 'sys', 'dc', 'ic'): return False
        w = writes(p)
        if w & {'sp', 'wsp', 'lr', 'x30', 'w30'}: return False
        if w & {'fp', 'x29', 'w29'}:
            sets_fp = p.op_str == 'x29, sp' and not disp if p.mnemonic == 'mov' else p.mnemonic == 'add' and p.op_str.startswith('x29, sp, #') and p.operands[2].imm == disp
            if k > restore or fp_set or not saved or not sets_fp: return False
            fp_set = True
            setup_end = insl[k + 1].address
            continue
        if not saved and p.mnemonic == 'stp' and p.op_str == insl[restore].op_str:
            saved = True
            setup_end = insl[k + 1].address
            continue
        if '[' not in p.op_str or p.mnemonic.startswith(('ldr', 'ldur', 'ldp', 'ldar', 'prfm')): continue
        # regs_access does not report memory writes, so a stack store's bytes are checked against the saved pair. A register offset is unproven. A store through another base can
        # alias it only through a pointer to the frame's own pair, which no compiled code holds; a function that calls out cannot prove that of its callees either, so only the thunk rejects them
        if k > restore or p.mnemonic not in ('str', 'strb', 'strh', 'stur', 'sturb', 'sturh', 'stp') or p.writeback: return False
        mem = p.operands[-1].mem
        base = p.reg_name(mem.base)
        if base not in ('sp', 'fp', 'x29'):
            if thunk: return False
            continue
        if mem.index or base != 'sp' and not fp_set: return False
        at = mem.disp + (disp if base != 'sp' else 0)
        size = {'b': 1, 'h': 2}.get(p.mnemonic[-1]) or {'x': 8, 'w': 4, 'q': 16, 'd': 8, 's': 4, 'h': 2, 'b': 1}.get(p.reg_name(p.operands[0].reg)[0], 8)
        if p.mnemonic == 'stp': size *= 2
        if at < 0 or at < disp + 16 and at + size > disp: return False
    if not saved: return False
    # What follows the br never comes back to a place whose stack it cannot vouch for. A thunk's is error blocks entered by a guard, leaving through a call or a trap; a function's may be any of its other paths
    suffix = insl[j + 1:]
    leaves = False
    for p in suffix:
        if p.mnemonic == 'bl':
            if insl[0].address <= p.operands[-1].imm <= insl[-1].address: return False
            leaves = True
        elif (cond(p) or p.mnemonic == 'b') and p.operands[-1].imm <= after_br: return False
    if thunk and (not guarded or not leaves or suffix[-1].mnemonic not in ('bl', 'udf', 'b') or any(p.mnemonic in ('ret', 'br', 'blr') for p in suffix)): return False
    # Nothing re-enters the frame's setup, or lands past the restore: the frame's end is what the proof rests on. A guard may skip a reload of x18 (Subnautica main+0x24f210 through x2)
    return not any(t < setup_end or insl[restore].address < t <= after_br for t in targets)

def jump_bytes_far(insl, j, half):
    """The byte / halfword table form with its add further back (Octopath Traveler 0 main+0x37f9bc8: adr x18,#L; ldrb w0,[x17,x15]; add x18,x18,x0,lsl #2, then 17 register moves before br x18).
    Up to 32 instructions back, and x18 must not change between the add and the br. Tried only where the 16-instruction search and the offset forms find nothing. Returns (base, width) or None;
    half(base): the pads a halfword table needs (half_pads)"""
    ctx = insl[max(0, j - 32):j]
    for k in range(len(ctx) - 1, -1, -1):
        p = ctx[k]
        if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str: break
        if writes_reg(p, 'x18'): return None
    else: return None
    rn = p.reg_name(p.operands[2].reg); idx = {'lr': 30, 'fp': 29}.get(rn, num(rn))
    base = next((q.operands[1].imm for q in reversed(ctx[:k]) if q.mnemonic == 'adr' and q.op_str.startswith('x18,')), None)
    ld = next((q for q in reversed(ctx[:k]) if q.mnemonic in ('ldrb', 'ldrsb', 'ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx), None)
    if base is None or ld is None: return None
    return base, 256 if ld.mnemonic in ('ldrb', 'ldrsb') else half(base) or 65536
def disasm_fn(fa, fb):
    """A function's instructions. Capstone stops at the first word it cannot decode, so go on after it: Monster Hunter Rise main+0x6b34540 is a trap word (0xe7ffdefe)
    in the middle of a function, and the br x18 jump table after it (main+0x6b346d0) was never seen"""
    out, a = [], fa
    while a < fb:
        l = list(md.disasm(bytes(IMG[a:fb]), a))
        out += l; a = (l[-1].address + 4 if l else a) + 4
    return out
# Text with no unwind entry is one function to func_of. FINAL FANTASY RESONANCE 1.0.1 main (Unreal Engine 5.6.1) has 82 MB of it (main+0x6c5008 to +0x59220c0) beside the 6.65 MB before its
# first FDE: disassembled with details at once, it took 100 GB of memory and the rewrite never finished on a 16 GB Mac. A function longer than BIG_FN is analysed in pieces of about PIECE
# bytes, cut only right after an unconditional ret / br / b where no conditional branch and no b within ±PIECE_B crosses: a window, a jump table's context (the bounds check's branch
# and the table base before it) and a retry block reach each other by fall-through or by such branches, so each lies within one piece. Farther b / bl that cross a cut (calls and
# tail calls) are checked once every piece is done: none may land inside a window or a retry block (far_into). With b up to 1 MB counted, tail calls between neighbouring functions left
# 21,385 cut points in that 82 MB and pieces of up to 6.5 MB (about 4 GB while disassembled); with 64 KB, 185,228 points and none over PIECE
BIG_FN, PIECE, PIECE_B = 0x40000, 0x40000, 0x10000
def pieces(fa, fb):
    """[(start, end)] covering [fa, fb), and the far b / bl in it as [(site, target)]"""
    from array import array
    n = (fb - fa) // 4
    w = memoryview(bytes(IMG[fa:fa + 4 * n])).cast('I')
    cross = array('i', bytes(4 * (n + 2))); far = []
    for i in range(n):
        x = w[i]; t = x >> 24
        if t == 0x54 and not x & 0x10 or t & 0x7E == 0x34: d = (x >> 5) & 0x7FFFF; d -= (d & 0x40000) << 1   # b.cond, cbz / cbnz
        elif t & 0x7E == 0x36: d = (x >> 5) & 0x3FFF; d -= (d & 0x2000) << 1                                  # tbz / tbnz
        elif t & 0x7C == 0x14:                                                                                # b, bl
            d = x & 0x3FFFFFF; d -= (d & 0x2000000) << 1
            if t & 0x80 or 4 * abs(d) > PIECE_B: far.append((fa + 4 * i, fa + 4 * (i + d))); continue
        else: continue
        lo, hi = max(min(i, i + d), -1), min(max(i, i + d), n)   # the branch crosses the cut before instruction k when lo < k <= hi
        cross[lo + 1] += 1; cross[hi + 1] -= 1
    cand, c = [], 0
    for k in range(n):
        c += cross[k]
        x = w[k - 1] if k else 0
        if k and not c and (x & 0xFFFFFC1F in (0xD65F0000, 0xD61F0000) or x >> 26 == 0x05): cand.append(k)   # after ret / br / b, nothing crossing
    cuts, s = [], 0
    while s + PIECE // 4 < n:
        j = bisect.bisect_right(cand, s + PIECE // 4) - 1
        if j < 0 or cand[j] <= s: j = bisect.bisect_right(cand, s + PIECE // 4)
        if j >= len(cand): break
        s = cand[j]; cuts.append(s)
    b = [0] + cuts + [n]
    return [(fa + 4 * p, fa + 4 * q) for p, q in zip(b, b[1:])], far
funcs = sorted({func_of(a) for a, (k, _) in kinds.items() if k == 'x18'})
far_into, rmid_all, newly_accepted = [], set(), set()
if any(fb - fa > BIG_FN for fa, fb in funcs):
    sites18 = sorted(a for a, (k, _) in kinds.items() if k == 'x18'); split = []
    for fa, fb in funcs:
        if fb - fa <= BIG_FN: split.append((fa, fb)); continue
        ps, far = pieces(fa, fb); far_into += far
        split += [(p, q) for p, q in ps if bisect.bisect_left(sites18, p) < bisect.bisect_left(sites18, q)]   # the pieces that hold x18 sites
        print(f'function {fa:#x}..{fb:#x} ({(fb - fa) / 2**20:.2f} MB, no unwind entries) analysed in {len(ps)} pieces')
    funcs = split
# Offset tables (ldrsw / ldrb / ldrh + add + br x18) are proved by jump_solver, over each function's (or piece's) control flow: the values the index can take at the
# load and the base on every path, every dispatch of the function together (see docs/reference/rewriter.md, "Jump tables proved over control flow"). Direct branches from
# the rest of the module are entries of the functions they land in; targets of tables elsewhere are checked once every function is done
import jump_solver
def branch_entries(ranges):
    """Direct branches (b, bl, b.cond, cbz / cbnz, tbz / tbnz) from anywhere in the module's code into each range from outside it: {range: {target}}. A data word that
    decodes as a branch only adds an entry, which makes a proof stricter. Only the words whose top byte is a branch's are decoded (found with bytes.translate and re, in
    C): b / bl in the whole text, the conditional branches, which reach 1 MB, only near the ranges (2.9 s for the 130 MB text of Sonic Superstars' main)"""
    out = {r: set() for r in ranges}
    if not ranges: return out
    lo_ = [a for a, _ in ranges]
    def marks(tops):
        b = bytearray(256)
        for t in tops: b[t] = 1
        return bytes(b)
    far, near = marks([*range(0x14, 0x18), *range(0x94, 0x98)]), marks([0x54, 0x34, 0x35, 0xB4, 0xB5, 0x36, 0x37, 0xB6, 0xB7])
    def scan(start, end, mark):
        n = (end - start) // 4
        if n <= 0: return
        data = bytes(IMG[start:start + 4 * n]); words = memoryview(data).cast('I')
        for m_ in re.finditer(b'\x01', data[3::4].translate(mark)):
            i = m_.start(); w = words[i]; t = w >> 24
            if t == 0x54 or t & 0x7E == 0x34:
                if t == 0x54 and w & 0x10: continue
                d = (w >> 5) & 0x7FFFF; d -= (d & 0x40000) << 1
            elif t & 0x7E == 0x36: d = (w >> 5) & 0x3FFF; d -= (d & 0x2000) << 1
            else: d = w & 0x3FFFFFF; d -= (d & 0x2000000) << 1
            at = start + 4 * i; dest = at + 4 * d
            k = bisect.bisect_right(lo_, dest) - 1
            if k >= 0 and dest < ranges[k][1] and not ranges[k][0] <= at < ranges[k][1]: out[ranges[k]].add(dest)
    windows = []
    for a, b in ranges:   # merged windows a conditional branch can reach the ranges from
        a, b = max(0, a - (1 << 20)) & ~3, b + (1 << 20)
        if windows and a <= windows[-1][1]: windows[-1][1] = max(windows[-1][1], b)
        else: windows.append([a, b])
    for start, end in CODE_RANGES:
        scan(start, end, far)
        for a, b in windows:
            if max(start, a) < min(end, b): scan(max(start, a), min(end, b), near)
    return out
def code_ok(a): return any(s_ <= a < e_ for s_, e_ in CODE_RANGES)
solved, entries_in, pad_starts, pads_at = {}, {}, [], {}
fde_starts = set(starts)
def solve_fn(fa, fb, insl, outside=()):
    global pad_starts
    if not solved and not entries_in:
        entries_in.update(branch_entries(funcs))
        pads_at.update(nso_code.landing_pads(IMG)); pad_starts = sorted(pads_at)   # C++ exception landing pads and the calls they catch
    i, j = bisect.bisect_left(pad_starts, fa), bisect.bisect_left(pad_starts, fb)
    landing = {a: pads_at[a] for a in pad_starts[i:j]}
    unwound = (fa, fb) == func_of(fa) and fa in fde_starts   # a function its FDE describes, not a piece of text without unwind entries
    solved[(fa, fb)] = jump_solver.solve(insl, IMG, code_ok, entries_in.get((fa, fb), ()), outside, landing, unwound)
    return solved[(fa, fb)]
def table_at(fa, fb, insl, at):
    """(targets, None) for the table the br at at reads, or (None, why)"""
    found, failed = solved[(fa, fb)] if (fa, fb) in solved else solve_fn(fa, fb, insl)
    return found.get(at), failed.get(at, 'not a dispatch')
def half_pads(fa, fb, insl, at, base):
    """Landing pads a halfword table needs (ldrh wE,[xT,xI,lsl #1]; ...; adr x18,#L; add x18,x18,xE,lsl #2; br x18: target = L + 4 x entry): its largest entry + 1, from the
    table the solver read, or None (site_br then gives all 65,536 entries a pad, 768 KB: Animal Crossing: New Horizons 3.0.3 main has 51 such tables, 40 MB, which put its patch
    area's end beyond the +-128 MB a B reaches). Byte tables keep their 256 pads (3 KB)"""
    tgt, _ = table_at(fa, fb, insl, at)
    if not tgt or any(t < base or (t - base) % 4 for t in tgt): return None
    return max(t - base for t in tgt) // 4 + 1
selects = {}
function_ends = dict(nso_code.fde_ranges(IMG))
for fa, fb in funcs:
    insl = disasm_fn(fa, fb)
    by_address = None
    ws = find_windows(insl)
    for wseg in ws:
        windows.append(wseg); done |= {x.address for x in wseg}
        if any(x.mnemonic == 'b' for x in wseg): newly_accepted.add(wseg[0].address)
    for j, ins in enumerate(insl):
        if ins.mnemonic == 'csel' and fa in function_ends and ins.address < function_ends[fa] <= fb and ins.address not in done:
            if args := select_x18(ins):
                if by_address is None: by_address = {p.address: p for p in insl}
                temp = select_scratch(ins.address, by_address, function_ends[fa], args[0])
                if temp is not None: selects[ins.address] = args, temp
        if ins.mnemonic == 'br' and ins.op_str == 'x18':
            base = width = None
            # 16 instructions back, as for the offset-table form: Hades main+0x1135d0 has six vector moves between the add and the br, which put its adr x18 nine back
            ctx = insl[max(0, j - 16):j]
            for p in reversed(ctx):
                if p.mnemonic == 'adr' and p.op_str.startswith('x18,'): base = p.operands[1].imm; break
            for p in reversed(ctx):
                if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                    # capstone names x30 'lr' (and x29 'fp'): Age of Calamity main+0x1382160 indexes with add x18,x18,x30,lsl #2 after ldrb w30,[x14,x12]
                    rn = p.reg_name(p.operands[2].reg); idx = {'lr': 30, 'fp': 29}.get(rn, num(rn))
                    for q in reversed(ctx):
                        if q.mnemonic in ('ldrb', 'ldrsb') and num(q.reg_name(q.operands[0].reg)) == idx: width = 256
                        if q.mnemonic in ('ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx: width = (half_pads(fa, fb, insl, ins.address, base) if q.mnemonic == 'ldrh' else None) or 65536 if base else 65536
                        if width: break
                    break
            if not (base and width):
                if (far := jump_bytes_far(insl, j, lambda b, fa=fa, fb=fb, insl=insl, at=ins.address: half_pads(fa, fb, insl, at, b))): jumps[ins.address] = far; continue   # a byte or halfword table keeps its pads wherever its add is
                tgt, why = table_at(fa, fb, insl, ins.address)
                if not tgt and tail_call(insl, j): tailcalls.add(ins.address); continue
                assert tgt, f'jump table {ins.address:x}: {why}'
                tabjumps[ins.address] = tgt; continue
            jumps[ins.address] = (base, width)
    # No jump may land in the middle of a window, except a retry block's branch onto the window's store-exclusive (retry_block), which gets a copy of its own
    inwin = {x.address for wseg in ws for x in wseg}; heads = {wseg[0].address for wseg in ws}
    rmid = set()
    for k, x in enumerate(insl):
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            t = x.operands[-1].imm
            if t in inwin and t not in heads:
                wseg = next(w for w in ws if any(y.address == t for y in w))
                if x.mnemonic != 'bl' and any(y.address == x.address for y in wseg):
                    newly_accepted.add(wseg[0].address)
                    continue
                rb = retry_block(insl, k, wseg) if x.mnemonic not in ('b', 'bl') else None
                assert rb, f'branch into window {x.address:x}'
                assert not any(y.address in done for y in rb[0]), f'retry block overlaps a window {rb[0][0].address:x}'
                tail_addrs = {y.address for y in rb[1]}
                for y in rb[1]:
                    if y.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz') or y.mnemonic.startswith('b.'):
                        dest = y.operands[-1].imm
                        assert dest not in inwin or dest in heads or dest in tail_addrs, f'retry copy branches into window {y.address:x}'
                retries.append([(rb[0], x.address + 4), (rb[1], wseg[-1].address + 4)])
                done |= {y.address for y in rb[0]}; rmid |= {y.address for y in rb[0][1:]}
    for x in insl:
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            assert x.operands[-1].imm not in rmid, f'branch into retry block {x.address:x}'
    for a_, tgt in tabjumps.items():
        if fa <= a_ < fb: assert not any(t in inwin and t not in heads for t in tgt), f'jump table target into window {a_:x}'
    rmid_all |= rmid
if far_into:   # functions analysed in pieces (pieces above): their far b / bl, and every jump table's targets (a cut may follow a br x18), may not land inside a window or a retry block
    win_mid = {x.address for wseg in windows for x in wseg[1:]}
    for at, to in far_into: assert to not in win_mid and to not in rmid_all, f'far branch into a window or retry block {at:x}'
    for at, tgt in tabjumps.items(): assert not any(t in win_mid or t in rmid_all for t in tgt), f'jump table target into a window or retry block {at:x}'

# Direct branches from other functions were not part of the per-function entry check above.
for w in windows:
    if w[0].address in newly_accepted:
        assert all(b.address == a.address + 4 for a, b in zip(w, w[1:])), f'noncontiguous window {w[0].address:x}'
new_mid = {x.address for w in windows if w[0].address in newly_accepted for x in w[1:]}
# A table's targets in another function (or piece) are entries there that its proof did not see: prove those functions again with them
fstarts = [fa for fa, _ in funcs]
def range_of(a):
    i = bisect.bisect_right(fstarts, a) - 1
    return funcs[i] if i >= 0 and a < funcs[i][1] else None
outside = {}
for at, tgt in tabjumps.items():
    for t in tgt:
        if (r := range_of(t)) is not None and r != range_of(at): outside.setdefault(r, set()).add(t)
for at, (b, width) in jumps.items():
    if (r0 := range_of(at)) is None: continue
    for t in range(b, b + 4 * width, 4):
        if (r := range_of(t)) is not None and r != r0: outside.setdefault(r, set()).add(t)
for (fa, fb), ts in outside.items():
    if (fa, fb) not in solved: continue
    before = solved[(fa, fb)][0]
    found, _ = solve_fn(fa, fb, disasm_fn(fa, fb), ts)
    for at in before:
        if at in tabjumps: assert set(found.get(at) or ()) == set(tabjumps[at]), f'jump table {at:x}: an entry from another function is not proved'
if new_mid:
    for start, end in CODE_RANGES:
        for at in range(start, end - 3, 4):
            w = struct.unpack_from('<I', IMG, at)[0]; t = w >> 24
            if t == 0x54 and not w & 0x10 or t & 0x7E == 0x34: d = (w >> 5) & 0x7FFFF; d -= (d & 0x40000) << 1
            elif t & 0x7E == 0x36: d = (w >> 5) & 0x3FFF; d -= (d & 0x2000) << 1
            elif t & 0x7C == 0x14: d = w & 0x3FFFFFF; d -= (d & 0x2000000) << 1
            else: continue
            dest = at + 4 * d
            if dest in new_mid: assert func_of(at) == func_of(dest), f'branch into window from another function {at:x}'

if GAP:
    _holes = hop_holes()
    front_holes = bisect.bisect_right(_holes, PATCH_OFF + REACH)
    back_holes = len(_holes) - bisect.bisect_left(_holes, BACK_MIN - 0x100000)
    gap_sites = sorted(a for a in (
        *(w[0].address for w in windows), *(p[0][0][0].address for p in retries),
        *(a for a in kinds if a not in done),
    ) if SPLIT <= a < BACK_MIN)
    if gap_sites and front_holes:
        toward_front = len(gap_sites) * front_holes // (front_holes + back_holes)
        SPLIT = gap_sites[toward_front] if toward_front < len(gap_sites) else BACK_MIN
        print(f'{NAME}: {len(gap_sites)} sites in the branch gap, split at {SPLIT:#x} ({front_holes} front and {back_holes} back padding words)', flush=True)


def put_branch(at, to): struct.pack_into('<I', IMG, at, BX(at, to))
for wseg in windows:
    use_area(wseg[0].address); st, lo, hi = site_window(wseg); stat('window'); put_branch(lo, st)
for parts in retries:
    use_area(parts[0][0][0].address); st = site_retry_window(parts); stat('retry_window'); put_branch(parts[0][0][0].address, st)
for a in sorted(kinds):
    if a in done: continue
    use_area(a)
    k, arg = kinds[a]
    ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
    if k == 'x18':
        m = ins.mnemonic
        if m == 'br' and a in tailcalls: st = site_br_call(ins); stat('br_call')
        elif m == 'br': st = site_br_tab(ins, tabjumps[a]) if a in tabjumps else site_br(ins, *jumps[a]); stat('br')
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz'): st = site_cond(ins); stat('cond')
        elif m in ('adr', 'adrp'): st = site_pcrel(ins); stat('pcrel')
        elif m == 'blr': st = site_blr(ins); stat('blr')
        elif re.match(r'(sp|wsp),', ins.op_str): st = site_sp(ins); stat('sp')
        elif casp_pair18(ins): st = site_casp(ins); stat('casp')
        elif a in selects: st = site_select(ins, *selects[a]); stat('select')
        elif m in ('ret', 'b', 'bl') or (m == 'ldr' and '[' not in ins.op_str): raise RuntimeError(f'{a:x} {m} {ins.op_str}')
        else: st = site_plain(ins); stat('plain')
    elif k == 'tpidr_r': st = site_sysreg_read(ins, 'KTLS'); stat(k)
    elif k == 'tpidrro_r': st = site_sysreg_read(ins, 'KTLR'); stat(k)
    elif k == 'tpidr_w': st = site_tpidr_write(ins); stat(k)
    elif k == 'svc': st = site_gate(ins, 'svc', arg); stat(k)
    elif k in ('cntpct', 'cntvct'): st = site_gate(ins, k, arg); stat(k)
    elif k == 'sp_align': st = site_sp_align(ins, arg); stat(k)
    elif k == 'ctr':   # a constant: DminLine = 4 (64-byte lines); movz xN, #4, lsl #16, in place, or into the slot K18 for x18
        if arg == 18: st = site_slot_copy(ins, 0xD2A00080 | 15, 'K18'); stat(k)
        else: struct.pack_into('<I', IMG, a, 0xD2A00080 | arg); stat(k); continue
    put_branch(a, st)

obs = list(PATCHES['OBS'].get(BUILD_ID, []))
for off, oid, ow, why in obs:
    w = struct.unpack_from('<I', IMG, off)[0]
    assert f'{w:08x}' == ow, f'observation gate original instruction mismatch {off:#x}: {w:08x}'
    # Any instruction site_obs can relocate: the gate frame saves x0–x28 and the flags and restores them (dm/host/entry.S), so the code around it sees its registers as they were
    # unless the callback writes one
    assert off < TEXT_END and off not in kinds and not any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'observation gate conflicts with a rewrite site {off:#x}'
    use_area(off); put_branch(off, site_obs(off, oid, w)); stat('obs')

# ---- Address regions (every module that defines the SDK's VammManager: sdk) ----
# Three observation gates, found by the SDK's own symbols in any build, let the host keep the table of address regions (hle_svc.c "Address regions"): the alias region space each takes,
# who asked for it and how much of it is used. They only read registers. VammManager::AllocateAddressRegion(this, uintptr_t *out, size_t size): at its entry the size (x2) and the caller's
# frames; at the one store of the region's address through out (copied from x1 in its prologue: str xA, [xB]) the address, A passed in the gate number's upper bits;
# VammManager::FreeAddressRegion(this, address): at its entry. A build where one of them is not found that way gets none of them (logged); the host then has no region table
OBS_UE_MALLOC, OBS_REGION_ASK, OBS_REGION_GOT, OBS_REGION_FREE, OBS_STDALLOC_INIT, OBS_UE_REGION = 21, 22, 23, 24, 25, 26   # dm/host/dm.h
def region_gates():
    from nso_syms import defined
    sym = defined(IMG)
    ask, free = sym.get('_ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm'), sym.get('_ZN2nn2os6detail11VammManager17FreeAddressRegionEm')
    if not ask or not free: return [], None
    from nso_code import out_store
    try: got, reg = out_store(IMG, ask[0], ask[1], md)
    except ValueError as e: return [], str(e)
    sites = [(ask[0], OBS_REGION_ASK, 'VammManager::AllocateAddressRegion entry: the size in x2'),
             (got, OBS_REGION_GOT | reg << 8, f'VammManager::AllocateAddressRegion: the new region\'s address in x{reg}, stored through the out pointer'),
             (free[0], OBS_REGION_FREE, 'VammManager::FreeAddressRegion entry: the address in x1')]
    for off, _, _ in sites:
        if off >= TEXT_END or off in kinds or any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]):
            return [], f'no observation gate possible at {off:#x}'
    return [(off, oid, f'{struct.unpack_from("<I", IMG, off)[0]:08x}', why) for off, oid, why in sites], None
region_obs, why_not = region_gates()
if why_not: print(f'{NAME}: no address region gates: {why_not}')
# nn::mem::StandardAllocator::Initialize(this, void *address, size_t size[, bool]): at its entry the host gives a region of virtual address memory (address null) no more address space
# than the alias region has room for (hle_svc.c "StandardAllocator's address space"): the SDK lays the region out from the size, so it holds all the way down. Both overloads (one may
# tail-call the other: the second sees the size already given). It writes x2, as the engine region's gate below writes its size
def stdalloc_gates():
    from nso_syms import defined
    sym = defined(IMG)
    out = []
    for name in ('_ZN2nn3mem17StandardAllocator10InitializeEPvm', '_ZN2nn3mem17StandardAllocator10InitializeEPvmb'):
        if name not in sym: continue
        off = sym[name][0]
        if off >= TEXT_END or off in kinds or any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]):
            print(f'{NAME}: no StandardAllocator gate at {off:#x}: a rewrite site'); continue
        out.append((off, OBS_STDALLOC_INIT, f'{struct.unpack_from("<I", IMG, off)[0]:08x}', f'StandardAllocator::Initialize(void *, size_t{", bool" if name.endswith("b") else ""}) entry: the size in x2, capped when the address in x1 is null'))
    return out
region_obs += stdalloc_gates()
# Unreal's FMallocBinned3 (above): the gate on each cut constructor call, a nop now; x0 is the object (the gate number's upper bits name the register, 0)
for _c in UE_CALLS or ():
    assert _c not in kinds and not any(lo <= _c < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'FMallocBinned3 gate conflicts with a rewrite site {_c:#x}'
    region_obs.append((_c, OBS_UE_MALLOC, f'{NOP:08x}', 'FMallocBinned3 constructor call (cut): the object in x0; the host puts its allocator\'s vtable in (hle_uemalloc.c)'))
# Unreal's engine region (above): the gate on the store of its size, which the callback changes in that register and x1, the size the call asks for
if UE_REGION is not None:
    assert UE_REGION_REG is not None, 'engine region gate has no size register'
    assert UE_REGION not in kinds and not any(lo <= UE_REGION < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'engine region gate conflicts with a rewrite site {UE_REGION:#x}'
    region_obs.append((UE_REGION, OBS_UE_REGION | UE_REGION_REG << 8, f'{struct.unpack_from("<I", IMG, UE_REGION)[0]:08x}',
                       f'Unreal\'s engine region: its size in x{UE_REGION_REG}, stored before nninitStartup reserves it; the host gives what the alias region has room for (hle_svc.c)'))
    if UE_REGION_X1 is not None:   # the request's size set again after the store (TRIANGLE STRATEGY): mov x1, xS, so it takes what the gate gave S
        assert UE_REGION_X1 not in kinds and not any(lo <= UE_REGION_X1 < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'engine region size conflicts with a rewrite site {UE_REGION_X1:#x}'
        struct.pack_into('<I', IMG, UE_REGION_X1, 0xAA0003E1 | UE_REGION_REG << 16)
for off, oid, ow, why in region_obs: use_area(off); put_branch(off, site_obs(off, oid, int(ow, 16))); stat('obs')
obs += region_obs

# ---- Frame step in whole vsyncs (the mains whose patch file has FRAME_STEP: The Legend of Zelda: Tears of the Kingdom 1.4.3, Breath of the Wild 1.9.0, Hyrule Warriors: Age of Calamity 1.3.0,
# Paper Mario: The Thousand-Year Door 1.0.1, ASTRAL CHAIN 1.0.1 and Pokemon Legends: Z-A 2.0.2; host only) ----
# The games step their game time by a whole number of vsyncs (1/60 s) per frame that their own interval logic picks. With the stubs the host picks it (hle_nvn.c fstep_tick,
# nvn_fstep.h): the step is the number of vsyncs the host keeps the frame on screen (1 = 60 fps, 2 = 30 when 60 does not hold), plus one while the game is behind real time after
# a late frame, so motion is even and game time = real time. The steps stay values the games' own logic uses (1 to 3). Age of Calamity's updates are not held by the presents:
# its stubs run them at the cap, and the host only paces its presents (its patches.py).
# State page = the page after the key page (zero in the file, read-write in both loaders): +0 float target fps written by the loader (fps_target, DM_FPS_TARGET; 0 = every stub
# runs the original instructions), +4 30 fps movie/cinematic flag, +8 double lead (TOTK), +0x10 float render scale (TOTK, BotW), +0x18 double game seconds (TOTK),
# +0x20 u32 step in vsyncs, or in the unit at +0x38 (host; 0 = not decided yet), +0x24 u32 a step the game fixes itself (BotW's VFR override; 0 = none), +0x28 u32 AoC's vsyncs per update, marked by
# its first stub (0 = off), +0x30 u64 units stepped (the stubs; logged by the host to check the game's speed), +0x38 u32 the step's unit in ticks a second: 120 for a stub that
# declares the FRAME_STEP_120 capability (Ocarina of Time's ratio stub; written by the loader, fixed for the run), 0 = vsyncs of 1/60 s, +0x40 u32 1 = the game's own dynamic resolution runs, 0 = its
# stub holds full resolution (DM_DYNAMIC_RESOLUTION, written by the loader; the stub declares the DYNAMIC_RESOLUTION capability). Stub bodies assembled with clang
# A build's stubs are FRAME_STEP[build ID](ctx) in its patches.py: ctx.here() / ctx.emit(word) assemble into the patch area, ctx.site(offset, original bytes) asserts a site and that it is
# no rewrite site, ctx.put(offset, words, reason) writes it (listed with the cuts), ctx.state is the state page's offset, ctx.render_scale = n marks a render-scale stub that takes scales up to n (1: it only lowers the game's size),
# ctx.step_max = n says the stubs hand the game steps of at most n vsyncs (its own interval logic stops there; 0 = FSTEP_MAX, 3), so the host holds frames and pays back at most n,
# ctx.others the other functions a MATCH_FRAME_STEP rule names, ctx.found what an anchored rule's find returned, ctx.address(offset) the data address an adrp there and the add / load / store after it make (in the original
# text: the fingerprints mask those pages, so a matched stub reads the globals it needs from its functions), and ctx.capabilities.add(name) declares a launcher capability the
# stubs provide (written as CAPABILITIES are)
FRAME_STEP = PATCHES['FRAME_STEP']
FS_STATE_OFF = 0
STEP_MAX = 0   # n = the stubs give the game steps of at most n vsyncs (ctx.step_max): the fixups table says so (STEP_MAX n), and the host holds frames at most n vsyncs (nvn_fstep.h smax)
RENDER_SCALE = 0   # n = the build has a render-scale stub (state +0x10) that takes scales up to n: the fixups table says so (RENDER_SCALE n), the loader holds DM_RENDER_SCALE to it and the launcher offers the setting up to it (dm/games/settings.tsv)
FS_CAPABILITIES = set()   # capabilities the frame step stubs declare (ctx.capabilities)
if (BUILD_ID in FRAME_STEP or FS_MATCH) and NAME == 'main':
    FS_STATE_OFF = KEY_PAGE_OFF + 0x4000
    if FAR:
        # The loader supplies zero-filled BSS/key pages for a front patch area.
        # Reserve the frame state after the key page there too; stubs reach it by ADRP.
        IMAGE_SIZE = FS_STATE_OFF + 0x4000
    assert FS_STATE_OFF % 0x1000 == 0 and (FAR or FS_STATE_OFF + 0x40 <= PATCH_OFF) and not any(IMG[FS_STATE_OFF:]), 'frame step state page'
# What frame step and stub code (STUBS below) may write: a site whose original bytes match and that holds no rewrite site, the cuts list (<name>.cuts.tsv) that names it, and
# data addresses read from the original text
def stub_site(off, orig, what):
    ob = bytes.fromhex(orig)
    assert IMG[off:off + len(ob)] == ob, f'{what} original bytes mismatch {off:#x}: {bytes(IMG[off:off + len(ob)]).hex()}'
    for o in range(off, off + len(ob), 4):
        assert o not in kinds and not any(seg[0].address <= o < seg[-1].address + 4 for seg in windows), f'{what} site is a rewrite site {o:#x}'
def stub_put(off, words, why, kind):
    old = bytes(IMG[off:off + 4 * len(words)])
    for i, w in enumerate(words): struct.pack_into('<I', IMG, off + 4 * i, w)
    cuts.append((off, old.hex(), bytes(IMG[off:off + 4 * len(words)]).hex(), why)); stat(kind)
def stub_address(off, what, lo_off=None):   # ADRP and its low instruction (adjacent unless explicitly supplied), read from the original text
    lo_off = off + 4 if lo_off is None else lo_off
    assert lo_off > off and not (off | lo_off) & 3, f'{what}: invalid address instruction offsets'
    a, b = struct.unpack_from('<I', ORIG_TEXT, off)[0], struct.unpack_from('<I', ORIG_TEXT, lo_off)[0]
    assert a & 0x9f000000 == 0x90000000, f'{what}: no adrp at {off:#x}'
    imm = (((a >> 5) & 0x7ffff) << 2 | (a >> 29) & 3) << 12
    page = (off & ~0xfff) + (imm - (1 << 33) if imm & 1 << 32 else imm)
    assert (b >> 5) & 31 == a & 31, f'{what}: the low instruction at {lo_off:#x} does not use the adrp register'
    if b & 0xffc00000 == 0x91000000: return page + ((b >> 10) & 0xfff)                       # add xD, xN, #imm
    assert b & 0x3b000000 == 0x39000000, f'{what}: no add, load or store after the adrp at {off:#x}'
    return page + ((b >> 10) & 0xfff) * (1 << (b >> 30)) * (16 if b & 0x04800000 == 0x04800000 and b >> 30 == 0 else 1)   # scaled by the access size (q: 16)
if FS_STATE_OFF:   # the build's stubs (FRAME_STEP of its patches.py), or those placed by their function (MATCH_FRAME_STEP: ctx.base is its start)
    use_area(0); fs = types.SimpleNamespace(here=here, emit=emit, site=lambda off, orig: stub_site(off, orig, 'frame step'),
                                            put=lambda off, words, why: stub_put(off, words, why + (' [function match]' if FS_MATCH else ''), 'frame_step'),
                                            state=FS_STATE_OFF, render_scale=0, step_max=0, base=FS_MATCH[1] if FS_MATCH else 0, others=FS_MATCH[2] if FS_MATCH else {},
                                            found=FS_MATCH[3] if FS_MATCH else None, capabilities=set(), address=lambda off, lo_off=None: stub_address(off, 'frame step', lo_off))
    (FS_MATCH[0]['stub'] if FS_MATCH else FRAME_STEP[BUILD_ID])(fs); RENDER_SCALE = fs.render_scale; STEP_MAX = fs.step_max; FS_CAPABILITIES = fs.capabilities

# ---- Stubs (the modules whose patch file has STUBS of their build) ----
# A fix of the game's own code that needs more instructions than its sites hold: STUBS[build ID](ctx) in its patches.py assembles them into the patch area and branches there from
# its sites, with the frame step's ctx (here / emit / site / put / address, above) and no state page, so the host's pacing does not change (FRAME_STEP would also make the host take
# the main for one with a frame step). A stub's code must hold nothing the rewriter would rewrite (x18, svc, tpidr, exclusive pairs), since it is not scanned.
# Ori and the Will of the Wisps 1.2.1 serializes the users of Unity's shared constant-buffer table this way (its patches.py)
STUBS = PATCHES['STUBS']
if BUILD_ID in STUBS or STUB_MATCH:
    use_area(0)
    st = types.SimpleNamespace(here=here, emit=emit, site=lambda off, orig: stub_site(off, orig, 'stub'),
                              put=lambda off, words, why: stub_put(off, words, why + (' [function match]' if STUB_MATCH else ''), 'stub'),
                              address=lambda off, lo_off=None: stub_address(off, 'stub', lo_off),
                              base=STUB_MATCH[1] if STUB_MATCH else 0, others=STUB_MATCH[2] if STUB_MATCH else {},
                              found=STUB_MATCH[3] if STUB_MATCH else None, fps_ready=False)
    (STUB_MATCH[0]['stub'] if STUB_MATCH else STUBS[BUILD_ID])(st)
    if st.fps_ready: NATIVE_FPS_BLOCKED = False
if NATIVE_FPS_BLOCKED: print(f'[RED] {NAME}: native frame target disabled: a required timing correction did not match')

# ---- Spin waits (every module) ----
# A guest loop that polls memory until another thread changes it fills only its own core on the Switch, where any higher-priority thread pinned there preempts it, so the
# spin costs the game nothing; on the host it takes a whole core from the threads doing the work. Paper Mario: The Thousand-Year Door's gfx::ParallelManager workers
# (main+0xa8e680, three, pinned at priority 45) poll their job list this way while a batch is pending; on an iPhone Rogueport ran at 24 fps with the three at 94% of a core each.
# A loop counts as a spin wait only when it provably cannot leave by itself. From its head T, every pass along the region R (the paths from T back to T, within SPIN_WIN bytes
# after T) runs only loads, register and flag arithmetic, direct branches and barriers: no store, call, SVC, system register or event wait; and no register or flag that is
# live at T is written in R (no counter, cursor or delay). One exception: an exclusive add of a constant (ld(a)xr; add/sub #k; st(l)xr; cbnz) that every path back to T
# undoes on the same address (a worker that registers with a job, sees it finished and backs out). Three more let a poll that takes a spin lock to look at the state it waits
# for count too (spin_lock, spin_store): a lock taken (an exclusive test-and-set retried until its store succeeds) and released (an exclusive compare-and-clear, or a store-release
# of zero) on the same address on every path back to T; a test loop on a word (load, compare with a constant, branch back to the load) inside R; and stores whose value and
# address registers R never writes, at most SPIN_STORES of them and only beside a load (see below). Every loop the search took without these still counts as before, the
# same region over the same back-edges or a wider one over more. Breath of the Wild's job queue (main+0xef62dc) is such a poll: the first item of some jobs is for the
# thread of core 0, and the others wait until it is taken, taking and releasing the queue's lock on every pass; polled at full speed by two workers, the lock went back and
# forth between their cores, and the main thread, whose item it was, lost nearly every exclusive store to them (CPU frames of 30-200 ms with three cores spinning there).
# 1.6.0's compiler writes the same lock without clrex, its failed test going straight back to the exclusive load (main+0x12c0e48); spin_lock takes both forms. A lock that
# heads no poll still has its own retry loop as a spin wait (Mario vs. Donkey Kong's test-and-test-and-set locks, main+0x470fa8).
# Such a pass leaves memory and registers as it found them (a pass with stores, from its last state change on), so only another thread can end the loop, and a pause
# there changes only timing. R is copied into the patch area; the original back-edges jump to the copy (no pause on the first pass, so a
# single retry costs nothing), the copy's back-edges go through a spin gate, which the host takes as a yield with the yield back-off (hle_thread.c dm_spin_wait), and exits
# from the copy go back to the original code. The gate keeps every register and the flags (entry.S)
SPIN_PATHS, SPIN_STEPS, SPIN_STORES = 256, 4096, 4   # complete paths back to T; search steps (R itself lies within SPIN_WIN bytes)
# Stores in a region: each writes a fixed value to a fixed address, so the words they write can hold only their first values or the stored ones, and the registers at T are the
# same on every pass: with at most SPIN_STORES of them a loop can change its own state only a few hundred passes at most (each a cycle of a few instructions), then its passes
# repeat, and only another thread ends it. The gate polls on for its first 2 ms, so a loop that ends by itself never sleeps there
SPIN_LSE = ('ldadd', 'ldclr', 'ldeor', 'ldset', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin')
SPIN_BAD = {'bl', 'blr', 'br', 'ret', 'eret', 'svc', 'hvc', 'smc', 'brk', 'hlt', 'udf', 'mrs', 'msr', 'sys', 'sysl', 'wfe', 'wfi', 'sev', 'sevl', 'clrex', 'dc', 'ic', 'at',
            'tlbi', 'drps', 'dcps1', 'dcps2', 'dcps3', 'blraa', 'blrab', 'blraaz', 'blrabz', 'braa', 'brab', 'braaz', 'brabz', 'retaa', 'retab', 'eretaa', 'eretab'}
# Per-head decode cache for spin_region: cleared in spin_region's finally block, so it holds at most the ~135 instructions in [T - 4, hi + 20] for the head being analysed
# (kept across heads, one 0.5-2.6 KB capstone object per instruction passed 24 GB on EA SPORTS FC 27 1.8e's 185 MB main; the parallel search strides heads by 8 * WORKERS,
# so cross-head reuse is effectively zero). Returned regions keep the instruction objects they need, and spin_emit decodes nothing.
spin_dec = {}
def spin_ins(a):
    if a not in spin_dec: spin_dec[a] = next(md.disasm(bytes(IMG[a:a + 4]), a), None)
    return spin_dec[a]
def spin_norm(n):
    n = {'fp': 'x29', 'lr': 'x30', 'wsp': 'sp'}.get(n, n)
    if n in ('wzr', 'xzr'): return None
    m = re.fullmatch(r'[wx](\d+)', n)
    if m: return 'x' + m.group(1)
    m = re.fullmatch(r'[bhsdqv](\d+)', n)
    return 'v' + m.group(1) if m else n
def spin_rw(ins):
    """(registers read, registers written), w / x and the vector views merged. Capstone's own lists are not enough: it lists a compare's first operand as written and the flags
    as read by cbz / tbz, and leaves out the destination of adds (sdk+0x31033c in Paper Mario: The Thousand-Year Door, adds w9,w9,#8; b.ne: a counted loop). So every register
    operand counts as well: operand 0 written and the others read, or for a load all of them written (its address registers are in capstone's reads)"""
    m = ins.mnemonic
    regs = [spin_norm(ins.reg_name(o.reg)) for o in ins.operands if o.type == ARM64_OP_REG]
    ops = set(regs) - {None}
    if m in ('cbz', 'cbnz', 'tbz', 'tbnz'): return ops, set()
    if m.startswith('b.'): return {'nzcv'}, set()
    if m == 'b': return set(), set()
    if m in ('cmp', 'cmn', 'tst', 'fcmp', 'fcmpe'): return ops, {'nzcv'}
    if m in ('ccmp', 'ccmn', 'fccmp', 'fccmpe'): return ops | {'nzcv'}, {'nzcv'}
    r, w = ins.regs_access()
    r, w = {spin_norm(ins.reg_name(x)) for x in r} - {None}, {spin_norm(ins.reg_name(x)) for x in w} - {None}
    if m.startswith(('ld', 'prfm', 'prfum')): return r, w | ops
    return r | (set(regs[1:]) - {None}), w | (set(regs[:1]) - {None})
def spin_kind(ins):
    """'br' direct branch, 'pc' adr / adrp, 'ok' a load or register / flag arithmetic, None anything else"""
    if ins is None: return None
    m = ins.mnemonic
    if m in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'): return 'br'
    if m in ('adr', 'adrp'): return 'pc'
    if m in SPIN_BAD or m.startswith(('st', 'cas', 'swp') + SPIN_LSE): return None
    mem = any(o.type == ARM64_OP_MEM for o in ins.operands)
    if m.startswith(('ld', 'prfm', 'prfum')): return 'ok' if mem else None   # no memory operand: a pc-relative literal load
    return None if mem else 'ok'
def spin_rmw(a):
    """ld(a)xr wA,[xN]; add|sub wB,wA,#k; st(l)xr wS,wB,[xN]; cbnz wS,<the load> at a → (xN, ±k, registers written, the four instructions), or None"""
    l = [spin_ins(a + 4 * i) for i in range(4)]
    if None in l or l[0].mnemonic not in ('ldxr', 'ldaxr') or l[1].mnemonic not in ('add', 'sub') or l[2].mnemonic not in ('stxr', 'stlxr') or l[3].mnemonic != 'cbnz': return None
    r = lambda x, i: x.reg_name(x.operands[i].reg)
    if len(l[1].operands) != 3 or l[1].operands[2].type != ARM64_OP_IMM or 'lsl' in l[1].op_str: return None
    base = l[0].operands[1].mem
    if base.disp or base.index or l[2].operands[2].mem.base != base.base or l[2].operands[2].mem.disp: return None
    if r(l[1], 1) != r(l[0], 0) or r(l[2], 1) != r(l[1], 0) or r(l[3], 0) != r(l[2], 0) or l[3].operands[1].imm != a: return None
    k = l[1].operands[2].imm * (1 if l[1].mnemonic == 'add' else -1)
    return spin_norm(l[0].reg_name(base.base)), k, {spin_norm(r(l[0], 0)), spin_norm(r(l[1], 0)), spin_norm(r(l[2], 0))} - {None}, l
def spin_lock(a):
    """An exclusive spin lock sequence or a test loop entered at a → (kind, xN, its instructions, how many of them come before a, how many from a on, registers read, registers written)
    'acq'  ld(a)xr wA,[xN]; cbz wA,+16; clrex; b <the load>; st(l)xr wS,wK,[xN]; cbnz wS,<the load>    test-and-set, retried until the store succeeds (wK not zero)
           ld(a)xr wA,[xN]; cbnz wA,<the load>; st(l)xr wS,wK,[xN]; cbnz wS,<the load>                    the same without clrex (Breath of the Wild 1.6.0's compiler)
    'rel'  clrex; ld(a)xr wA,[xN]; cmp wA,#k; b.ne <the clrex>; st(l)xr wS,wzr,[xN]; cbnz wS,<the load>  compare-and-clear, entered at the load; or stlr wzr,[xN]
           ld(a)xr wA,[xN]; cmp wA,#k; b.ne <the load>; st(l)xr wS,wzr,[xN]; cbnz wS,<the load>           the same without clrex
    'tst'  ldr|ldrb|ldrh wA,[xN{,#imm}]; cmp wA,#k; b.eq|b.ne <the load>                                 a test loop that only another thread ends
    The forms without clrex retry the exclusive load at once: the next exclusive load replaces the reservation the failed test left"""
    x0 = spin_ins(a)
    if x0 is None: return None
    r = lambda x, i: x.reg_name(x.operands[i].reg)
    plain = lambda x, i: x.operands[i].type == ARM64_OP_MEM and not x.operands[i].mem.index and not x.operands[i].mem.disp
    m0 = x0.mnemonic
    if m0 == 'stlr' and len(x0.operands) == 2 and r(x0, 0) in ('wzr', 'xzr') and plain(x0, 1):
        bn = spin_norm(x0.reg_name(x0.operands[1].mem.base)); return ('rel', bn, [x0], 0, 1, {bn}, set())
    if m0 in ('ldxr', 'ldaxr') and r(x0, 0).startswith('w') and plain(x0, 1):
        l = [x0] + [spin_ins(a + 4 * i) for i in range(1, 6)]
        if None not in l:
            base = l[0].operands[1].mem.base; bn = spin_norm(l[0].reg_name(base))
            if (l[1].mnemonic == 'cbnz' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].imm == a
                    and l[2].mnemonic in ('stxr', 'stlxr') and plain(l[2], 2) and l[2].operands[2].mem.base == base and r(l[2], 1) not in ('wzr', 'xzr')
                    and l[3].mnemonic == 'cbnz' and r(l[3], 0) == r(l[2], 0) and l[3].operands[1].imm == a):
                return ('acq', bn, l[:4], 0, 4, {bn, spin_norm(r(l[2], 1))}, {spin_norm(r(l[0], 0)), spin_norm(r(l[2], 0))})
            if (l[1].mnemonic == 'cmp' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].type == ARM64_OP_IMM
                    and l[2].mnemonic == 'b.ne' and l[2].operands[0].imm == a and l[3].mnemonic in ('stxr', 'stlxr') and r(l[3], 1) == 'wzr'
                    and plain(l[3], 2) and l[3].operands[2].mem.base == base and l[4].mnemonic == 'cbnz' and r(l[4], 0) == r(l[3], 0) and l[4].operands[1].imm == a):
                return ('rel', bn, l[:5], 0, 5, {bn}, {spin_norm(r(l[0], 0)), spin_norm(r(l[3], 0)), 'nzcv'})
            if (l[1].mnemonic == 'cbz' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].imm == a + 16 and l[2].mnemonic == 'clrex' and l[3].mnemonic == 'b' and l[3].operands[0].imm == a
                    and l[4].mnemonic in ('stxr', 'stlxr') and plain(l[4], 2) and l[4].operands[2].mem.base == base and r(l[4], 1) not in ('wzr', 'xzr')
                    and l[5].mnemonic == 'cbnz' and r(l[5], 0) == r(l[4], 0) and l[5].operands[1].imm == a):
                return ('acq', bn, l, 0, 6, {bn, spin_norm(r(l[4], 1))}, {spin_norm(r(l[0], 0)), spin_norm(r(l[4], 0))})
            p = spin_ins(a - 4)
            if (p is not None and p.mnemonic == 'clrex' and l[1].mnemonic == 'cmp' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].type == ARM64_OP_IMM
                    and l[2].mnemonic == 'b.ne' and l[2].operands[0].imm == a - 4 and l[3].mnemonic in ('stxr', 'stlxr') and r(l[3], 1) == 'wzr'
                    and plain(l[3], 2) and l[3].operands[2].mem.base == base and l[4].mnemonic == 'cbnz' and r(l[4], 0) == r(l[3], 0) and l[4].operands[1].imm == a):
                return ('rel', bn, [p] + l[:5], 1, 5, {bn}, {spin_norm(r(l[0], 0)), spin_norm(r(l[3], 0)), 'nzcv'})
    if (m0 in ('ldr', 'ldrb', 'ldrh') and len(x0.operands) == 2 and r(x0, 0).startswith('w') and x0.operands[1].type == ARM64_OP_MEM
            and not x0.operands[1].mem.index and not x0.writeback):
        l = [x0, spin_ins(a + 4), spin_ins(a + 8)]
        if (None not in l and l[1].mnemonic == 'cmp' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].type == ARM64_OP_IMM
                and l[2].mnemonic in ('b.eq', 'b.ne') and l[2].operands[0].imm == a):
            bn = spin_norm(l[0].reg_name(l[0].operands[1].mem.base)); return ('tst', bn, l, 0, 3, {bn}, {spin_norm(r(l[0], 0)), 'nzcv'})
    return None
def spin_store(ins):
    """str / strb / strh / stur / sturb / sturh of a w register or zr to [xN{,#imm}] (no writeback, no register offset) → (value register or None for zr, base register), or None"""
    if ins is None or ins.mnemonic not in ('str', 'strb', 'strh', 'stur', 'sturb', 'sturh') or ins.writeback or len(ins.operands) != 2: return None
    v, o = ins.operands
    if v.type != ARM64_OP_REG or o.type != ARM64_OP_MEM or o.mem.index: return None
    vn = ins.reg_name(v.reg)
    if not (vn.startswith('w') or vn in ('xzr',)): return None
    return (spin_norm(vn), spin_norm(ins.reg_name(o.mem.base)))
def spin_rebranch(ins, pc, tgt):
    """the direct branch ins re-encoded at pc with target tgt"""
    w = struct.unpack('<I', ins.bytes)[0]; off = tgt - pc
    if ins.mnemonic == 'b': return BX(pc, tgt)
    if ins.mnemonic in ('tbz', 'tbnz'):
        assert -(1 << 15) <= off < (1 << 15) and off % 4 == 0, f'spin wait tbz out of range {pc:#x}'
        return (w & ~(0x3FFF << 5)) | (((off >> 2) & 0x3FFF) << 5)
    assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0, f'spin wait branch out of range {pc:#x}'
    return (w & ~(0x7FFFF << 5)) | (((off >> 2) & 0x7FFFF) << 5)
spin_touched = set(kinds) | done | {o for off, oid, ow, why in obs for o in (off,)} | {o for off, ob, nb, why in cuts for o in range(off, off + len(ob) // 2, 4)}
# Instruction words that end every path through them, told apart by their encoding alone: bl; br / blr / ret; svc / brk; str / strb / strh and the vector str (unsigned offset;
# unscaled, pre / post-index; register offset); stp / stnp; ldadd and the other LSE atomics. Exclusive stores are left to the full check (the exclusive add), and so are the pre /
# post-index loads: a seqlock read in SDK 15's nn::time (sdk+0x3d3270) moves a base it sets again on each pass
SPIN_STOP = ((0xFC000000, 0x94000000), (0xFE000000, 0xD6000000), (0xFF000000, 0xD4000000), (0x3FC00000, 0x39000000), (0x3FC00000, 0x3D000000), (0x3FC00000, 0x3D800000),
             (0x3FE00000, 0x38000000), (0x3FE00C00, 0x38200800), (0x3FE00000, 0x3C000000), (0x3FE00000, 0x3C800000), (0x3A400000, 0x28000000), (0x3F200C00, 0x38200000))
def spin_quick(T, hi):
    """a back-edge to T can be reached from T without passing a word of SPIN_STOP (only such heads get the full check, which decodes)"""
    seen, todo = set(), [T]
    while todo:
        a = todo.pop()
        if a in seen or not T <= a <= hi: continue
        seen.add(a); w = struct.unpack_from('<I', IMG, a)[0]; t = w >> 24
        if 0x14 <= t <= 0x17: succ = (a + (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4,)
        elif t == 0x54 or t & 0x7E == 0x34: succ = (a + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000) * 4, a + 4)
        elif t & 0x7E == 0x36: succ = (a + ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000) * 4, a + 4)
        elif any(w & m == v for m, v in SPIN_STOP) and not (w & 0xBFC00000 in (0xB9000000, 0x39000000, 0x79000000) or w & 0xBFE00C00 in (0xB8000000, 0x38000000, 0x78000000)): continue   # str / strb / strh (immediate, unscaled) may store the same on every pass
        else: succ = (a + 4,)
        if T in succ: return True
        todo.extend(succ)
    return False
class SpinNo(Exception): pass
def spin_region(T, bes):
    """the spin wait region with head T (bes: the backward branches to T found by the scan) → dict, or None. A store whose value or address a pass changes is no part of a region:
    the paths through it are dropped and the region is found again without them (a poll that takes its item once the wait is over stores the new cursor there). When the paths
    through stores bring in a register that a pass changes, the region is looked for once more with every store ending its path, as before stores could be part of one.
    A lock sequence at T is one node of a poll that it heads (its own branches back to T stay inside it); when there is no such poll, T is the head of the sequence's own retry
    loop (a lock's wait for its holder, ld(a)xr; cbnz <the load>), found as plain instructions as before the lock rules"""
    hi = max(bes)
    if any(s < hi + 4 and T < e for s, e in DATA_BLOCKS) or not spin_quick(T, hi): return None
    try:
        lk = spin_lock(T)
        for head in (True, False) if lk and lk[0] in ('acq', 'rel') else (True,):
            ban = set()
            for _ in range(8):
                g = spin_region1(T, bes, ban, True, head)
                if not isinstance(g, set): break
                ban |= g
            old = spin_region1(T, bes, set(), False, head)   # the region without stores, which the search took before: the wider one only when it covers its back-edges
            g = g if isinstance(g, dict) and (not old or old['valid'] <= g['valid']) else old
            if g: break
        return g
    finally:
        spin_dec.clear()
def spin_region1(T, bes, ban, stores, head):
    hi = max(bes)
    nodes, back, bad, inner = {}, set(), set(), set()
    todo = [T]
    while todo:
        a = todo.pop()
        if a in nodes or a in bad: continue
        if a in ban: bad.add(a); continue
        rmw = spin_rmw(a) if a + 12 <= hi and struct.unpack_from('<I', IMG, a)[0] & 0x3FE00000 == 0x08400000 else None   # ldxr / ldaxr
        lk = None if rmw else spin_lock(a)
        if lk and a == T and (lk[0] == 'tst' or not head): lk = None   # the region's own test loop, or a lock sequence taken as plain instructions (spin_region)
        if rmw:
            n = {'kind': 'rmw', 'base': rmw[0], 'k': rmw[1], 'ins': rmw[3], 'uses': {rmw[0]}, 'defs': rmw[2], 'succ': [a + 16]}
        elif lk:
            n = {'kind': lk[0], 'base': lk[1], 'k': {'acq': 1, 'rel': -1, 'tst': 0}[lk[0]], 'ins': lk[2], 'pre': lk[3], 'uses': lk[5], 'defs': lk[6], 'succ': [a + 4 * lk[4]]}
        elif stores and (st := spin_store(x := spin_ins(a))):
            n = {'kind': 'stc', 'st': st, 'ins': [x], 'uses': {st[1]} | ({st[0]} - {None}), 'defs': set(), 'succ': [a + 4]}
        else:
            x = spin_ins(a); k = spin_kind(x)
            if k is None: bad.add(a); continue
            if k == 'br':
                t = x.operands[-1].imm
                succ = [t] if x.mnemonic == 'b' else [t, a + 4]
            else: succ = [a + 4]
            n = {'kind': k, 'ins': [x], 'succ': succ}
        pre = n.get('pre', 0)
        if any(a + 4 * (i - pre) in spin_touched for i in range(len(n['ins']))): bad.add(a); continue
        nodes[a] = n
        if n['kind'] == 'rmw': inner |= {a + 4, a + 8, a + 12}
        if n['kind'] in ('acq', 'rel', 'tst'): inner |= {a + 4 * (i - pre) for i in range(len(n['ins']))} - {a}
        for s in n['succ']:
            if s == T: back.add(a)
            elif T < s <= hi: todo.append(s)
    if not back or inner & set(nodes) or any(s in inner for n in nodes.values() for s in n['succ']): return None
    def prune(ends):
        keep, more = set(ends), True
        while more:
            more = False
            for a, n in nodes.items():
                if a not in keep and any(s in keep and s != T for s in n['succ']): keep.add(a); more = True
        return keep
    R = prune(back)
    for a in R:
        if 'uses' not in nodes[a]: nodes[a]['uses'], nodes[a]['defs'] = spin_rw(nodes[a]['ins'][0])
    # every path from T to a back-edge: no inner cycle, and the exclusive adds on each address (a register between two writes to it) sum to 0
    bad_back, cnt = set(), [0, 0]
    def walk(a, onpath, ver, delta):
        cnt[1] += 1
        if cnt[1] > SPIN_STEPS: raise SpinNo
        n = nodes[a]
        if n['kind'] in ('rmw', 'acq', 'rel'):   # exclusive adds and lock / unlock pairs, each kind on its own
            key = (n['kind'] != 'rmw', n['base'], ver.get(n['base'], 0)); delta = dict(delta); delta[key] = delta.get(key, 0) + n['k']
            if n['kind'] != 'rmw' and not 0 <= delta[key] <= 1: raise SpinNo   # a release before its acquire or a second acquire: not a lock this pass takes once
        if n['defs']: ver = dict(ver); ver.update({r: ver.get(r, 0) + 1 for r in n['defs']})
        for s in n['succ']:
            if s == T:
                cnt[0] += 1
                if cnt[0] > SPIN_PATHS: raise SpinNo
                if any(delta.values()): bad_back.add(a)
            elif s in R:
                if s in onpath: raise SpinNo
                walk(s, onpath | {s}, ver, delta)
    try: walk(T, {T}, {}, {})
    except SpinNo: return None
    valid = back - bad_back
    if not valid: return None
    R = prune(valid)
    # no state carried from one pass to the next: nothing live at T is written in R
    live, more = {a: set() for a in R}, True
    while more:
        more = False
        for a in sorted(R, reverse=True):
            n, out = nodes[a], set()
            for s in n['succ']:
                if s == T: out |= live[T] if a in valid else set()
                elif s in R: out |= live[s]
            new = n['uses'] | (out - n['defs'])
            if new != live[a]: live[a] = new; more = True
    defs = set().union(*(nodes[a]['defs'] for a in R))
    if any(nodes[a]['kind'] == 'stc' for a in R) and not any(nodes[a]['kind'] in ('rmw', 'acq', 'rel', 'tst') or nodes[a]['ins'][0].mnemonic.startswith('ld') for a in R): return None   # stores and nothing polled
    vary = {a for a in R if nodes[a]['kind'] == 'stc' and nodes[a]['uses'] & defs}   # a store's value and address must come from registers no pass writes
    if vary: return vary
    if sum(nodes[a]['kind'] == 'stc' for a in R) > SPIN_STORES: return None
    if live[T] & defs: return None
    return {'T': T, 'R': sorted(R), 'nodes': nodes, 'valid': valid}
def spin_emit(g):
    T, R, nodes, valid = g['T'], g['R'], g['nodes'], g['valid']
    use_area(T); cpy, fixes, tramp = {}, [], {}
    for i, a in enumerate(R):
        n = nodes[a]; cpy[a] = here(); x = n['ins'][0]
        if n['kind'] == 'rmw':
            for y in n['ins'][:3]: emit(struct.unpack('<I', y.bytes)[0])
            fixes.append((len(patch), n['ins'][3], a)); emit(0)         # retry: the copy's exclusive load
        elif n['kind'] in ('acq', 'rel'):   # copied as they are: their branches stay inside the sequence
            cpy[a] = here() + 4 * n['pre']
            for y in n['ins']: emit(struct.unpack('<I', y.bytes)[0])
        elif n['kind'] == 'tst':   # its branch back goes to the original loop, which passes its own spin gate when it is a spin wait itself
            for y in n['ins'][:2]: emit(struct.unpack('<I', y.bytes)[0])
            fixes.append((len(patch), n['ins'][2], ('orig', a))); emit(0)
        elif n['kind'] == 'pc':
            rd, tgt = num(spin_norm(x.reg_name(x.operands[0].reg))), x.operands[1].imm
            emit(ADRP(rd, here(), tgt))
            if x.mnemonic == 'adr': emit(ADDI(rd, rd, tgt & 0xFFF))
        elif n['kind'] == 'br': fixes.append((len(patch), x, None)); emit(0)
        else: emit(struct.unpack('<I', x.bytes)[0])
        if not (n['kind'] == 'br' and x.mnemonic == 'b'):
            ft = a + 4 * (len(n['ins']) - n.get('pre', 0))
            if not (i + 1 < len(R) and R[i + 1] == ft): fixes.append((len(patch), None, ft)); emit(0)   # the next instruction is not the next one copied
    pause = here()
    emit(STP_PRE(29, 30, 31, -16)); emit_gate_call(); emit(BLR(30)); ret = here(); emit(LDP_POST(29, 30, 31, 16))
    gates.append((ret, 'spin', 0, T)); emit(BX(here(), cpy[T]))
    def out(t):   # an exit taken by a conditional branch: through a b next to the copy
        if t not in tramp: tramp[t] = here(); emit(BX(here(), t))
        return tramp[t]
    for idx, x, info in fixes:
        pc = PATCH_BASE + idx
        if x is None: w = BX(pc, cpy.get(info, info))
        elif isinstance(info, tuple): w = spin_rebranch(x, pc, out(info[1]))
        elif info is not None: w = spin_rebranch(x, pc, cpy[info])
        else:
            t, cond = x.operands[-1].imm, x.mnemonic != 'b'
            d = pause if t == T and x.address in valid else cpy[t] if t in cpy and t != T else out(t) if cond else t
            w = spin_rebranch(x, pc, d)
        struct.pack_into('<I', patch, idx, w)
    for b in sorted(valid):   # the original back-edges enter the copy
        x = nodes[b]['ins'][0]
        if x.mnemonic == 'b': put_branch(b, cpy[T])
        else: st = here(); emit(spin_rebranch(x, here(), cpy[T])); emit(BX(here(), b + 4)); put_branch(b, st)
    stat('spin')
spin_heads = sorted(spin_cand)

def spin_heads_with_region(first, step):
    return [T for T in spin_heads[first::step] if spin_region(T, spin_cand[T])]

def find_spin_regions():
    """spin_region for every candidate head that has one, in address order. Each job takes every nth head, so the slow stretches
    of code are spread across the workers. A region holds capstone objects, which can't be sent back from a worker, so the workers
    only say which heads have one and the regions are built again here."""
    step = min(8 * WORKERS, len(spin_heads))
    heads = []
    for found in in_parallel(spin_heads_with_region, [(first, step) for first in range(step)]):
        heads += found
    return [g for T in sorted(heads) if (g := spin_region(T, spin_cand[T]))]

spin_regions = find_spin_regions()   # all found on the unchanged image first: emitting patches back-edges
spin_sites = [(g['T'], sorted(g['valid']), sum(len(g['nodes'][a]['ins']) for a in g['R'])) for g in spin_regions]
for g in spin_regions: spin_emit(g)
if os.environ.get('DM_SPIN_LIST'):
    for T, bes, n in spin_sites: print(f'{NAME} spin wait {T:#x} ({n} instructions), back-edges ' + ' '.join(f'{b:#x}' for b in bes))

os.makedirs(outdir, exist_ok=True)
P = lambda ext: os.path.join(outdir, f'{NAME}.{ext}')
patch = patch_front
BACK_LEN = (len(patch_back) + 0x3FFF) & ~0x3FFF
if FAR:   # patch area before the base: file = text + ro + data (padded to 16K) + patch area (TWO: the front area padded to 16K, then the back area, padded to 16K)
    if BIG_TEXT and len(patch) > FRONT - 0x4000 and 'DM_PATCH_FRONT' not in os.environ:
        need = (len(patch) + 0x4000 + 0xFFFF) & ~0xFFFF
        print(f'{NAME}: patch area of {len(patch):#x} bytes does not fit in {FRONT:#x} before the base; rewriting again with {need:#x}', flush=True)
        os.execve(sys.executable, [sys.executable] + sys.argv, dict(os.environ, DM_PATCH_FRONT=hex(need)))
    assert len(patch) <= FRONT - 0x4000, f'patch area of {len(patch):#x} bytes does not fit in the {FRONT:#x} before the base'
    if TWO:
        assert IMAGE_SIZE <= BACK_OFF, 'back patch area overlaps the key page or the frame step page'
        IMAGE_SIZE = BACK_OFF + BACK_LEN
        patch = patch + bytearray(-len(patch) & 0x3FFF) + patch_back + bytearray(BACK_LEN - len(patch_back))
    open(P('patched.bin'), 'wb').write(IMG + bytearray(FILE_IMAGE_LEN - len(IMG)) + patch)
else:
    open(P('patched.bin'), 'wb').write(IMG + bytearray(PATCH_OFF - len(IMG)) + patch)
import hashlib
def movie_native_color_keys():
    # NintendoSDK_movie 7.3.2 rotates hw-buf-ColorSpace right by five, then accepts values 1 through 4 as-is.
    # SDK 4.4.1 adds one. Match the reviewed converter at its export and verify what key the result is written to.
    key = b'nv12-colorspace\0'
    symbol = '_ZN5movie25convertMessageToMediaDataERKN7android2spINS0_8AMessageEEEPNS_9MediaDataE'
    if IMG.find(key, RO_OFF, DATA_OFF) < 0 or IMG.find(symbol.encode() + b'\0', RO_OFF, DATA_OFF) < 0:
        return False
    start, size = nso_syms.defined(IMG).get(symbol, (0, 0))
    if not 0 < start < start + size <= TEXT_END:
        return False
    signature = struct.pack('<4I', 0x13881508, 0x51000509, 0x7100113f, 0x1a9f3102)
    at = ORIG_TEXT.find(signature, start, start + size)
    if at < 0 or at % 4 or at + 24 > start + size or ORIG_TEXT.find(signature, at + 4, start + size) >= 0:
        return False
    pair = list(md.disasm(ORIG_TEXT[at + 16:at + 24], at + 16))
    if len(pair) != 2 or pair[0].mnemonic != 'adrp' or pair[1].mnemonic != 'add':
        return False
    page, low = pair[0].operands, pair[1].operands
    if (len(low) != 3 or low[2].type != ARM64_OP_IMM or low[2].shift.value or
            any(md.reg_name(op.reg) != 'x1' for op in (page[0], low[0], low[1]))):
        return False
    target = page[1].imm + low[2].imm
    return RO_OFF <= target < DATA_OFF and IMG[target:target + len(key)] == key

with open(os.path.join(outdir, 'build_src.sha256'), 'w') as f:   # the same for every module: the rewriter and every patch file, by path from dm/tools
    for src_ in (os.path.abspath(__file__), nso.__file__, nso_code.__file__, nso_asm.__file__, nso_syms.__file__, ue_cvar.__file__, ue_malloc.__file__, fps_target.__file__, monolith_fps.__file__, ue_fixed_rate.__file__, unity_fps.__file__, unity_bucket.__file__, ue_region.__file__, ue_ring.__file__, snowdrop_pools.__file__, os.path.join(os.path.dirname(ue_malloc.__file__), 'loader_census.py'), *PATCH_FILES): f.write(hashlib.sha256(open(src_, 'rb').read()).hexdigest() + f'  {os.path.relpath(os.path.abspath(src_), os.path.dirname(os.path.abspath(__file__)))}\n')
with open(P('fixups.tsv'), 'w') as f:
    f.write(f'# PATCH_OFF\t{PATCH_OFF:#x}\n# KEY_PAGE_OFF\t{KEY_PAGE_OFF:#x}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{TEXT_END:#x}\n# RO_OFF\t{RO_OFF:#x}\n# DATA_OFF\t{DATA_OFF:#x}\n')
    if FAR: f.write(f'# IMAGE_SIZE\t{IMAGE_SIZE:#x}\n# FILE_IMAGE_LEN\t{FILE_IMAGE_LEN:#x}\n')   # patch area before the base (negative PATCH_OFF): whole image size, length of the image part of the file
    if TWO: f.write(f'# BACK_OFF\t{BACK_OFF:#x}\n# BACK_LEN\t{BACK_LEN:#x}\n')   # the back patch area (TWO): its offset in the image, and its length, the last bytes of the file
    for k in KEY_INDEX: f.write(f'# KEY_{k}_IDX\t{KEY_INDEX[k]}\n')
    if FS_STATE_OFF: f.write(f'# FPS_STATE_OFF\t{FS_STATE_OFF:#x}\n')   # frame step state page (FRAME_STEP): the loader writes the target fps there
    if UE_CALLS: f.write(f'# UE_MALLOC_VT\t{UE_VT:#x}\n# UE_MALLOC_SLOT\t{UE_SLOT:#x}\n')   # Unreal's FMallocBinned3 (ue_malloc.py): its vtable and the slot of its name getter, which names the slot layout
    if FPS_TARGET_PTR is not None: f.write(f'# FPS_TARGET_PTR\t{FPS_TARGET_PTR:#x}\n# FPS_TARGET_FIELD\t{FPS_TARGET_FIELD:#x}\n')   # a target frame rate the game steps by (fps_target.py)
    if FPS_TARGET_F32: f.write('# FPS_TARGET_F32\t0x1\n')
    if MONOLITH_INTERVAL is not None: f.write(f'# MONOLITH_INTERVAL\t{MONOLITH_INTERVAL:#x}\n')   # native wait and simulation interval (monolith_fps.py)
    if NATIVE_FPS_BLOCKED: f.write('# NATIVE_FPS_BLOCKED\t0x1\n')   # a required matched timing correction was not installed
    if UE_FIXED_PTR is not None: f.write(f'# UE_FIXED_RATE_PTR\t{UE_FIXED_PTR:#x}\n# UE_FIXED_RATE_FIELD\t{UE_FIXED_FIELD:#x}\n# UE_FIXED_RATE_FLAG\t{UE_FIXED_FLAG:#x}\n')   # Unreal's GEngine, UEngine::FixedFrameRate and bUseFixedFrameRate (ue_fixed_rate.py)
    if 'target' in UNITY_FPS: f.write(f"# UNITY_TARGET_FPS\t{UNITY_FPS['target']:#x}\n")   # Unity's Application.targetFrameRate (unity_fps.py)
    if 'qs' in UNITY_FPS: f.write(''.join(f'# UNITY_QS_{k}\t{v:#x}\n' for k, v in zip(('PTR', 'IDX', 'ARR', 'SIZE', 'VSYNC'), UNITY_FPS['qs'])))   # its QualitySettings' vSyncCount (unity_fps.py)
    if movie_native_color_keys(): f.write('# MOVIE_NATIVE_COLOR_KEYS\t0x1\n')
    for n, o in UE_CVARS.items(): f.write(f'# UE_CVAR:{n}\t{o:#x}\n')   # Unreal console variables (ue_cvar.py): the TAutoConsoleVariable registered under each name
    if RENDER_SCALE:   # a render-scale stub reads state +0x10, up to this scale (the loader holds DM_RENDER_SCALE to it; dm/games/settings.py offers the setting up to it)
        assert isinstance(RENDER_SCALE, int) and 1 <= RENDER_SCALE <= 9, f'render scale stub: ctx.render_scale is a whole largest scale, 1 to 9: {RENDER_SCALE!r}'
        f.write(f'# RENDER_SCALE\t{RENDER_SCALE}\n')
    if STEP_MAX:   # the largest step the frame step stubs give the game (the host holds frames and pays back within it)
        assert isinstance(STEP_MAX, int) and 1 <= STEP_MAX <= 3, f'frame step stub: ctx.step_max is a whole step, 1 to 3: {STEP_MAX!r}'
        f.write(f'# STEP_MAX\t{STEP_MAX}\n')
    if NAME == 'main':
        for capability in sorted(set(PATCHES['CAPABILITIES'].get(BUILD_ID, ())) | FS_CAPABILITIES):
            assert re.fullmatch(r'[A-Z][A-Z0-9_]*', capability), f'invalid capability {capability}'
            f.write(f'# {capability}\t1\n')
    for off, ob, nb, why in ALIAS_APPLIED:
        nbb = bytes.fromhex(nb)
        assert IMG[off:off + len(nbb)] == nbb, f'ALIAS_CUT at {off:#x} was modified by a later rewriter pass: {bytes(IMG[off:off + len(nbb)]).hex()}'
        f.write(f'# ALIAS_CUT\t{off:#x}\t{ob}\t{nb}\n')   # iOS-only alias cut the Mac loader undoes at load when alias >= 64 GB
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')   # the host identifies the build by it: features with hard-coded addresses only run on the build they belong to
with open(P('gates.tsv'), 'w') as f:
    for ret, k, arg, orig in gates: f.write(f'{ret:#x}\t{k}\t{arg}\t{orig:#x}\n')
with open(P('sites.tsv'), 'w') as f:
    for a in sorted(kinds):
        if a not in done: f.write(f'{a:#x}\t{kinds[a][0]}\n')
    for wseg in windows: f.write(f'{wseg[0].address:#x}\tW\t{wseg[-1].address + 4:#x}\n')
    for a, (b, c) in jumps.items(): f.write(f'{a:#x}\tJ\t{b:#x}\t{c}\n')
    for a, tgt in tabjumps.items(): f.write(f'{a:#x}\tT\t' + ' '.join(f'{t:#x}' for t in tgt) + '\n')   # offset-table jump and all its targets
    for t, bes, n in spin_sites: f.write(f'{t:#x}\tS\t' + ' '.join(f'{b:#x}' for b in bes) + '\n')   # spin wait: its head and the back-edges that enter its copy
with open(P('cuts.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, ob, nb, why in cuts: f.write(f'{off:#x}\t{ob}\t{nb}\t{why}\n')   # Columns: offset, original bytes, new bytes, reason
# The text words whose instructions no longer run as the file has them: every word this rewriter changed (sites, cuts,
# jump tables; not an exclusive access that only gained ordering), the windows whose copies in the patch area run instead, and the spin waits' instructions, which run
# from their copies after the first pass. Runs of offset and length; the host's cheat engine refuses a write into any of them
# (hle_cheats.c main_write_pages: a cheat's instruction there would undo a site or never run)
def changed_runs():
    words = set()
    for o in range(0, TEXT_END, 0x1000):   # whole blocks first: almost all of the text is unchanged
        if IMG[o:o + 0x1000] != ORIG_TEXT[o:o + 0x1000]:
            words.update(w for w in range(o, min(o + 0x1000, TEXT_END), 4) if IMG[w:w + 4] != ORIG_TEXT[w:w + 4]
                         and not (w in ORDERED and struct.unpack_from('<I', IMG, w)[0] == struct.unpack_from('<I', ORIG_TEXT, w)[0] | 0x8000))   # ordering alone
    for wseg in windows: words.update(range(wseg[0].address, wseg[-1].address + 4, 4))
    for g in spin_regions:
        for a in g['R']:
            n = g['nodes'][a]; pre = n.get('pre', 0)
            words.update(a + 4 * (i - pre) for i in range(len(n['ins'])))
    runs = []
    for w in sorted(words):
        if runs and runs[-1][0] + runs[-1][1] == w: runs[-1][1] += 4
        else: runs.append([w, 4])
    return runs
with open(P('changed.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, n in changed_runs(): f.write(f'{off:#x}\t{n:#x}\n')   # Columns: offset, length in bytes
with open(P('obs.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, oid, ow, why in obs: f.write(f'{off:#x}\t{oid}\t{ow}\t{why}\n')
print(NAME, stats, 'cuts', len(cuts), 'obs', len(obs), 'patch', len(patch_front) + len(patch_back), 'PATCH_OFF', hex(PATCH_OFF), *(('BACK_OFF', hex(BACK_OFF), 'back', len(patch_back)) if TWO else ()), 'slot_refs', len(fix), 'gates', len(gates))
if DATA_BLOCKS: print(NAME, 'data blocks left alone:', ', '.join(f'{s:#x}..{e:#x}' for s, e in DATA_BLOCKS))
