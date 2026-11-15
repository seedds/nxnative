#!/usr/bin/env python3
"""The port's bundle: what a player downloads, unpacks and runs on Linux or Windows (docs/development.md, "The port's bundles").

    python3 scripts/package_port.py --platform linux --build build/linux --aot build/aot --tools build/port_tools/linux-x64 --out build/bundle/nxnative-linux-x86_64 --archive --check
    python3 scripts/package_port.py --platform windows --build build/win64 --aot build/aot-win64 --tools build/port_tools/win-x64 --out build/bundle/nxnative-windows-x86_64 --archive --check

It builds nothing: it assembles the bundle folder from build trees that CMake already made and from this checkout's tracked files, and fails with the missing file's name
when something is not there.
  --build   the host's CMake tree (dm/host/CMakeLists.txt, with NXHOST_PROGRAM and NXHOST_LAUNCHER): nxlauncher, and the host <build>/nxhost-run, one program for every game
            profile (it reads the profile at start: --profile <file> from the pack, dm/host/game_rt.c; the tree's GAME is only its default), which goes to bin/; SDL3's licence
            from <build>/_deps/sdl3-src/LICENSE.txt (FetchContent's copy); any file in <build>/licenses/ goes to the bundle's licenses/ too. On Windows the .exe names
  --aot     the CMake tree of the recompiler (dm/tools/aot): nxaot, and licenses/LLVM.txt (the licence of the LLVM and lld libraries linked into it; the CI copies the image's
            /usr/share/doc/libllvm18/copyright there). Required on Linux. On Windows nxaot.exe, cross-built with llvm-mingw against the static LLVM of
            dm/tools/aot/tools/build_llvm_win64.sh (dm/tools/aot/README.md, "Building nxaot for Windows"), with that LLVM's LICENSE.TXT as licenses/LLVM.txt; the packer
            runs the bundle's bin/nxaot.exe. Without --aot a Windows bundle has no nxaot and its README.txt says how to pack
  --tools   the port's prebuilt tools of dm/tools/publish_port_tools.sh for this platform: nx_extract and the Vulkan shader translator dmsh_xlate_vk (.exe on Windows), programs
            that need no .NET, which go to bin/ (the packer runs them from there instead of building nx_extract with dotnet), and the licences of what is linked into them
            (<tools>/licenses/, at least dotnet-runtime.txt), which go to licenses/. Without --tools the bundle has neither and its README.txt asks for the .NET SDK
  --out     the bundle folder, created (or replaced when it holds an earlier bundle: a VERSION file); its name is the archive's top folder
  --archive <out>.tar.gz (Linux: executable bits kept) or <out>.zip (Windows) beside it; the same inputs give the same bytes (sorted entries, the commit's time on every entry)
  --check   the bundle's own smoke test: no symlink, no file naming the build trees or this checkout, no ELF run path outside the bundle and no ELF that needs a shared library
            of NOT_NEEDED (a dynamic nxaot: build it with -DNXAOT_LLVM_STATIC=ON), the launcher's --list on an empty games folder and nxaot version (Linux only: a Windows bundle's executables are not run here), the packer's --help through the bundle's path, the shader chain versions the
            bundle's sources give against its headers, the host for every game profile the bundle carries (Linux only: bin/nxhost-run started with --game <id> and with --profile
            <the file the bundle's game.py runtime writes>, its profile line read); with --tools, nx_extract's usage and the translator's source hash (Linux only), and the translator
            against its row of the bundle's dm/gfx/tools/dmsh_chain_vk.lock (dmsh_xlate_vk_linux-x64 / dmsh_xlate_vk_win-x64), which the packer's shader step checks
Exit status 0 when done, 1 on an error (the message says which file or check), 2 on a usage error."""
import argparse
import gzip
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile

