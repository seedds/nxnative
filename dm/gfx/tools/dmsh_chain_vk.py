#!/usr/bin/env python3
# Translation chain version of the Vulkan shader libraries, DMSH_CHAIN_VK (docs/design/vulkan-backend.md, 2.4): what DMSH_CHAIN (dmsh_chain.py) is for Metal, in a file of its own,
# because dmsh_chain.py hashes its own bytes into DMSH_CHAIN, so a Vulkan option there would change the Metal version (the reason chain_tools.py sits beside it, step M0).
# V = the first 12 digits of the sha256 of the files in FILES + the translation options (dmsh_chain.py's: XLATE_DEBUG, XLATE_REDUCED): this script, the build script, the SPIR-V
# passes, the Vulkan translator's sources, the Metal chain modules the build takes code or decisions from (dmsh_build.py's analysis functions, dmsh_chain.py's inputs digest and
# stamp, dmsh_facts.py, dmsh_atomic.py, c1_guard.py, cb_extent.py), the Ryujinx sources of DMSH_CHAIN's list, and the tool lock dmsh_chain_vk.lock. A change to a shared file
# changes both versions; a Vulkan-only change changes this one alone.
# The build (dmsh_build_vk.py) writes into <cache dir>/vk-<V>/ and each reflection json records chain = V; the Vulkan host takes V from dm/gfx/nvn/dmsh_chain_vk.h and loads
# only from vk-<V>/. The profile inputs digest and the stamp (vk-<V>/inputs.tsv) are dmsh_chain.py's: one digest for both targets.
# Usage: dmsh_chain_vk.py                 print V
#        dmsh_chain_vk.py --header        rewrite dm/gfx/nvn/dmsh_chain_vk.h (after changing any file in FILES; commit it with the change: dm/tools/tests/test_nsp_chain.py checks it)
#        dmsh_chain_vk.py --inputs        print the digest of the current game profile's inputs (DM_GAME), as dmsh_chain.py --inputs does
#        dmsh_chain_vk.py --check-tools   verify that the tools match the lock: the translator's dlls (a published translator: its program, a row of its platform) and embedded
#                                         source hash, and this platform's SPIRV-Tools, glslang and SPIRV-Cross (rows named <tool>_<platform>); on a mismatch print [RED] lines and exit 1
#        dmsh_chain_vk.py --update-lock   rewrite the lock from the tools here (in the chain image, docker/Dockerfile.chain), keeping other platforms' rows; commit it. It takes the
#                                         published translators too (XLATE_VK_PUBLISHED), and stops without them: the port's bundles carry them
#        dmsh_chain_vk.py --check-published <platform> <program>   a published translator against its row of the lock (dm/tools/publish_port_tools.sh --check); exit 1 on a mismatch
# Environment: XLATE_VK (the translator to check and run: the dmsh_xlate_vk.dll of a dotnet build, default the build output of docker/README.md, run with DOTNET; or a published
#   translator, dmsh_xlate_vk or dmsh_xlate_vk.exe of dm/tools/publish_port_tools.sh, run as it is), XLATE_VK_PUBLISHED (--update-lock: a folder holding the published translator
#   of each platform of PUBLISHED as <platform>/dmsh_xlate_vk[.exe]), SPIRV_CROSS_VK (default spirv-cross on PATH), DOTNET (default dotnet)
import hashlib, os, platform, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); GFX = os.path.dirname(HERE); REPO = os.path.dirname(os.path.dirname(GFX))
sys.path.insert(0, HERE); import dmsh_chain   # noqa: E402
FILES = ['tools/dmsh_chain_vk.py', 'tools/dmsh_build_vk.py', 'tools/dmsh_spv.py', 'tools/dmsh_xlate_vk/Program.cs', 'tools/dmsh_xlate_vk/dmsh_xlate_vk.csproj',
         'tools/dmsh_build.py', 'tools/dmsh_chain.py', 'tools/dmsh_facts.py', 'tools/dmsh_atomic.py', 'tools/c1_guard.py', 'tools/cb_extent.py']
