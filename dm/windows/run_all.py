#!/usr/bin/env python3
# Runs the real Mac build script on Windows through the test adapter, up to its Mac host step, then packs the results. It writes only inside this folder (and pip's own cache).
import argparse, glob, hashlib, json, os, platform, re, shutil, subprocess, sys, time, zipfile
sys.stdout.reconfigure(encoding='utf-8', errors='replace'); sys.stderr.reconfigure(encoding='utf-8', errors='replace')
HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser(); ap.add_argument('--game', default=os.path.join(HERE, 'game')); ap.add_argument('--keys', default=os.path.join(HERE, 'keys', 'prod.keys'))
ap.add_argument('--ipa', help='also build the iPhone IPA here, after the Mac app (the Mac compiles its executable: queue.py)'); ap.add_argument('--metal-root'); ap.add_argument('--spirv'); ap.add_argument('--work', default=os.path.join(HERE, 'w')); ap.add_argument('--kit', default=os.path.join(HERE, 'kit')); ap.add_argument('--app-dir', dest='work_app', default=os.path.join(HERE, 'app'))
ap.add_argument('--ipa-only', action='store_true', help='only the iPhone IPA (--ipa): no Mac app, for a game whose Mac app already exists'); a = ap.parse_args()
if a.ipa_only and not a.ipa: print('--ipa-only needs --ipa'); sys.exit(2)
def stop(m): print('\n' + m + '\n'); sys.exit(1)
print('Checking what the test needs...')
if sys.version_info[:2] not in ((3, 14), (3, 13)): stop(f'This is Python {sys.version.split()[0]}. The test needs Python 3.14, which can sit beside other versions:\nfrom python.org, install the latest 3.14 for Windows (64-bit), then run this again.')
games = [g for e in ('nsp', 'xci', 'nsz', 'xcz') for g in glob.glob(os.path.join(glob.escape(a.game), '*.' + e))]
if not games: stop(f'Put the game file (.nsp or .xci) in this folder first:\n  {a.game}')
if not os.path.isfile(a.keys): stop(f'Put your prod.keys here first:\n  {a.keys}')
dn = shutil.which('dotnet')
sdks = subprocess.run([dn, '--list-sdks'], capture_output=True, text=True).stdout if dn else ''
if not re.search(r'^10\.', sdks, re.M): stop('The .NET 10 SDK was not found. Install it from dot.net/download, then run this again.')
roots = [a.metal_root] if a.metal_root else [os.path.join(os.environ.get('ProgramFiles', ''), 'Metal Developer Tools')]
metals = [m for r in roots for m in glob.glob(os.path.join(glob.escape(r), 'metal', '*', 'bin', 'metal.exe'))]
if not metals: stop('metal.exe was not found under: ' + ', '.join(roots))
metal = sorted(metals)[-1]; mroot = os.path.dirname(os.path.dirname(metal))
spirv = a.spirv or next((p for p in (os.path.join(HERE, 'tools', 'spirv-cross.exe'), os.path.join(os.environ.get('VULKAN_SDK', '?'), 'Bin', 'spirv-cross.exe')) if os.path.isfile(p)), None)
if not spirv: stop('spirv-cross.exe was not found (tools folder or the Vulkan SDK).')
deps = os.path.join(HERE, 'pydeps')
def have():
    r = subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, sys.argv[1])\nimport lz4.block, capstone, fontTools, PIL, zbic\ntry: import compression.zstd\nexcept ImportError: import backports.zstd\nassert capstone.cs_version()[0] == 5', deps], capture_output=True)
    return r.returncode == 0
if not have():
    print('Installing the Python modules into this folder...')
    subprocess.run([sys.executable, '-m', 'pip', 'install', '--only-binary=:all:', '--upgrade', '--target', deps, 'lz4', 'capstone>=5,<6', 'fonttools', 'pillow', 'zbic'] + (['backports.zstd'] if sys.version_info < (3, 14) else []), check=False)
    if not have(): stop('The Python modules could not be installed. The messages above say why; send them to Claude.')