REPO = Path(__file__).resolve().parents[1]
PLATFORMS = {'linux': {'exe': '', 'archive': '.tar.gz'}, 'windows': {'exe': '.exe', 'archive': '.zip'}}
# The packer and what it reads (scripts/build_ipa_from_nsp.py --platform linux / windows): this checkout's tracked files under these paths, at the same place in the bundle, so that
# the packer's find_repo stops at the bundle's root and its defaults find the bundle's nxaot and host (bin/). dm/games: every profile and its tables; dm/tools:
# patch_nso.py and the rewriter, nx_extract's sources (built with .NET at the first pack); dm/gfx: the shader tools and the chain headers, whose versions follow from those files
# (dmsh_chain.py, dmsh_chain_vk.py), with dm/ios/tools/ios_metal.py and Ryujinx's shader sources among their inputs; dm/host: the host's sources, which the survey of a game without
# a profile reads (nso_syms.py)
PACKER_TREES = ('scripts/build_ipa_from_nsp.py', 'dm/tools', 'dm/games', 'dm/gfx', 'dm/host', 'dm/ios/tools', 'dm/third_party/ryujinx')
PACKER_SKIP = ('dm/tools/aot/', 'dm/tools/tests/', 'dm/host/tests/', 'dm/gfx/tests/', 'dm/ios/tests/')   # the recompiler's C++ sources (bin/nxaot is built) and the tests
TEXT_SUFFIXES = {'.sh', '.cmd', '.txt', '.md', '.tsv', '.json', '.py'}   # scanned by --check for the paths of the build trees and this checkout
MARKER = 'VERSION'
# Shared libraries no program of the bundle may need (--check): LLVM's, which a player's PC does not have (Ubuntu's libllvm18), and the ones a shared LLVM brings whose sonames
# change between the distributions' releases (libxml2.so.2 is libxml2.so.16 from libxml2 2.14; ICU's carry its version). The bundle's nxaot is linked statically
# (-DNXAOT_LLVM_STATIC=ON: dm/tools/aot/README.md, "Building and testing in Docker") and needs only libc, libm, libstdc++, libgcc_s, libz, libzstd, libtinfo and liblzma
NOT_NEEDED = ('libLLVM', 'libclang', 'liblld', 'libxml2', 'libicu', 'libffi', 'libedit')


class PackError(Exception):
    pass


def say(msg):
    print(f'package_port: {msg}', flush=True)


def git(*args):
    try: r = subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, text=True)
    except OSError as e: raise PackError(f'git does not run ({e}): the bundle takes the packer\'s files and its version from this checkout\'s git') from e
    if r.returncode: raise PackError(f'git {" ".join(args)}: {r.stderr.strip() or "exit " + str(r.returncode)} (is {REPO} a git checkout?)')
    return r.stdout


def version_info():
    """(short SHA, commit time as seconds since 1970, ISO date, whether the packer's tracked files differ from that commit): VERSION's lines and every entry's time"""
    sha, ts = git('log', '-1', '--format=%h %ct').split()
    dirty = bool(git('status', '--porcelain', '--untracked-files=no', '--', *PACKER_TREES).strip())
    return sha, int(ts), time.strftime('%Y-%m-%d', time.gmtime(int(ts))), dirty


def tracked_files():
    """[(path relative to the checkout, mode)] of the packer's files: git's own list (no build output, no __pycache__), its executable bit kept"""
    out, rows = git('ls-files', '-s', '-z', '--', *PACKER_TREES), []
    for entry in out.split('\0'):
        if not entry: continue
        meta, path = entry.split('\t', 1)
        mode = meta.split()[0]
        if mode == '120000': raise PackError(f'{path} is a symbolic link in git: a bundle holds no links')
        if mode == '160000' or path.startswith(PACKER_SKIP): continue   # a submodule; the skipped trees
        rows.append((path, 0o755 if mode == '100755' else 0o644))
    if not rows: raise PackError(f'git lists no file under {", ".join(PACKER_TREES)}')
    return sorted(rows)


HOST = 'bin/nxhost-run'   # the bundle's host program, beside its other programs (build_ipa_from_nsp.py BUNDLE_HOST, launcher_model.cpp find_host, run.sh / run.cmd)


def find_host(build, exe):
    """<build>/nxhost-run: the host program, one for every game profile (the tree's GAME is only the profile it starts with when it is given none; packs give theirs with
    --profile). A tree of the layout before it (<build>/hosts/<title ID>/nxhost-run, one host per profile, copied there by an older CI) is named in the error"""
    top = build / f'nxhost-run{exe}'
    if top.is_file(): return top
    old = f'; {build}/hosts/ holds the layout before it (one host per game profile), which a bundle no longer takes: every pack runs with the one host' if (build / 'hosts').is_dir() else ''
    raise PackError(f'no host in {build}: no {top.name} (cmake --build {build} --target nxhost-run: one program for every game profile){old}')


def bundle_profiles(files):   # the game profiles the bundle carries: dm/games/<id>/game.tsv among the packer's files
    return sorted({Path(rel).parts[2] for rel, _ in files if re.fullmatch(r'dm/games/[^/]+/game\.tsv', rel)})


def need(path, why):
    if not path.is_file(): raise PackError(f'missing {path}: {why}')
    return path


