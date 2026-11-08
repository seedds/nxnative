#!/usr/bin/env python3
"""Sign built IPAs with your own Apple account through Xcode, keep the Increased Memory Limit, and install them.

For a Mac where a sideloading tool cannot sign in (AltServer on macOS 27) or drops the entitlement (Sideloadly). The original IPA is not changed.

For each IPA it:
  1. builds a one-file stub app with the same bundle ID and the entitlement, so that Xcode (automatic signing) makes the certificate, the App ID and the provisioning profile;
  2. unpacks a copy of the IPA beside it, in resign_work/, and gives it that bundle ID, that profile and the stub's entitlements;
  3. signs the copy, checks the signature, installs it with devicectl and deletes the copy.

The bundle ID is org.nxnative.<team>.<title ID> and does not change between runs, so installing again (a free Apple ID's profile lasts 7 days) keeps the save in the app's Documents.

  python3 scripts/resign_ipa.py --dry-run "/path/Hades/Hades.ipa"
  python3 scripts/resign_ipa.py "/path/Hades/Hades.ipa"

Needs: Xcode signed in to your Apple account (Settings > Apple Accounts), the iPhone connected and trusted, Developer Mode on.
Details: docs/development.md, "Signing an IPA with Xcode".
"""
import argparse
import hashlib
import json
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

MEMORY_KEY = 'com.apple.developer.kernel.increased-memory-limit'
FREE_TEAM_APPS = 3      # Apple's limit for a free Apple ID: apps signed by it and installed at one time
SPACE_MARGIN = 1 << 30  # beyond the unpacked size, for the stub's build and the signature
SHORT_PROFILE_DAYS = 7  # a free Apple ID's profile lasts this long; a paid team's lasts a year
XCODE_PROFILES = Path.home() / 'Library/Developer/Xcode/UserData/Provisioning Profiles'


class Fail(Exception):
    pass


def say(text=''):
    print(text, flush=True)


def run(cmd, what, log=None):
    """Run a command and return its stdout. With a log the output goes there and only its error lines are shown on a failure."""
    done = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, errors='replace')
    if log:
        Path(log).write_text(done.stdout + done.stderr)
    if done.returncode:
        text = done.stdout + done.stderr
        if log:
            errors = [line.strip() for line in text.splitlines() if re.search(r'\berror:', line)]
            text = '\n'.join(dict.fromkeys(errors)) or text[-2000:]
            text += f'\nFull log: {log}'
        raise Fail(f'{what} failed:\n{text.strip()}')
    return done.stdout


# ----- the IPA

