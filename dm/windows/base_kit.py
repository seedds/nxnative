#!/usr/bin/env python3
# base_kit.py <repository> <kit folder> <source label>: what every game's kit shares (the launcher, the button-icon font, kit.json without a game), compiled here from the repository. new_kit.py copies it per game.
# <source label> is what the apps report as their commit: the checkout may hold an uncommitted merge.
import json, os, shutil, subprocess, sys, re
from pathlib import Path
repo, kit, label = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3]
sys.path.insert(0, str(repo / 'dm/mac')); import pack_app as pa
host = repo / 'dm/host/dmhost'
if not host.exists(): sys.exit(f'[RED] no host at {host} to read the minimum macOS from: make -C dm/host GAME=<id> dmhost')
shutil.rmtree(kit, ignore_errors=True); (kit / 'font').mkdir(parents=True); (kit / 'x' / 'y' / 'z').mkdir(parents=True)
minos = pa.min_os(host); sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
clang, swiftc = [subprocess.check_output(['xcrun', '--find', t], text=True).strip() for t in ('clang', 'swiftc')]
out = kit / 'x' / 'y' / 'z' / 'nxnative'; pa.build_launcher(out, minos, clang, swiftc, sdk); shutil.move(out, kit / 'nxnative'); shutil.rmtree(kit / 'x')
subprocess.run([sys.executable, str(repo / 'dm/tools/make_ext_font.py'), str(kit / 'font/nintendo_ext.ttf')], check=True, stdout=subprocess.DEVNULL)
chain = re.search(r'DMSH_CHAIN "(\w+)"', (repo / 'dm/gfx/nvn/dmsh_chain.h').read_text())[1]
json.dump({'commit': label, 'minos': minos, 'chain': chain}, open(kit / 'kit.json', 'w'))
print(kit, minos, chain)
