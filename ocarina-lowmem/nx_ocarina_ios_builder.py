#!/usr/bin/env python3
"""Ocarina of Time (nxnative / NX2Apple) iPhone builder.

One window for: patching the checkout (RAM saver, texture stand-ins, shadow divider, sweep, 20 fps),
preparing the Xcode project with nxnative's own build script, building it signed with your Apple team
(Increased Memory Limit entitlement included), installing it on the iPhone over USB-C, pushing the
game files, and writing Documents/env.txt with your switches.

Keep it in the same folder as nx_ocarina_patcher.py and the ocarina-*.patch files.
Needs a Mac with Xcode (iOS platform installed), Python 3.12+, the nxnative or NX2Apple checkout
(its scripts/build_ipa_from_nsp.py), the game's NSP folder and prod.keys, and the iPhone connected by
cable, unlocked, trusted, with Developer Mode on.  Not tested on a device by its author: read the log.
"""
import argparse, json, os, queue, shlex, subprocess, sys, threading, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import nx_ocarina_patcher as P   # apply_host_patch, apply_optional_patches, run, finder_pick, GAME_ID

SETTINGS = Path.home() / '.nx_ocarina_ios_builder.json'


def log_default(s): print(s, flush=True)


def build_env(o):
    e = {}
    if o.get('guest_file', True): e['DM_GUEST_FILE'] = '1'
    if o.get('fps20'): e['DM_FPS_TARGET'] = '20'
    for k, key in (('stand_kb', 'DM_STAND_KB'), ('stand_kb_mid', 'DM_STAND_KB_MID'), ('stand_kb_small', 'DM_STAND_KB_SMALL'), ('shadow_div', 'DM_SHADOW_DIV')):
        v = str(o.get(k, '')).strip()
        if v.isdigit() and int(v) > 1 or (k != 'shadow_div' and v == '1'): e[key] = v
    toks = [t for t in o.get('keep', '').replace(' ', '').split(',') if t and t.lower() != 'ui']
    e['DM_TEXTURES_KEEP'] = ','.join(['ui'] + toks)   # the UI / map bypass is built in
    if o.get('textures') in ('all', 'big'): e['DM_TEXTURES_OFF'] = o['textures']
    if o.get('sweep', True): e['DM_ALIAS_SWEEP'] = '600'
    if o.get('water_off'): e['DM_SKIP_WATER'] = '1'
    if o.get('effects_off'): e['DM_SKIP_EFFECTS'] = '1'
    if o.get('hud'): e['DM_METAL_HUD'] = '1'
    if o.get('extra', '').strip():
        for ln in o['extra'].replace(';', '\n').splitlines():
            if '=' in ln: k, v = ln.strip().split('=', 1); e[k.strip()] = v.strip()
    return e


NEEDED = 'import lz4.block, capstone, fontTools, zbic'
VENV = Path.home() / '.nx_ocarina_ios_venv'


