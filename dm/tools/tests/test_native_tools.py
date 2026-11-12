"""The repository's two C# tools are committed as native universal executables (dm/tools/native_tools.sh, NativeAOT), so building an IPA needs no .NET (GitHub issue #1853):
the shader translator dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate and the extractor dm/tools/nx_extract_bin/nx_extract. Each must hold both Mac architectures (an Intel Mac builds IPAs
too: GitHub issues #54 and #803), and each must be built from the sources beside it, which it embeds and names with --source-sha: the translator also matches dmsh_chain.lock.
Neither may import a function weakly: that is one newer than the executable's minimum macOS, which binds to 0 on a Mac without it (GitHub issue #2190)."""
from pathlib import Path
import hashlib
import importlib.util
import struct
import subprocess
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
XLATE = REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
NX = REPO / 'dm/tools/nx_extract_bin/nx_extract'
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain   # noqa: E402


def macho_archs(data):
    magic, n = struct.unpack_from('>II', data, 0)
    assert magic == 0xCAFEBABE, 'not a universal binary'
    return sorted({0x01000007: 'x86_64', 0x0100000C: 'arm64'}.get(struct.unpack_from('>I', data, 8 + 20 * i)[0], '?') for i in range(n))


def weak_imports(data):
    """{architecture: the undefined symbols marked N_WEAK_REF} of a universal binary's 64-bit little-endian slices, as nm -m -u names them 'weak external'"""
    out = {}
    for i in range(struct.unpack_from('>I', data, 4)[0]):
        cpu, _, off, _, _ = struct.unpack_from('>5I', data, 8 + 20 * i)
        assert struct.unpack_from('<I', data, off)[0] == 0xFEEDFACF, 'not a 64-bit Mach-O slice'
        ncmds, weak, at = struct.unpack_from('<I', data, off + 16)[0], [], off + 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, at)
            if cmd == 2:   # LC_SYMTAB
                symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, at + 8)
                for k in range(nsyms):
                    strx, n_type, _, n_desc, _ = struct.unpack_from('<IBBHQ', data, off + symoff + 16 * k)
                    if n_type & 0xE0 == 0 and n_type & 0x0E == 0 and n_desc & 0x40:   # not a debug entry, N_UNDF, N_WEAK_REF
                        weak.append(data[off + stroff + strx:data.index(b'\0', off + stroff + strx)].decode())
            at += size
        out[{0x01000007: 'x86_64', 0x0100000C: 'arm64'}.get(cpu, '?')] = sorted(weak)
    return out


def nsp_script():
    spec = importlib.util.spec_from_file_location('build_ipa_from_nsp_native', REPO / 'scripts/build_ipa_from_nsp.py')
    assert spec and spec.loader
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


class NativeTools(unittest.TestCase):
    def test_universal(self):
        for exe in (XLATE, NX):
            with self.subTest(exe=exe.name):
                self.assertEqual(macho_archs(exe.read_bytes()), ['arm64', 'x86_64'])
                self.assertTrue(exe.stat().st_mode & 0o111, f'{exe} is not executable')

    def test_no_weak_imports(self):   # the arm64 translator of 641636e2 called pipe2 (macOS 27) unchecked and faulted at address 0 on macOS 26
        for exe in (XLATE, NX):
            with self.subTest(exe=exe.name):
                self.assertEqual(weak_imports(exe.read_bytes()), {'arm64': [], 'x86_64': []},
                                 f"{exe.name} imports functions its minimum macOS does not have: rebuild it with Microsoft's .NET SDK (dm/tools/native_tools.sh {exe.name})")

    def test_only_the_executable_is_committed(self):   # no dlls, runtimeconfig or satellite folders of a framework-dependent build left beside it
        for exe in (XLATE, NX):
            with self.subTest(exe=exe.name): self.assertEqual(sorted(p.name for p in exe.parent.iterdir() if not p.name.startswith('.')), [exe.name])

    def test_projects_publish_native(self):
        for proj in ('dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj', 'dm/tools/nx_extract/nx_extract.csproj'):
            with self.subTest(proj=proj): self.assertIn('<PublishAot>true</PublishAot>', (REPO / proj).read_text())

    def test_translator_matches_the_lock(self):   # dmsh_build.py refuses another translator (tool_lock.py); this says so in CI, before any game
        with open(dmsh_chain.LOCK) as f: lock = dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))
        self.assertEqual(hashlib.sha256(XLATE.read_bytes()).hexdigest()[:16], lock.get('dmsh_xlate'), 'rebuilt translator: tool_lock.py --update-lock and dmsh_chain.py --header')
        self.assertEqual(lock.get('xlate_source'), dmsh_chain.xlate_source_sha(), 'dmsh_xlate sources changed: rebuild it with dm/tools/native_tools.sh dmsh_xlate')

    @unittest.skipUnless(sys.platform == 'darwin', 'runs the Mac executables')
    def test_built_from_the_sources_beside_them(self):
        want = {XLATE: dmsh_chain.xlate_source_sha(), NX: nsp_script().nx_extract_source_sha(REPO)}
        for exe, sha in want.items():
            with self.subTest(exe=exe.name):
                got = subprocess.run([str(exe), '--source-sha'], capture_output=True, text=True, timeout=60).stdout.strip()
                self.assertEqual(got, sha, f'{exe.name} was not built from its in-repo sources: rebuild it with dm/tools/native_tools.sh {exe.name}')


if __name__ == '__main__': unittest.main()
