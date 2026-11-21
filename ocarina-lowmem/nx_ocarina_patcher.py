#!/usr/bin/env python3
"""Ocarina of Time (nxnative) low-memory patcher.

Pick the game's .app in Finder, choose what to change, press Apply. It then
  1. bakes a fixed resolution level into the app's main.patched.bin (no rebuild),
  2. sets the app's settings (launcher.json defaults and Contents/Resources/dm/env.txt),
  3. optionally builds a patched host (grass off, textures off) from an nxnative checkout and swaps it in,
  4. signs the app again (ad-hoc), host first and then the bundle, as nxnative's own pack_app.py does.
The first change to each file keeps a copy beside it (name.orig); "Restore originals" puts them back.

Needs macOS with Python 3 (tkinter for the window; --cli works without it). Building the host also needs
Xcode command line tools and the Metal toolchain, and a checkout of github.com/seedds/nxnative at the commit
the app was built from (the tool compares the shader-library version and refuses a different one).
"""
import argparse, json, os, plistlib, queue, re, shutil, struct, subprocess, sys, threading
from pathlib import Path

GAME_ID = '0400486024DEC000'
HERE = Path(__file__).resolve().parent
REPO_URL = 'https://github.com/seedds/nxnative'
CHAIN_H = 'dm/gfx/nvn/dmsh_chain.h'
BEGIN, END = '# >>> nx_ocarina_patcher', '# <<< nx_ocarina_patcher'

# The Ocarina of Time frame-step stub's "off" path (state +0x40 = 0): ldr w17,[x16,#0x40]; cbnz w17,+3; <the level word>; ret.
# The level word is `mov w0,wzr` (level 0, full resolution) in the build, or `ldr w0,[x16,#0x44]` once built with DM_FIXED_RES_LEVEL; here it becomes `mov w0,#N`.
W_A, W_B, W_RET = 0xb9404211, 0x35000071, 0xd65f03c0
W_ORIG = 0x2a1f03e0


def is_level_word(w):
    return w in (W_ORIG, 0xb9404600) or (w & 0xffe0001f) == 0x52800000 and ((w >> 5) & 0xffff) <= 15


def level_word(n):
    return W_ORIG if not n else 0x52800000 | (n << 5)   # mov w0,#n


def find_stub(data):
    pat = re.compile(re.escape(struct.pack('<II', W_A, W_B)) + b'.{4}' + re.escape(struct.pack('<I', W_RET)), re.S)
    hits = []
    for m in pat.finditer(data):
        if m.start() % 4: continue
        if is_level_word(struct.unpack_from('<I', data, m.start() + 8)[0]): hits.append(m.start() + 8)
    return hits


def backup(path, log):
    b = path.with_name(path.name + '.orig')
    if not b.exists(): shutil.copy2(path, b); log(f'  kept the original as {b.name}')


def patch_level(app, level, log):
    """level 0 / None puts the original word back. Returns True when the file is as asked."""
    p = app / 'Contents/Resources/dm/main.patched.bin'
    if not p.is_file(): log(f'  {p} not found'); return False
    data = bytearray(p.read_bytes())
    hits = find_stub(bytes(data))
    if len(hits) != 1: log(f'  expected one resolution-level stub in main.patched.bin, found {len(hits)}: left alone (a different build?)'); return False
    want = level_word(level or 0)
    cur = struct.unpack_from('<I', data, hits[0])[0]
    if cur == want: log(f'  resolution level already {level or 0}'); return True
    backup(p, log)
    struct.pack_into('<I', data, hits[0], want)
    tmp = p.with_name(p.name + '.partial'); tmp.write_bytes(bytes(data)); os.replace(tmp, p)
    log(f'  main.patched.bin: fixed resolution level {level or 0} (word at {hits[0]:#x}: {cur:#010x} -> {want:#010x})')
    return True


