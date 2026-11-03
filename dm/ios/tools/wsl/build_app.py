#!/usr/bin/env python3
# The iOS app without Xcode (WSL builds; build_ipa.sh runs it where xcodebuild would run): the DmIOS target of dm/ios/project.yml compiled with the Swift toolchain's clang and swiftc
# against the iOS SDK xtool made from Xcode.xip, linked with that SDK's ld64.lld, and laid out as Xcode lays out DmIOS.app (executable, Info.plist, PkgInfo, the Gen/dm folder, icons).
#   Sources: dm/ios/Sources (Objective-C and Swift, with the bridging header), Gen/embed.S, Gen/src (the host's sources, linked there by gen_build.py), Gen/main/main.c (-Dmain=dm_main)
#   Settings: Gen/dm.xcconfig (header paths, defines, frameworks and the embedded modules' link flags, as Xcode reads it) and project.yml's target settings, with Xcode's Release
#   defaults that the project relies on (-Os, modules with autolinking, no common blocks, NS_BLOCK_ASSERTIONS)
# Usage: build_app.py <dm/ios dir> <output DmIOS.app>   (Gen/ and Gen/Info.plist made by build_ipa.sh; objects go to <output>.obj/)
import concurrent.futures, glob, os, re, shlex, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); import app_plist

ios, app = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
gen, obj = os.path.join(ios, 'Gen'), app + '.obj'
project, settings = app_plist.target(os.path.join(ios, 'project.yml'))
st = settings.get('settings', {})
xcrun = os.path.join(HERE, 'xcrun')
sdk = subprocess.run([xcrun, '-sdk', 'iphoneos', '--show-sdk-path'], capture_output=True, text=True, check=True).stdout.strip()
bundle = os.environ.get('DM_XTOOL_SDK') or os.path.expanduser('~/.swiftpm/swift-sdks/darwin.artifactbundle')
swiftc = shutil.which('swiftc') or sys.exit('[RED] swiftc not found (the Swift 6.4 toolchain)')
tc = os.path.dirname(os.path.realpath(swiftc)); clang = os.path.join(tc, 'clang')
res = os.path.join(bundle, 'Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift')
ld = os.path.join(bundle, 'toolset/bin/ld64.lld')
# compiler-rt for iOS (__isPlatformVersionAtLeast, which @available checks call): Xcode's clang links it from its own resource folder, the Swift toolchain's clang has none
rt = sorted(glob.glob(os.path.join(bundle, 'Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/*/lib/darwin/libclang_rt.ios.a')))
for p in (clang, res, ld):
    if not os.path.exists(p): sys.exit(f'[RED] {p} not found (Swift toolchain / xtool sdk install)')
if not rt: sys.exit(f'[RED] libclang_rt.ios.a not found in {bundle} (xtool sdk install)')
osmin = str(project['options']['deploymentTarget']['iOS'])
triple = f'arm64-apple-ios{osmin}'

def xcconfig(path):   # KEY = value lines; $(inherited) is empty here and $(KEY) refers to an earlier key
    out = {}
    for line in open(path):
        m = re.match(r'\s*(\w+)\s*=\s*(.*)$', line)
        if m: out[m[1]] = re.sub(r'\$\((\w+)\)', lambda r: out.get(r[1], ''), m[2]).strip()
    return out
xc = xcconfig(os.path.join(gen, 'dm.xcconfig'))
incs = [f'-I{d}' for d in shlex.split(xc.get('HEADER_SEARCH_PATHS', ''))]
defs = shlex.split(xc.get('OTHER_CFLAGS', ''))
ldflags = shlex.split(xc.get('OTHER_LDFLAGS', ''))
cflags = shlex.split(str(st.get('OTHER_CFLAGS', '')).replace('$(inherited)', ''))   # -mcpu=apple-a18 (LSE atomics in the host's assembly)
std = '-std=' + str(st.get('GCC_C_LANGUAGE_STANDARD', 'gnu11'))
base = ['-target', triple, '-isysroot', sdk, *cflags, '-g', '-w' if st.get('GCC_WARN_INHIBIT_ALL_WARNINGS') in (True, 'YES') else '-Wall']
mc = os.path.join(obj, 'ModuleCache')
lang = {'.c': [std, '-Os', '-fno-common', '-fmodules', f'-fmodules-cache-path={mc}', '-DNS_BLOCK_ASSERTIONS=1'],
        '.m': [std, '-Os', '-fno-common', '-fmodules', f'-fmodules-cache-path={mc}', '-DNS_BLOCK_ASSERTIONS=1', '-fobjc-arc'],
        '.S': []}

srcs = []   # (source, extra flags)
for d in (os.path.join(ios, 'Sources'), os.path.join(gen, 'src')):
    srcs += [(os.path.join(d, n), []) for n in sorted(os.listdir(d)) if os.path.splitext(n)[1] in lang]