FILES += [f for f in dmsh_chain.FILES if f.startswith('../third_party/ryujinx/')] + ['tools/dmsh_chain_vk.lock']
LOCK = os.path.join(HERE, 'dmsh_chain_vk.lock'); HEADER = os.path.join(GFX, 'nvn/dmsh_chain_vk.h')
PREFIX = 'vk-'   # the library directory <cache dir>/vk-<V>/
XLATE_VK = os.environ.get('XLATE_VK', os.path.join(REPO, 'build/xlate_vk/bin/dmsh_xlate_vk/release/dmsh_xlate_vk.dll'))
DLLS = ('dmsh_xlate_vk.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll')
# The platforms the translator is published for (dm/tools/publish_port_tools.sh: Native AOT for linux-x64, a trimmed self-contained single file for win-x64), each fingerprinted
# by its program in the row dmsh_xlate_vk_<platform>: a published program is the dlls compiled ahead of time with the runtime, so its bytes are its identity
PUBLISHED = {'linux-x64': 'dmsh_xlate_vk', 'win-x64': 'dmsh_xlate_vk.exe'}


def published():   # whether the translator (XLATE_VK) is a published program rather than the dll of a dotnet build
    return not XLATE_VK.lower().endswith('.dll')


def xlate_cmd():   # the command that runs the translator: a published program as it is, a dll with dotnet
    return [XLATE_VK] if published() else [os.environ.get('DOTNET', 'dotnet'), XLATE_VK]


def version():
    h = hashlib.sha256()
    for f in FILES:
        with open(os.path.join(GFX, f), 'rb') as fh: h.update(f.encode() + b'\0' + fh.read() + b'\0')
    h.update(dmsh_chain.options().encode())
    return h.hexdigest()[:12]


def plat():   # linux-x64, win-x64, osx-arm64, ...: the suffix of a tool row that holds only on one platform
    os_ = {'linux': 'linux', 'win32': 'win', 'darwin': 'osx'}.get(sys.platform, sys.platform)
    arch = {'x86_64': 'x64', 'amd64': 'x64', 'arm64': 'arm64', 'aarch64': 'arm64'}.get(platform.machine().lower(), platform.machine().lower())
    return f'{os_}-{arch}'


def spirv_cross():
    return os.environ.get('SPIRV_CROSS_VK') or shutil.which('spirv-cross') or 'spirv-cross'


def _first_line(cmd):   # a tool's first output line, 'missing' when it cannot start or prints nothing
    try: r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except OSError: return 'missing'
    return ((r.stdout or r.stderr).strip().splitlines() or ['missing'])[0].strip()


def tools_now():
    """The tool fingerprints here: the translator's dlls (as dmsh_chain.py takes the Metal one's), or a published translator's program (its platform's row), its embedded source
    hash, and this platform's SPIRV-Tools, glslang and SPIRV-Cross (their version lines: --revision for SPIRV-Cross, built per OS from the tag dm/gfx/tools/spirv_cross_bin/build.sh pins)"""
    d, t, p = os.path.dirname(XLATE_VK), {}, plat()
    if published(): t['dmsh_xlate_vk_' + p] = dmsh_chain.sha16(XLATE_VK) if os.path.isfile(XLATE_VK) else 'missing'
    else:
        for dll in DLLS:
            f = os.path.join(d, dll)
            t[dll] = dmsh_chain.sha16(f) if os.path.isfile(f) else 'missing'
    t['xlate_source'] = _first_line([*xlate_cmd(), '--source-sha']) if os.path.isfile(XLATE_VK) else 'missing'
    t['spirv-tools_' + p] = _first_line(['spirv-val', '--version'])
    t['glslang_' + p] = _first_line(['glslangValidator', '--version'])
    t['spirv-cross_' + p] = _first_line([spirv_cross(), '--revision'])
    return t


def xlate_source_sha():   # as dmsh_xlate_vk --source-sha: the embedded Program.cs and csproj concatenated in that order
    b = b''
    for f in ('Program.cs', 'dmsh_xlate_vk.csproj'):
        with open(os.path.join(HERE, 'dmsh_xlate_vk', f), 'rb') as fh: b += fh.read()
    return hashlib.sha256(b).hexdigest()[:16]


def lock():
    with open(LOCK) as f: return dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))


def check_tools():
    want, now = lock(), tools_now()
    bad = [f'{k}: lock {want.get(k)} / actual {v}' for k, v in now.items() if want.get(k) != v]
    if now['xlate_source'] != xlate_source_sha(): bad.append(f'the translator was not built from the in-repo source: embedded source {now["xlate_source"]} / in repo {xlate_source_sha()} (rebuild dmsh_xlate_vk)')
    return bad


