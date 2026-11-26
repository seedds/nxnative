"""Shared runner for build_ipa.sh's archive and signing steps in unit tests."""
import os
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[3]
SCRIPT = (REPO / 'dm/ios/tools/build_ipa.sh').read_text()
SIGNING = SCRIPT[SCRIPT.index('# The entitlements of an unsigned IPA'):SCRIPT.index('\nP=Payload/DmIOS.app\n')]
PACKING = SCRIPT[SCRIPT.index('\nP=Payload/DmIOS.app\n'):]
PREAMBLE = 'set -u\nt() { :; }; die() { echo "$*" >&2; exit 1; }\ngp() { python3 "$PROFILE_TOOL" "$GAME" "$1"; }\n'


def run_step(script, root, game_id, **extra):
    """Run a sliced build_ipa.sh step under set -u with check_program's invariants."""
    env = {k: v for k, v in os.environ.items() if not k.startswith('DM_') and k not in ('IOS_PULL', 'AOC', 'PSO')}
    # GFXAPI and GD as check_program leaves them before the packing step, which reads both (the supplemental GL library beside an NVN game's shaders).
    env.update(GAME=game_id, DM=str(root / 'dm'), PROFILE_TOOL=str(REPO / 'dm/games/game.py'), GFXAPI='nvn', GD=str(REPO / 'dm/games' / game_id))
    env.update(extra)
    return subprocess.run(['/bin/bash', '-c', PREAMBLE + script], cwd=root, env=env, capture_output=True, text=True)