def licence_files(platform, build, aot, tools=None):
    """[(name in licenses/, source)]: this project's licence, every third party vendored in dm/third_party (its LICENSE / COPYING, and libvpx's PATENTS), dynarmic's bundled
    externals, SDL3 (fetched by the host's CMake), the LLVM and lld libraries linked into nxaot, the .NET runtime of the prebuilt tools, and whatever the trees' licenses/
    folders hold (a name already taken keeps its first source: the tools' ryujinx.txt is dm/third_party/ryujinx's licence)"""
    out = [('nxnative.txt', need(REPO / 'LICENSE', 'this project\'s licence'))]
    def first(d):
        found = sorted(p for p in d.iterdir() if p.is_file() and re.match(r'(LICEN[CS]E|COPYING)', p.name, re.I))
        if not found: raise PackError(f'no LICENSE or COPYING file in {d}: every vendored third party carries its licence')
        return found[0]
    tp = REPO / 'dm/third_party'
    for d in sorted(p for p in tp.iterdir() if p.is_dir()):
        out.append((f'{d.name}.txt', first(d)))
        if (d / 'PATENTS').is_file(): out.append((f'{d.name}-PATENTS.txt', d / 'PATENTS'))
    ext = tp / 'dynarmic/externals'
    if ext.is_dir():
        for d in sorted(p for p in ext.iterdir() if p.is_dir()):
            if any(re.match(r'(LICEN[CS]E|COPYING)', p.name, re.I) for p in d.iterdir()): out.append((f'dynarmic-{d.name}.txt', first(d)))
    out.append(('SDL3.txt', need(build / '_deps/sdl3-src/LICENSE.txt', 'SDL3\'s licence, which the host\'s CMake fetches with SDL3 (FetchContent) into the build tree')))
    where = ('copy LICENSE.TXT from the prefix dm/tools/aot/tools/build_llvm_win64.sh built' if platform == 'windows'
             else 'copy /usr/share/doc/libllvm18/copyright from the nxnative-aot image')
    if aot: out.append(('LLVM.txt', need(aot / 'licenses/LLVM.txt', f'the licence of the LLVM and lld libraries linked into nxaot ({where})')))
    if tools: need(tools / 'licenses/dotnet-runtime.txt', 'the licence of the .NET runtime in the prebuilt tools (dm/tools/publish_port_tools.sh writes licenses/ beside them)')
    for tree in (build, aot, tools):
        if tree and (tree / 'licenses').is_dir():
            for p in sorted((tree / 'licenses').iterdir()):
                if p.is_file() and p.name not in {n for n, _ in out}: out.append((p.name, p))
    return out


def readme(platform, sha, date, profiles, nxaot, tools=False):   # profiles: the game profiles the bundle carries (bundle_profiles)
    win, lines = platform == 'windows', []
    exe, py, pack = ('.exe', 'py -3', 'pack.cmd') if win else ('', 'python3', './pack.sh')
    lines += [f'nxnative for {"Windows" if win else "Linux"} (x86-64), {sha} of {date}',
              '',
              'Nintendo Switch games you own, packed on this PC from your own dumps and keys, and played with the host program nxhost-run. No game files or keys are in this folder.',
              '',
              'What this PC needs',
              f'  - {"Windows 10 or later" if win else "Linux with glibc 2.38 and the C++ library of GCC 13 or later (Ubuntu 24.04 or later, Debian 13)"} and a GPU driver with Vulkan 1.3.']
    if tools: lines += [
              f'  - To pack a game: Python 3.12 or later with lz4, capstone and backports.zstd ({py} -m pip install lz4 capstone backports.zstd; backports.zstd only before',
              '    Python 3.14). The packer\'s other tools are in bin/ and need no .NET. The Vulkan shaders of NVN games also need SPIRV-Tools, glslang and SPIRV-Cross,',
              '    which this folder does not carry yet: until then the packer reports that step as not run (docs/development.md, "The port\'s bundles").']
    else: lines += [
              f'  - To pack a game: Python 3.12 or later with lz4, capstone and backports.zstd ({py} -m pip install lz4 capstone backports.zstd; backports.zstd only before',
              '    Python 3.14), and the .NET SDK 10 (the packer builds its extraction tool, nx_extract, with it on the first pack; NVN games also need the Vulkan shader',
              '    translator, which this folder does not carry yet: docs/development.md, "The port\'s bundles").']
    if win and not nxaot: lines += ['  - The recompiler nxaot is a Linux program: this folder has none. Pack with --nxaot naming a script that runs the Linux nxaot (in WSL or Docker), or pack',
                                    '    on Linux.']
    lines += ['',
              'Packing a game',
              f'  1. Put each game\'s NSP files (base game, update, DLC) in a folder of its own, outside this folder.',
              f'  2. {pack} "<the game\'s NSP folder>" -k "<your prod.keys>"',
              '     (pack.sh / pack.cmd run scripts/build_ipa_from_nsp.py --platform ' + platform + '; --pack-mode max packs a faster game more slowly; --help lists the options).',
              '     It writes <NSP folder>/<title ID>_build/ (the packs, game-profile-' + platform + '.tsv, launch-' + platform + '.tsv, ' + ('run.cmd' if win else 'run.sh')
              + ') and <NSP folder>/<title ID>-work/ (the save).',
              '',
              'Playing',
              f'  Open nxlauncher{exe}. In Games folder, type the folder that holds your NSP folders and choose Open; pick a game and Play. The settings, the pack modes and',
              '  the second pack ("Repack with profile") are described in docs/settings.md, "On Windows and Linux", of the nxnative repository.',
              '',
              'This folder',
              f'  {"nxlauncher" + exe:<25} the launcher',
              f'  {"bin/nxhost-run" + exe:<25} the host program the launcher starts, one for every game (each pack gives it its game profile)']
    if nxaot: lines += [f'  {"bin/nxaot" + exe:<25} the recompiler the packer runs']
    if tools: lines += [f'  {"bin/nx_extract" + exe:<25} the NSP extraction tool the packer runs',
                        f'  {"bin/dmsh_xlate_vk" + exe:<25} the Vulkan shader translator the packer runs for NVN games']
    lines += [f'  {pack:<25} the packer: scripts/ and dm/ hold it and the game profiles',
              f'  {"licenses/":<25} the licences of nxnative and of the third-party code in it',
              f'  {"VERSION":<25} the commit this bundle was built from',
              '',
              'Moving or replacing this folder: each pack names the host it runs with (its launch file\'s host row); the launcher run from the new folder',
              f'takes this folder\'s bin/nxhost-run{exe} instead, and so do run.sh and run.cmd when the NSP folder is inside it; otherwise set NXHOST or',
              'pack the game again. The pack\'s game profile stays with it (<title ID>_build/game-profile-' + platform + '.tsv in the NSP folder).',
              f'Game profiles in this folder: {len(profiles)}. A game packed from one of them, or from the profile the packer derives for another version of the game',
              'or guesses for a game without one, runs with the same nxhost-run.']
    return '\r\n'.join(lines) + '\r\n' if win else '\n'.join(lines) + '\n'


