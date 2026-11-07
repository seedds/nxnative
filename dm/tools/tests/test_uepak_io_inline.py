"""Unreal Engine 4.27 IoStore containers whose cooked packages keep their shaders inline (Tetris Effect: Connected): uepak.io_inline_tocs finds them and shader_split.io_inline_shaders
scans their chunks and the global shader cache in the pak beside them. Synthetic containers and paks; no game files or keys."""
from pathlib import Path
import contextlib
import io
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
sys.path.insert(0, str(REPO / 'dm/tools/tests'))
import uepak
from test_uepak_iostore import chunk_id, container, pak11, ss


BUNDLE = uepak.IO_EXPORT_BUNDLE_427


class FakePak:
    """uepak.Pak as io_inline_shaders uses it: the file names and their bytes"""
    def __init__(self, files): self.files = files
    def read(self, name): return self.files[name]


class IoInline(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.romfs = Path(tmp.name) / 'RomFS'; self.content = self.romfs / 'Game/Content/Paks'

    def test_tocs(self):
        """A version 3 container with an inline shader record counts; one without, a UE 5 one (version 4 and later: shader libraries) and an encrypted one do not"""
        inline = container(self.content, 'Game-Switch.utoc', [(chunk_id(1, BUNDLE), b'header'), (chunk_id(2, BUNDLE), b'..LSLG..')], version=3)
        container(self.content, 'NoShaders.utoc', [(chunk_id(1, BUNDLE), b'header')], version=3)
        container(self.content, 'Ue5.utoc', [(chunk_id(1, BUNDLE), b'..LSLG..')], version=8)
        container(self.content, 'Locked.utoc', [(chunk_id(1, BUNDLE), b'..LSLG..')], version=3, encrypted=True)
        self.assertEqual(uepak.io_inline_tocs(str(self.romfs)), [str(inline)])

    def scan(self, paks=(), inline_paks=(), files=None):
        seen = []
        with patch.object(ss, 'ue_inline_shaders', side_effect=lambda d, label, out: (seen.append((d, label)), out.append((label, 'ps', b'', d)))), \
             patch.object(uepak, 'unreal_paks', side_effect=lambda romfs, inline=None: list(inline_paks if inline else paks)), \
             patch.object(uepak, 'footer', return_value=(0, 0, 0, [], 11)), \
             patch.object(uepak, 'Pak', side_effect=lambda p, key=None: FakePak(files or {})):
            out = ss.io_inline_shaders(str(self.romfs), 'unused-main', 'v1')
        return seen, out

    def test_scans_chunks_and_global_cache(self):
        container(self.content, 'Game-Switch.utoc', [(chunk_id(1, BUNDLE), b'header'), (chunk_id(2, BUNDLE), b'..LSLG..')], version=3)
        pak = self.content / 'Game-Switch.pak'; pak11(pak, ['Engine/GlobalShaderCache-SF_NVN.bin'])
        files = {'Engine/GlobalShaderCache-SF_NVN.bin': b'LSLG-global', 'Engine/Config/a.ini': b'[x]'}
        seen, out = self.scan(paks=[str(pak)], files=files)
        self.assertEqual(seen, [(b'..LSLG..', f'/v1/Game/Content/Paks/Game-Switch.utoc#{chunk_id(2, BUNDLE).hex()}'),
                                (b'LSLG-global', '/v1/Game/Content/Paks/Game-Switch.pak#Engine/GlobalShaderCache-SF_NVN.bin')])
        self.assertEqual(len(out), 2)

    def test_containers_beside_an_inline_pak_left_to_ue_legacy_jobs(self):
        """A container beside a pak that ue_legacy_jobs reads is scanned there (UE_IO), not again here"""
        container(self.content, 'Game-Switch.utoc', [(chunk_id(1, BUNDLE), b'..LSLG..')], version=3)
        seen, out = self.scan(inline_paks=[str(self.content / 'Game-Switch.pak')])
        self.assertEqual((seen, out), ([], []))

    def test_no_containers(self):
        self.assertEqual(self.scan(), ([], []))

    def test_unreadable_global_cache_is_a_red_line(self):
        container(self.content, 'Game-Switch.utoc', [(chunk_id(1, BUNDLE), b'..LSLG..')], version=3)
        pak = self.content / 'Game-Switch.pak'; pak.parent.mkdir(parents=True, exist_ok=True); pak.write_bytes(b'')
        said = io.StringIO()
        with contextlib.redirect_stdout(said), patch.object(ss, 'ue_inline_shaders', side_effect=lambda d, label, out: out.append(label)), \
             patch.object(uepak, 'unreal_paks', side_effect=lambda romfs, inline=None: [] if inline else [str(pak)]):
            out = ss.io_inline_shaders(str(self.romfs), 'unused-main')
        self.assertEqual(len(out), 1)   # the container's record is kept
        self.assertIn('[RED] /Game/Content/Paks/Game-Switch.pak: the global shader cache cannot be read', said.getvalue())


if __name__ == '__main__':
    unittest.main()