def python_with_modules(log):
    """The build script needs lz4, capstone, fontTools, zbic (and backports.zstd before Python 3.14). Use a Python that has them, or make a venv that does."""
    import shutil
    cands = [sys.executable, str(VENV / 'bin/python')] + [shutil.which(n) for n in ('python3.14', 'python3.13', 'python3.12', 'python3')] + ['/opt/homebrew/bin/python3', '/usr/local/bin/python3']
    seen = []
    for c in cands:
        if not c or c in seen or not Path(c).exists(): continue
        seen.append(c)
        if subprocess.run([c, '-c', NEEDED], capture_output=True).returncode == 0: log(f'   Python for the build script: {c}'); return c
    base = next((c for c in seen if subprocess.run([c, '-c', 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)'], capture_output=True).returncode == 0), None)
    if not base: raise RuntimeError('the build script needs Python 3.12 or newer: brew install python@3.13')
    log(f'   installing the build script\'s Python modules into {VENV} (needs the internet, once)')
    if not (VENV / 'bin/python').exists(): P.run([base, '-m', 'venv', VENV], log)
    py = str(VENV / 'bin/python')
    ver = subprocess.run([py, '-c', 'import sys; print(sys.version_info >= (3, 14))'], capture_output=True, text=True).stdout.strip()
    P.run([py, '-m', 'pip', 'install', '--upgrade', 'pip', 'lz4', 'capstone>=5,<6', 'fonttools', 'zbic'] + ([] if ver == 'True' else ['backports.zstd']), log)
    if subprocess.run([py, '-c', NEEDED], capture_output=True).returncode: raise RuntimeError('the modules did not install: run  ' + py + ' -m pip install lz4 "capstone>=5,<6" fonttools zbic backports.zstd')
    return py


def devices(log):
    """(identifier, name, udid) of connected devices, from devicectl's JSON."""
    out = Path(tempfile.mkdtemp()) / 'dev.json'
    subprocess.run(['xcrun', 'devicectl', 'list', 'devices', '--json-output', str(out)], capture_output=True)
    try: data = json.loads(out.read_text())
    except Exception: return []
    res = []
    for d in data.get('result', {}).get('devices', []):
        hw, cp, dp = d.get('hardwareProperties', {}), d.get('connectionProperties', {}), d.get('deviceProperties', {})
        if hw.get('platform') != 'iOS': continue
        res.append((d.get('identifier', ''), dp.get('name', '?'), hw.get('udid', ''), cp.get('transportType', '')))
    return res


def env_text(env): return '# written by nx_ocarina_ios_builder\n' + ''.join(f'{k}={v}\n' for k, v in env.items())


def patch_ipa(ipa, out, env, log):
    """Copy an IPA with dm/env.txt in its .app replaced by these switches (the iOS host reads it at boot, main.m). The code signature no longer matches: install the result with a sideloader (AltStore, Sideloadly, LiveContainer), which signs it again."""
    import zipfile
    ipa, out = Path(ipa), Path(out)
    with zipfile.ZipFile(ipa) as zi:
        apps = sorted({n.split('/')[1] for n in zi.namelist() if n.startswith('Payload/') and n.split('/')[1].endswith('.app')})
        if not apps: raise RuntimeError('no Payload/*.app in that IPA')
        target = f'Payload/{apps[0]}/dm/env.txt'
        with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as zo:
            for it in zi.infolist():
                if it.filename in (target, f'Payload/{apps[0]}/_CodeSignature/CodeResources'): continue   # the old env file; the old seal goes too
                if it.filename.startswith(f'Payload/{apps[0]}/_CodeSignature/'): continue
                zo.writestr(it, zi.read(it.filename))
            zo.writestr(target, env_text(env))
    log(f'   wrote {out}: {target} now holds {len(env)} switches (unsigned: install it with AltStore, Sideloadly or LiveContainer)')
    return out


def pack_ipa(app, out, log):
    """A built .app (already signed by Xcode) as Payload/<app>.app inside a zip: an IPA for a sideloader (the game files are pushed separately when the build is --romfs separate)."""
    import zipfile
    app, out = Path(app), Path(out)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(app.rglob('*')):
            if f.is_file() or f.is_symlink(): z.write(f, f'Payload/{app.name}/{f.relative_to(app)}')
    log(f'   wrote {out}')



def build(o, log):
    repo = Path(o['repo']).expanduser()
    nsp, keys = Path(o['nsp']).expanduser(), Path(o['keys']).expanduser()
    if not (repo / 'scripts/build_ipa_from_nsp.py').is_file(): raise RuntimeError('the checkout has no scripts/build_ipa_from_nsp.py: choose the nxnative / NX2Apple folder')
    if ' ' in str(repo): raise RuntimeError('the checkout\'s path has a space: move it')
    if not nsp.is_dir(): raise RuntimeError('choose the game folder (the one with the .nsp or .nup files)')
    team, bundle = o['team'].strip(), o['bundle'].strip()
    if not team: raise RuntimeError('enter your Apple team ID (Xcode > Settings > Apple Accounts > Manage Certificates, or developer.apple.com > Membership; a free Personal Team has one too)')
    dev = o.get('device', '').strip()
    env = build_env(o)

    log('1. Patching the checkout')
    patches = sorted(HERE.glob('ocarina-host*.patch'))
    if not patches: raise RuntimeError('no ocarina-host*.patch beside this script')
    P.apply_host_patch(repo, patches, log)
    P.apply_optional_patches(repo, log)

    envf = Path(tempfile.mkdtemp()) / 'env.txt'; envf.write_text(env_text(env))
    wd0 = next(iter(sorted(nsp.glob(f'{P.GAME_ID}_build'))), None) or next(iter(sorted(nsp.glob('*_build'))), None)
    have = wd0 and next(iter(sorted((wd0 / 'xcode').glob('*.xcodeproj'))), None)
    if o.get('reuse', True) and have:
        log('2. Reusing the Xcode project and extracted game files (no extraction, no shader rebuild); only the host is recompiled')
        (wd0 / 'xcode/Gen/dm').mkdir(parents=True, exist_ok=True)
        (wd0 / 'xcode/Gen/dm/env.txt').write_text(env_text(env))
    else:
        log('2. Preparing the Xcode project (extraction and shaders are reused when unchanged; the first run takes 30-60 minutes; .nsp and .nup both work)')
        cmd = [python_with_modules(log), repo / 'scripts/build_ipa_from_nsp.py', nsp, '-k', keys, '--prepare-xcode', '--team-id', team, '--romfs', 'separate', '--env-file', envf, '--keep-extracted']
        if bundle: cmd += ['--bundle-id', bundle]
        if o.get('lang', '').strip(): cmd += ['-l', o['lang'].strip()]
        P.run(cmd, log, cwd=repo)

    wd = next(iter(sorted(nsp.glob(f'{P.GAME_ID}_build'))), None) or next(iter(sorted(nsp.glob('*_build'))), None)
    if not wd: raise RuntimeError(f'no *_build folder appeared in {nsp}')
    xdir = wd / 'xcode'
    ws = next(iter(sorted(xdir.glob('*.xcworkspace'))), None) or next(iter(sorted(nsp.glob('*.xcworkspace'))), None)
    proj = next(iter(sorted(xdir.glob('*.xcodeproj'))), None)
    if not proj and not ws: raise RuntimeError(f'no Xcode project found in {xdir}')
    res_env = xdir / 'Gen/resources.env'

    # signing: the project is generated with the team as given; Xcode asks Apple for the profile, the Increased Memory Limit entitlement is in Gen/DmIOS.entitlements
    ent = xdir / 'Gen/DmIOS.entitlements'
    log('3. Building and signing with Xcode' + (f' (entitlements: {ent.name} present)' if ent.is_file() else ' (WARNING: no Gen/DmIOS.entitlements: the app will run without Increased Memory Limit)'))
    dd = wd / 'xcode-derived-data'
    scheme = 'DmIOS'
    try:
        lst = json.loads(subprocess.run(['xcodebuild', *(['-workspace', str(ws)] if ws else ['-project', str(proj)]), '-list', '-json'], capture_output=True, text=True).stdout)
        names = (lst.get('workspace') or lst.get('project') or {}).get('schemes', [])
        scheme = next((n for n in names if P.GAME_ID in n), scheme)
    except Exception: pass
    log(f'   scheme: {scheme}')
    xb = ['xcodebuild', *(['-workspace', str(ws)] if ws else ['-project', str(proj)]), '-scheme', scheme, '-configuration', 'Release', '-derivedDataPath', str(dd),
          '-destination', f'id={dev}' if dev else 'generic/platform=iOS', '-allowProvisioningUpdates', f'DEVELOPMENT_TEAM={team}', 'CODE_SIGN_STYLE=Automatic', 'DM_COPY_ROMFS=NO', 'build']
    if bundle: xb.append(f'PRODUCT_BUNDLE_IDENTIFIER={bundle}')
    P.run(xb, log, cwd=xdir)
    apps = sorted((dd / 'Build/Products').glob('Release-iphoneos/*.app'))
    if not apps: raise RuntimeError('the build produced no .app under ' + str(dd))
    app = apps[0]
    if o.get('ipa_out', '').strip(): pack_ipa(app, Path(o['ipa_out']).expanduser(), log)
    if not dev: log('   built; no device chosen, so nothing is installed. Choose one and run again to install.'); return

    log('4. Installing over USB')
    P.run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', dev, app], log, cwd=xdir)
    bid = bundle or subprocess.run(['/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleIdentifier', str(app / 'Info.plist')], capture_output=True, text=True).stdout.strip()

    log('5. Writing Documents/env.txt on the phone')
    P.run(['xcrun', 'devicectl', 'device', 'copy', 'to', '--device', dev, '--domain-type', 'appDataContainer', '--domain-identifier', bid,
           '--source', envf, '--destination', 'Documents/env.txt'], log, cwd=xdir, check=False)

    if o.get('push_romfs', True):
        log('6. Pushing the game files (once per game version; needs the phone\'s UDID and, for speed, afcclient from libimobiledevice: brew install libimobiledevice)')
        udid = next((d[2] for d in devices(log) if dev in (d[0], d[2])), dev)
        P.run(['bash', repo / 'scripts/push_romfs.sh', udid, bid, res_env], log, cwd=repo, check=False)
    log('Done. On the iPhone: Settings > Privacy & Security > Developer Mode on; trust your developer profile (Settings > General > VPN & Device Management); then open the app.')


def gui():
    import tkinter as tk
    from tkinter import ttk
    s = {}
    try: s = json.loads(SETTINGS.read_text())
    except Exception: pass
    root = tk.Tk(); root.title('Ocarina of Time iPhone builder'); root.geometry('1300x980'); root.minsize(1000, 650)
    v = {k: tk.StringVar(value=s.get(k, d)) for k, d in dict(repo=str(Path.home() / 'NX2Apple'), nsp='', keys=str(Path.home() / '.switch/prod.keys'), team='', bundle='', lang='en-US', device='',
                                                             stand_kb='8', stand_kb_mid='4', stand_kb_small='2', shadow_div='16', keep='ui', textures='big', extra='', ipa_in='', ipa_out='').items()}
    b = {k: tk.BooleanVar(value=s.get(k, d)) for k, d in dict(guest_file=True, sweep=True, fps20=True, water_off=False, effects_off=False, hud=False, push_romfs=True, reuse=True).items()}
    q = queue.Queue()
    left, right = ttk.Frame(root, padding=10), ttk.Frame(root, padding=10)
    left.pack(side='left', fill='y'); right.pack(side='left', fill='both', expand=True)
    r = [0]
    def row(label, w):
        ttk.Label(left, text=label).grid(row=r[0], column=0, sticky='w', pady=2); w.grid(row=r[0], column=1, sticky='we', pady=2, padx=4); r[0] += 1
        return w
    def pick(var, folder, title):
        p = P.finder_pick(True) if folder else subprocess.run(['osascript', '-e', f'POSIX path of (choose file with prompt "{title}")'], capture_output=True, text=True).stdout.strip()
        if p: var.set(p.rstrip('/'))
    def path_row(label, key, folder):
        f = ttk.Frame(left); ttk.Entry(f, textvariable=v[key], width=44).pack(side='left'); ttk.Button(f, text='Choose…', command=lambda: pick(v[key], folder, label)).pack(side='left', padx=4); row(label, f)
    path_row('nxnative / NX2Apple checkout', 'repo', True); path_row('Game folder (.nsp or .nup)', 'nsp', True); path_row('prod.keys', 'keys', False)
    row('Apple team ID', ttk.Entry(left, textvariable=v['team'], width=20)); row('Bundle ID (blank = default)', ttk.Entry(left, textvariable=v['bundle'], width=34)); row('Language', ttk.Entry(left, textvariable=v['lang'], width=10))
    devs = ttk.Combobox(left, textvariable=v['device'], width=46, state='readonly'); row('iPhone (USB-C)', devs)
    def refresh():
        found = devices(q.put); devs['values'] = [d[0] for d in found]; devs.names = {d[0]: f'{d[1]} {d[3]}' for d in found}
        if found and not v['device'].get(): v['device'].set(found[0][0])
        q.put('devices: ' + (', '.join(f'{d[1]} ({d[3]}) {d[0]}' for d in found) or 'none found: connect the phone, unlock it and tap Trust'))
    row('', ttk.Button(left, text='Refresh devices', command=lambda: threading.Thread(target=refresh, daemon=True).start()))
    ttk.Separator(left).grid(row=r[0], columnspan=2, sticky='we', pady=6); r[0] += 1
    for lab, key, vals in (('Detail of dropped textures', 'stand_kb', ['1', '2', '4', '6', '8', '16', '32']), ('Medium textures', 'stand_kb_mid', ['', '1', '2', '4', '6', '8', '16']),
                           ('Small textures', 'stand_kb_small', ['', '1', '2', '4', '6', '8', '16']), ('Shadow map divider', 'shadow_div', ['1', '2', '4', '8', '16', '24', '32', '48']), ('Textures dropped', 'textures', ['big', 'all', 'none'])):
        row(lab, ttk.Combobox(left, textvariable=v[key], values=vals, width=8))
    row('Also keep sharp (ui is always on)', ttk.Entry(left, textvariable=v['keep'], width=20)); row('Extra DM_X=Y (; separated)', ttk.Entry(left, textvariable=v['extra'], width=34))
    for lab, key in (('RAM saver (DM_GUEST_FILE)', 'guest_file'), ('Texture sweep (DM_ALIAS_SWEEP)', 'sweep'), ('20 fps cap', 'fps20'), ('Water off', 'water_off'), ('Effects off', 'effects_off'), ('Metal HUD', 'hud'), ('Push game files over USB / Wi-Fi', 'push_romfs'), ('Fast rebuild: reuse the extracted game and Xcode project', 'reuse')):
        ttk.Checkbutton(left, text=lab, variable=b[key]).grid(row=r[0], column=1, sticky='w'); r[0] += 1
    def preset_iphone():
        v['stand_kb'].set('2'); v['stand_kb_mid'].set('1'); v['stand_kb_small'].set('1'); v['shadow_div'].set('48'); v['textures'].set('all'); v['keep'].set('')
        b['guest_file'].set(True); b['sweep'].set(True); b['fps20'].set(True); b['water_off'].set(True); b['effects_off'].set(True)
        q.put('iPhone preset (untested): the lowest settings. An iPhone 12 Pro Max reports a Metal working set of only 2.74 GB, about half of the Mac\'s.')
    ttk.Button(left, text='iPhone preset: lowest memory (untested)', command=preset_iphone).grid(row=r[0], column=1, sticky='we', pady=2); r[0] += 1
    ttk.Separator(left).grid(row=r[0], columnspan=2, sticky='we', pady=6); r[0] += 1
    path_row('IPA to write (for AltStore / Sideloadly / AirDrop)', 'ipa_out', False)
    path_row('IPA already built (settings only)', 'ipa_in', False)
    def patch_only():
        o = {k: x.get() for k, x in v.items()}; o.update({k: x.get() for k, x in b.items()})
        try:
            src = Path(o['ipa_in']).expanduser()
            if not src.is_file(): raise RuntimeError('choose the IPA NX2Apple built')
            patch_ipa(src, src.with_name(src.stem + '-settings.ipa'), build_env(o), q.put)
            q.put('Only the switches the host already knows take effect this way; the texture tiers, the sweep and 20 fps need the rebuilt host (Build, sign and install).')
        except Exception as e: q.put(f'FAILED: {e}')
    ttk.Button(left, text='Patch the IPA\'s settings only (no rebuild)', command=patch_only).grid(row=r[0], column=1, sticky='we', pady=2); r[0] += 1
    txt = tk.Text(right, wrap='word', font=('Menlo', 11)); txt.pack(fill='both', expand=True)
    btn = ttk.Button(left, text='Build, sign and install')
    btn.grid(row=r[0], column=1, sticky='we', pady=10)
    def log(m): q.put(m)
    def work():
        o = {k: x.get() for k, x in v.items()}; o.update({k: x.get() for k, x in b.items()})
        try: SETTINGS.write_text(json.dumps({**{k: x.get() for k, x in v.items()}, **{k: x.get() for k, x in b.items()}}))
        except Exception: pass
        try: build(o, log)
        except Exception as e: log(f'FAILED: {e}')
        q.put('\0done')
    def go(): btn.state(['disabled']); txt.delete('1.0', 'end'); threading.Thread(target=work, daemon=True).start()
    btn.config(command=go)
    def pump():
        try:
            while True:
                m = q.get_nowait()
                if m == '\0done': btn.state(['!disabled'])
                else: txt.insert('end', m + '\n'); txt.see('end')
        except queue.Empty: pass
        root.after(150, pump)
    pump(); threading.Thread(target=refresh, daemon=True).start(); root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cli', action='store_true'); ap.add_argument('--repo'); ap.add_argument('--nsp'); ap.add_argument('--keys'); ap.add_argument('--team'); ap.add_argument('--bundle', default='')
    ap.add_argument('--device', default=''); ap.add_argument('--lang', default='en-US'); ap.add_argument('--stand-kb', default='8'); ap.add_argument('--stand-kb-mid', default='4'); ap.add_argument('--stand-kb-small', default='2')
    ap.add_argument('--shadow-div', default='16'); ap.add_argument('--keep', default='ui'); ap.add_argument('--textures', default='big'); ap.add_argument('--no-guest-file', action='store_true'); ap.add_argument('--no-sweep', action='store_true')
    ap.add_argument('--fps20', action='store_true'); ap.add_argument('--no-push-romfs', action='store_true'); ap.add_argument('--list-devices', action='store_true'); ap.add_argument('--no-reuse', action='store_true'); ap.add_argument('--ipa-out', default=''); ap.add_argument('--patch-ipa', default='', help='write <ipa>-settings.ipa with these switches and stop')
    a = ap.parse_args()
    if a.patch_ipa:
        src = Path(a.patch_ipa); o = dict(stand_kb=a.stand_kb, stand_kb_mid=a.stand_kb_mid, stand_kb_small=a.stand_kb_small, shadow_div=a.shadow_div, keep=a.keep, textures=a.textures, guest_file=not a.no_guest_file, sweep=not a.no_sweep, fps20=a.fps20)
        patch_ipa(src, src.with_name(src.stem + '-settings.ipa'), build_env(o), log_default); return
    if a.list_devices: [print(*d) for d in devices(log_default)]; return
    if not a.cli: return gui()
    build(dict(repo=a.repo, nsp=a.nsp, keys=a.keys, team=a.team, bundle=a.bundle, device=a.device, lang=a.lang, stand_kb=a.stand_kb, stand_kb_mid=a.stand_kb_mid, stand_kb_small=a.stand_kb_small, shadow_div=a.shadow_div,
               keep=a.keep, textures=a.textures, guest_file=not a.no_guest_file, sweep=not a.no_sweep, fps20=a.fps20, push_romfs=not a.no_push_romfs, reuse=not a.no_reuse, ipa_out=a.ipa_out), log_default)


if __name__ == '__main__':
    main()
