#!/bin/sh
# The port's prebuilt tools for players (docs/development.md, "The port's bundles"): nx_extract (dm/tools/nx_extract, which the packer extracts the NSPs with) and the Vulkan
# shader translator dmsh_xlate_vk (dm/gfx/tools/dmsh_xlate_vk, which dmsh_build_vk.py runs for an NVN game's shaders), published as programs that run on a PC with no .NET
# installed, with the licences of what is linked into them. Run in the chain image (docker/Dockerfile.chain, which has the .NET SDK and, for Native AOT, clang), from the
# repository root, one platform per run:
#   docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-chain sh dm/tools/publish_port_tools.sh linux build/port_tools/linux-x64
#   docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-chain sh dm/tools/publish_port_tools.sh windows build/port_tools/win-x64
# It writes <out>/nx_extract and <out>/dmsh_xlate_vk (nx_extract.exe and dmsh_xlate_vk.exe for windows) and <out>/licenses/, which scripts/package_port.py --tools puts into
# a bundle (bin/ and licenses/). The forms (measured on 2026-11-15: docker/README.md, "The port's tools"):
#   linux    Native AOT, both: one ELF each that needs libc and libm (nx_extract loads OpenSSL 3, libssl.so.3, at run time for its hashes, as .NET does on Linux)
#   windows  Native AOT cannot be built from Linux, so a self-contained, trimmed single file; the translator with ReadyToRun code too (it starts once per batch of shaders,
#            and the JIT's start-up cost dominated: 0.17 s against 0.61 s for the 40 test shaders, measured on the linux-x64 build of the same form)
# Both are deterministic: the same source and image give the same bytes. The translator is fingerprinted in dm/gfx/tools/dmsh_chain_vk.lock (dmsh_xlate_vk_<platform>), so it is
# published with the artifacts path of docker/README.md's dotnet build (build/xlate_vk: its PathMap and the lock assume it); --check stops (exit 1) when it does not match the lock.
# The intermediate files go to build/xlate_vk and build/port_tools/artifacts. Exit status 0 when done, 1 on an error, 2 on a usage error.
usage() { echo "usage: sh dm/tools/publish_port_tools.sh <linux|windows> <out folder> [--check]" >&2; exit 2; }
die() { echo "publish_port_tools: [RED] $*" >&2; exit 1; }
say() { echo "publish_port_tools: $*"; }
[ $# -ge 2 ] && [ $# -le 3 ] || usage
platform=$1 out=$2 check=
[ $# -eq 3 ] && { [ "$3" = --check ] || usage; check=1; }
case $platform in
  linux) rid=linux-x64 exe= ;;
  windows) rid=win-x64 exe=.exe ;;
  *) usage ;;
esac
cd "$(dirname "$0")/../.." || die "cannot enter the repository root"
[ -f dm/tools/nx_extract/nx_extract.csproj ] || die "run from a checkout of the repository: no dm/tools/nx_extract here"
command -v dotnet > /dev/null || die "no dotnet: run in the chain image (docker/Dockerfile.chain)"
export PYTHONDONTWRITEBYTECODE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
nuget=${NUGET_PACKAGES:-$HOME/.nuget/packages}
runtime=$(dotnet --list-runtimes | awk '$1 == "Microsoft.NETCore.App" { v = $2 } END { print v }')
[ -n "$runtime" ] || die "dotnet lists no Microsoft.NETCore.App runtime"
tmp=$(mktemp -d) || die "mktemp"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out/licenses" || die "cannot create $out"

# The publish options: the commit stays out of the assemblies (Source Link would put HEAD's into their informational version) and no PDB, so no path is in the program
common="-c Release -r $rid -nologo -p:IncludeSourceRevisionInInformationalVersion=false -p:DebugType=none"
if [ "$platform" = linux ]; then form="-p:PublishAot=true" xlate_form=$form
else form="--self-contained true -p:PublishSingleFile=true -p:PublishTrimmed=true" xlate_form="$form -p:PublishReadyToRun=true"
fi

