#!/usr/bin/env python3
# Where things are on this Mac and its network, for queue.py and watchdog.py: local.json beside this file (git ignores it; local.json.example shows the keys),
# or the environment (DM_WIN_ROOT, DM_WIN_REPO, DM_WIN_HOST, DM_WIN_SHARE), which wins. Nothing of a user's setup is written in the scripts themselves.
import json, os, sys
KEYS = {
    'root': 'the folder that holds windows-tools/, kits/ and queue/ on this Mac',
    'repo': 'the nxnative checkout the Mac compiles the host and the iPhone executable from',
    'host': 'the PC\'s build account, user@address',
    'share': 'the shared folder with the game files, as the PC names it (\\\\server\\share\\folder)',
}
_here = os.path.dirname(os.path.abspath(__file__))
try:
    with open(os.path.join(_here, 'local.json'), encoding='utf-8') as f: _file = json.load(f)
except FileNotFoundError: _file = {}
def get(key):
    v = os.environ.get('DM_WIN_' + key.upper()) or _file.get(key)
    if not v:
        sys.exit(f'{key} is not set: put "{key}" ({KEYS[key]}) in {os.path.join(_here, "local.json")} (see local.json.example), or set DM_WIN_{key.upper()}')
    return v