def write_env(app, items, log):
    p = app / 'Contents/Resources/dm/env.txt'
    old = p.read_text().splitlines() if p.is_file() else []
    keep, skip = [], False
    for ln in old:
        if ln.strip() == BEGIN: skip = True; continue
        if ln.strip() == END: skip = False; continue
        if not skip: keep.append(ln)
    out = keep + ([BEGIN] + [f'{k}={v}' for k, v in items.items()] + [END] if items else [])
    if out or p.is_file(): p.write_text('\n'.join(out) + ('\n' if out else ''))
    log(f'  env.txt: {", ".join(f"{k}={v}" for k, v in items.items()) or "nothing set"}')


def set_launcher_defaults(app, updates, log):
    p = app / 'Contents/Resources/dm/launcher.json'
    if not p.is_file(): log('  launcher.json not found, skipped'); return
    j = json.loads(p.read_text()); changed = []
    for s in j.get('settings', []):
        k = s.get('env')
        if k not in updates: continue
        v = str(updates[k])
        opts = [o[0] for o in s.get('options', [])]
        if opts and v not in opts: log(f'  {k}={v} is not one of the app\'s options {opts}: skipped'); continue
        if s.get('default') != v: s['default'] = v; changed.append(f'{k}={v}')
    if changed:
        backup(p, log); p.write_text(json.dumps(j, indent=1) + '\n')
    log(f'  launcher.json defaults: {", ".join(changed) or "already as asked"}')


def app_info(app):
    info = {}
    try: pl = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    except Exception: return info
    info['name'] = pl.get('CFBundleName') or pl.get('CFBundleDisplayName') or app.stem
    info['commit'] = pl.get('DMCommit'); info['chain'] = pl.get('DMChain')
    if not info['chain']:
        for m in (app / 'Contents/Resources/msl').glob('b-*'):
            info['chain'] = re.sub(r'\.img$', '', m.name[2:]); break
    host = app / 'Contents/MacOS/dmhost'
    info['host_new'] = host.is_file() and b'DM_ALIAS_SWEEP' in host.read_bytes()
    return info


def run(cmd, log, cwd=None, check=True):
    log('  $ ' + ' '.join(str(c) for c in cmd))
    p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tail = []
    for ln in p.stdout:
        ln = ln.rstrip()
        if ln: tail.append(ln); tail = tail[-40:]
    rc = p.wait()
    if rc and check:
        for ln in tail[-12:]: log('    ' + ln)
        raise RuntimeError(f'{cmd[0]} {cmd[1] if len(cmd) > 1 else ""} failed ({rc})')
    return rc


def repo_chain(repo):
    h = repo / 'dm/gfx/nvn/dmsh_chain.h'
    m = re.search(r'#define DMSH_CHAIN "([0-9a-f]+)"', h.read_text()) if h.is_file() else None
    return m.group(1) if m else None


def get_checkout(dest, chain, log, url=REPO_URL):
    """Clone nxnative into dest/nxnative and check out the newest commit whose shader-library version (DMSH_CHAIN) is the app's."""
    dest = Path(dest); repo = dest / 'nxnative'
    if ' ' in str(repo): raise RuntimeError('the path has a space; make cannot build there: choose another folder')
    if not chain: raise RuntimeError('the app\'s shader-library version is unknown')
    if not (repo / '.git').is_dir():
        run(['git', 'clone', url, repo], log)
    else: log('  nxnative is already there; using it')
    return checkout_matching(repo, chain, log)


def git_out(repo, *a):
    return subprocess.run(['git', *a], cwd=repo, capture_output=True, text=True).stdout