def read_ipa(ipa):
    """The app's folder in the archive, its Info.plist and its unpacked size."""
    try:
        with zipfile.ZipFile(ipa) as z:
            plists = [n for n in z.namelist() if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', n)]
            if len(plists) != 1:
                raise Fail(f'{ipa}: expected one app in Payload/, found {len(plists)}')
            info = plistlib.loads(z.read(plists[0]))
            return plists[0].rsplit('/', 1)[0], info, sum(i.file_size for i in z.infolist())
    except (zipfile.BadZipFile, plistlib.InvalidFileException, OSError) as e:
        raise Fail(f'{ipa}: not a readable IPA: {e}') from e


def new_bundle_id(old_id, team):
    return f'org.nxnative.{team.lower()}.{old_id.rsplit(".", 1)[-1].lower()}'


def set_bundle_id(info_plist, bundle_id):
    raw = Path(info_plist).read_bytes()
    fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
    info = plistlib.loads(raw)
    info['CFBundleIdentifier'] = bundle_id
    Path(info_plist).write_bytes(plistlib.dumps(info, fmt=fmt))


# ----- the Mac's account and the iPhone

def find_teams():
    done = subprocess.run(['defaults', 'export', 'com.apple.dt.Xcode', '-'], capture_output=True)
    if done.returncode:
        return []
    by_account = plistlib.loads(done.stdout).get('IDEProvisioningTeamByIdentifier', {})
    teams = {t['teamID']: t for account in by_account.values() for t in account if t.get('teamID')}
    return list(teams.values())


def pick_team(wanted):
    teams = find_teams()
    if wanted:
        for t in teams:
            if t['teamID'] == wanted:
                return t
        return {'teamID': wanted}  # a team Xcode has not listed yet: xcodebuild says whether it is usable
    if not teams:
        raise Fail('Xcode has no Apple account. Add one in Xcode > Settings > Apple Accounts, then run this again.')
    if len(teams) > 1:
        names = '\n'.join(f'  {t["teamID"]}  {t.get("teamName", "")}' for t in teams)
        raise Fail(f'Xcode has several teams; choose one with --team:\n{names}')
    return teams[0]


def find_devices():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / 'devices.json'
        run(['xcrun', 'devicectl', 'list', 'devices', '--json-output', out], 'Listing devices')
        devices = json.loads(out.read_text())['result']['devices']
    found = []
    for d in devices:
        hw, conn = d.get('hardwareProperties', {}), d.get('connectionProperties', {})
        if (hw.get('reality') == 'physical' and hw.get('platform') == 'iOS' and hw.get('udid')
                and conn.get('pairingState') == 'paired' and conn.get('tunnelState') != 'unavailable'):
            found.append((hw['udid'], d.get('deviceProperties', {}).get('name', '')))
    return found


def pick_device(wanted):
    devices = find_devices()
    if wanted:
        if wanted not in [udid for udid, _ in devices]:
            raise Fail(f'Device {wanted} is not connected and paired.')
        return wanted
    if not devices:
        raise Fail('No paired iPhone or iPad is connected. Connect it by USB, unlock it, and trust this Mac.')
    if len(devices) > 1:
        names = '\n'.join(f'  {udid}  {name}' for udid, name in devices)
        raise Fail(f'Several devices are connected; choose one with --device:\n{names}')
    return devices[0][0]


# ----- the stub app, which makes Xcode provision the bundle ID

STUB_PROJECT = """// !$*UTF8*$!
{
	archiveVersion = 1;
	objectVersion = 56;
	objects = {
		A1 /* main.m in Sources */ = {isa = PBXBuildFile; fileRef = A2; };
		A2 /* main.m */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.objc; path = main.m; sourceTree = "<group>"; };
		A3 /* Stub.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = Stub.app; sourceTree = BUILT_PRODUCTS_DIR; };
		A4 = {isa = PBXGroup; children = (A2, A5); sourceTree = "<group>"; };
		A5 /* Products */ = {isa = PBXGroup; children = (A3); name = Products; sourceTree = "<group>"; };
		A6 /* Sources */ = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (A1); runOnlyForDeploymentPostprocessing = 0; };
		A7 /* Stub */ = {isa = PBXNativeTarget; buildConfigurationList = B2; buildPhases = (A6); buildRules = (); dependencies = (); name = Stub; productName = Stub; productReference = A3; productType = "com.apple.product-type.application"; };
		A8 /* Project */ = {isa = PBXProject; attributes = {LastUpgradeCheck = 2600; TargetAttributes = {A7 = {ProvisioningStyle = Automatic; }; }; }; buildConfigurationList = B1; compatibilityVersion = "Xcode 14.0"; developmentRegion = en; hasScannedForEncodings = 0; knownRegions = (en); mainGroup = A4; productRefGroup = A5; projectDirPath = ""; projectRoot = ""; targets = (A7); };
		B1 = {isa = XCConfigurationList; buildConfigurations = (C1); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
		B2 = {isa = XCConfigurationList; buildConfigurations = (C2); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
		C1 /* Debug */ = {isa = XCBuildConfiguration; buildSettings = {IPHONEOS_DEPLOYMENT_TARGET = 26.0; SDKROOT = iphoneos; }; name = Debug; };
		C2 /* Debug */ = {isa = XCBuildConfiguration; buildSettings = {CODE_SIGN_ENTITLEMENTS = Stub.entitlements; CODE_SIGN_STYLE = Automatic; DEVELOPMENT_TEAM = "@TEAM@"; GENERATE_INFOPLIST_FILE = YES; PRODUCT_BUNDLE_IDENTIFIER = "@BUNDLE_ID@"; PRODUCT_NAME = Stub; TARGETED_DEVICE_FAMILY = "1,2"; }; name = Debug; };
	};
	rootObject = A8;
}
"""


def write_stub(folder, team, bundle_id, memory_limit):
    project = Path(folder) / 'Stub.xcodeproj'
    project.mkdir(parents=True, exist_ok=True)
    (Path(folder) / 'main.m').write_text('int main(void) { return 0; }\n')
    entitlements = {MEMORY_KEY: True} if memory_limit else {}
    (Path(folder) / 'Stub.entitlements').write_bytes(plistlib.dumps(entitlements))
    (project / 'project.pbxproj').write_text(STUB_PROJECT.replace('@TEAM@', team).replace('@BUNDLE_ID@', bundle_id))
    return project


def provision(folder, team, bundle_id, device, memory_limit):
    """Build the stub for this device. Returns its app, which holds the profile and the entitlements Xcode chose."""
    project = write_stub(folder, team, bundle_id, memory_limit)
    derived = Path(folder) / 'derived'
    run(['xcodebuild', '-project', project, '-scheme', 'Stub', '-configuration', 'Debug', '-destination', f'id={device}', '-derivedDataPath', derived,
         '-allowProvisioningUpdates', '-allowProvisioningDeviceRegistration', 'build'], 'Getting a certificate and profile from Xcode', log=Path(folder) / 'xcodebuild.log')
    return derived / 'Build/Products/Debug-iphoneos/Stub.app'


def read_profile(path):
    return plistlib.loads(subprocess.run(['security', 'cms', '-D', '-i', str(path)], capture_output=True, check=True).stdout)


def provision_lasting(folder, team, bundle_id, device, memory_limit, free, profiles_dir=None):
    """Provision, and on a paid team do not keep a free account's profile. A team that went from free to paid keeps its ID, and Xcode reuses a local profile while it is valid,
    so a 7-day one made before the upgrade would be signed in again until it expires. Removing that one file (Xcode's own cache) makes Xcode ask Apple for the team's current one."""
    stub = provision(folder, team, bundle_id, device, memory_limit)
    profile = read_profile(stub / 'embedded.mobileprovision')
    if free or profile.get('TimeToLive', SHORT_PROFILE_DAYS + 1) > SHORT_PROFILE_DAYS:
        return stub, profile
    cached = Path(profiles_dir or XCODE_PROFILES) / f'{profile.get("UUID")}.mobileprovision'
    if cached.is_file():
        say(f'  replacing the {profile["TimeToLive"]}-day profile from before this team was paid')
        cached.unlink()
        shutil.rmtree(folder, ignore_errors=True)
        stub = provision(folder, team, bundle_id, device, memory_limit)
        profile = read_profile(stub / 'embedded.mobileprovision')
    if profile.get('TimeToLive', SHORT_PROFILE_DAYS + 1) <= SHORT_PROFILE_DAYS:
        say(f'[RED] Xcode still gave a {profile.get("TimeToLive")}-day profile for {bundle_id}; this app stops opening on {profile.get("ExpirationDate")} unless it is sent again')
    return stub, profile


def identity_for(profile, identities_text):
    """The keychain's signing identity for one of the profile's certificates."""
    have = set(re.findall(r'\b[0-9A-F]{40}\b', identities_text))
    for cert in profile.get('DeveloperCertificates', []):
        sha1 = hashlib.sha1(cert).hexdigest().upper()
        if sha1 in have:
            return sha1
    raise Fail('The keychain has no signing identity for this profile. Check Xcode > Settings > Apple Accounts > Manage Certificates.')


def read_entitlements(binary):
    out = subprocess.run(['codesign', '-d', '--entitlements', ':-', str(binary)], capture_output=True, check=True).stdout
    return plistlib.loads(out) if out.strip() else {}


# ----- signing the copy

def nested_code(app):
    """Frameworks, plug-ins and libraries, innermost first: each is signed before what contains it."""
    found = [p for p in Path(app).rglob('*') if p.suffix in ('.framework', '.appex', '.dylib')]
    return sorted(found, key=lambda p: len(p.parts), reverse=True)


def sign_app(app, identity, entitlements_file, profile_file=None):
    """Sign an unpacked app. The executable's old ad-hoc signature and any old bundle seal are replaced."""
    app = Path(app)
    shutil.rmtree(app / '_CodeSignature', ignore_errors=True)
    if profile_file:
        shutil.copyfile(profile_file, app / 'embedded.mobileprovision')
    run(['xattr', '-cr', app], 'Clearing extended attributes')  # codesign refuses resource forks, Finder information and quarantine
    for code in nested_code(app):
        run(['codesign', '--force', '--sign', identity, '--timestamp=none', code], f'Signing {code.name}')
    run(['codesign', '--force', '--sign', identity, '--timestamp=none', '--entitlements', entitlements_file, app], 'Signing the app')
    run(['codesign', '--verify', '--deep', '--strict', app], 'Checking the signature')


def resign(ipa, work, team, device, bundle_id, memory_limit, install, free=False):
    ipa = Path(ipa)
    top, info, size = read_ipa(ipa)
    title = bundle_id.rsplit('.', 1)[-1]
    folder = Path(work) / title
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)

    say('  getting the certificate and profile from Xcode')
    stub, profile = provision_lasting(folder / 'stub', team, bundle_id, device, memory_limit, free)
    profile_file = stub / 'embedded.mobileprovision'
    entitlements = read_entitlements(stub)
    if memory_limit and not entitlements.get(MEMORY_KEY):
        raise Fail('Xcode signed without the Increased Memory Limit. Pass --no-memory-limit only for a game that fits in about 3.3 GB.')
    entitlements_file = folder / 'app.entitlements'
    entitlements_file.write_bytes(plistlib.dumps(entitlements))
    identities = run(['security', 'find-identity', '-v', '-p', 'codesigning'], 'Listing signing identities')
    identity = identity_for(profile, identities)

    say(f'  unpacking {size / 1e9:.1f} GB')
    unpacked = folder / 'unpacked'
    run(['unzip', '-q', '-o', ipa, '-d', unpacked], 'Unpacking the IPA')
    app = unpacked / top
    set_bundle_id(app / 'Info.plist', bundle_id)

    say('  signing')
    sign_app(app, identity, entitlements_file, profile_file)
    got = read_entitlements(app)
    if memory_limit and not got.get(MEMORY_KEY):
        raise Fail('The signed app does not carry the Increased Memory Limit.')

    if not install:
        say(f'  signed, not installed: {app}')
        return
    say('  installing on the device')
    run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', device, app], 'Installing the app')
    shutil.rmtree(folder, ignore_errors=True)
    say(f'  installed as {bundle_id}; profile expires {profile.get("ExpirationDate", "unknown")}')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Sign IPAs with your own Apple account through Xcode and install them.')
    ap.add_argument('ipa', nargs='+', help='the IPA files (the files themselves are not changed)')
    ap.add_argument('--team', help='Apple team ID (default: the only team in Xcode)')
    ap.add_argument('--device', help='device UDID (default: the only paired device connected)')
    ap.add_argument('--bundle-id', help='bundle ID, for one IPA (default: org.nxnative.<team>.<title ID>); another ID is a separate app with a separate save')
    ap.add_argument('--work-dir', help='where to unpack (default: resign_work beside each IPA); needs room for the unpacked game')
    ap.add_argument('--no-install', action='store_true', help='sign only, and leave the signed app in the work folder')
    ap.add_argument('--no-memory-limit', action='store_true', help='do not ask for the Increased Memory Limit (about 3.3 GB for the game)')
    ap.add_argument('--ignore-app-limit', action='store_true', help=f'try more than a free Apple ID\'s {FREE_TEAM_APPS} apps anyway')
    ap.add_argument('--dry-run', action='store_true', help='check everything and print the plan; contact no Apple service, sign and install nothing')
    args = ap.parse_args(argv)

    try:
        if args.bundle_id and len(args.ipa) > 1:
            raise Fail('--bundle-id is for one IPA at a time.')
        team = pick_team(args.team)
        free = bool(team.get('isFreeProvisioningTeam'))
        if free and len(args.ipa) > FREE_TEAM_APPS and not args.ignore_app_limit:
            raise Fail(f'A free Apple ID can keep {FREE_TEAM_APPS} signed apps installed at one time, and you gave {len(args.ipa)}. '
                       'Choose fewer, delete another signed app first if you need the room, or pass --ignore-app-limit.')
        device = pick_device(args.device)
        say(f'Team {team["teamID"]} {team.get("teamName", "")}; device {device}')

        plan = []
        for name in args.ipa:
            ipa = Path(name).expanduser().resolve()
            if not ipa.is_file():
                raise Fail(f'{ipa}: no such file')
            _, info, size = read_ipa(ipa)
            old_id = info.get('CFBundleIdentifier')
            if not old_id:
                raise Fail(f'{ipa}: Info.plist has no bundle ID')
            bundle_id = args.bundle_id or new_bundle_id(old_id, team['teamID'])
            work = Path(args.work_dir).expanduser().resolve() if args.work_dir else ipa.parent / 'resign_work'
            anchor = next(p for p in [work, *work.parents] if p.exists())
            free_space = shutil.disk_usage(anchor).free
            if free_space < size + SPACE_MARGIN:
                raise Fail(f'{ipa.name} needs {(size + SPACE_MARGIN) / 1e9:.1f} GB in {anchor}, which has {free_space / 1e9:.1f} GB free.')
            plan.append((ipa, work, bundle_id))
            say(f'{ipa.name}: {size / 1e9:.1f} GB, {old_id} -> {bundle_id}, unpacks in {work}')
        if args.dry_run:
            say('Dry run: nothing was signed or installed.')
            return 0

        for index, (ipa, work, bundle_id) in enumerate(plan, 1):
            say(f'[{index}/{len(plan)}] {ipa.name}')
            resign(ipa, work, team['teamID'], device, bundle_id, not args.no_memory_limit, not args.no_install, free)
            if not args.no_install and not any(work.iterdir()):
                work.rmdir()
    except Fail as e:
        print(f'[RED] {e}', file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:
        print(f'[RED] {" ".join(map(str, e.cmd))} failed: {(e.stderr or b"").decode(errors="replace").strip()}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130

    say()
    say('On the iPhone, the first time: Settings > General > VPN & Device Management, trust your Apple ID.')
    if not args.no_memory_limit:
        say('To confirm the memory limit, play for a few hundred frames, then read "memory up to frame" in the app\'s Documents/log.txt: resident + headroom should be well above 3,375 MB.')
    if free:
        say('A free Apple ID\'s apps stop opening after 7 days. Run this again with the same IPA to renew; the save stays. Deleting the app deletes the save.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