PACK_SH = '''#!/bin/sh
# Packs a game for this bundle: scripts/build_ipa_from_nsp.py --platform linux with the bundle's recompiler (bin/nxaot) and host (bin/nxhost-run), which the packer finds
# beside itself. PYTHON names another Python 3.12 or later. Usage: ./pack.sh "<NSP folder>" -k "<prod.keys>" [options of build_ipa_from_nsp.py]
here=$(cd "$(dirname "$0")" && pwd) || exit 1
exec "${PYTHON:-python3}" "$here/scripts/build_ipa_from_nsp.py" --platform linux "$@"
'''

PACK_CMD = '''@echo off
rem Packs a game for this bundle: scripts\\build_ipa_from_nsp.py --platform windows with the bundle's host (bin\\nxhost-run.exe), which the packer finds beside itself. nxaot is
rem a Linux program: give --nxaot a script that runs it (README.txt). PYTHON names another Python 3.12 or later. Usage: pack.cmd "<NSP folder>" -k "<prod.keys>" [options]
setlocal
if not defined PYTHON (where py > nul 2>&1 && set "PYTHON=py -3" || set "PYTHON=python")
%PYTHON% "%~dp0scripts\\build_ipa_from_nsp.py" --platform windows %*
exit /b %errorlevel%
'''

# pack.cmd of a bundle with nxaot.exe (--aot): the packer runs it from bin\ (build_ipa_from_nsp.py find_nxaot)
PACK_CMD_NXAOT = '''@echo off
rem Packs a game for this bundle: scripts\\build_ipa_from_nsp.py --platform windows with the bundle's recompiler (bin\\nxaot.exe) and host (bin\\nxhost-run.exe), which the
rem packer finds beside itself. PYTHON names another Python 3.12 or later. Usage: pack.cmd "<NSP folder>" -k "<prod.keys>" [options of build_ipa_from_nsp.py]
setlocal
if not defined PYTHON (where py > nul 2>&1 && set "PYTHON=py -3" || set "PYTHON=python")
%PYTHON% "%~dp0scripts\\build_ipa_from_nsp.py" --platform windows %*
exit /b %errorlevel%
'''


