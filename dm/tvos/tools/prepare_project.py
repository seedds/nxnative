#!/usr/bin/env python3
"""Prepare an independent tvOS Xcode project from existing iOS host inputs."""
import argparse
from pathlib import Path
import shutil
import subprocess
import re


def replace_symlink(path: Path, target: Path, *, directory: bool = False) -> None:
    """Replace regular, directory, valid, or broken prior output uniformly."""
    if path.is_symlink() or path.exists():
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        else:
            path.unlink()
    path.symlink_to(target, target_is_directory=directory)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ios-project', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--team', required=True)
    p.add_argument('--bundle-id', default='org.nxnative.0100f43008c44000.tv')
    p.add_argument('--shader-archive-sha256', required=True)
    p.add_argument('--build-number', default='1')
    a = p.parse_args()
    if not re.fullmatch('[0-9a-f]{64}', a.shader_archive_sha256):
        p.error('--shader-archive-sha256 must be a lowercase SHA-256 digest')
    tv = Path(__file__).resolve().parents[1]
    out = a.output.resolve()
    if out == a.ios_project.resolve():
        p.error('--output must differ from the prepared iOS project')
    out.mkdir(parents=True, exist_ok=True)
    gen = out / 'Gen'; gen.mkdir(exist_ok=True)
    iosgen = a.ios_project.resolve() / 'Gen'
    for n in ('src', 'main', 'dm'):
        dest = gen / n
        # These are generated inputs. Refresh them together: retaining old
        # fixup tables while embed.S points at a newer module mixes layouts.
        replacement = gen / (n + '.incoming')
        if replacement.is_symlink():
            replacement.unlink()
        elif replacement.exists():
            shutil.rmtree(replacement)
        shutil.copytree(iosgen / n, replacement, symlinks=True)
        if n == 'dm':
            # ios.env is a platform-specific production profile. A prepared
            # tvOS project must use its own writable env.txt or frontend
            # defaults, never silently inherit iPad refresh/timing settings.
            (replacement / 'env.txt').unlink(missing_ok=True)
        if dest.is_symlink():
            dest.unlink()
        elif dest.exists():
            shutil.rmtree(dest)
        replacement.rename(dest)
    # Older prepared iOS projects have a frozen source-file list. The runtime
    # BC-to-ASTC implementation may have been added to the checkout after that
    # preparation, so add exactly that shared source when it is absent. Fresh
    # iOS preparations already contain it through the host Makefile wildcard.
    bc2astc = tv.parent / 'gfx' / 'tex' / 'nvn_bc2astc.c'
    bc2astc_link = gen / 'src' / 'gfx__tex__nvn_bc2astc.c'
    if bc2astc.is_file():
        replace_symlink(bc2astc_link, bc2astc)
    # Let ld own segment addresses, fixup indices and signing metadata. The
    # guest's own PC-relative references keep their original module offsets.
    original = (iosgen / 'embed.S').read_text()
    bins = {Path(s).name.removesuffix('.patched.bin'): Path(s)
            for s in re.findall(r'\.incbin "([^"]+)"', original)}
    asm, flags, address = [], [], 0x104000000
    for mod in ('main', 'sdk'):
        hdr = dict(line[2:].split() for line in
                   (gen / 'dm' / f'{mod}.fixups.tsv').read_text().splitlines()
                   if line.startswith('# '))
        binary = bins[mod]
        start = (int(hdr['TEXT_END'], 16) + 0x3fff) & ~0x3fff
        patch = int(hdr['PATCH_OFF'], 16)
        size = binary.stat().st_size; end = (size + 0x3fff) & ~0x3fff
        if patch <= start or patch % 0x4000:
            raise ValueError('tvOS static layout requires positive aligned PATCH_OFF')
        for seg, section, lo, hi, prot in (
            (f'__DM{mod}0', f'__dm{mod}x0', 0, start, 'rx'),
            (f'__DM{mod}D', '__data', start, patch, 'rw'),
            (f'__DM{mod}1', f'__dm{mod}x1', patch, end, 'rx')):
            attrs = ',regular,pure_instructions' if prot == 'rx' else ',regular'
            fill = min(hi, size) - lo
            asm.extend([f'.section {seg},{section}{attrs}', '.p2align 14',
                        f'.incbin "{binary}", {lo:#x}, {fill:#x}'])
            if hi - lo > fill:
                asm.append(f'.space {hi - lo - fill:#x}')
            flags.extend([f'-Wl,-segprot,{seg},{prot},{prot}',
                          f'-Wl,-segaddr,{seg},{address:#x}'])
            address += hi - lo
        address += 0x4000
    (gen / 'embed.S').write_text('\n'.join(asm) + '\n')
    config = []
    for line in (iosgen / 'dm.xcconfig').read_text().splitlines():
        if line.startswith(('CODE_SIGN_ENTITLEMENTS', 'DM_XCODE_OUTPUT_ROOT', 'OTHER_LDFLAGS', 'DM_APPICON')):
            continue
        config.append(line)
    config.append('OTHER_LDFLAGS = $(inherited) $(DM_FRAMEWORKS) -lz ' + ' '.join(flags))
    (gen / 'dm-tv.xcconfig').write_text('\n'.join(config) + '\n')
    from app_icon import create as create_icon
    icon = iosgen / 'Assets.xcassets/AppIcon.appiconset/icon.png'
    if not icon.is_file():
        raise ValueError('Prepared iOS icon artwork is missing')
    create_icon(icon, gen / 'TVAssets.xcassets')
    sources = out / 'Sources'
    replace_symlink(sources, tv / 'Sources', directory=True)
    (out / 'project.yml').write_text(f'''name: DmTV
configFiles:
  Release: Gen/dm-tv.xcconfig
  Debug: Gen/dm-tv.xcconfig
targets:
  DmTV:
    type: application
    platform: tvOS
    deploymentTarget: '17.0'
    sources:
      - path: Sources
      - path: Gen/src
      - path: Gen/main/main.c
        compilerFlags: -Dmain=dm_main
      - path: Gen/TVAssets.xcassets
      - path: Gen/embed.S
      - path: Gen/dm
        type: folder
        buildPhase: resources
    info:
      path: Gen/TV-Info.plist
      properties:
        CFBundleDisplayName: Pokémon Legends Z-A
        CFBundleShortVersionString: $(MARKETING_VERSION)
        CFBundleVersion: $(CURRENT_PROJECT_VERSION)
        DMLanguage: zh-CN
        DMShaderArchiveSHA256: '{a.shader_archive_sha256}'
        GCSupportsControllerUserInteraction: true
        GCSupportedGameControllers:
          - ProfileName: ExtendedGamepad
          - ProfileName: MicroGamepad
        UIApplicationSceneManifest:
          UIApplicationSupportsMultipleScenes: false
          UISceneConfigurations:
            UIWindowSceneSessionRoleApplication:
              - UISceneConfigurationName: Default
                UISceneDelegateClassName: TVSceneDelegate
    settings:
      PRODUCT_BUNDLE_IDENTIFIER: {a.bundle_id}
      PRODUCT_NAME: DmTV
      MARKETING_VERSION: '2.0.2'
      CURRENT_PROJECT_VERSION: '{a.build_number}'
      ASSETCATALOG_COMPILER_APPICON_NAME: App Icon & Top Shelf Image
      DEVELOPMENT_TEAM: {a.team}
      CODE_SIGN_STYLE: Automatic
      ENABLE_USER_SCRIPT_SANDBOXING: NO
      CLANG_ENABLE_OBJC_ARC: YES
      GCC_C_LANGUAGE_STANDARD: gnu11
      OTHER_CFLAGS: $(inherited) -mcpu=apple-a15
      GCC_WARN_INHIBIT_ALL_WARNINGS: YES
      ENABLE_BITCODE: NO
      DEBUG_INFORMATION_FORMAT: dwarf
schemes:
  DmTV:
    build:
      targets:
        DmTV: all
    run:
      config: Release
''')
    subprocess.run(['xcodegen', 'generate', '--spec', str(out / 'project.yml')], check=True, cwd=out)
    print(out / 'DmTV.xcodeproj')


if __name__ == '__main__':
    main()