def header_text(v):
    return (f'// Generated file (dm/gfx/tools/dmsh_chain_vk.py --header): the Vulkan shader library translation chain version this source tree expects (default options), one for every\n'
            f'// game; the Vulkan host loads only <library dir>/vk-<V>/. Run dmsh_chain_vk.py --header after changing the chain\'s tools and commit this file with the change\n'
            f'#define DMSH_CHAIN_VK "{v}"\n')


def published_rows(folder):
    """{dmsh_xlate_vk_<platform>: fingerprint} of the published translators in folder (<platform>/<program> for each platform of PUBLISHED); the one of this platform must print the
    in-repo source hash (the others cannot run here)"""
    rows = {}
    for p, name in PUBLISHED.items():
        f = os.path.join(folder, p, name)
        if not os.path.isfile(f): sys.exit(f'[RED] no published translator {f}: run dm/tools/publish_port_tools.sh for {p} into {os.path.join(folder, p)} first')
        if p == plat() and _first_line([f, '--source-sha']) != xlate_source_sha(): sys.exit(f'[RED] {f} was not built from the in-repo source: publish it again')
        rows['dmsh_xlate_vk_' + p] = dmsh_chain.sha16(f)
    return rows


if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['--update-lock']:
        if published(): sys.exit('[RED] --update-lock takes the dlls of a dotnet build (XLATE_VK names a .dll, by default build/xlate_vk), and the published translators through XLATE_VK_PUBLISHED')
        if not os.environ.get('XLATE_VK_PUBLISHED'): sys.exit('[RED] XLATE_VK_PUBLISHED is not set: the lock records the published translators too (dm/tools/publish_port_tools.sh, docker/README.md)')
        t = tools_now()
        if t['xlate_source'] != xlate_source_sha(): sys.exit(f'[RED] the translator was not built from the in-repo source (embedded {t["xlate_source"]} / in repo {xlate_source_sha()}): rebuild dmsh_xlate_vk first')
        if 'missing' in t.values(): sys.exit('[RED] a tool is missing here (' + ', '.join(k for k, v in t.items() if v == 'missing') + '): update the lock in the chain image (docker/Dockerfile.chain)')
        t.update(published_rows(os.environ['XLATE_VK_PUBLISHED']))
        old = lock() if os.path.exists(LOCK) else {}
        rows = {**{k: v for k, v in old.items() if k not in t and '_' in k and k.rsplit('_', 1)[1] != plat()}, **t}   # other platforms' rows stay
        with open(LOCK, 'w', newline='\n') as f:
            f.write('# Vulkan translation chain tool fingerprints (written by dmsh_chain_vk.py --update-lock in the chain image, docker/Dockerfile.chain; regenerate after changing\n'
                    '# a tool and commit it). The dlls hold on every OS; a row named <tool>_<platform> only on that platform (dmsh_xlate_vk_<platform>: the published translator)\n'
                    + ''.join(f'{k}\t{v}\n' for k, v in rows.items()))
        print('wrote', LOCK)
    elif len(a) == 3 and a[0] == '--check-published':
        p, f = a[1], a[2]
        want, have = lock().get('dmsh_xlate_vk_' + p), dmsh_chain.sha16(f) if os.path.isfile(f) else 'missing'
        if want != have: sys.exit(f'[RED] the published translator {f} does not match the lock: dmsh_xlate_vk_{p} lock {want} / actual {have} (if the translator or the SDK changed, '
                                  'run dmsh_chain_vk.py --update-lock with XLATE_VK_PUBLISHED and commit the lock and the header: docker/README.md)')
        print(f'{f}: dmsh_xlate_vk_{p} {have}, as the lock')
    elif a == ['--check-tools']:
        bad = check_tools()
        for b in bad: print('[RED] Vulkan translation chain tools do not match the lock file: ' + b)
        sys.exit(1 if bad else 0)
    elif a == ['--header']:
        with open(HEADER, 'w', newline='\n') as f: f.write(header_text(version()))
        print(version())
    elif a == ['--inputs']: print(dmsh_chain.inputs(dmsh_chain.load_game()))
    elif not a: print(version())
    else: sys.exit('usage: see the file header')