env = dict(os.environ, DM_WINSHIM='1', DM_WIN_METAL=metal, DM_WIN_METALLIB=os.path.join(os.path.dirname(metal), 'metallib.exe'), DM_WIN_SPIRV=spirv, PYTHONUTF8='1', DMSH_PCH='0', DM_REWRITE_J='1',
           PYTHONPATH=os.pathsep.join([os.path.join(HERE, 'winshim'), deps]), DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', MSBUILDDISABLENODEREUSE='1', DOTNET_CLI_USE_MSBUILD_SERVER='0', TEMP=os.path.join(HERE, 'tmp'), TMP=os.path.join(HERE, 'tmp'), TMPDIR=os.path.join(HERE, 'tmp'))
dll = sorted({os.path.dirname(p) for p in glob.glob(os.path.join(glob.escape(mroot), 'lib', '**', '*.dll'), recursive=True)})
env['PATH'] = os.pathsep.join(dll + [env.get('PATH', '')])
for d in (env['TEMP'], a.work): os.makedirs(d, exist_ok=True)
res = os.path.join(HERE, 'results'); shutil.rmtree(res, ignore_errors=True); os.makedirs(res)
log = os.path.join(res, 'build_console.log'); t0 = time.time(); print('Building. This takes several minutes; the last line shows the current step.\n')
env['DM_WIN_KIT'] = a.kit; kit_info = json.load(open(os.path.join(a.kit, 'kit.json'), encoding='utf-8')); APP = os.path.join(a.work_app, kit_info.get('name', 'Devil May Cry 3 Special Edition') + '.app'); shutil.rmtree(a.work_app, ignore_errors=True); os.makedirs(a.work_app)
cmd = [sys.executable, os.path.join(HERE, 'winshim', 'win_main.py'), a.game, '-k', a.keys, '--work-dir', a.work, '--platform', 'mac-app', '--allow-other-metal', '--no-heap-check', '--progress', 'lines', '-o', APP]
with open(log, 'w', encoding='utf-8', errors='replace', newline='\n') as f:
    if a.ipa_only: f.write('--ipa-only: no Mac app\n'); code = 0
    else:
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', cwd=HERE)
        for l in p.stdout:
            f.write(l); f.flush()
            if re.search(r'\[\d/\d\]|error|RED|Traceback', l): print(l.rstrip()[:150])
        code = p.wait()
text = open(log, encoding='utf-8', errors='replace').read(); logs_dir = os.path.join(a.work, 'logs')
reached = code == 0 and os.path.isfile(os.path.join(APP, 'Contents', 'MacOS', 'dmhost'))
summary = {'app_made': reached, 'exit_code': code, 'seconds': round(time.time() - t0), 'python': sys.version.split()[0], 'windows': platform.platform(), 'metal': metal, 'spirv': spirv, 'sdks': sdks.strip()}
json.dump(summary, open(os.path.join(res, 'status.json'), 'w'), indent=1)
# Every file's hash, to compare with the Mac's; the files themselves except the very large ones and anything holding keys
print('Packing the results...')
rows, z = [], zipfile.ZipFile(os.path.join(HERE, 'results.zip'), 'w', zipfile.ZIP_DEFLATED, allowZip64=True)
for r, ds, fs in os.walk(a.work):
    for n in fs:
        q = os.path.join(r, n); rel = os.path.relpath(q, a.work).replace('\\', '/')
        if n.lower() in ('prod.keys', 'keys.txt', 'title.keys'): continue
        h = hashlib.sha256()
        with open(q, 'rb') as g:
            for b in iter(lambda: g.read(1 << 20), b''): h.update(b)
        s = os.path.getsize(q); rows.append(f'{h.hexdigest()}\t{s}\t{rel}')
        if rel.startswith('tools/') or rel.startswith('extract/') or s > 300 << 20: continue
        z.write(q, 'build/' + rel)
open(os.path.join(res, 'hashes.tsv'), 'w', newline='\n').write('\n'.join(rows) + '\n')
for n in os.listdir(res): z.write(os.path.join(res, n), 'results/' + n)
z.close()
# The app as a zip a Mac unpacks with its executable marks, which Windows files do not have
if reached:
    print('Packing the Mac app...')
    zp = os.path.join(HERE, 'app.zip'); rows2 = []
    with zipfile.ZipFile(zp, 'w', allowZip64=True) as az:
        for r, ds, fs in os.walk(APP):
            for n in sorted(fs):
                q = os.path.join(r, n); rel = os.path.relpath(q, a.work_app).replace('\\', '/'); zi = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0)); zi.create_system = 3
                ex = '/Contents/MacOS/' in rel; zi.external_attr = (0o100755 if ex else 0o100644) << 16
                big = os.path.getsize(q) > 64 << 20; zi.compress_type = zipfile.ZIP_STORED if big else zipfile.ZIP_DEFLATED
                h = hashlib.sha256()
                with open(q, 'rb') as g, az.open(zi, 'w', force_zip64=True) as w:
                    for b in iter(lambda: g.read(1 << 20), b''): h.update(b); w.write(b)
                rows2.append(f'{h.hexdigest()}\t{os.path.getsize(q)}\t{rel}')
    with zipfile.ZipFile(os.path.join(HERE, 'results.zip'), 'a', zipfile.ZIP_DEFLATED) as z2: z2.writestr('results/app_hashes.tsv', '\n'.join(rows2) + '\n')
if (reached or (a.ipa_only and code == 0)) and a.ipa:
    print('Building the iPhone app...'); shutil.rmtree(os.path.join(a.work, 'tmp'), ignore_errors=True)
    ic = [sys.executable, os.path.join(HERE, 'winshim', 'win_main.py'), a.game, '-k', a.keys, '--work-dir', a.work, '--platform', 'ios', '--allow-other-metal', '--no-heap-check', '--progress', 'lines', '--gpu', 'none', '-o', a.ipa]
    with open(os.path.join(res, 'ipa_console.log'), 'w', encoding='utf-8', errors='replace', newline='\n') as f:
        p = subprocess.Popen(ic, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', cwd=HERE)
        for l in p.stdout:
            f.write(l); f.flush()
            if re.search(r'\[\d/\d\]|error|RED|Traceback|iPhone app', l): print('ipa: ' + l.rstrip()[:150])
        ipa_code = p.wait()
    print(f'IPA_RESULT {ipa_code} {os.path.isfile(a.ipa)}')
    with zipfile.ZipFile(os.path.join(HERE, 'results.zip'), 'a', zipfile.ZIP_DEFLATED) as z2: z2.write(os.path.join(res, 'ipa_console.log'), 'results/ipa_console.log')
print('\nOnly the IPA was asked for.' if a.ipa_only else '\nThe Mac app was made.' if reached else f'\nThe build stopped early (exit code {code}). That is useful too: the reason is in the results.')
print('Copy these back to the Mac (the shared folder is fine) and tell Claude:\n  ' + os.path.join(HERE, 'results.zip') + ('\n  ' + os.path.join(HERE, 'app.zip') if reached else ''))
