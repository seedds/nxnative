#!/usr/bin/env python3
# kit_host.py <profile folder> <title ID> <kit folder>: the kit's host, compiled on a Mac with the NSP script's own step from the profile a PC derived or guessed: <profile folder>/<title ID>/game.tsv and
# the tables beside it, with no game files. Run it after the PC's shader split, when the profile is final. The checks that boot the game (heap, address regions) have not run on that profile.
import importlib.util, os, sys
from pathlib import Path
profiles, title, kit = sys.argv[1:4]; repo = Path(os.environ.get('DM_REPO') or Path(__file__).resolve().parents[2]); kit = Path(kit).resolve()
os.environ['DM_GAMES_DIR'] = str(Path(profiles).resolve()); os.environ['DM_GAMES_FIRST'] = '1'
sys.path.insert(0, str(repo / 'dm/games')); import game as gamemod
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', repo / 'scripts/build_ipa_from_nsp.py'); m = importlib.util.module_from_spec(spec); sys.modules[spec.name] = m; spec.loader.exec_module(m)
env = dict(os.environ, PATH=os.pathsep.join([*m.TOOL_PATHS, os.environ['PATH']]))
out = kit / 'dmhost'; logs = kit / 'logs'; logs.mkdir(exist_ok=True)
m.build_mac_host(repo, out, env, logs, gamemod.load(title.upper()))
print(out, out.stat().st_size)