def checkout_matching(repo, chain, log):
    """Detach the checkout at the newest commit whose shader-library version (DMSH_CHAIN) is the app's. Refuses a checkout with uncommitted changes."""
    repo = Path(repo)
    if git_out(repo, 'status', '--porcelain', '--untracked-files=no').strip():
        raise RuntimeError('the checkout has uncommitted changes, so it was not switched to the app\'s commit. Commit or stash them (git stash), or use "Get the checkout" for a fresh clone')
    if git_out(repo, 'rev-parse', '--is-shallow-repository').strip() == 'true': run(['git', 'fetch', '--unshallow', '-q'], log, cwd=repo)
    hits = git_out(repo, 'log', '--format=%H', f'-S{chain}', '--', CHAIN_H).split()
    target = None
    for h in hits:   # newest first: a commit that took the version out, whose parent is the newest that has it
        if chain not in git_out(repo, 'show', f'{h}:{CHAIN_H}'): target = h + '^'; break
    if target is None:
        if hits: target = 'HEAD'
        else: raise RuntimeError(f'no commit of this checkout has shader-library version {chain}')
    run(['git', 'checkout', '-q', '--detach', target], log, cwd=repo)
    got = repo_chain(repo)
    if got != chain: raise RuntimeError(f'checked out {got}, not {chain}')
    log(f'  checkout is now at {git_out(repo, "rev-parse", "--short", "HEAD").strip()} (detached): shader-library version {got}, the app\'s')
    return repo


def apply_host_patch(repo, patches, log):
    def can(args, q): return run(['git', 'apply', *args, q], log, cwd=repo, check=False) == 0
    if any(can(['--reverse', '--check'], q) for q in patches): log('  host patch already applied'); return
    use = next((q for q in patches if can(['--check'], q)), None)
    if not use:
        # an earlier version of this patch, applied by a previous run, is still in the checkout: put it aside (git stash keeps it) and try again
        mine = {'dm/gfx/nvn/nvn_mtl.m', 'dm/gfx/nvn/hle_nvn.c', 'dm/host/loader.c', 'docs/env_switches.tsv', 'dm/games/' + GAME_ID + '/patches.py', 'dm/games/' + GAME_ID + '/README.md'}
        st = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=repo, capture_output=True, text=True).stdout.splitlines()
        files = [l[3:] for l in st]
        if files and all(f in mine for f in files):
            log('  the checkout holds an earlier version of the host patch: setting it aside (git stash) and applying the current one')
            run(['git', 'stash', 'push', '-m', 'ocarina patcher: earlier host patch', '--', *files], log, cwd=repo)
            use = next((q for q in patches if can(['--check'], q)), None)
        elif files:
            raise RuntimeError('the checkout has changes of its own that are not this tool\'s (' + ', '.join(files[:4]) + '): commit or stash them, then run again')
    if not use: raise RuntimeError('none of the host patches applies to this checkout\'s source (' + ', '.join(q.name for q in patches) + '): it is probably not the commit the app was built from; use "Get the checkout" for the right one')
    run(['git', 'apply', use], log, cwd=repo); log(f'  host patch applied to the checkout ({use.name})')


def apply_optional_patches(repo, log):
    # the 20 fps part touches other files (hle_nvn.c, loader.c) that differ between commits: it is applied when it fits and skipped, with a note, when it does not
    for q in sorted(HERE.glob('ocarina-fps20*.patch')):
        def can(args): return run(['git', 'apply', *args, q], log, cwd=repo, check=False) == 0
        if can(['--reverse', '--check']): log(f'  {q.name}: already applied'); continue
        if can(['--check']): run(['git', 'apply', q], log, cwd=repo); log(f'  {q.name} applied (DM_FPS_TARGET=20 possible)'); continue
        log(f'  {q.name} does not fit this checkout\'s source: the build goes ahead without it, so a 20 fps cap is not available')


def build_host(repo, app_chain, log):
    host_dir = repo / 'dm/host'
    if not host_dir.is_dir(): raise RuntimeError('that folder is not an nxnative checkout (no dm/host)')
    if ' ' in str(repo): raise RuntimeError('the checkout\'s path has a space; make cannot build there: move it')
    rc = repo_chain(repo)
    if app_chain and rc != app_chain:
        log(f'  the checkout\'s shader-library version is {rc} but the app\'s is {app_chain}: a host from another source version refuses the app\'s shaders. Switching the checkout to the app\'s commit')
        checkout_matching(repo, app_chain, log)
    patches = sorted(HERE.glob('ocarina-host*.patch'))
    if not patches: raise RuntimeError('no ocarina-host*.patch beside this script')
    apply_host_patch(repo, patches, log)
    apply_optional_patches(repo, log)
    run(['make', '-s', f'GAME={GAME_ID}', f'gen/{GAME_ID}/game_profile.h', 'gen/vpx/libvpx.a', 'gen/dav1d/libdav1d.a', 'host_msl'], log, cwd=host_dir)
    run(['make', '-s', '-B', f'GAME={GAME_ID}', 'dmhost'], log, cwd=host_dir)
    out = host_dir / 'dmhost'
    if not out.is_file() or b'DM_ALIAS_SWEEP' not in out.read_bytes(): raise RuntimeError('the host built, but it does not contain the new switches')
    return out