def assemble(platform, build, aot, out, tools=None):
    """The bundle folder out: [(relative path, mode)] of every file it holds"""
    exe = PLATFORMS[platform]['exe']
    sha, ts, date, dirty = version_info()
    build = build.resolve()
    if not build.is_dir(): raise PackError(f'--build: no such folder: {build}')
    host = find_host(build, exe)
    launcher = need(build / f'nxlauncher{exe}', f'the launcher (cmake --build {build} --target nxlauncher)')
    nxaot = None
    if aot:
        aot = aot.resolve()
        if not aot.is_dir(): raise PackError(f'--aot: no such folder: {aot}')
        nxaot = need(aot / f'nxaot{exe}', f'the recompiler (cmake --build {aot} --target nxaot)')
    elif platform == 'linux': raise PackError('--aot is required on Linux: the packer needs the recompiler nxaot (dm/tools/aot)')
    else: say('no --aot: the Windows bundle has no nxaot (dm/tools/aot/README.md, "Building nxaot for Windows", builds nxaot.exe; README.txt says how to pack without it)')
    progs = []   # the prebuilt tools (--tools): [(name in bin/, source)]
    if tools:
        tools = tools.resolve()
        if not tools.is_dir(): raise PackError(f'--tools: no such folder: {tools}')
        for n in ('nx_extract', 'dmsh_xlate_vk'):
            progs.append((f'{n}{exe}', need(tools / f'{n}{exe}', f'a prebuilt tool (sh dm/tools/publish_port_tools.sh {platform} <folder>, in the chain image: docker/README.md)')))
    lic = licence_files(platform, build, aot, tools)
    files = tracked_files()
    profiles = bundle_profiles(files)
    if not profiles: raise PackError('git lists no dm/games/<title ID>/game.tsv: a bundle carries the game profiles its host runs')
    if dirty: say('[RED] the packer\'s files differ from the commit: VERSION says so')

    if out.exists():
        if not out.is_dir() or (any(out.iterdir()) and not (out / MARKER).is_file()):
            raise PackError(f'--out {out} exists and is not an earlier bundle (no {MARKER} file in it): choose another folder or remove it')
        shutil.rmtree(out)
    out.mkdir(parents=True)
    held = []

    def put(rel, src=None, data=None, mode=0o644):
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if data is not None: dst.write_bytes(data)
        else: shutil.copyfile(src, dst)   # the bytes only: no link, the mode set below
        held.append((rel, mode))

    put(f'nxlauncher{exe}', launcher, mode=0o755)
    put(f'{HOST}{exe}', host, mode=0o755)
    if nxaot: put(f'bin/nxaot{exe}', nxaot, mode=0o755)
    for name, src in progs: put(f'bin/{name}', src, mode=0o755)
    for rel, mode in files: put(rel, REPO / rel, mode=mode)
    for name, src in lic: put(f'licenses/{name}', src)
    if platform == 'linux': put('pack.sh', data=PACK_SH.encode(), mode=0o755)
    else: put('pack.cmd', data=(PACK_CMD_NXAOT if nxaot else PACK_CMD).replace('\n', '\r\n').encode())
    put('README.txt', data=readme(platform, sha, date, profiles, bool(nxaot), bool(progs)).encode())
    nl = '\r\n' if platform == 'windows' else '\n'
    put(MARKER, data=nl.join([f'nxnative {sha}{"-modified" if dirty else ""}', f'date {date}', f'platform {platform}-x86_64',
                              f'host {HOST}{exe}', f'profiles {len(profiles)}', f'nxaot {"yes" if nxaot else "no"}']).encode() + nl.encode())
    for rel, mode in held:
        os.chmod(out / rel, mode); os.utime(out / rel, (ts, ts))
    for d in sorted({p for rel, _ in held for p in Path(rel).parents if str(p) != '.'}, key=lambda p: -len(p.parts)):
        os.chmod(out / d, 0o755); os.utime(out / d, (ts, ts))
    say(f'{out}: {len(held)} files, {sum((out / r).stat().st_size for r, _ in held) / 1e6:.1f} MB; host {HOST}{exe} from {host} for {len(profiles)} game profiles; '
        f'nxaot {"from " + str(nxaot) if nxaot else "none"}; prebuilt tools {"from " + str(tools) if progs else "none"}; {sha} of {date}')
    return sorted(held), ts


def bundle_files(out):   # [(relative path, mode)] of what the folder holds now, sorted (the archive's order)
    rows = []
    for d, dirs, names in os.walk(out):
        dirs.sort()
        for n in names:
            p = Path(d) / n
            rows.append((p.relative_to(out).as_posix(), stat.S_IMODE(p.lstat().st_mode)))
    return sorted(rows)


