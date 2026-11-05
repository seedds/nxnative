#!/usr/bin/env python3
"""Run an existing Mac game build with LAN discovery relayed to a routed peer."""
import argparse
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import plistlib
import socket
import subprocess
import tempfile
import unicodedata
from xml.parsers.expat import ExpatError


def route_source(peer):
    # UDP connect selects a route/source address without sending a packet.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect((str(peer), 9))
        return route.getsockname()[0]


def player_name(value):
    value = value.strip()
    if not value or any(unicodedata.category(char) == 'Cc' for char in value):
        raise argparse.ArgumentTypeError('enter a nonempty username without control characters')
    # nn::account::Nickname has 32 UTF-8 bytes plus its terminator. Reject long
    # names before the host's bounded copy could split a multibyte character.
    try:
        size = len(value.encode('utf-8'))
    except UnicodeEncodeError:
        raise argparse.ArgumentTypeError('username must be valid UTF-8') from None
    if size > 32:
        raise argparse.ArgumentTypeError('username is too long: use at most 32 UTF-8 bytes')
    return value


def remembered_username(path):
    try:
        data = json.loads(path.read_text())
        return player_name(data['username']) if isinstance(data, dict) and isinstance(data.get('username'), str) else None
    except (OSError, ValueError, argparse.ArgumentTypeError):
        return None


def remember_username(path, username):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                     prefix=path.name + '.', suffix='.partial', delete=False) as stream:
        temporary = Path(stream.name)
        json.dump({'username': username}, stream, ensure_ascii=False)
        stream.write('\n')
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def build_layout(root, profile=None):
    """Resolve both registered and derived profiles of an existing Mac build."""
    versions = sorted((path for path in root.glob('patched_v*') if path.name[9:].isdigit()),
                      key=lambda path: int(path.name[9:]))
    if not versions:
        raise ValueError(f'no rewritten modules in {root}; first build the ROM with build_ipa_from_nsp.py')
    derived = list((root / 'profile').glob('*/shader_map.tsv'))
    title_id = (profile.name if profile else derived[0].parent.name if len(derived) == 1
                else root.name.removesuffix('_build')).upper()
    if len(title_id) != 16 or any(c not in '0123456789ABCDEF' for c in title_id):
        raise ValueError('cannot determine the title ID; use --profile with this build\'s game profile folder')
    candidates = ([profile / 'shader_map.tsv'] if profile else [
        root / 'profile' / title_id / 'shader_map.tsv',
        Path(__file__).resolve().with_name('runtime') / 'tables' / title_id / 'shader_map.tsv',
        Path(__file__).resolve().parents[1] / 'dm/games' / title_id / 'shader_map.tsv'])
    shader_map = next((path for path in candidates if path.is_file()), None)
    if shader_map is None:
        raise ValueError('cannot find the shader profile; use --profile with the profile used to build this game')
    patched = versions[-1]
    return title_id, patched, root / 'extract' / patched.name.removeprefix('patched_') / 'RomFS.img', shader_map


def regular_app(root, title_id, requested=None):
    candidates = [requested.expanduser().resolve()] if requested else sorted(set(
        path for parent in (root.parent, root, root / 'app', root / 'mac', Path('/Applications'), Path.home() / 'Applications')
        for path in parent.glob('*.app')))
    found = []
    for path in candidates:
        try:
            info = plistlib.loads((path / 'Contents/Info.plist').read_bytes())
        except (OSError, ValueError, plistlib.InvalidFileException, ExpatError):
            if requested:
                raise ValueError(f'cannot read app metadata: {path}') from None
            continue
        if str(info.get('DMTitleID', '')).upper() == title_id:
            found.append((path, info))
    if requested and not found:
        raise ValueError('the selected app belongs to another title ID')
    return found[0] if len(found) == 1 else (None, {})