def sign(app, log):
    cs = shutil.which('codesign')
    if not cs: log('  codesign not found (this is not a Mac?): the app was not signed'); return False
    for target in (app / 'Contents/MacOS/dmhost', app):
        try: run([cs, '--force', '--sign', '-', target], log)
        except RuntimeError:
            subprocess.run(['xattr', '-cr', str(app)], capture_output=True)   # Finder information on copied files: codesign refuses them
            run([cs, '--force', '--sign', '-', target], log)
    log('  signed (ad-hoc), the host first and then the bundle')
    return True


def apply(app, opt, log):
    app = Path(app)
    if not (app / 'Contents/MacOS').is_dir() or not (app / 'Contents/Resources/dm').is_dir(): raise RuntimeError('that is not an nxnative game app (no Contents/Resources/dm)')
    info = app_info(app)
    log(f'App: {info.get("name")}; shader libraries {info.get("chain")}; commit {info.get("commit") or "unrecorded"}')
    dyn = bool(opt.get('dynamic'))
    log('Resolution')
    patch_level(app, 0 if dyn else int(opt.get('level') or 0), log)
    log('Settings')
    if str(opt.get('opmode', '1')) == '0': log('  note: handheld mode (0) renders from a 2560-wide base, bigger than docked (1) at 2048, and uses more GPU memory')
    set_launcher_defaults(app, {'DM_DYNAMIC_RESOLUTION': '1' if dyn else '0', 'DM_FPS_TARGET': '30' if str(opt.get('fps', '30')) == '20' else opt.get('fps', '30'), 'DM_OPMODE': opt.get('opmode', '1')}, log)
    env = {}
    if str(opt.get('fps', '30')) == '20': env['DM_FPS_TARGET'] = '20'; log('  20 fps: written to env.txt as DM_FPS_TARGET=20 (the app\'s own list offers only 30 and 60); needs the host built with the 20 fps part')
    go_ = opt.get('grass_off')
    if go_ is True or str(go_) == '1': env['DM_SKIP_GRASS'] = '1'
    elif str(go_) == '2': env['DM_SKIP_GRASS'] = '2'
    if opt.get('water_off'): env['DM_SKIP_WATER'] = '1'
    if opt.get('effects_off'): env['DM_SKIP_EFFECTS'] = '1'
    if opt.get('small_staging'): env['DM_UP_BLOCK_MB'] = '8'
    if str(opt.get('stand_kb', '1')).isdigit() and int(opt.get('stand_kb', '1')) > 1: env['DM_STAND_KB'] = str(opt['stand_kb'])
    if str(opt.get('stand_kb_small', '')).isdigit() and int(opt['stand_kb_small']) >= 1: env['DM_STAND_KB_SMALL'] = str(opt['stand_kb_small'])
    if str(opt.get('stand_kb_mid', '')).isdigit() and int(opt['stand_kb_mid']) >= 1: env['DM_STAND_KB_MID'] = str(opt['stand_kb_mid'])
    if opt.get('trim_mem'): env['DM_TRIM_MEM'] = '1'
    if opt.get('sweep'): env['DM_ALIAS_SWEEP'] = '600'
    if opt.get('guest_file'): env['DM_GUEST_FILE'] = '1'
    # the UI / map bypass is built in: whenever textures are reduced, "ui" is always on the keep list (the text box only adds sizes such as 256x256 to it)
    if opt.get('textures') in ('all', 'big') or 'DM_STAND_KB' in env or 'DM_STAND_KB_MID' in env or 'DM_STAND_KB_SMALL' in env or opt.get('keep', '').strip():
        toks = [t for t in opt.get('keep', '').replace(' ', '').split(',') if t and t.lower() != 'ui']
        env['DM_TEXTURES_KEEP'] = ','.join(['ui'] + toks)
    if opt.get('texlog'): env['DM_TEXTURES_LOG'] = '1'
    if str(opt.get('shadow_div', '1')).isdigit() and 2 <= int(opt.get('shadow_div', '1')) <= 48: env['DM_SHADOW_DIV'] = str(opt.get('shadow_div'))
    if opt.get('textures') in ('all', 'big'): env['DM_TEXTURES_OFF'] = opt['textures']
    if opt.get('match', '').strip(): env['DM_SKIP_SHADER_MATCH'] = opt['match'].strip()
    write_env(app, env, log)
    if opt.get('build_host'):
        log('Host')
        repo = Path(opt.get('repo') or '')
        built = build_host(repo, info.get('chain'), log)
        dst = app / 'Contents/MacOS/dmhost'
        backup(dst, log); shutil.copy(built, dst); os.chmod(dst, 0o755); log('  new host installed')
    elif (env.get('DM_SKIP_GRASS') or env.get('DM_TEXTURES_OFF') or env.get('DM_SKIP_SHADER_MATCH') or env.get('DM_SKIP_WATER') or env.get('DM_SKIP_EFFECTS') or env.get('DM_UP_BLOCK_MB') or env.get('DM_SHADOW_DIV') or env.get('DM_STAND_KB') or env.get('DM_TEXTURES_KEEP') or env.get('DM_TEXTURES_LOG')) and not info.get('host_new'):
        log('  note: this app\'s host does not have the grass / textures switches yet; they do nothing until "Build and install the patched host" is ticked')
    log('Signing')
    sign(app, log)
    log('Done. If the app\'s own settings window had saved choices, press Reset there so these defaults apply.')