def archive(platform, out, ts):
    """<out>.tar.gz or <out>.zip beside out, with out's name as its top folder: entries sorted, every time the commit's, owner 0, modes 0755 / 0644; gzip without a name or time"""
    top, rows = out.name, bundle_files(out)
    if platform == 'linux':
        dst = out.parent / f'{top}.tar.gz'
        # streamed into gzip (no name, no time in its header), not held in memory: the bundle is hundreds of megabytes
        with open(dst, 'wb') as f, gzip.GzipFile(filename='', mode='wb', fileobj=f, mtime=0, compresslevel=9) as gz, \
                tarfile.open(fileobj=gz, mode='w', format=tarfile.PAX_FORMAT) as tar:
            dirs = sorted({str(p) for rel, _ in rows for p in Path(rel).parents if str(p) != '.'})
            entries = sorted([(d, None) for d in dirs] + [(rel, mode) for rel, mode in rows])
            for rel, mode in [('', None)] + entries:
                ti = tarfile.TarInfo(f'{top}/{rel}'.rstrip('/'))
                ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = ts, 0, 0, '', ''
                if mode is None:
                    ti.type, ti.mode = tarfile.DIRTYPE, 0o755; tar.addfile(ti)
                else:
                    ti.mode = 0o755 if mode & 0o111 else 0o644; ti.size = (out / rel).stat().st_size
                    with open(out / rel, 'rb') as src: tar.addfile(ti, src)
    else:
        dst = out.parent / f'{top}.zip'
        when = time.gmtime(max(ts, 315532800))[:6]   # a zip's times start in 1980
        with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for rel, mode in rows:
                zi = zipfile.ZipInfo(f'{top}/{rel}', when)
                zi.compress_type, zi.create_system = zipfile.ZIP_DEFLATED, 3
                zi.external_attr = ((0o100755 if mode & 0o111 else 0o100644) << 16)
                z.writestr(zi, (out / rel).read_bytes())
    say(f'archive: {dst} ({dst.stat().st_size / 1e6:.1f} MB)')
    return dst


def elf_dynamic(path):
    """(DT_NEEDED names, DT_RPATH and DT_RUNPATH strings) of a 64-bit little-endian ELF file, or None when it is not one"""
    with open(path, 'rb') as f: data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1: return None
    phoff, = struct.unpack_from('<Q', data, 0x20); phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    loads, dyn = [], None
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if p_type == 1: loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2: dyn = (p_offset, p_filesz)
    if not dyn: return [], []
    entries, strtab = [], None
    for off in range(dyn[0], dyn[0] + dyn[1], 16):
        tag, val = struct.unpack_from('<qQ', data, off)
        if tag == 0: break
        if tag == 5: strtab = val
        entries.append((tag, val))
    base = next((o + strtab - v for v, o, s in loads if v <= strtab < v + s), None) if strtab is not None else None
    def s(i):
        if base is None: return '?'
        end = data.index(b'\0', base + i); return data[base + i:end].decode(errors='replace')
    return [s(v) for t, v in entries if t == 1], [s(v) for t, v in entries if t in (15, 29)]


