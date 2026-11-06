"""Offer the resolution control only when the rewritten build supports it, the game's upscaler choice only to games whose shader map holds their own upscaler (agl's NDSR network: game.py upscaler_keys), and the HDR output only to Switch 2 games (their sdk module names the Ounce spec), on the Mac and iOS."""
from pathlib import Path
import os,sys,tempfile,unittest
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

def hdr_rows(gid,platform,sdk):   # the DM_HDR rows settings.py offers for a game whose sdk module is sdk (bytes; None: no sdk module)
    with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']),patch.dict(os.environ,{'SETTINGS_PLATFORM':platform}):
        (Path(tmp)/'main.fixups.tsv').write_text('')
        if sdk is not None:(Path(tmp)/'sdk.patched.bin').write_bytes(sdk)
        return [s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_HDR']

class HdrSetting(unittest.TestCase):
    def test_switch2_games_on_mac_and_ios(self):   # general, not per game: shown where the sdk module names the Switch 2 spec, on either platform
        ounce,nx=b'FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }',b'FS_ACCESS: { sdk_version: 12.0.0, spec: NX }'
        for platform in ('mac','ios'):
            for gid,sdk,shown in [('040074A01BF12000',ounce,True),('040074A01BF12000',nx,False),('010056B015FE8000',nx,False),('040074A01BF12000',None,False)]:   # Donkey Kong Bananza (Switch 2); the same game on a Switch 1 SDK; NieR:Automata (Switch 1); no sdk module
                hdr=hdr_rows(gid,platform,sdk)
                self.assertEqual(bool(hdr),shown,(platform,gid,sdk))
                if hdr:self.assertEqual((hdr[0]['kind'],hdr[0]['default']),('toggle','0'))

if __name__=='__main__':unittest.main()
