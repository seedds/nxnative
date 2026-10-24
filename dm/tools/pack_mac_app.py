#!/usr/bin/env python3
"""Pack a native macOS .app bundle for an nxnative game.

Supports:
  --mode thin: Symlinks large assets (RomFS, shader libraries) so the .app is lightweight (~50MB) and fast to generate for local play.
  --mode standalone: Copies everything for a completely self-contained .app bundle.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO))
from dm.tools import nacp
from dm.games import game as game_mod
from dm.games import settings as settings_mod


def make_icns(icon_jpg, out_icns):
    """Generate high-resolution multi-size .icns using sips and iconutil."""
    icon_jpg = Path(icon_jpg)
    out_icns = Path(out_icns)
    if not icon_jpg.is_file():
        return False
    tmp = Path(tempfile.mkdtemp(prefix='nxnative_icns_'))
    try:
        iconset = tmp / 'AppIcon.iconset'
        iconset.mkdir()
        for sz in [16, 32, 128, 256, 512]:
            subprocess.run(['sips', '-s', 'format', 'png', '-z', str(sz), str(sz), str(icon_jpg),
                            '--out', str(iconset / f'icon_{sz}x{sz}.png')],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(['sips', '-s', 'format', 'png', '-z', str(sz * 2), str(sz * 2), str(icon_jpg),
                            '--out', str(iconset / f'icon_{sz}x{sz}@2x.png')],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out_icns.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(out_icns)], check=True)
        return True
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def build_app_bundle(game_id, work_dir, host_bin, patched_dir, romfs_dir, msl_dir, font_dir,
                     shmap_path, control_nacp, icon_jpg, out_app, mode='thin', aoc_dir=None):
    work_dir = Path(work_dir).resolve()
    host_bin = Path(host_bin).resolve()
    patched_dir = Path(patched_dir).resolve()
    romfs_dir = Path(romfs_dir).resolve() if romfs_dir else None
    msl_dir = Path(msl_dir).resolve() if msl_dir else None
    font_dir = Path(font_dir).resolve() if font_dir else None
    shmap_path = Path(shmap_path).resolve() if shmap_path else None
    control_nacp = Path(control_nacp).resolve() if control_nacp else None
    out_app = Path(out_app).resolve()

    app_name = None
    if control_nacp and Path(control_nacp).is_file():
        try:
            app_name = nacp.name(control_nacp)
        except Exception:
            app_name = None
    if not app_name:
        try:
            g = game_mod.by_title(game_id)
            app_name = g.get('name') if g else None
        except Exception:
            app_name = None
    if not app_name:
        app_name = 'Game'

    version = '1.0.0'
    if control_nacp and Path(control_nacp).is_file():
        try:
            version = nacp.version(control_nacp) or '1.0.0'
        except Exception:
            version = '1.0.0'
    bundle_id = f"org.nxnative.{game_id.lower()}"

    if out_app.exists():
        if out_app.is_dir():
            shutil.rmtree(out_app)
        else:
            out_app.unlink()

    contents = out_app / 'Contents'
    macos_dir = contents / 'MacOS'
    res_dir = contents / 'Resources'
    dm_res_dir = res_dir / 'dm'

    macos_dir.mkdir(parents=True, exist_ok=True)
    res_dir.mkdir(parents=True, exist_ok=True)
    dm_res_dir.mkdir(parents=True, exist_ok=True)

    dest_bin = macos_dir / app_name
    shutil.copy2(host_bin, dest_bin)
    dest_bin.chmod(0o755)

    icon_dest = res_dir / 'AppIcon.icns'
    has_icon = False
    if icon_jpg and Path(icon_jpg).is_file():
        has_icon = make_icns(icon_jpg, icon_dest)

    (contents / 'PkgInfo').write_bytes(b'APPL????')

    info_dict = {
        'CFBundleDevelopmentRegion': 'en',
        'CFBundleDisplayName': app_name,
        'CFBundleExecutable': app_name,
        'CFBundleIdentifier': bundle_id,
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': app_name,
        'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': version,
        'CFBundleVersion': version,
        'LSApplicationCategoryType': 'public.app-category.games',
        'LSMinimumSystemVersion': '14.0',
        'LSSupportsGameMode': True,
        'NSHighResolutionCapable': True,
        'NSPrincipalClass': 'NSApplication',
        'GCSupportsControllerUserInteraction': True,
        'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}],
    }
    if has_icon:
        info_dict['CFBundleIconFile'] = 'AppIcon'

    with (contents / 'Info.plist').open('wb') as f:
        plistlib.dump(info_dict, f)

    # Patched binaries and tables
    for f in patched_dir.iterdir():
        if f.is_file():
            dest = dm_res_dir / f.name
            if mode == 'thin':
                dest.symlink_to(f)
            else:
                shutil.copy2(f, dest)

    dest_shmap = dm_res_dir / 'shader_map.tsv'
    if shmap_path and Path(shmap_path).is_file():
        if mode == 'thin':
            dest_shmap.symlink_to(shmap_path)
        else:
            shutil.copy2(shmap_path, dest_shmap)

    dest_font = dm_res_dir / 'font'
    if font_dir and Path(font_dir).is_dir():
        if mode == 'thin':
            dest_font.symlink_to(font_dir)
        else:
            shutil.copytree(font_dir, dest_font)

    launcher_json = dm_res_dir / 'launcher.json'
    try:
        import json
        cfg = settings_mod.build(game_id, patched_dir, control_nacp)
        with launcher_json.open('w') as f:
            json.dump(cfg, f, indent=1)
    except BaseException:
        try:
            with launcher_json.open('w') as f:
                json.dump({"game": game_id, "settings": []}, f)
        except Exception:
            pass

    if romfs_dir and Path(romfs_dir).is_dir():
        dest_romfs = res_dir / 'romfs'
        if mode == 'thin':
            dest_romfs.symlink_to(romfs_dir)
        else:
            shutil.copytree(romfs_dir, dest_romfs)

    if msl_dir and Path(msl_dir).is_dir():
        dest_msl = res_dir / 'msl'
        if mode == 'thin':
            dest_msl.symlink_to(msl_dir)
        else:
            shutil.copytree(msl_dir, dest_msl)

    if aoc_dir and Path(aoc_dir).is_dir():
        dest_aoc = res_dir / 'aoc'
        if mode == 'thin':
            dest_aoc.symlink_to(aoc_dir)
        else:
            shutil.copytree(aoc_dir, dest_aoc)

    # Ad-hoc codesign with developer entitlements
    tmp_ent = Path(tempfile.gettempdir()) / f"nxnative_ent_{os.getpid()}.plist"
    try:
        with tmp_ent.open('wb') as ef:
            plistlib.dump({'com.apple.security.get-task-allow': True}, ef)
        subprocess.run(['codesign', '-s', '-', '--force', '--deep', '--entitlements', str(tmp_ent), str(out_app)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        try:
            subprocess.run(['codesign', '-s', '-', '--force', '--deep', str(out_app)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError as e:
            print(f"Warning: codesign failed: {e}", file=sys.stderr)
    finally:
        try:
            tmp_ent.unlink(missing_ok=True)
        except Exception:
            pass

    print(f"Packed macOS application: {out_app} (mode: {mode})")
    return out_app


def main():
    ap = argparse.ArgumentParser(description='Pack an nxnative game into a macOS .app bundle')
    ap.add_argument('--game', required=True, help='Game Title ID (e.g. 010093801237C000)')
    ap.add_argument('--work', required=True, help='Work directory (<title_id>_build)')
    ap.add_argument('--out', help='Output .app path (default: <work>/<App Name>.app)')
    ap.add_argument('--mode', choices=['thin', 'standalone'], default='thin',
                    help='thin: symlink large RomFS and shaders (default); standalone: full copy')
    ap.add_argument('--host', help='Path to compiled dmhost binary (default: <work>/tools/dmhost)')
    ap.add_argument('--patched', help='Path to patched modules dir (default: newest <work>/patched_*)')
    ap.add_argument('--romfs', help='Path to RomFS dir')
    ap.add_argument('--msl', help='Path to Metal shader dir (default: <work>/shader/cache)')
    ap.add_argument('--font', help='Path to font dir (default: <work>/font)')
    ap.add_argument('--shmap', help='Path to shader_map.tsv')
    ap.add_argument('--nacp', help='Path to control.nacp')
    ap.add_argument('--icon', help='Path to icon.jpg')
    ap.add_argument('--aoc', help='Path to AOC (DLC) directory if any')

    args = ap.parse_args()
    work = Path(args.work).resolve()

    host = Path(args.host) if args.host else work / 'tools/dmhost'
    if not host.is_file():
        sys.exit(f"Error: host binary not found at {host}")

    if args.patched:
        patched = Path(args.patched)
    else:
        patched_dirs = sorted([d for d in work.glob('patched_*') if d.is_dir()])
        if not patched_dirs:
            sys.exit(f"Error: no patched_* directory found in {work}")
        patched = patched_dirs[-1]

    # Find control.nacp and icon.jpg
    nacp_file = Path(args.nacp) if args.nacp else None
    icon_file = Path(args.icon) if args.icon else None
    romfs = Path(args.romfs) if args.romfs else None

    if not (nacp_file and icon_file and romfs):
        for candidate in sorted(work.glob('extract/*'), reverse=True):
            if candidate.is_dir():
                if not nacp_file and (candidate / 'control.nacp').is_file():
                    nacp_file = candidate / 'control.nacp'
                if not icon_file and (candidate / 'icon.jpg').is_file():
                    icon_file = candidate / 'icon.jpg'
                if not romfs and (candidate / 'RomFS').is_dir():
                    romfs = candidate / 'RomFS'

    if not romfs or not romfs.is_dir():
        sys.exit(f"Error: RomFS not found under {work}")

    msl = Path(args.msl) if args.msl else work / 'shader/cache'
    font = Path(args.font) if args.font else work / 'font'
    shmap = Path(args.shmap) if args.shmap else REPO / f"dm/games/{args.game}/shader_map.tsv"

    app_name = nacp.name(nacp_file) if nacp_file else "Game"
    out_app = Path(args.out) if args.out else work / f"{app_name}.app"

    build_app_bundle(
        game_id=args.game,
        work_dir=work,
        host_bin=host,
        patched_dir=patched,
        romfs_dir=romfs,
        msl_dir=msl,
        font_dir=font,
        shmap_path=shmap,
        control_nacp=nacp_file,
        icon_jpg=icon_file,
        out_app=out_app,
        mode=args.mode,
        aoc_dir=args.aoc,
    )


if __name__ == '__main__':
    main()
