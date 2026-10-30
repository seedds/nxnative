"""game.py profile: the runtime profile file the host reads from DM_PROFILE (dm/host/hle_profile.c set_kv) carries every key the host reads, with the "none" spellings
for a key the game leaves out (the host keeps the compiled game's value for a key the file lacks, so a second game's file must say it has none). No game files needed."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game  # pyright: ignore[reportMissingImports]  (dm/games, put on sys.path above)

KEYS = ('title_id', 'program_id', 'modules', 'stdalloc_cap', 'shader_key_skip', 'tex_bc_cpu', 'ue_texture_pool', 'enc_thread', 'core_preempt', 'fps_target', 'device_api', 'game_args')


def profile(gid):
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / 'profile.tsv'
        subprocess.run([sys.executable, str(REPO / 'dm/games/game.py'), 'profile', gid, str(out)], check=True, capture_output=True)
        rows = {}
        for ln in out.read_text().splitlines():
            if ln and not ln.startswith('#'):
                k, _, v = ln.partition('\t'); rows.setdefault(k, v)
        return rows


class GameProfileTest(unittest.TestCase):
    def test_every_game_writes_every_key(self):
        for g in game.all_games():
            rows = profile(g.id)
            for k in KEYS: self.assertIn(k, rows, f'{g.id}: {k}')
            self.assertRegex(rows['shader_key_skip'], r'^0x[0-9a-f]+\+0x[0-9a-f]+$', g.id)
            self.assertIn('shared_font', rows, g.id)   # ttf / bfttf as 1 / 0, or auto

    def test_values_follow_game_tsv(self):
        for g in game.all_games():
            rows = profile(g.id)
            sks = g.get('shader_key_skip', '')
            if sks:
                off, _, ln = sks.partition('+')
                self.assertEqual(int(rows['shader_key_skip'].split('+')[0], 0), int(off, 0), g.id)
                self.assertEqual(int(rows['shader_key_skip'].split('+')[1], 0), int(ln, 0), g.id)
            else:
                self.assertEqual(rows['shader_key_skip'], '0x0+0x0', g.id)
            self.assertEqual(int(rows['stdalloc_cap'], 0), int(g.get('stdalloc_cap', '0'), 0), g.id)
            self.assertEqual(int(rows['ue_texture_pool']), int(g.get('ue_texture_pool', '0')), g.id)
            self.assertEqual(int(rows['tex_bc_cpu']), int(g.get('tex_bc_cpu', 'off') == 'on'), g.id)
            self.assertEqual(int(rows['enc_thread']), int(g.get('enc_thread', 'off') == 'on'), g.id)
            self.assertEqual(rows['device_api'] == '0.0', not g.get('device_api', ''), g.id)
            self.assertEqual(rows['shared_font'] == 'auto', not g.get('shared_font', ''), g.id)


if __name__ == '__main__':
    unittest.main()