srcs += [(os.path.join(gen, 'embed.S'), []), (os.path.join(gen, 'main/main.c'), ['-Dmain=dm_main'])]
swift = sorted(os.path.join(ios, 'Sources', n) for n in os.listdir(os.path.join(ios, 'Sources')) if n.endswith('.swift'))

shutil.rmtree(obj, ignore_errors=True); os.makedirs(mc)
def compile_one(i, src, extra):
    o = os.path.join(obj, f'{i:04d}_{os.path.basename(src)}.o')
    cmd = [clang, *base, *lang[os.path.splitext(src)[1]], *incs, *defs, *extra, '-c', src, '-o', o]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return o, r.returncode, r.stderr, cmd
def compile_swift():
    o = os.path.join(obj, 'swift.o')
    bridging = os.path.join(ios, st.get('SWIFT_OBJC_BRIDGING_HEADER', 'Sources/DmIOS-Bridging-Header.h'))
    cmd = [swiftc, '-target', triple, '-sdk', sdk, '-resource-dir', res, '-swift-version', str(st.get('SWIFT_VERSION', '5')).split('.')[0], '-O', '-wmo',
           '-parse-as-library', '-module-name', st.get('PRODUCT_NAME', 'DmIOS'), '-module-cache-path', mc,
           '-import-objc-header', bridging, *[a for i in incs for a in ('-Xcc', i)], '-c', *swift, '-o', o]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return o, r.returncode, r.stderr, cmd

print(f'compile {len(srcs)} C / Objective-C / assembly files and {len(swift)} Swift files ({triple}, {os.path.basename(sdk)})', flush=True)
objs, failed = [], []
with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
    jobs = [pool.submit(compile_one, i, s, e) for i, (s, e) in enumerate(srcs)]; swift_job = pool.submit(compile_swift)
    for j in concurrent.futures.as_completed(jobs): print(f'CompileC {j.result()[3][-3]}', flush=True)   # a line per file as it is done, as in xcodebuild's log: the NSP script counts them
    for j in jobs + [swift_job]:   # in the sources' order, which the link keeps
        o, rc, err, cmd = j.result()
        if rc: failed.append((cmd, err))
        else: objs.append(o)
for cmd, err in failed[:10]: print(f'[RED] {" ".join(shlex.quote(c) for c in cmd[-3:])}\n{err.strip()[-2000:]}', file=sys.stderr)
if failed: sys.exit(f'[RED] {len(failed)} files did not compile')

shutil.rmtree(app, ignore_errors=True); os.makedirs(app)
exe = os.path.join(app, st.get('PRODUCT_NAME', 'DmIOS'))
link = [clang, '-target', triple, '-isysroot', sdk, f'-fuse-ld={ld}', '-mlinker-version=1100', '-L', os.path.join(sdk, 'usr/lib/swift'), '-L', os.path.join(res, 'iphoneos'),
        *objs, *ldflags, '-framework', 'UIKit', '-framework', 'Foundation', rt[-1], '-Xlinker', '-rpath', '-Xlinker', '@executable_path/Frameworks', '-o', exe]
print(f'Ld {exe} ({len(objs)} objects)', flush=True)
r = subprocess.run(link, capture_output=True, text=True)
# LLD's notice for every non-macOS Mach-O platform, and the auto-link requests for frameworks the SDK has no stub of (UIUtilities, CoreAudioTypes), which Apple's ld64 skips silently
warn = [l for l in r.stderr.splitlines() if 'does not support linking for platform iOS' not in l and 'auto-linked framework not found' not in l]
if r.returncode: sys.exit('[RED] link failed:\n' + '\n'.join(warn[-40:]))
for l in warn[:20]: print(l, file=sys.stderr)

# The bundle: Info.plist (build_ipa.sh made Gen/Info.plist), PkgInfo, the Gen/dm folder (a folder reference in project.yml), the icons from the asset catalog's 1024 px PNG
shutil.copy(os.path.join(gen, 'Info.plist'), os.path.join(app, 'Info.plist'))
open(os.path.join(app, 'PkgInfo'), 'w').write('APPL????')
shutil.copytree(os.path.join(gen, 'dm'), os.path.join(app, 'dm'), symlinks=False)
icon = os.path.join(gen, 'Assets.xcassets/AppIcon.appiconset/icon.png')
if os.path.exists(icon):
    from PIL import Image   # pyright: ignore[reportMissingImports]  # Pillow: a WSL build's dependency, not the lint environment's
    img = Image.open(icon).convert('RGB')   # iOS icons have no alpha
    for name, px in (('AppIcon60x60@2x.png', 120), ('AppIcon60x60@3x.png', 180), ('AppIcon76x76@2x~ipad.png', 152), ('AppIcon83.5x83.5@2x~ipad.png', 167)):
        img.resize((px, px), Image.LANCZOS).save(os.path.join(app, name))
print(f'app: {app}', flush=True)