def apply_app_settings(env, root, title_id, app, info):
    app_data = Path.home() / 'Library/Application Support/nxnative' / title_id
    resources = app / 'Contents/Resources' if app else None
    # Same precedence as the regular launcher: process, user env.txt, app env.txt,
    # validated saved choices, then the defaults offered by this app.
    files = [app_data / 'env.txt']
    if resources:
        files.append(resources / 'dm/env.txt')
    for path in files:
        if not path.is_file():
            continue
        for line in path.read_text().splitlines():
            key, sep, value = line.strip().partition('=')
            if sep and key.strip() and not key.startswith('#'):
                env.setdefault(key.strip(), value)
    settings = resources / 'dm/launcher.json' if resources else root / 'app/launcher.json'
    rules = json.loads(settings.read_text()).get('settings', []) if settings.is_file() else []
    domain = info.get('CFBundleIdentifier', f'org.nxnative.{title_id.lower()}')
    preferences = {}
    if rules and env.get('DM_SAVED_CHOICES') != '0':
        result = subprocess.run(['/usr/bin/defaults', 'export', domain, '-'], capture_output=True)
        if result.returncode == 0:
            try:
                preferences = plistlib.loads(result.stdout)
            except (ValueError, plistlib.InvalidFileException, ExpatError):
                pass
    for rule in rules:
        key = rule['env']
        if key in env:
            continue
        value = preferences.get(f'launcher.{key}')
        accepted = False
        if isinstance(value, str):
            if rule['kind'] == 'choice':
                accepted = value in [option[0] for option in rule['options']]
            elif rule['kind'] == 'toggle':
                accepted = value in (rule.get('on', '1'), rule.get('off', '0'))
            elif rule['kind'] == 'range':
                try:
                    number = float(value)
                    accepted = value == value.strip() and math.isfinite(number) and rule['min'] <= number <= rule['max']
                except ValueError:
                    pass
        env[key] = value if accepted else str(rule['default'])
    if resources and (resources / 'pso/prebuilt.tsv').is_file():
        # This is this Mac's own app archive. The host chooses its GPU architecture
        # and handles missing/unsupported entries through its normal device cache.
        env.setdefault('DM_PSO_BUNDLE', str(resources / 'pso'))
    env.setdefault('DM_LANG', info.get('DMLanguage', 'en-US'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', nargs='?', type=Path, help='Completed nxnative <title ID>_build folder, not a raw ROM folder')
    peers = parser.add_mutually_exclusive_group(required=True)
    peers.add_argument('--peer', type=ipaddress.IPv4Address)
    peers.add_argument('--choose-peer', action='store_true', help='Ask for the other Mac\'s IPv4 address in a dialog')
    names = parser.add_mutually_exclusive_group()
    names.add_argument('--username', type=player_name, help='In-game account nickname for this launch (up to 32 UTF-8 bytes)')
    names.add_argument('--choose-username', action='store_true', help='Ask for an in-game nickname and remember it on this Mac')
    parser.add_argument('--save', type=Path, help='Override the regular nxnative app save (also honors DM_SAVE)')
    parser.add_argument('--control', type=Path, help='Optional DM_CTL input file')
    parser.add_argument('--host', type=Path, help='Use this host instead of the build host')
    parser.add_argument('--profile', type=Path, help='Profile used for this game build (registered and derived profiles are normally found automatically)')
    parser.add_argument('--app', type=Path, help='Regular game .app whose settings and local pipeline archive should be reused (normally found automatically)')
    parser.add_argument('--check', action='store_true', help='Check inputs and print module hashes without starting the game')
    parser.add_argument('--choose-build', action='store_true', help='Choose the completed build folder in Finder when no path is given')
    args = parser.parse_args()
    while args.choose_peer and args.peer is None:
        selection = subprocess.run(['/usr/bin/osascript', '-e',
            'text returned of (display dialog "Enter the other player\'s Mac IPv4 address:" '
            'default answer "" with title "nxnative LAN" buttons {"Cancel", "Continue"} default button "Continue")'],
            capture_output=True, text=True)
        if selection.returncode:
            print('Peer selection cancelled.', flush=True)
            return
        try:
            args.peer = ipaddress.IPv4Address(selection.stdout.strip())
        except ValueError:
            print('Enter a valid IPv4 address, such as 192.168.1.100.', flush=True)
    try:
        local_ip = route_source(args.peer)
    except OSError as error:
        parser.error(f'cannot select a route to {args.peer}: {error}')
    if local_ip == str(args.peer):
        parser.error('--peer must identify the other Mac')
    if args.build is None:
        if not args.choose_build:
            parser.error('provide a completed nxnative build folder, or use --choose-build')
        selection = subprocess.run(['/usr/bin/osascript', '-e',
            'POSIX path of (choose folder with prompt "Select your completed nxnative title-ID_build folder (not the raw ROM or .app).")'],
            capture_output=True, text=True)
        if selection.returncode or not selection.stdout.strip():
            print('Folder selection cancelled.', flush=True)
            return
        args.build = Path(selection.stdout.strip())
    root = args.build.expanduser().resolve()
    try:
        title_id, patched, romfs, shader_map = build_layout(root, args.profile.expanduser().resolve() if args.profile else None)
    except ValueError as error:
        parser.error(str(error))
    version = patched.name.removeprefix('patched_')
    bundled_host = Path(__file__).resolve().with_name('dmhost')
    host = (args.host.expanduser().resolve() if args.host else
            bundled_host if bundled_host.is_file() else root / 'tools/dmhost')
    shader_dir = root / 'shader/cache'
    runtime = bundled_host.with_name('runtime')
    # A portable kit can carry the libraries and profile tables used with its
    # host. Those form one runtime; the selected build still supplies game data.
    if host == bundled_host and runtime.is_dir():
        shader_map = runtime / 'tables' / title_id / 'shader_map.tsv'
        shader_dir = runtime / 'shaders'
    for path in (romfs, host, shader_map, shader_dir, root / 'font'):
        if not path.exists():
            parser.error(f'missing build input: {path}')
    app_data = Path.home() / 'Library/Application Support/nxnative' / title_id
    env = dict(os.environ)
    try:
        app, info = regular_app(root, title_id, args.app)
        apply_app_settings(env, root, title_id, app, info)
    except (OSError, ValueError) as error:
        parser.error(f'cannot load regular app settings: {error}')
    username_file = app_data / 'lan-user.json'
    username = args.username or env.get('DM_NICK') or remembered_username(username_file) or 'Player'
    if args.choose_username:
        while True:
            selection = subprocess.run(['/usr/bin/osascript', '-e',
                'on run argv\ntext returned of (display dialog "Choose your in-game username:" '
                'default answer (item 1 of argv) with title "nxnative LAN" '
                'buttons {"Cancel", "Continue"} default button "Continue")\nend run', username],
                capture_output=True, text=True)
            if selection.returncode:
                print('Username selection cancelled.', flush=True)
                return
            try:
                username = player_name(selection.stdout.strip())
                break
            except argparse.ArgumentTypeError as error:
                print(str(error), flush=True)
    try:
        env['DM_NICK'] = player_name(username)
    except argparse.ArgumentTypeError as error:
        parser.error(str(error))
    save_override = args.save or env.get('DM_SAVE')
    save = Path(save_override).expanduser().resolve() if save_override else app_data / 'save'
    temp = save.parent / 'temp'
    print(f'LAN source {local_ip}; discovery peer {args.peer}; build {version}', flush=True)
    print(f'Host: {host}', flush=True)
    print(f'Shaders: {shader_dir}', flush=True)
    print(f'Save: {save}', flush=True)
    print(f'Username: {env["DM_NICK"]}', flush=True)
    print(f'Regular app: {app or "not found; using build settings and local preferences"}', flush=True)
    print(f'Pipeline archive: {env.get("DM_PSO_BUNDLE", "device cache only")}', flush=True)
    for key in ('DM_OPMODE', 'DM_RENDER_SCALE', 'DM_PRESENT_UPSCALE', 'DM_UPSCALER', 'DM_PRESENT_SNAP', 'DM_PSO_MTL4', 'DM_PSO_PREBUILD', 'DM_NVN_ENC_THREAD'):
        if key in env:
            print(f'{key}={env[key]}', flush=True)
    print('Use the same game build and this modified host on both Macs.', flush=True)
    if args.check:
        for path in (host, *sorted(patched.glob('*.patched.bin'))):
            with path.open('rb') as stream:
                digest = hashlib.sha256()
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            print(f'{digest.hexdigest()}  {path.name}', flush=True)
        return
    if args.choose_username:
        try:
            remember_username(username_file, env['DM_NICK'])
        except OSError as error:
            parser.exit(1, f'[RED] cannot remember the LAN username: {error}\n')
    save.mkdir(parents=True, exist_ok=True)
    temp.mkdir(parents=True, exist_ok=True)
    env.update(DM_LAN='1', DM_LAN_PEERS=str(args.peer), DM_LAN_IP=local_ip,
               DM_ROOT=str(save.parent), DM_ROMFS=str(romfs), DM_SAVE=str(save), DM_TEMP=str(temp),
               DM_MSL_DIR=str(shader_dir), DM_FONT_DIR=str(root / 'font'),
               DM_NVN_SHMAP=str(shader_map), DM_WINDOW='1', DM_AUDIO='1',
               DM_LANG=env['DM_LANG'])
    if args.control:
        env['DM_CTL'] = str(args.control.resolve())
    os.chdir(save.parent)
    os.execve(host, [str(host), str(patched)], env)


if __name__ == '__main__':
    main()