publish() {   # publish <name> <project> <artifacts path> <options...>: the program alone into $out, else an error naming what else the publish wrote
  name=$1 proj=$2 art=$3; shift 3
  say "$name for $rid: dotnet publish $proj $common $*"
  # shellcheck disable=SC2086
  dotnet publish "$proj" $common "$@" --artifacts-path "$art" -o "$tmp/$name" > "$tmp/$name.log" 2>&1 || { tail -40 "$tmp/$name.log" >&2; die "dotnet publish $proj failed"; }
  grep -E 'warning (IL|NU)[0-9]+' "$tmp/$name.log" | sed 's/^/  /' | sort -u
  extra=$(ls -A "$tmp/$name" | grep -vx "$name$exe")
  [ -z "$extra" ] || die "the $name publish wrote more than the program, which it would need beside it: $extra"
  cp "$tmp/$name/$name$exe" "$out/$name$exe" && chmod 755 "$out/$name$exe" || die "cannot copy $name$exe to $out"
}
publish nx_extract dm/tools/nx_extract build/port_tools/artifacts $form
publish dmsh_xlate_vk dm/gfx/tools/dmsh_xlate_vk build/xlate_vk -m:1 $xlate_form

# What the programs are: run here (linux), or their PE headers (windows: they cannot run here)
source_sha=$(python3 -c 'import sys; sys.path.insert(0, "dm/gfx/tools"); import dmsh_chain_vk; print(dmsh_chain_vk.xlate_source_sha())') || die "dmsh_chain_vk.py"
if [ "$platform" = linux ]; then
  "$out/nx_extract" > /dev/null 2> "$tmp/usage"; rc=$?
  [ $rc -eq 2 ] && grep -q '^usage: nx_extract' "$tmp/usage" || die "$out/nx_extract does not print its usage (exit $rc)"
  got=$("$out/dmsh_xlate_vk" --source-sha) || die "$out/dmsh_xlate_vk does not run"
  [ "$got" = "$source_sha" ] || die "$out/dmsh_xlate_vk --source-sha gives $got, the source in the repository $source_sha"
else
  python3 - "$out/nx_extract.exe" "$out/dmsh_xlate_vk.exe" <<'EOF' || die "not a Windows x86-64 program"
import struct, sys
for p in sys.argv[1:]:
    b = open(p, 'rb').read(4096); pe = struct.unpack_from('<I', b, 0x3c)[0]
    ok = b[:2] == b'MZ' and b[pe:pe + 4] == b'PE\0\0' and struct.unpack_from('<H', b, pe + 4)[0] == 0x8664 and struct.unpack_from('<H', b, pe + 24)[0] == 0x20b
    print(f'  {p}: ' + ('PE32+, x86-64' if ok else 'not a PE32+ x86-64 program')); ok or sys.exit(1)
EOF
fi
if [ -n "$check" ]; then python3 dm/gfx/tools/dmsh_chain_vk.py --check-published "$rid" "$out/dmsh_xlate_vk$exe" || exit 1; fi

# The licences: the .NET runtime compiled or bundled into both (the runtime pack's licence and third-party notices), Ryujinx's shader translator, and every NuGet package the
# two projects restore (their project.assets.json, what dotnet list package --include-transitive lists): its licence file, or the text of its licence expression with its
# copyright line. Ryujinx.Common's own dependencies (MsgPack.Cli, Humanizer, Gommon, Microsoft.IO.RecyclableMemoryStream, System.CodeDom) are not in the programs, which
# trimming leaves without their code (checked on 2026-11-15: ILLink's output and the AOT compiler's map); their licences are written all the same. The compilers' own packages
# (ILCompiler, ILLink) put no code of theirs in a program beyond the runtime's
python3 - "$platform" "$rid" "$runtime" "$nuget" "$out/licenses" build/port_tools/artifacts/obj/nx_extract/project.assets.json \
    build/xlate_vk/obj/dmsh_xlate_vk/project.assets.json <<'EOF' || die "the licences"
import json, os, re, shutil, sys
platform, rid, runtime, nuget, dst, *assets = sys.argv[1:]
MIT = ('Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the '
       'Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, '
       'and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\n\nThe above copyright notice and this permission notice shall be '
       'included in all copies or substantial portions of the Software.\n\nTHE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT '
       'LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY '
       'CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER '
       'DEALINGS IN THE SOFTWARE.\n')
