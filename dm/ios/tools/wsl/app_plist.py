#!/usr/bin/env python3
# The app's Info.plist without Xcode (WSL builds, build_ipa.sh): what xcodegen writes from dm/ios/project.yml and Xcode completes when it builds, and the PlistBuddy / plutil edits
# build_ipa.sh makes afterwards.
#   app_plist.py make <project.yml> <Info.plist> <SDK path> [--icon]   the target's info.properties with ${ENV} (xcodegen) and $(SETTING) (Xcode: its settings in project.yml) expanded,
#       over the keys Xcode gives every iOS app (bundle identity, platform, minimum OS, device family, the SDK it was built with); --icon adds the loose icon files build_app.py writes
#   app_plist.py add <Info.plist> <key> string|bool <value>             PlistBuddy -c "Add :key type value"
#   app_plist.py get <Info.plist> <key>                                 PlistBuddy -c "Print :key"
#   app_plist.py json <plist>                                           plutil -convert json -o - (the entitlements line in the log)
import json, os, plistlib, re, sys

def target(project):
    import yaml   # pyright: ignore[reportMissingModuleSource]  # PyYAML (Arch: python-yaml), which only this command needs: a WSL build's dependency, not the lint environment's
    with open(project) as f: p = yaml.safe_load(f)
    return p, p['targets']['DmIOS']

def expand(v, settings):
    if isinstance(v, str):
        v = re.sub(r'\$\{(\w+)\}', lambda m: os.environ.get(m[1], ''), v)   # xcodegen: the environment, when it generates the project
        return re.sub(r'\$\((\w+)\)', lambda m: str(expand(settings.get(m[1], ''), settings)), v)   # Xcode: the target's build settings
    if isinstance(v, list): return [expand(x, settings) for x in v]
    if isinstance(v, dict): return {k: expand(x, settings) for k, x in v.items()}
    return v

def make(project, out, sdk, icon):
    p, t = target(project)
    settings = dict(t.get('settings', {}))
    sdk_info = json.load(open(os.path.join(sdk, 'SDKSettings.json')))
    build = plistlib.load(open(os.path.join(sdk, 'System/Library/CoreServices/SystemVersion.plist'), 'rb'))['ProductBuildVersion']
    name = expand(settings.get('PRODUCT_NAME', 'DmIOS'), settings)
    info: dict = {'CFBundleDevelopmentRegion': 'en', 'CFBundleExecutable': name, 'CFBundleIdentifier': expand(settings['PRODUCT_BUNDLE_IDENTIFIER'], settings),
            'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleName': name, 'CFBundlePackageType': 'APPL', 'CFBundleSupportedPlatforms': ['iPhoneOS'],
            'LSRequiresIPhoneOS': True, 'MinimumOSVersion': str(p['options']['deploymentTarget']['iOS']),
            'UIDeviceFamily': [int(x) for x in str(settings.get('TARGETED_DEVICE_FAMILY', '1,2')).split(',')], 'UIRequiredDeviceCapabilities': ['arm64'],
            'DTPlatformName': 'iphoneos', 'DTPlatformVersion': sdk_info['Version'], 'DTSDKName': sdk_info['CanonicalName'], 'DTSDKBuild': build}
    info.update({k: expand(v, settings) for k, v in t['info'].get('properties', {}).items()})
    if icon:   # the PNGs build_app.py makes from the asset catalog's 1024 px icon (actool's job on a Mac)
        info['CFBundleIcons'] = {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60']}}
        info['CFBundleIcons~ipad'] = {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60', 'AppIcon76x76', 'AppIcon83.5x83.5']}}
    with open(out, 'wb') as f: plistlib.dump(info, f)

def load(path):
    with open(path, 'rb') as f: return plistlib.load(f)

if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['make'] and len(a) in (4, 5): make(a[1], a[2], a[3], a[4:] == ['--icon'])
    elif a[:1] == ['add'] and len(a) == 5:
        d = load(a[1])
        if a[2] in d: sys.exit(f'{a[1]}: {a[2]} already exists')   # as PlistBuddy's Add
        d[a[2]] = {'string': lambda v: v, 'bool': lambda v: v.lower() in ('true', 'yes', '1')}[a[3]](a[4])
        with open(a[1], 'wb') as f: plistlib.dump(d, f)
    elif a[:1] == ['get'] and len(a) == 3: print(load(a[1])[a[2]])
    elif a[:1] == ['json'] and len(a) == 2: print(json.dumps(load(a[1]), separators=(',', ':')))
    else: sys.exit(__doc__ or 'usage: see the comment at the top of app_plist.py')
