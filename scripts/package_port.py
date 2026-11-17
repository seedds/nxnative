#!/usr/bin/env python3
"""The port's bundle: what a player downloads, unpacks and runs on Linux or Windows (docs/development.md, "The port's bundles").

    python3 scripts/package_port.py --platform linux --build build/linux --aot build/aot --tools build/port_tools/linux-x64 --python build/port_python/linux-x64 \
        --spirv build/port_spirv/linux-x64 --out build/bundle/nxnative-linux-x86_64 --archive --check
    python3 scripts/package_port.py --platform windows --build build/win64 --aot build/aot-win64 --tools build/port_tools/win-x64 --python build/port_python/win-x64 \
        --spirv build/port_spirv/win-x64 --out build/bundle/nxnative-windows-x86_64 --archive --check

It builds nothing: it assembles the bundle folder from build trees that CMake already made and from this checkout's tracked files, and fails with the missing file's name
when something is not there.
  --build   the host's CMake tree (dm/host/CMakeLists.txt, with NXHOST_PROGRAM and NXHOST_LAUNCHER): nxlauncher, and the host <build>/nxhost-run, one program for every game
            profile (it reads the profile at start: --profile <file> from the pack, dm/host/game_rt.c; the tree's GAME is only its default), which goes to bin/; SDL3's licence
            from <build>/_deps/sdl3-src/LICENSE.txt (FetchContent's copy); any file in <build>/licenses/ goes to the bundle's licenses/ too. On Windows the .exe names, and the
            licences of what llvm-mingw links into every .exe (MINGW_LICENCES: dm/tools/mingw_licences.sh copies them from the toolchain into a tree's licenses/; --check
            fails a Windows bundle without them)
  --aot     the CMake tree of the recompiler (dm/tools/aot): nxaot, and licenses/LLVM.txt (the licence of the LLVM and lld libraries linked into it; the CI copies the image's
            /usr/share/doc/libllvm18/copyright there). Required on Linux. On Windows nxaot.exe, cross-built with llvm-mingw against the static LLVM of
            dm/tools/aot/tools/build_llvm_win64.sh (dm/tools/aot/README.md, "Building nxaot for Windows"), with that LLVM's LICENSE.TXT as licenses/LLVM.txt; the packer
            runs the bundle's bin/nxaot.exe. Without --aot a Windows bundle has no nxaot and its README.txt says how to pack
  --tools   the port's prebuilt tools of dm/tools/publish_port_tools.sh for this platform: nx_extract and the Vulkan shader translator dmsh_xlate_vk (.exe on Windows), programs
            that need no .NET, which go to bin/ (the packer runs them from there instead of building nx_extract with dotnet), and the licences of what is linked into them
            (<tools>/licenses/, at least dotnet-runtime.txt), which go to licenses/. Without --tools the bundle has neither and its README.txt asks for the .NET SDK
  --python  the packer's own Python of dm/tools/publish_port_python.py for this platform: <python>/python/ (CPython 3.14 with lz4, capstone and fontTools) goes to the bundle's
            python/, which pack.sh / pack.cmd run the packer with (the program its python.tsv names; PYTHON names another), and <python>/licenses/ (at least CPython.txt) to
            licenses/. Without --python README.txt asks for a Python with those modules
  --spirv   the SPIR-V tools of docker/build_spirv_tools.sh for this platform: spirv-val, spirv-link, spirv-cross and glslangValidator (.exe on Windows), which go to bin/ (the
            packer puts it first on PATH: build_ipa_from_nsp.py bundled_spirv), and their licences (<spirv>/licenses/: SPIRV-Tools, SPIRV-Headers, glslang, SPIRV-Cross).
            Without --spirv README.txt says that the shaders of NVN and OpenGL games need them
  --out     the bundle folder, created (or replaced when it holds an earlier bundle: a VERSION file); its name is the archive's top folder
  --archive <out>.tar.gz (Linux: executable bits kept) or <out>.zip (Windows) beside it; the same inputs give the same bytes (sorted entries, the commit's time on every entry)
  --check   the bundle's own smoke test: no symlink, no file naming the build trees or this checkout, no ELF run path outside the bundle and no ELF that needs a shared library
            of NOT_NEEDED (a dynamic nxaot: build it with -DNXAOT_LLVM_STATIC=ON), the launcher's --list on an empty games folder and nxaot version (Linux only: a Windows bundle's executables are not run here), the packer's --help through the bundle's path, the shader chain versions the
            bundle's sources give against its headers, the host for every game profile the bundle carries (Linux only: bin/nxhost-run started with --game <id> and with --profile
            <the file the bundle's game.py runtime writes>, its profile line read); with --tools, nx_extract's usage and the translator's source hash (Linux only), and the translator
            against its row of the bundle's dm/gfx/tools/dmsh_chain_vk.lock (dmsh_xlate_vk_linux-x64 / dmsh_xlate_vk_win-x64), which the packer's shader step checks; with
            --python, Linux: the bundle's Python, with none of this machine's Python environment, imports lz4.block, capstone, fontTools and compression.zstd (a round trip each)
            and has no sys.path entry outside the bundle, and pack.sh --help prints the packer's help with it; Windows: the files it needs, and no python314._pth; with --spirv,
            Linux: each tool prints the version line of its row of the bundle's lock (spirv-tools_linux-x64, glslang_linux-x64, spirv-cross_linux-x64), and with --tools too
            dmsh_chain_vk.py --check-tools passes with PATH holding only the bundle's bin/; Windows: each is a PE32+ x86-64 program and the lock has its win-x64 row
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
import textwrap
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
# The SPIR-V tools of docker/build_spirv_tools.sh the packer runs from bin/ (--spirv; build_ipa_from_nsp.py BUNDLE_SPIRV), their licences, and the row of dmsh_chain_vk.lock each
# version line is held to (with the platform's suffix) and the option that prints it
SPIRV_TOOLS = ('spirv-val', 'spirv-link', 'spirv-cross', 'glslangValidator')
SPIRV_LICENCES = ('SPIRV-Tools.txt', 'SPIRV-Headers.txt', 'glslang.txt', 'SPIRV-Cross.txt')
SPIRV_ROWS = (('spirv-tools', 'spirv-val', '--version'), ('spirv-tools', 'spirv-link', '--version'), ('glslang', 'glslangValidator', '--version'),
              ('spirv-cross', 'spirv-cross', '--revision'))
RID = {'linux': 'linux-x64', 'windows': 'win-x64'}   # the platform suffix of the lock's rows
# What llvm-mingw links statically into every Windows program of the bundle (nxhost-run.exe, nxlauncher.exe, nxaot.exe, the SPIR-V tools): libc++, libc++abi, libunwind and
# compiler-rt (the LLVM project's licence), mingw-w64's runtime (the CRT) and winpthreads. dm/tools/mingw_licences.sh copies their texts from the toolchain the trees were built
# with into a tree's licenses/, which assemble() takes; a Windows bundle without them fails --check
MINGW_LICENCES = ('llvm-mingw-LLVM.txt', 'llvm-mingw-mingw-w64.txt', 'llvm-mingw-winpthreads.txt')


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


def licence_files(platform, build, aot, tools=None, python=None, spirv=None):
    """[(name in licenses/, source)]: this project's licence, every third party vendored in dm/third_party (its LICENSE / COPYING, and libvpx's PATENTS), dynarmic's bundled
    externals, SDL3 (fetched by the host's CMake), the LLVM and lld libraries linked into nxaot, the .NET runtime of the prebuilt tools, CPython and the modules of the packer's
    Python, the SPIR-V tools' projects, and whatever the trees' licenses/ folders hold (a name already taken keeps its first source: the tools' ryujinx.txt is
    dm/third_party/ryujinx's licence)"""
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
    if python: need(python / 'licenses/CPython.txt', 'the licence of the packer\'s Python (dm/tools/publish_port_python.py writes licenses/ beside it)')
    for n in SPIRV_LICENCES if spirv else ():
        need(spirv / 'licenses' / n, 'a licence of the SPIR-V tools (docker/build_spirv_tools.sh writes licenses/ beside them)')
    for tree in (build, aot, tools, python, spirv):
        if tree and (tree / 'licenses').is_dir():
            for p in sorted((tree / 'licenses').iterdir()):
                if p.is_file() and p.name not in {n for n, _ in out}: out.append((p.name, p))
    return out


def bullet(text, width=168):   # a README.txt list item: '  - ' and the text, wrapped at width with its later lines under the text
    return textwrap.wrap(text, width=width, initial_indent='  - ', subsequent_indent='    ', break_long_words=False, break_on_hyphens=False)


def readme(platform, sha, date, profiles, nxaot, tools=False, python=None, spirv=False):
    """README.txt. profiles: the game profiles the bundle carries (bundle_profiles); nxaot, tools, spirv: whether it carries the recompiler, the prebuilt tools (--tools) and the
    SPIR-V tools (--spirv); python: the version of its own Python (--python), None without one. "What this PC needs" names what the bundle does not carry, and only that"""
    win, lines = platform == 'windows', []
    exe, py, pack = ('.exe', 'py -3', 'pack.cmd') if win else ('', 'python3', './pack.sh')
    lines += [f'nxnative for {"Windows" if win else "Linux"} (x86-64), {sha} of {date}',
              '',
              'Nintendo Switch games you own, packed on this PC from your own dumps and keys, and played with the host program nxhost-run. No game files or keys are in this folder.',
              '',
              'What this PC needs']
    lines += bullet(f'{"Windows 10 or later" if win else "Linux with glibc 2.38 and the C++ library of GCC 13 or later (Ubuntu 24.04 or later, Debian 13)"} and a GPU driver '
                    'with Vulkan 1.3' + (' and the Vulkan loader (vulkan-1.dll, which the GPU driver installs): without it a game starts and draws nothing.' if win else
                                         ' and the Vulkan loader (libvulkan.so.1: the package libvulkan1 on Ubuntu and Debian, which the drivers\' packages bring): without it a game '
                                         'starts and draws nothing. Games are given a font of the system as their standard font, and the packer draws the button icons with its '
                                         'letters: DejaVu Sans, Liberation Sans, Noto Sans or FreeSans (every desktop distribution installs one of them).'))
    if win: lines += bullet('Movies (H.264 video, AAC sound) are decoded by Windows\' Media Foundation. The N editions of Windows come without it: install the Media Feature Pack '
                            '(Settings, Apps, Optional features), or a game\'s movies play black and silent.')
    need = []   # what packing needs that this folder does not carry
    if not python: need.append(f'Python 3.12 or later with lz4, capstone 5.0.2 or a later 5.0.x, fonttools and backports.zstd ({py} -m pip install lz4 "capstone>=5.0.2,<6" fonttools backports.zstd; '
                               'backports.zstd only before Python 3.14; without fonttools the button icons show as \'?\')')
    if not tools: need.append('the .NET SDK 10 (the packer builds its extraction tool, nx_extract, with it on the first pack)')
    has = []    # what it carries for packing
    if python: has.append(f'the packer runs with this folder\'s own Python {python} (python/, with the modules it needs; PYTHON=<path> names another)')
    progs = (['extraction tool and Vulkan shader translator'] if tools else []) + (['SPIR-V tools (SPIRV-Tools, glslang, SPIRV-Cross)'] if spirv else [])
    if progs: has.append(('its ' if python else 'the packer\'s ') + ' and '.join(progs) + ' are in bin/' + (', and none of them needs .NET' if tools else ''))
    text = 'To pack a game: ' + ('nothing to install' if not need else ', and '.join(need)) + '.'
    if has: text += ' ' + '; '.join(has)[:1].upper() + '; '.join(has)[1:] + '.'
    if not tools: text += ' NVN games also need the Vulkan shader translator, which this folder does not carry (docs/development.md, "The port\'s bundles").'
    if not spirv: text += (' The shaders of NVN and OpenGL games also need SPIRV-Tools, glslang and SPIRV-Cross of the versions dm/gfx/tools/dmsh_chain_vk.lock records, which '
                           'this folder does not carry: without them the packer reports the NVN shaders as not run and stops at an OpenGL game\'s.')
    lines += bullet(text)
    if win and not nxaot: lines += bullet('The recompiler nxaot is a Linux program: this folder has none. Pack with --nxaot naming a script that runs the Linux nxaot (in WSL or Docker), '
                                          'or pack on Linux.')
    lines += ['',
              'Packing a game',
              f'  1. Put each game\'s NSP (or XCI) files (base game, update, DLC) in a folder of its own, outside this folder.',
              f'  2. {pack} "<the game\'s NSP folder>" -k "<your prod.keys>"',
              '     (pack.sh / pack.cmd run scripts/build_ipa_from_nsp.py --platform ' + platform + '; --pack-mode max packs a faster game more slowly; --help lists the options).',
              '     It writes <NSP folder>/<title ID>_build/ (the packs, game-profile-' + platform + '.tsv, launch-' + platform + '.tsv, ' + ('run.cmd' if win else 'run.sh')
              + ') and <NSP folder>/<title ID>-work/ (the save).',
              '     It needs no network, and free space on the NSP folder\'s drive of about the game\'s size and 6 GB more (it checks that first).',
              '',
              'Playing',
              f'  Open nxlauncher{exe}. In Games folder, type the folder that holds your NSP folders and choose Open; pick a game and Play. Without the launcher\'s window,',
              f'  <NSP folder>/<title ID>_build/{"run.cmd" if win else "run.sh"} starts the game as Play does, and {"nxlauncher.exe" if win else "./nxlauncher"} --games "<the folder that holds your NSP folders>"',
              '  --play <title ID> does too (--help lists its options). The game\'s log is <NSP folder>/<title ID>-work/run/log.txt. The settings, the pack modes and',
              '  the second pack ("Repack with profile") are described in docs/settings.md in this folder, "On Windows and Linux".',
              '',
              'This folder',
              f'  {"nxlauncher" + exe:<25} the launcher',
              f'  {"bin/nxhost-run" + exe:<25} the host program the launcher starts, one for every game (each pack gives it its game profile)']
    if nxaot: lines += [f'  {"bin/nxaot" + exe:<25} the recompiler the packer runs']
    if tools: lines += [f'  {"bin/nx_extract" + exe:<25} the NSP extraction tool the packer runs',
                        f'  {"bin/dmsh_xlate_vk" + exe:<25} the Vulkan shader translator the packer runs for NVN games']
    if spirv: lines += [f'  {"bin/spirv-val" + exe:<25} the SPIR-V validator (SPIRV-Tools) the packer runs for the Vulkan shaders',
                        f'  {"bin/spirv-link" + exe:<25} the SPIR-V linker (SPIRV-Tools) the packer runs for the Vulkan shaders of NVN games',
                        f'  {"bin/spirv-cross" + exe:<25} SPIRV-Cross, which the packer runs for the analysis of the Vulkan shaders of NVN games',
                        f'  {"bin/glslangValidator" + exe:<25} glslang, which the packer runs for the shaders of OpenGL games']
    if python: lines += [f'  {"python/":<25} the packer\'s Python {python} with lz4, capstone and fontTools']
    lines += [f'  {pack:<25} the packer: scripts/ and dm/ hold it and the game profiles',
              f'  {"docs/settings.md":<25} the games\' settings and the launcher (its section "On Windows and Linux")',
              f'  {"licenses/":<25} the licences of nxnative and of the third-party code in it',
              f'  {"VERSION":<25} the commit this bundle was built from',
              '',
              'Moving or replacing this folder: each pack names the host it runs with (its launch file\'s host row); the launcher run from the new folder',
              f'takes this folder\'s bin/nxhost-run{exe} instead, and so do run.sh and run.cmd when the NSP folder is inside it; otherwise set NXHOST or',
              'pack the game again. The pack\'s game profile stays with it (<title ID>_build/game-profile-' + platform + '.tsv in the NSP folder).']
    if python: lines += [f'The launcher\'s "Repack with profile" runs the Python the game was packed with (its launch file\'s python row), and this folder\'s own python/',
                         f'when that one is gone, with this folder\'s packer and nxaot.']
    lines += [f'Game profiles in this folder: {len(profiles)}. A game packed from one of them, or from the profile the packer derives for another version of the game',
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

# pack.sh and pack.cmd of a bundle with its own Python (--python; @PROGRAM@: its program, python.tsv's row): the packer and every Python tool it starts (sys.executable) run
# with it, with none of the PC's PYTHONHOME, PYTHONPATH or user site-packages, which could put another lz4 or capstone first; PYTHON names another Python as before
PACK_SH_PYTHON = '''#!/bin/sh
# Packs a game for this bundle: scripts/build_ipa_from_nsp.py --platform linux with the bundle's own Python (@PROGRAM@), recompiler (bin/nxaot), host (bin/nxhost-run) and
# shader tools (bin/), which the packer finds beside itself. PYTHON names another Python 3.12 or later. Usage: ./pack.sh "<NSP folder>" -k "<prod.keys>" [options of
# build_ipa_from_nsp.py]
here=$(cd "$(dirname "$0")" && pwd) || exit 1
if [ -z "${PYTHON:-}" ]; then
    unset PYTHONHOME PYTHONPATH; export PYTHONNOUSERSITE=1   # the bundle's Python with its own modules only
    PYTHON=$here/@PROGRAM@
fi
exec "$PYTHON" "$here/scripts/build_ipa_from_nsp.py" --platform linux "$@"
'''

PACK_CMD_PYTHON = '''@echo off
rem Packs a game for this bundle: scripts\\build_ipa_from_nsp.py --platform windows with the bundle's own Python (@PROGRAM@), @TOOLS@,
rem which the packer finds beside itself. PYTHON names another Python 3.12 or later. Usage: pack.cmd "<NSP folder>" -k "<prod.keys>" [options of build_ipa_from_nsp.py]
setlocal
if defined PYTHON goto nxpack_run
rem the bundle's Python with its own modules only
set "PYTHONHOME="
set "PYTHONPATH="
set "PYTHONNOUSERSITE=1"
set PYTHON="%~dp0@PROGRAM@"
:nxpack_run
%PYTHON% "%~dp0scripts\\build_ipa_from_nsp.py" --platform windows %*
exit /b %errorlevel%
'''


def pack_script(platform, nxaot, program=None):   # pack.sh or pack.cmd (its text, CR LF on Windows); program: the bundle's Python (--python), None without one
    if platform == 'linux': return (PACK_SH_PYTHON.replace('@PROGRAM@', program) if program else PACK_SH).encode()
    if not program: return (PACK_CMD_NXAOT if nxaot else PACK_CMD).replace('\n', '\r\n').encode()
    tools = 'recompiler (bin\\nxaot.exe), host (bin\\nxhost-run.exe) and shader tools (bin\\)' if nxaot else ('host (bin\\nxhost-run.exe) and shader tools (bin\\); nxaot is a '
                                                                                                         'Linux program: give --nxaot a script that runs it (README.txt)')
    return PACK_CMD_PYTHON.replace('@PROGRAM@', program.replace('/', '\\')).replace('@TOOLS@', tools).replace('\n', '\r\n').encode()


def python_info(python):
    """(program, version) of dm/tools/publish_port_python.py's folder: its python.tsv's program row (the path in the bundle) and python row"""
    rows = dict(l.split('\t', 1) for l in need(python / 'python.tsv', 'what the packer\'s Python holds (dm/tools/publish_port_python.py writes it)').read_text(encoding='utf-8')
                .splitlines() if l and not l.startswith('#') and '\t' in l)
    if not rows.get('program', '').startswith('python/') or not rows.get('python'): raise PackError(f'{python / "python.tsv"} names no program under python/ or no version')
    need(python / rows['program'], 'the packer\'s Python, which python.tsv names')
    return rows['program'], rows['python']


def assemble(platform, build, aot, out, tools=None, python=None, spirv=None):
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
    shaders = []   # the SPIR-V tools (--spirv): [(name in bin/, source)]
    if spirv:
        spirv = spirv.resolve()
        if not spirv.is_dir(): raise PackError(f'--spirv: no such folder: {spirv}')
        for n in SPIRV_TOOLS:
            shaders.append((f'{n}{exe}', need(spirv / f'{n}{exe}', f'a SPIR-V tool (sh docker/build_spirv_tools.sh {platform} <folder>, in its image: docker/README.md)')))
    runtime, program, pyver = [], None, None   # the packer's Python (--python): [(path under python/, source, mode)], its program and version
    if python:
        python = python.resolve()
        if not python.is_dir(): raise PackError(f'--python: no such folder: {python}')
        program, pyver = python_info(python)
        for d, dirs, names in os.walk(python / 'python'):
            dirs.sort()
            for n in sorted(names):
                p = Path(d) / n
                if p.is_symlink(): raise PackError(f'{p} is a symbolic link: a bundle holds none (dm/tools/publish_port_python.py leaves them out)')
                runtime.append((p.relative_to(python).as_posix(), p, 0o755 if p.stat().st_mode & 0o111 else 0o644))
    lic = licence_files(platform, build, aot, tools, python, spirv)
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
    for name, src in progs + shaders: put(f'bin/{name}', src, mode=0o755)
    for rel, src, mode in runtime: put(rel, src, mode=mode)
    for rel, mode in files: put(rel, REPO / rel, mode=mode)
    put('docs/settings.md', REPO / 'docs/settings.md')   # README.txt's "Playing" sends the player to it: the settings and the launcher, "On Windows and Linux"
    for name, src in lic: put(f'licenses/{name}', src)
    if platform == 'linux': put('pack.sh', data=pack_script(platform, nxaot, program), mode=0o755)
    else: put('pack.cmd', data=pack_script(platform, nxaot, program))
    put('README.txt', data=readme(platform, sha, date, profiles, bool(nxaot), bool(progs), pyver, bool(shaders)).encode())
    nl = '\r\n' if platform == 'windows' else '\n'
    put(MARKER, data=nl.join([f'nxnative {sha}{"-modified" if dirty else ""}', f'date {date}', f'platform {platform}-x86_64',
                              f'host {HOST}{exe}', f'profiles {len(profiles)}', f'nxaot {"yes" if nxaot else "no"}',
                              f'python {pyver} {program}' if program else 'python no', f'spirv {"yes" if shaders else "no"}']).encode() + nl.encode())
    for rel, mode in held:
        os.chmod(out / rel, mode); os.utime(out / rel, (ts, ts))
    for d in sorted({p for rel, _ in held for p in Path(rel).parents if str(p) != '.'}, key=lambda p: -len(p.parts)):
        os.chmod(out / d, 0o755); os.utime(out / d, (ts, ts))
    say(f'{out}: {len(held)} files, {sum((out / r).stat().st_size for r, _ in held) / 1e6:.1f} MB; host {HOST}{exe} from {host} for {len(profiles)} game profiles; '
        f'nxaot {"from " + str(nxaot) if nxaot else "none"}; prebuilt tools {"from " + str(tools) if progs else "none"}; '
        f'Python {pyver + " (" + program + ") from " + str(python) if program else "none"}; SPIR-V tools {"from " + str(spirv) if shaders else "none"}; {sha} of {date}')
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
    if platform == 'windows':
        held = {rel for rel, _ in rows}
        for n in MINGW_LICENCES:
            if f'licenses/{n}' not in held:
                fails.append(f'licenses/{n} is missing: a licence of what llvm-mingw links into the bundle\'s .exe programs (sh dm/tools/mingw_licences.sh <tree>, in the '
                             'llvm-mingw image after the build, copies it into the Windows build trees\' licenses/)')
        if all(f'licenses/{n}' in held for n in MINGW_LICENCES): say(f'check: the licences of llvm-mingw\'s static runtime: {", ".join(MINGW_LICENCES)}')

    def run(argv, want, cwd=None, env=None, codes=(0,)):
        try: r = subprocess.run([str(a) for a in argv], capture_output=True, text=True, errors='replace', timeout=120, cwd=cwd, env=env)
        except (OSError, subprocess.TimeoutExpired) as e: fails.append(f'{" ".join(map(str, argv))}: does not run ({e})'); return None
        text = r.stdout + r.stderr
        ok = r.returncode in codes and re.search(want, text, re.M)
        shown = ['<code>' if '\n' in str(a) else str(a) if not str(a).startswith(str(out)) else Path(a).relative_to(out).as_posix() for a in argv]
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
        check_packing(platform, out, Path(t), env, run, fails)
        check_host(platform, out, Path(t), env, fails)
    return fails


# The bundle's Python on Linux: the modules the packer imports, a round trip each, and where its sys.path points (every entry inside the bundle, or '' for the current folder)
PYTHON_PROBE = '''import sys
import lz4, lz4.block, capstone, fontTools, fontTools.ttLib, compression.zstd
data = bytes(range(256)) * 64
assert lz4.block.decompress(lz4.block.compress(data)) == data and compression.zstd.decompress(compression.zstd.compress(data)) == data
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
assert [i.mnemonic for i in md.disasm(bytes.fromhex("1f2003d5c0035fd6"), 0)] == ["nop", "ret"]
print(f"python {sys.version.split()[0]}, lz4 {lz4.__version__}, capstone {capstone.__version__}, fontTools {fontTools.version}, zstd {compression.zstd.zstd_version}")
for p in sys.path: print("path", p)
'''


def bundle_rows(out):   # VERSION's rows: {name: value}
    p = out / MARKER
    return dict(l.split(' ', 1) for l in p.read_text(encoding='utf-8').splitlines() if ' ' in l) if p.is_file() else {}


def check_packing(platform, out, tmp, env, run, fails):
    """What packing runs beyond the packer's own files: the bundle's Python (VERSION's python row) and its SPIR-V tools (bin/), each against what it must be. Linux runs them:
    the Python with no environment of this machine's Python (no PYTHON* variable; user site-packages off) on PYTHON_PROBE and on pack.sh --help, the SPIR-V tools' version lines
    against the bundle's lock, and dmsh_chain_vk.py --check-tools with PATH holding only the bundle's bin/ (with the translator of --tools). Windows: the files, and the lock's rows"""
    exe, rid, rows = PLATFORMS[platform]['exe'], RID[platform], bundle_rows(out)
    program = rows.get('python', 'no').split(' ', 1)[1] if rows.get('python', 'no') != 'no' else None
    lock = {}
    try: lock = dict(l.rstrip('\n').split('\t', 1) for l in (out / 'dm/gfx/tools/dmsh_chain_vk.lock').read_text().splitlines() if '\t' in l and not l.startswith('#'))
    except OSError: fails.append('dm/gfx/tools/dmsh_chain_vk.lock is missing')
    tools = [t for t in SPIRV_TOOLS if (out / f'bin/{t}{exe}').is_file()]
    if tools and len(tools) != len(SPIRV_TOOLS): fails.append(f'bin/ holds {", ".join(tools)} but not every SPIR-V tool of {", ".join(SPIRV_TOOLS)}')
    for row in sorted({r for r, _, _ in SPIRV_ROWS}) if tools else ():
        if not lock.get(f'{row}_{rid}'): fails.append(f'dm/gfx/tools/dmsh_chain_vk.lock has no {row}_{rid} row for the bundle\'s SPIR-V tools')
    penv = {k: v for k, v in env.items() if not k.startswith('PYTHON')}
    penv['PYTHONNOUSERSITE'] = '1'
    if platform == 'windows' or sys.platform != 'linux':
        if program:
            base = out / Path(program).parent
            want = [out / program, *sorted(base.glob('python3*.dll'))[:1], *sorted(base.glob('python3*.zip'))[:1], base / 'Lib/site-packages/lz4/block',
                    base / 'Lib/site-packages/capstone/lib', base / 'Lib/site-packages/fontTools/ttLib']
            missing = [p.relative_to(out).as_posix() for p in want if not p.exists()] + ([] if any(base.glob('python3*.zip')) else ['python/python3XY.zip'])
            if missing: fails.append('the bundle\'s Python lacks ' + ', '.join(missing))
            if any(base.glob('python3*._pth')): fails.append('python/ holds a ._pth file: Python would run isolated, without a script\'s folder on sys.path (the packer\'s tools need it)')
            say(f'check: the bundle\'s Python {program}: its runtime, lz4, capstone and fontTools are there, no ._pth file (not run here: a Windows program)')
        for t in tools:
            with open(out / f'bin/{t}{exe}', 'rb') as f: head = f.read(4096)
            pe = struct.unpack_from('<I', head, 0x3c)[0] if len(head) >= 0x40 else 0
            if not (head[:2] == b'MZ' and head[pe:pe + 4] == b'PE\0\0' and struct.unpack_from('<H', head, pe + 4)[0] == 0x8664 and struct.unpack_from('<H', head, pe + 24)[0] == 0x20b):
                fails.append(f'bin/{t}{exe} is not a PE32+ x86-64 program')
        if tools: say(f'check: the SPIR-V tools {", ".join(t + exe for t in tools)}: PE32+ x86-64 programs, the lock has their {rid} rows (not run here)')
        return
    if program:
        py = out / program
        r = run([py, '-c', PYTHON_PROBE], r'^python ', cwd=tmp, env=penv)
        outside = [l[5:] for l in (r.stdout.splitlines() if r else []) if l.startswith('path ') and l[5:] and not Path(l[5:]).resolve().is_relative_to(out.resolve())]
        if outside: fails.append(f'the bundle\'s Python has sys.path entries outside the bundle: {outside}')
        elif r and r.returncode == 0: say(f'check: the bundle\'s Python: every sys.path entry is inside the bundle (or the current folder)')
        run(['sh', out / 'pack.sh', '--help'], r'--platform', cwd=tmp, env=penv)   # what a player runs: pack.sh with the bundle's Python (no PYTHON set)
    for row, t, opt in SPIRV_ROWS if tools else ():
        want = lock.get(f'{row}_{rid}')
        r = run([out / f'bin/{t}', opt], r'\S', cwd=tmp, env=env)
        got = (r.stdout or r.stderr).strip().splitlines()[0].strip() if r and (r.stdout or r.stderr).strip() else 'nothing'
        if r and want and got != want: fails.append(f'bin/{t} {opt} prints {got!r}, not the {row}_{rid} row of the bundle\'s lock, {want!r}')
    xlate = out / 'bin/dmsh_xlate_vk'
    if tools and xlate.is_file():   # the packer's shader step's own check, with nothing of this machine's on PATH: the bundle's tools and translator only
        argv = [out / program if program else sys.executable, *([] if program else ['-I']), out / 'dm/gfx/tools/dmsh_chain_vk.py', '--check-tools']
        try: r = subprocess.run([str(a) for a in argv], capture_output=True, text=True, errors='replace', timeout=120, cwd=tmp,
                                env=dict(penv, PATH=str(out / 'bin'), XLATE_VK=str(xlate), DOTNET='/nonexistent'))
        except (OSError, subprocess.TimeoutExpired) as e: r = subprocess.CompletedProcess(argv, -1, '', str(e))
        if r.returncode: fails.append(f'dm/gfx/tools/dmsh_chain_vk.py --check-tools with PATH={out / "bin"}: exit {r.returncode}, {(r.stdout + r.stderr).strip()[-600:]!r}')
        else: say(f'check: dm/gfx/tools/dmsh_chain_vk.py --check-tools ({Path(argv[0]).name}): passed with PATH holding only the bundle\'s bin/ and XLATE_VK its translator')
    elif tools: say('check: dmsh_chain_vk.py --check-tools not run: the bundle has no translator (--tools)')


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
    ap.add_argument('--python', type=Path, help='the packer\'s own Python of dm/tools/publish_port_python.py for this platform: its python/ and licenses/ (python/ in the bundle)')
    ap.add_argument('--spirv', type=Path, help='the SPIR-V tools of docker/build_spirv_tools.sh for this platform, with their licenses/ (bin/ in the bundle)')
    ap.add_argument('--out', required=True, type=Path, help='the bundle folder; its name is the archive\'s top folder')
    ap.add_argument('--archive', action='store_true', help='also write <out>.tar.gz (Linux) or <out>.zip (Windows) beside it')
    ap.add_argument('--check', action='store_true', help='run the bundle\'s smoke test')
    a = ap.parse_args(argv)
    try:
        out = a.out.resolve()
        _, ts = assemble(a.platform, a.build, a.aot, out, a.tools, a.python, a.spirv)
        if a.archive: archive(a.platform, out, ts)
        if a.check:
            fails = check(a.platform, out, [a.build, a.aot, a.tools, a.python, a.spirv])
            if fails: raise PackError('the check failed:\n  ' + '\n  '.join(fails))
            say('check: passed')
    except PackError as e:
        print(f'[RED] package_port: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