TOOLS = ('microsoft.dotnet.ilcompiler', 'runtime.linux-x64.microsoft.dotnet.ilcompiler', 'microsoft.net.illink.tasks', 'microsoft.netcore.app.crossgen2.linux-x64')   # compilers
# Packages whose own metadata names no licence: the licence of their source repository (Gommon: https://github.com/GreemDev/Gommon, LICENSE, MIT)
KNOWN = {'gommon': ('MIT', 'Copyright (c) 2019- Evan Husted')}
FAMILY = lambda n: 'Humanizer' if n.lower().startswith('humanizer') else n   # Humanizer.Core and its language packages are one project
written = {}
def put(name, text):
    with open(os.path.join(dst, name), 'w', newline='\n') as f: f.write(text)
    written[name] = len(text)
packs = [f'microsoft.netcore.app.runtime.nativeaot.{rid}'] if platform == 'linux' else [f'microsoft.netcore.app.runtime.{rid}', f'microsoft.netcore.app.host.{rid}']
lic = os.path.join(nuget, packs[0], runtime, 'LICENSE.TXT')
put('dotnet-runtime.txt', f'.NET runtime {runtime} ({packs[0]}), compiled or bundled into nx_extract and dmsh_xlate_vk\n\n' + open(lic, encoding='utf-8-sig').read())
notices = [open(os.path.join(nuget, p, runtime, 'THIRD-PARTY-NOTICES.TXT'), encoding='utf-8-sig').read() for p in packs]
if any(n != notices[0] for n in notices): sys.exit(f'the third-party notices of {", ".join(packs)} differ: write each')
put('dotnet-runtime-THIRD-PARTY-NOTICES.txt', notices[0])
put('ryujinx.txt', open('dm/third_party/ryujinx/LICENSE.txt', encoding='utf-8-sig').read())   # the name the bundle gives dm/third_party/ryujinx's licence (scripts/package_port.py)
pkgs = {}
for a in assets:
    for k, v in json.load(open(a))['libraries'].items():
        if v['type'] == 'package': n, ver = k.split('/'); pkgs[(n, ver)] = v['path']
texts = {}
for (n, ver), path in sorted(pkgs.items(), key=lambda x: x[0][0].lower()):
    if n.lower() in TOOLS: continue
    d = os.path.join(nuget, path); spec = open(os.path.join(d, n.lower() + '.nuspec'), encoding='utf-8-sig').read()
    tag = lambda t: (re.search(rf'<{t}[^>]*>([^<]*)</{t}>', spec) or [None, ''])[1].strip()
    copyright, expr, url = tag('copyright'), tag('license') if 'type="expression"' in spec else '', tag('licenseUrl')
    files = [f for f in os.listdir(d) if re.fullmatch(r'(LICEN[CS]E|COPYING)(\.(txt|md))?', f, re.I)]
    head = f'{n} {ver} (NuGet), {url or expr or "licence of its repository"}\n\n'
    if files: body = open(os.path.join(d, files[0]), encoding='utf-8-sig', errors='replace').read()
    elif expr == 'MIT': body = f'MIT License\n\n{copyright}\n\n{MIT}'
    elif 'apache.org/licenses/LICENSE-2.0' in url: body = f'{copyright}\n\n' + open('/usr/share/common-licenses/Apache-2.0').read()
    elif n.lower() in KNOWN: body = f'{KNOWN[n.lower()][0]} License\n\n{KNOWN[n.lower()][1]}\n\n{MIT}'
    else: sys.exit(f'{n} {ver}: its package names no licence this script knows ({expr or url or "none"}): add it to KNOWN with its repository\'s licence')
    texts.setdefault(FAMILY(n), head + body)
for name, text in texts.items(): put(f'{name}.txt', text)
print(f'  licenses: {len(written)} files ({", ".join(sorted(written))})')
EOF
for p in nx_extract dmsh_xlate_vk; do say "$out/$p$exe: $(wc -c < "$out/$p$exe") bytes, sha256 $(sha256sum < "$out/$p$exe" | cut -c1-16)"; done
