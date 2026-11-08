#!/usr/bin/env python3
# make_kit.py <a game's Mac app> <kit folder>: what a Windows build takes from a Mac, for one game: the host, the launcher and the button-icon font, from an app the NSP script made. No game data goes in.
import json, os, plistlib, shutil, sys
app, kit = sys.argv[1:3]; c = os.path.join(app, 'Contents')
shutil.rmtree(kit, ignore_errors=True); os.makedirs(os.path.join(kit, 'font'))
for n in ('dmhost', 'nxnative'): shutil.copy(os.path.join(c, 'MacOS', n), kit)
shutil.copy(os.path.join(c, 'Resources', 'dm', 'font', 'nintendo_ext.ttf'), os.path.join(kit, 'font'))
with open(os.path.join(c, 'Info.plist'), 'rb') as f: p = plistlib.load(f)
with open(os.path.join(kit, 'kit.json'), 'w', encoding='utf-8') as f:
    json.dump({'game': p['DMTitleID'].upper(), 'commit': p.get('DMCommit', ''), 'minos': p['LSMinimumSystemVersion'], 'chain': p.get('DMChain', ''), 'name': p['CFBundleName']}, f)
print(kit)