def check(platform, out, inputs):
    """The bundle's own smoke test (see the module's docstring). Returns the failures ([] when it passes); prints what it ran"""
    fails, rows = [], bundle_files(out)
    leaks = sorted({str(Path(p).resolve()) for p in inputs if p} | {str(REPO), str(out.resolve())}, key=len, reverse=True)
    for d, dirs, names in os.walk(out):
        for n in dirs + names:
            if (Path(d) / n).is_symlink(): fails.append(f'{Path(d, n).relative_to(out)} is a symbolic link')
    # The text files this script writes or takes from the build trees (README.txt, VERSION, pack.sh / pack.cmd, licenses/): the tracked files are the same in every checkout
    # (their docs name the Docker images' /src, which is no path of this machine), and the packer computes its defaults from its own place (find_repo, find_nxaot, port_host)
    tracked = {rel for rel, _ in tracked_files()}
    for rel, _ in rows:
        p = out / rel
        if rel not in tracked and (p.suffix in TEXT_SUFFIXES or rel == MARKER):
            text = p.read_text(errors='replace')
            for x in leaks:
                if x in text: fails.append(f'{rel} names {x}, a path outside the bundle'); break
        dyn = elf_dynamic(p) if p.stat().st_size >= 64 else None
        if dyn:
            needed, rpaths = dyn
            # a run path inside the bundle ($ORIGIN) or a system folder is what a player's PC can have; one that names a build tree, this checkout or a relative folder (taken
            # from wherever the program is started) is not
            bad = [part for r in rpaths for part in r.split(':') if part and not part.startswith('$ORIGIN')
                   and (not part.startswith(('/usr/', '/lib', '/opt/')) or any(part.startswith(x) for x in leaks))]
            if bad: fails.append(f'{rel} has the run path {":".join(bad)}, outside the bundle and the system\'s folders')
            banned = [n for n in needed if n.startswith(NOT_NEEDED)]
            if banned: fails.append(f'{rel} needs {" ".join(banned)}, which a player\'s PC need not have' + (': it was linked with the shared LLVM; build nxaot with '
                                    '-DNXAOT_LLVM_STATIC=ON (dm/tools/aot/README.md)' if rel.startswith('bin/nxaot') else ''))
            say(f'check: {rel}: needs {" ".join(needed) or "no shared library"}' + (f'; run path {":".join(rpaths)}' if rpaths else ''))
    say(f'check: {len(rows)} files, no symbolic link, no path of the build trees or this checkout in the text files it wrote or copied from them' if not fails else f'check: {len(fails)} file problem(s)')

    def run(argv, want, cwd=None, env=None, codes=(0,)):
        try: r = subprocess.run([str(a) for a in argv], capture_output=True, text=True, errors='replace', timeout=120, cwd=cwd, env=env)
        except (OSError, subprocess.TimeoutExpired) as e: fails.append(f'{" ".join(map(str, argv))}: does not run ({e})'); return None
        text = r.stdout + r.stderr
        ok = r.returncode in codes and re.search(want, text, re.M)
        shown = [str(a) if not str(a).startswith(str(out)) else Path(a).relative_to(out).as_posix() for a in argv]
        say(f'check: {" ".join(shown)}: exit {r.returncode}' + (f': {text.strip().splitlines()[0][:160]}' if text.strip() else ''))
        label = shown[2] if argv[0] == sys.executable else shown[0]   # the bundle's script a Python runs, else the program
        if not ok: fails.append(f'{label}: exit {r.returncode}, output {text.strip()[-300:]!r} (wanted {want!r})')
        return r

    with tempfile.TemporaryDirectory(prefix='nxnative_check_') as t:
        games, cfg = Path(t) / 'games', Path(t) / 'config'
        games.mkdir(); cfg.mkdir()
        env = dict(os.environ, HOME=t, XDG_CONFIG_HOME=str(cfg), APPDATA=str(cfg), SDL_VIDEO_DRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
        if platform == 'linux':
            if sys.platform != 'linux': say('check: the Linux executables are not run on this system')
            else:
                run([out / 'nxlauncher', '--list', games], r'^# title_id\t', env=env)   # the header line and no game; "0 games in <folder>" on standard error
                if (out / 'bin/nxaot').is_file(): run([out / 'bin/nxaot', 'version'], r'^llvm\t', env=env)
                else: fails.append('bin/nxaot is missing')
                if (out / 'bin/nx_extract').is_file(): run([out / 'bin/nx_extract'], r'^usage: nx_extract', env=env, codes=(2,))   # its usage, exit 2
                if (out / 'bin/dmsh_xlate_vk').is_file(): run([out / 'bin/dmsh_xlate_vk', '--source-sha'], r'^[0-9a-f]{16}$', env=env)
        else: say('check: skipped running nxlauncher.exe, nxhost-run.exe and nxaot.exe: a Windows bundle\'s executables run on Windows (open nxlauncher.exe there)')
        # the prebuilt translator against its row of the bundle's lock: the packer's shader step (dmsh_chain_vk.py --check-tools) takes no other
        xlate = out / f'bin/dmsh_xlate_vk{PLATFORMS[platform]["exe"]}'
        if xlate.is_file():
            rid = {'linux': 'linux-x64', 'windows': 'win-x64'}[platform]
            run([sys.executable, '-I', out / 'dm/gfx/tools/dmsh_chain_vk.py', '--check-published', rid, xlate], r'as the lock', cwd=t, env=env)
        # the packer through the bundle's own path, from another folder, with a Python that does not see this checkout's files (-I)
        run([sys.executable, '-I', out / 'scripts/build_ipa_from_nsp.py', '--help'], r'--platform', cwd=t, env=env)
        # the shader chain versions the bundle's sources give, against the headers the host was compiled with: a difference would make the packer write the headers again
        # and translate shaders the host does not load
        for tool, header, macro in (('dmsh_chain.py', 'dmsh_chain.h', 'DMSH_CHAIN'), ('dmsh_chain_vk.py', 'dmsh_chain_vk.h', 'DMSH_CHAIN_VK')):
            r = run([sys.executable, '-I', out / 'dm/gfx/tools' / tool], r'\S', cwd=t, env=env)
            m = re.search(rf'#define {macro} "(.*)"', (out / 'dm/gfx/nvn' / header).read_text()) if (out / 'dm/gfx/nvn' / header).is_file() else None
            if r and r.returncode == 0 and (not m or r.stdout.split()[-1] != m.group(1)):
                fails.append(f'{tool} in the bundle gives {r.stdout.split()[-1]}, its {header} holds {m.group(1) if m else "nothing"}: a file of the chain is missing from the bundle')
            elif r and r.returncode == 0: say(f'check: {macro} {m.group(1)}: the bundle\'s sources give the version its header holds')
        check_host(platform, out, Path(t), env, fails)
    return fails


def check_host(platform, out, tmp, env, fails):
    """The bundle's one host for every game profile it carries (dm/games/<id>/game.tsv). On Linux bin/nxhost-run is started twice per profile: with --game <id> (the table of
    profiles built into it, which must hold every profile the bundle's packer can pack from) and with --profile <file>, the file the bundle's own dm/games/game.py runtime writes
    for it (what the packer writes beside every pack, and the only way a derived or provisional profile reaches the host: the two must agree on its keys). Its first lines name
    the profile, its name and where it came from (dm/host/game_rt.c); with no module folder it then stops with its usage, status 2, before anything else starts (the arena, the
    window, the engine), so each start is a few milliseconds. A Windows bundle's host is not run"""
    exe = PLATFORMS[platform]['exe']
    host = out / f'{HOST}{exe}'
    profiles = sorted(p.parent.name for p in (out / 'dm/games').glob('*/game.tsv'))
    if not host.is_file(): fails.append(f'{HOST}{exe} is missing'); return
    if platform != 'linux' or sys.platform != 'linux':
        say(f'check: {HOST}{exe} for the {len(profiles)} game profiles in the bundle: not run here (a Windows bundle\'s programs run on Windows)'); return
    files = tmp / 'profiles'
    r = subprocess.run([sys.executable, '-I', str(out / 'dm/games/game.py'), 'runtime', '--all', str(files)], capture_output=True, text=True, errors='replace', timeout=300, env=env)
    if r.returncode: fails.append(f'dm/games/game.py runtime --all: exit {r.returncode}, output {(r.stdout + r.stderr).strip()[-300:]!r}'); return
    henv = {k: v for k, v in env.items() if not k.startswith('DM_')}   # DM_GAME_PROFILE would be a second profile beside --game; DM_PRELOAD and the like start more than the selection
    bad, t0 = [], time.monotonic()
    for gid in profiles:
        f = files / f'{gid}.tsv'
        rows = dict(l.split('\t', 1) for l in f.read_text(encoding='utf-8').splitlines() if l and not l.startswith('#') and '\t' in l) if f.is_file() else {}
        if rows.get('id') != gid: bad.append(f'{gid}: game.py runtime wrote no file of it ({f.name})'); continue
        for args, source in ((['--game', gid], 'the table built into the program (--game)'), (['--profile', str(f)], f'the file {f} (--profile)')):
            want = f'[dm] game profile {gid} ({rows.get("name", "")}): {source}'
            try: p = subprocess.run([str(host), *args], capture_output=True, timeout=60, env=henv)
            except (OSError, subprocess.TimeoutExpired) as e: bad.append(f'{HOST} {args[0]} {gid}: does not run ({e})'); continue
            text = (p.stdout + p.stderr).decode('utf-8', 'replace')
            if p.returncode != 2 or want not in text.splitlines():
                bad.append(f'{HOST} {args[0]} {gid}: exit {p.returncode} (2 wanted: its usage), ' + ('no line ' + repr(want) if want not in text.splitlines() else 'its line')
                           + f'; output {text.strip()[-300:]!r}')
    if bad: fails += [f'the host does not start with every game profile of the bundle ({len(bad)} of {2 * len(profiles)} starts):'] + bad[:10]
    say(f'check: {HOST}{exe} started with each of the {len(profiles)} game profiles in the bundle, from its table (--game) and from the file game.py runtime writes (--profile): '
        f'{2 * len(profiles) - len(bad)} of {2 * len(profiles)} starts named the profile ({time.monotonic() - t0:.1f} s)')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Assemble the port\'s bundle for Linux or Windows from the build trees (docs/development.md, "The port\'s bundles").')
    ap.add_argument('--platform', required=True, choices=sorted(PLATFORMS))
    ap.add_argument('--build', required=True, type=Path, help='the host\'s CMake tree: nxlauncher, nxhost-run (one program for every game profile), _deps/sdl3-src')
    ap.add_argument('--aot', type=Path, help='the recompiler\'s CMake tree: nxaot and licenses/LLVM.txt (required on Linux)')
    ap.add_argument('--tools', type=Path, help='the prebuilt nx_extract and dmsh_xlate_vk of dm/tools/publish_port_tools.sh for this platform, with their licenses/ (bin/ in the bundle)')
    ap.add_argument('--out', required=True, type=Path, help='the bundle folder; its name is the archive\'s top folder')
    ap.add_argument('--archive', action='store_true', help='also write <out>.tar.gz (Linux) or <out>.zip (Windows) beside it')
    ap.add_argument('--check', action='store_true', help='run the bundle\'s smoke test')
    a = ap.parse_args(argv)
    try:
        out = a.out.resolve()
        _, ts = assemble(a.platform, a.build, a.aot, out, a.tools)
        if a.archive: archive(a.platform, out, ts)
        if a.check:
            fails = check(a.platform, out, [a.build, a.aot, a.tools])
            if fails: raise PackError('the check failed:\n  ' + '\n  '.join(fails))
            say('check: passed')
    except PackError as e:
        print(f'[RED] package_port: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
