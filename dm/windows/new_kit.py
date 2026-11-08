#!/usr/bin/env python3
# new_kit.py <kit folder> <title ID> <app name> <an existing kit or Mac app>: a kit for a game that has no host yet: the launcher and font (the same for every game), and kit.json. The host comes
# later, from the profile the PC derives or guesses (kit_host.py). No game data goes in.
import json, os, plistlib, shutil, sys
kit, title, name, src = sys.argv[1:5]
app = os.path.join(src, 'Contents'); from_app = os.path.isdir(app)
launcher = os.path.join(app, 'MacOS', 'nxnative') if from_app else os.path.join(src, 'nxnative')
font = os.path.join(app, 'Resources', 'dm', 'font', 'nintendo_ext.ttf') if from_app else os.path.join(src, 'font', 'nintendo_ext.ttf')
if from_app:
    with open(os.path.join(app, 'Info.plist'), 'rb') as f: p = plistlib.load(f)
    base = {'commit': p.get('DMCommit', ''), 'minos': p['LSMinimumSystemVersion'], 'chain': p.get('DMChain', '')}
else:
    with open(os.path.join(src, 'kit.json'), encoding='utf-8') as f: b = json.load(f)
    base = {k: b.get(k, '') for k in ('commit', 'minos', 'chain')}
shutil.rmtree(kit, ignore_errors=True); os.makedirs(os.path.join(kit, 'font'))
shutil.copy(launcher, kit); shutil.copy(font, os.path.join(kit, 'font'))
with open(os.path.join(kit, 'kit.json'), 'w', encoding='utf-8') as f: json.dump({'game': title.upper(), 'name': name, **base}, f)
print(kit)
