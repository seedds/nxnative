"""NSP script Mac host (build_ipa_from_nsp.build_mac_host): every static library on the host's link line is built before the link line is run, and a library the Makefile gains
needs no change in the script. A stand-in dm/host/Makefile with a profile header, a host_msl target, libraries and a link rule runs under the real make; the link line (clang) is only recorded,
with the state of its libraries at that moment. No game files, keys, Xcode or GPU needed."""
import importlib.util
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from types import SimpleNamespace as NS
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

MAKEFILE = '''LIBS=%s
.PHONY: host_msl
host_msl:
gen/$(GAME)/game_profile.h:
\t@mkdir -p $(dir $@); touch $@
gen/%%.a:
\t@mkdir -p $(dir $@); touch $@
dmhost: gen/$(GAME)/game_profile.h $(LIBS)
\tclang -o dmhost main.c $(LIBS) -framework Cocoa
'''


class MacHostLibraries(unittest.TestCase):
    def build(self, libs):
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp)
        host = tmp / 'repo/dm/host'
        for d in nsp.HOST_DIRS: (tmp / 'repo' / d).mkdir(parents=True, exist_ok=True)
        (host / 'Makefile').write_text(MAKEFILE % ' '.join(libs))
        (tmp / 'logs').mkdir()
        links, real_run = [], nsp.run

        def run(cmd, log, **kw):   # make runs for real; the link line is only recorded
            if cmd[0] == 'make': return real_run(cmd, log, **kw)
            links.append([(a, (host / a).is_file()) for a in map(str, cmd) if a.endswith('.a')])
            Path(cmd[cmd.index('-o') + 1]).write_bytes(b'host')

        with patch.object(nsp, 'apple_silicon', return_value=True), patch.object(nsp, 'run', side_effect=run):
            nsp.build_mac_host(tmp / 'repo', tmp / 'out/dmhost', dict(os.environ), tmp / 'logs', NS(id='T'))
        return links

    def test_builds_every_library_the_link_line_names(self):
        self.assertEqual(self.build(['gen/vpx/libvpx.a', 'gen/dav1d/libdav1d.a', 'gen/next/libnext.a']),
                         [[('gen/vpx/libvpx.a', True), ('gen/dav1d/libdav1d.a', True), ('gen/next/libnext.a', True)]])

    def test_a_link_line_without_libraries_builds_none(self):
        self.assertEqual(self.build([]), [[]])


if __name__ == '__main__': unittest.main()
