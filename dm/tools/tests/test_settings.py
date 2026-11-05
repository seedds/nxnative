"""Offer the resolution control only when the rewritten build supports it, and the game's upscaler choice only to games whose shader map holds their own upscaler (agl's NDSR network: game.py upscaler_keys)."""
from pathlib import Path
import sys,tempfile,unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'games'))
import settings

class ResolutionSetting(unittest.TestCase):
    def test_verified_rewrite_required(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            p=Path(tmp)/'main.fixups.tsv'
            for header,supported in [('# BUILD_ID\t3c10ec35c5ef4c3686b4053afb0e58e7\n',False),
                                     ('# DYNAMIC_RESOLUTION\t1\n',True),('',False)]:
                p.write_text(header)
                rows=settings.build('010056B015FE8000',tmp,'unused')['settings']
                dr=[s for s in rows if s['env']=='DM_DYNAMIC_RESOLUTION']
                self.assertEqual(bool(dr),supported)
                if dr:self.assertEqual((dr[0]['kind'],dr[0]['default']),('toggle','0'))
                fps=next(s for s in rows if s['env']=='DM_FPS_TARGET')
                self.assertEqual(fps['default'],'60')
                self.assertEqual(fps['options'],[['30','30 fps'],['60','60 fps']])

class FrameRateOption(unittest.TestCase):
    def test_120_only_on_the_mac_for_a_game_checked_at_that_speed(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,platform,has in [('04007EF01F8AE000','mac',True),('04007EF01F8AE000','ios',False),('010056B015FE8000','mac',False)]:   # Fortune's Weave (fps_max 120), NieR:Automata (none)
                with patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':platform}):
                    fps=next(s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_FPS_TARGET')
                self.assertEqual(fps['default'],'60',gid)
                self.assertEqual(fps['options'],[['30','30 fps'],['60','60 fps']]+([['120','120 fps']] if has else []),(gid,platform))

class UpscalerSetting(unittest.TestCase):
    def test_only_for_games_naming_their_upscaler(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,shown in [('040074A01BF12000',True),('010056B015FE8000',False)]:   # Donkey Kong Bananza (its NDSR network), NieR:Automata (none)
                up=[s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_UPSCALER']
                self.assertEqual(bool(up),shown,gid)
                if up:self.assertEqual((up[0]['default'],up[0]['options']),('game',[['game',"Game's own"],['metalfx','MetalFX']]))

class HdrSetting(unittest.TestCase):
    def test_offered_for_hdr_games_on_both_platforms(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US']):
            (Path(tmp) / 'main.fixups.tsv').write_text('')
            cases = [
                ('0400C3F00006E000', 'mac', True),
                ('0400C3F00006E000', 'ios', True),
                ('040074A01BF12000', 'mac', True),
                ('040074A01BF12000', 'ios', True),
                ('010056B015FE8000', 'mac', False),
                ('010056B015FE8000', 'ios', False),
            ]
            for game_id, platform, expected in cases:
                with self.subTest(game=game_id, platform=platform):
                    with patch.dict(settings.os.environ, {'SETTINGS_PLATFORM': platform}):
                        rows = settings.build(game_id, tmp, 'unused')['settings']
                    hdr = [row for row in rows if row['env'] == 'DM_HDR']
                    self.assertEqual(bool(hdr), expected)
                    if hdr:
                        self.assertEqual(hdr[0]['kind'], 'toggle')
                        self.assertEqual(hdr[0]['default'], '1')


if __name__=='__main__':unittest.main()
