#!/usr/bin/env python3
"""Cross-compile host/iOS translation units for arm64 iOS 16.1, without private game inputs.

Supports explicit unit filtering, zero-match detection (fails with code 2),
and automated multi-profile cross-compilation across all 7 game profiles.
"""
from pathlib import Path
import subprocess, json, concurrent.futures, sys, os

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'dm'
BUILD_DIR = ROOT / 'build/compile'
BUILD_DIR.mkdir(parents=True, exist_ok=True)

sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()

# All directory includes inside dm/ excluding git/build
incs = [
    str(p) for p in DM.rglob('*')
    if p.is_dir() and '.git' not in p.parts and ('Gen' not in p.parts) and p.name not in ('gen', 'bin', 'obj')
]

# Discover all 7 registered game profiles
profiles = sorted(p.parent.name for p in (DM / 'games').glob('*/game.tsv'))
if not profiles:
    sys.stderr.write("ERROR: No game profiles found in dm/games/*\n")
    sys.exit(2)

# Full repository source inventory across all relevant subdirectories
ALL_SOURCES = sorted(list(dict.fromkeys(
    list((DM / 'ios/Sources').glob('*.[mc]')) +
    list((DM / 'gfx/nvn').glob('*.[mc]')) +
    list((DM / 'gfx/tex').glob('*.[mc]')) +
    list((DM / 'host').glob('*.[mc]'))
)))
ALL_SOURCES = [p for p in ALL_SOURCES if p.name != 'front_mac.m']

# Check Gen/src if present (from local xcode gen)
gen_src = DM / 'ios/Gen/src'
if gen_src.exists():
    ALL_SOURCES = sorted(list(dict.fromkeys(ALL_SOURCES + list(gen_src.glob('*.[mc]')))))

def resolve_target_sources(argv):
    """Resolve sources from command line arguments with strict non-zero exit on zero matches."""
    if not argv:
        # Default target set: the 3 modified core units
        default_names = ['front_ios.m', 'nvn_mtl.m', 'nvn_pipeline_cache.m']
        resolved = []
        for name in default_names:
            hits = [p for p in ALL_SOURCES if p.name == name]
            if not hits:
                sys.stderr.write(f"ERROR: Default target '{name}' not found in source tree!\n")
                sys.exit(2)
            resolved.extend(hits)
        return resolved

    if '--all-sources' in argv:
        return ALL_SOURCES

    targets = [a for a in argv if not a.startswith('--')]
    if not targets:
        return resolve_target_sources([])

    matched = []
    for target in targets:
        hits = [p for p in ALL_SOURCES if target in p.name or target in str(p.relative_to(DM))]
        if not hits:
            sys.stderr.write(f"ERROR: Requested compile target '{target}' matched 0 source files in repository!\n")
            sys.exit(2)
        matched.extend(hits)

    return list(dict.fromkeys(matched))

def compile_profile(selected_profile, sources):
    out_dir = BUILD_DIR / 'profiles' / selected_profile
    out_dir.mkdir(parents=True, exist_ok=True)

    header_path = out_dir / 'game_profile.h'
    subprocess.run(['python3', str(DM / 'games/game.py'), 'header', selected_profile, str(header_path)], check=True)

    base = [
        'xcrun', '--sdk', 'iphoneos', 'clang',
        '-target', 'arm64-apple-ios16.1',
        '-isysroot', sdk,
        '-mcpu=apple-m2',
        '-O1',
        '-Werror=unguarded-availability',
        '-Werror=unguarded-availability-new',
        '-Wno-deprecated-declarations',
        '-I' + str(out_dir),
        '-I' + str(DM / 'games' / selected_profile),
        '-I' + str(DM / 'third_party/libvpx/config')
    ] + ['-I' + i for i in incs]

    def run_one(p):
        name = str(p.relative_to(DM)).replace('/', '__')
        obj_file = out_dir / (name + '.o')
        log_file = out_dir / (name + '.log')
        cmd = base + (['-fobjc-arc'] if p.suffix == '.m' else []) + (['-Dmain=dm_main'] if p == DM / 'host/main.c' else []) + ['-c', str(p), '-o', str(obj_file)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        log_file.write_text(r.stdout + r.stderr)
        return {
            'source': str(p.relative_to(DM)),
            'exit': r.returncode,
            'log': str(log_file)
        }

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(run_one, sources))

    report = {
        'profile': selected_profile,
        'translation_units': len(results),
        'failures': sum(r['exit'] != 0 for r in results),
        'results': results
    }
    (out_dir / 'report.json').write_text(json.dumps(report, indent=2))
    return report

def main():
    run_all_profiles = '--all-profiles' in sys.argv or 'NX_COMPILE_ALL_PROFILES' in os.environ
    filtered_argv = [a for a in sys.argv[1:] if a != '--all-profiles']

    sources = resolve_target_sources(filtered_argv)
    if not sources:
        sys.stderr.write("ERROR: Resolved source list is empty!\n")
        sys.exit(2)

    target_profiles = profiles if run_all_profiles else [os.environ.get('NX_COMPILE_GAME', profiles[0])]

    all_reports = []
    total_failures = 0
    total_units = 0

    print(f"Cross-compiling {len(sources)} unit(s) across {len(target_profiles)} profile(s):")
    for s in sources:
        print(f"  - {s.relative_to(DM)}")

    for p in target_profiles:
        rep = compile_profile(p, sources)
        all_reports.append(rep)
        total_units += rep['translation_units']
        total_failures += rep['failures']
        status_str = "PASS" if rep['failures'] == 0 else f"FAIL ({rep['failures']} errors)"
        print(f"Profile {p}: {rep['translation_units']} units -> {status_str}")

    summary = {
        'scope': 'All actual iOS/host sources cross-compiled for arm64 iOS17, not linked or device-run',
        'profiles_evaluated': target_profiles,
        'total_units_compiled': total_units,
        'total_failures': total_failures,
        'reports': all_reports
    }
    (BUILD_DIR / 'summary.json').write_text(json.dumps(summary, indent=2))

    print(f"SUMMARY: {total_units} total unit compilations; {total_failures} failures.")
    if total_failures != 0:
        for rep in all_reports:
            for r in rep['results']:
                if r['exit'] != 0:
                    print(f"FAILED {rep['profile']} {r['source']}:\n{Path(r['log']).read_text()[-1000:]}")
        sys.exit(1)

    sys.exit(0)

if __name__ == '__main__':
    main()
