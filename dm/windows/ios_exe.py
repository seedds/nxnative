#!/usr/bin/env python3
# ios_exe.py <repository> <ready folder from the PC> <out folder>: the iPhone app's executable, the one part of an IPA that needs Apple's linker. It runs the repository's own build_ipa.sh up to its ad-hoc
# signature, and stops before the packing, which the PC does (winshim/win_main.py). It takes the rewritten modules and the profile from the ready folder; the game's files and shaders stay on the PC, so the
# RomFS and shader libraries the script checks are stand-ins here: a folder with the profile's marker, and a library folder with the real stamp of the PC's build.
# Writes <out>/app.tar and <out>/made.json (what the packing reads from the generated profile). No game files other than the rewritten modules come to the Mac.
# The stop is made in a copy of the script under the system's temporary folder's name in dm/ios/tools, removed afterwards: the script finds the repository from its own place.
import json, os, shutil, subprocess, sys, tarfile
repo, ready, out = [os.path.abspath(x) for x in sys.argv[1:4]]; meta = json.load(open(os.path.join(ready, 'meta.json'), encoding='utf-8'))
gid = meta['GAME']; w = os.path.join(out, 'w'); os.makedirs(out, exist_ok=True); shutil.rmtree(w, ignore_errors=True); os.makedirs(w)
stub = os.path.join(out, 'stub'); shutil.rmtree(stub, ignore_errors=True); romfs = os.path.join(stub, 'game', 'RomFS'); os.makedirs(romfs)
mk = os.path.join(romfs, meta['marker'].lstrip('/')); os.makedirs(os.path.dirname(mk), exist_ok=True); open(mk, 'w').close()
msl = os.path.join(stub, 'msl'); lib = os.path.join(msl, meta['msldir']); os.makedirs(lib)
if meta['GFXAPI'] == 'nvn':
    open(os.path.join(lib, 'stub_b.metallib'), 'w').close(); shutil.copyfile(os.path.join(ready, 'inputs.tsv'), os.path.join(lib, 'inputs.tsv'))
else: open(os.path.join(lib, f'{meta["msldir"]}_shaders.metallib'), 'w').close()
src = os.path.join(repo, 'dm/ios/tools/build_ipa.sh'); s = open(src, encoding='utf-8').read(); a = 'P=Payload/DmIOS.app\n'
if s.count(a) != 1: sys.exit('[RED] build_ipa.sh has changed: no single packing start')
tmp = os.path.join(repo, 'dm/ios/tools/_exe_only.sh'); open(tmp, 'w', encoding='utf-8').write(s.replace(a, 'echo "DM_APP=$APP"; exit 0\n'))
env = dict(os.environ, W=w, ROMFS=romfs, MSL_IOS=msl, PATCHED=os.path.join(ready, 'patched'), AOC='', DM_GAMES_DIR=os.path.join(ready, 'profile'), DM_GAMES_FIRST='1', LC_ALL='C')
env['PATH'] = '/opt/homebrew/bin:/usr/local/bin:' + env['PATH']
for k, v in meta.items():
    if k in ('marker', 'msldir'): continue
    env[k] = os.path.join(ready, v) if k in ('DM_ICON', 'DM_ENV_FILE', 'CONTROL') else v
env['DM_PSO_GPUS'] = 'none'
try: r = subprocess.run(['bash', tmp, os.path.join(out, 'unused.ipa')], env=env, capture_output=True, text=True, errors='replace')
finally: os.unlink(tmp)
open(os.path.join(out, 'build_exe.log'), 'w').write(r.stdout + r.stderr)
app = next((l[7:] for l in r.stdout.splitlines() if l.startswith('DM_APP=')), None)
if r.returncode or not app or not os.path.isdir(app): sys.exit('[RED] the iPhone executable failed: ' + (r.stdout + r.stderr)[-1500:])
ex = sorted(os.path.relpath(os.path.join(d, n), app) for d, _, fs in os.walk(app) for n in fs if os.access(os.path.join(d, n), os.X_OK) and not os.path.islink(os.path.join(d, n)))
with tarfile.open(os.path.join(out, 'app.tar'), 'w') as t: t.add(app, 'DmIOS.app')
hdr = open(os.path.join(repo, 'dm/host/gen', gid, 'game_profile.h')).read(); pull = int(hdr.split('#define DM_GAME_PSO_MTL4 ')[1].split()[0])
zs = subprocess.run(['python3', os.path.join(repo, 'dm/games/game.py'), gid, 'zip_store'], env=env, capture_output=True, text=True, check=True).stdout.strip()
json.dump({'pull': pull, 'zip_store': zs, 'executable': ex}, open(os.path.join(out, 'made.json'), 'w'))
print('app', app, os.path.getsize(os.path.join(out, 'app.tar')), 'executable files', ex)