def restore(app, log):
    app = Path(app); n = 0
    for rel in ('Contents/Resources/dm/main.patched.bin', 'Contents/Resources/dm/launcher.json', 'Contents/MacOS/dmhost'):
        p = app / rel; b = p.with_name(p.name + '.orig')
        if b.is_file(): shutil.copy2(b, p); n += 1; log(f'  restored {p.name}')
    write_env(app, {}, log)
    sign(app, log)
    log(f'Restored {n} file(s) and cleared the patcher\'s env lines.')


def finder_pick(folder=False):
    what = 'folder' if folder else 'file of type {"com.apple.application-bundle"}'
    prompt = 'the nxnative checkout' if folder else 'the game app'
    r = subprocess.run(['osascript', '-e', f'POSIX path of (choose {what} with prompt "Select {prompt}")'], capture_output=True, text=True)
    return r.stdout.strip().rstrip('/') if r.returncode == 0 else ''


def gui():
    import tkinter as tk
    from tkinter import ttk, filedialog
    root = tk.Tk(); root.title('Ocarina of Time low-memory patcher'); root.geometry('1500x900'); root.minsize(1100, 700)
    q = queue.Queue(); v = {k: tk.StringVar(value=d) for k, d in dict(app='', repo='', fps='30', opmode='1', level='9', textures='keep', match='', shadow='1 (normal)', detail='1 (tiny: blocky)', detail_small='same as above', detail_mid='same as small', keep='').items()}
    b = {k: tk.BooleanVar(value=d) for k, d in dict(dynamic=False, grass=False, water=False, effects=False, staging=False, trim=False, sweep=False, guest=False, texlog=False, build=False).items()}
    pad = dict(padx=8, pady=3)

    def row(parent, label, var, browse):
        f = ttk.Frame(parent); f.pack(fill='x', **pad); ttk.Label(f, text=label, width=14).pack(side='left')
        ttk.Entry(f, textvariable=var).pack(side='left', fill='x', expand=True); ttk.Button(f, text='Browse in Finder…', command=browse).pack(side='left', padx=4)

    def pick_app():
        p = finder_pick()
        if p: v['app'].set(p); show_info()

    def pick_repo():
        p = finder_pick(True)
        if p: v['repo'].set(p)

    info_lbl = tk.StringVar(value='')
    def show_info():
        i = app_info(Path(v['app'].get())) if v['app'].get() else {}
        info_lbl.set(f'{i.get("name", "")}  shader libraries {i.get("chain")}  commit {i.get("commit") or "unrecorded"}  host has grass/texture switches: {"yes" if i.get("host_new") else "no"}' if i else '')

    top = ttk.Frame(root); top.pack(fill='x'); top.columnconfigure(0, weight=1, uniform='half'); top.columnconfigure(1, weight=1, uniform='half')
    r = ttk.LabelFrame(top, text='Resolution and frame rate'); r.grid(row=0, column=0, sticky='nsew', **pad)
    h = ttk.LabelFrame(top, text='Game app and patched host'); h.grid(row=0, column=1, sticky='nsew', **pad)
    row(h, 'Game app', v['app'], pick_app)
    ttk.Label(h, textvariable=info_lbl, foreground='gray', wraplength=640).pack(anchor='w', padx=8)
    def line(parent, label, widget):
        f = ttk.Frame(parent); f.pack(fill='x', padx=6, pady=2); ttk.Label(f, text=label, width=26).pack(side='left'); widget(f).pack(side='left')
    line(r, 'Dynamic resolution', lambda f: ttk.Checkbutton(f, variable=b['dynamic'], text='on (the game\'s own range)'))
    line(r, 'Fixed level (dynamic off)', lambda f: ttk.Spinbox(f, from_=0, to=9, textvariable=v['level'], width=6))
    line(r, 'Frame rate cap', lambda f: ttk.Combobox(f, textvariable=v['fps'], values=['20', '30', '60'], width=6, state='readonly'))
    line(r, 'Console mode (1 docked, 0 handheld)', lambda f: ttk.Combobox(f, textvariable=v['opmode'], values=['1', '0'], width=6, state='readonly'))
    ttk.Label(r, text='Docked (1) renders from a 2048-wide base, handheld (0) from 2560: handheld is the bigger one, and uses more memory. Level 0 is 0.75 of the base (1536x864 docked) and level 9, the game\'s lowest, 0.333 (683x384 docked): nothing goes below that. '
                      'The fixed level is used only when dynamic resolution is off; if the app\'s own settings window has it saved as on, switch it off there.', foreground='gray', wraplength=640, justify='left').pack(anchor='w', padx=6)
    bottom = ttk.Frame(root); bottom.pack(fill='both', expand=True); bottom.columnconfigure(0, weight=1, uniform='half'); bottom.columnconfigure(1, weight=1, uniform='half'); bottom.rowconfigure(0, weight=1)
    m = ttk.LabelFrame(bottom, text='Memory (needs the patched host)'); m.grid(row=0, column=0, sticky='nsew', **pad)
    right = ttk.Frame(bottom); right.grid(row=0, column=1, sticky='nsew', **pad)
    grass_names = {'draw grass (normal)': '0', 'remove the grass effect only (keeps tree leaves)': '2', 'remove all foliage (grass and leaves: saves most)': '1'}
    grass_lbl = tk.StringVar(value='draw grass (normal)')
    line(m, 'Grass', lambda f: ttk.Combobox(f, textvariable=grass_lbl, values=list(grass_names), width=50, state='readonly'))
    line(m, 'Water', lambda f: ttk.Checkbutton(f, variable=b['water'], text='turn off water effects (lake surface may stay)'))
    line(m, 'Effects', lambda f: ttk.Checkbutton(f, variable=b['effects'], text='turn off ALL particle/effect shaders (test)'))
    line(m, 'Upload staging', lambda f: ttk.Checkbutton(f, variable=b['staging'], text='8 MB upload buffers (frees ~140 MB GPU, loads slower)'))
    line(m, 'Detail of dropped textures', lambda f: ttk.Combobox(f, textvariable=v['detail'], values=['1 (tiny: blocky)', '2 (very small)', '4 (small: 64x64 in a 4x4-block texture)', '6 (between 4 and 8)', '8 (about 90x90)', '16 (128x128)', '32', '64 (sharper)', '128 (sharp, more memory)', '256 (sharpest here)'], state='readonly', width=34))
    line(m, 'Detail of small textures', lambda f: ttk.Combobox(f, textvariable=v['detail_small'], values=['same as above', '1', '2', '4', '6', '8', '16', '32'], state='readonly', width=34))
    line(m, 'Detail of medium textures', lambda f: ttk.Combobox(f, textvariable=v['detail_mid'], values=['same as small', '1', '2', '4', '6', '8', '16', '32'], state='readonly', width=34))
    line(m, 'Game memory in files', lambda f: ttk.Checkbutton(f, variable=b['guest'], text='RAM saver: guest memory in disk files (DM_GUEST_FILE; works with any host)'))
    line(m, 'RAM trim', lambda f: ttk.Checkbutton(f, variable=b['trim'], text='return free host memory every 300 frames'))
    line(m, 'Texture sweep', lambda f: ttk.Checkbutton(f, variable=b['sweep'], text='release idle cached textures every 300 frames (flushes a cutscene)'))
    line(m, 'Also keep sharp (ui is always on)', lambda f: ttk.Entry(f, textvariable=v['keep'], width=36))
    line(m, 'Texture log', lambda f: ttk.Checkbutton(f, variable=b['texlog'], text='log each replaced texture (find the map\'s size)'))
    line(m, 'Shadow map', lambda f: ttk.Combobox(f, textvariable=v['shadow'], values=['1 (normal)', '2 (half: saves ~216 MB, experimental)', '3 (a third: saves ~256 MB, experimental)', '4 (a quarter: saves ~270 MB, experimental)', '5 (a fifth: saves ~276 MB, experimental)', '12 (1024x1024)', '16 (768x768)', '18 (683x683, about 700)', '24 (512x512)', '32 (384x384)', '48 (256x256)'], state='readonly', width=40))
    tex_names = {'Keep textures (normal)': 'keep', 'Drop the big ones (512 KB and up)': 'big', 'Drop all art textures (test)': 'all'}
    tex_lbl = tk.StringVar(value='Keep textures (normal)')
    line(m, 'Textures', lambda f: ttk.Combobox(f, textvariable=tex_lbl, values=list(tex_names), width=34, state='readonly'))
    line(m, 'Skip shaders matching', lambda f: ttk.Entry(f, textvariable=v['match'], width=36))
    def clone_repo():
        d = finder_pick(True)
        if not d: return
        go(lambda: (v['repo'].set(str(get_checkout(d, app_info(Path(v['app'].get())).get('chain'), log)))))
    ttk.Checkbutton(h, variable=b['build'], text='Build and install the patched host from an nxnative checkout (minutes; needs Xcode tools)').pack(anchor='w', padx=6)
    row(h, 'Checkout', v['repo'], pick_repo)
    bar = ttk.Frame(right); bar.pack(fill='x', pady=3)
    ttk.Label(right, text='Progress and results').pack(anchor='w')
    lf = ttk.Frame(right); lf.pack(fill='both', expand=True)
    logbox = tk.Text(lf, wrap='word'); sb = ttk.Scrollbar(lf, command=logbox.yview); logbox.configure(yscrollcommand=sb.set); sb.pack(side='right', fill='y'); logbox.pack(side='left', fill='both', expand=True)
    def log(s): q.put(s + '\n')
    def pump():
        try:
            while True: logbox.insert('end', q.get_nowait()); logbox.see('end')
        except queue.Empty: pass
        root.after(100, pump)
    busy = []
    def go(fn):
        if busy: return
        if not v['app'].get(): log('Pick the game app first.'); return
        busy.append(1)
        def work():
            try: fn()
            except Exception as e: log(f'ERROR: {e}')
            finally: busy.clear()
        threading.Thread(target=work, daemon=True).start()
    def do_apply():
        opt = dict(dynamic=b['dynamic'].get(), level=v['level'].get(), fps=v['fps'].get(), opmode=v['opmode'].get(), grass_off=grass_names[grass_lbl.get()], water_off=b['water'].get(), effects_off=b['effects'].get(), small_staging=b['staging'].get(), shadow_div=v['shadow'].get().split()[0], stand_kb=v['detail'].get().split()[0], stand_kb_mid=(v['detail_mid'].get().split()[0] if v['detail_mid'].get()[0].isdigit() else ''), stand_kb_small=(v['detail_small'].get().split()[0] if v['detail_small'].get()[0].isdigit() else ''), trim_mem=b['trim'].get(), sweep=b['sweep'].get(), guest_file=b['guest'].get(), keep=v['keep'].get(), texlog=b['texlog'].get(),
                   textures=tex_names[tex_lbl.get()], match=v['match'].get(), build_host=b['build'].get(), repo=v['repo'].get())
        go(lambda: (apply(v['app'].get(), opt, log), root.after(0, show_info)))
    ttk.Button(h, text='Get the checkout (clone nxnative into a folder)…', command=clone_repo).pack(anchor='w', padx=6, pady=2)
    ttk.Button(bar, text='Apply and sign', command=do_apply).pack(side='left')
    ttk.Button(bar, text='Restore originals', command=lambda: go(lambda: restore(v['app'].get(), log))).pack(side='left', padx=6)
    pump(); root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cli', action='store_true', help='no window: apply with the options below')
    ap.add_argument('--app'); ap.add_argument('--restore', action='store_true')
    ap.add_argument('--dynamic', action='store_true'); ap.add_argument('--level', type=int, default=0, choices=range(10)); 
    ap.add_argument('--fps', default='30', choices=['20', '30', '60']); ap.add_argument('--opmode', default='1', choices=['0', '1'])
    ap.add_argument('--stand-kb-small', default=''); ap.add_argument('--stand-kb-mid', default=''); ap.add_argument('--grass-only', action='store_true'); ap.add_argument('--trim-mem', action='store_true'); ap.add_argument('--sweep', action='store_true'); ap.add_argument('--guest-file', action='store_true'); ap.add_argument('--grass-off', action='store_true'); ap.add_argument('--water-off', action='store_true'); ap.add_argument('--effects-off', action='store_true'); ap.add_argument('--small-staging', action='store_true'); ap.add_argument('--stand-kb', default='1'); ap.add_argument('--keep', default=''); ap.add_argument('--texlog', action='store_true'); ap.add_argument('--shadow-div', default='1', choices=['1', '2', '3', '4']); ap.add_argument('--textures', default='keep', choices=['keep', 'big', 'all']); ap.add_argument('--match', default='')
    ap.add_argument('--build-host', action='store_true'); ap.add_argument('--repo'); ap.add_argument('--clone-to', help='clone nxnative into this folder at the app\'s commit and use it')
    a = ap.parse_args()
    if not a.cli:
        try: return gui()
        except ImportError: print('tkinter is not available in this Python; use --cli (see --help)'); return 2
    if not a.app: print('--app is required with --cli'); return 2
    try:
        if a.restore: restore(a.app, print)
        elif a.clone_to: print(get_checkout(a.clone_to, app_info(Path(a.app)).get('chain'), print))
        else: apply(a.app, dict(dynamic=a.dynamic, level=a.level, fps=a.fps, opmode=a.opmode, grass_off='2' if a.grass_only else a.grass_off, water_off=a.water_off, effects_off=a.effects_off, small_staging=a.small_staging, shadow_div=a.shadow_div, stand_kb=a.stand_kb, stand_kb_small=a.stand_kb_small, stand_kb_mid=a.stand_kb_mid, trim_mem=a.trim_mem, sweep=a.sweep, guest_file=a.guest_file, keep=a.keep, texlog=a.texlog, textures=a.textures, match=a.match, build_host=a.build_host, repo=a.repo), print)
    except RuntimeError as e:
        print(f'ERROR: {e}'); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
