"""Offer the resolution control only when the rewritten build supports it, the game's upscaler choice only to games whose shader map holds their own upscaler (an NDSR pack's network: game.py upscaler_keys), and the HDR setting, with its paper white and peak, only to Switch 2 games (their sdk module names the Ounce spec), on the Mac and iOS."""
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
    def test_120_for_a_game_checked_at_that_speed_on_either_platform(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,platform,has in [('04007EF01F8AE000','mac',True),('04007EF01F8AE000','ios',True),('010056B015FE8000','mac',False),('010056B015FE8000','ios',False)]:   # Fortune's Weave (fps_max 120), NieR:Automata (none)
                with patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':platform}):
                    fps=next(s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_FPS_TARGET')
                self.assertEqual(fps['default'],'60',gid)
                self.assertEqual(fps['options'],[['30','30 fps'],['60','60 fps']]+([['120','120 fps']] if has else []),(gid,platform))

class UpscalerSetting(unittest.TestCase):
    def test_only_for_games_naming_their_upscaler(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,shown in [('040074A01BF12000',True),('040077201BE50000',True),('010056B015FE8000',False)]:   # Donkey Kong Bananza (agl's NDSR pack), Kirby Air Riders (the pack under its own name), NieR:Automata (none)
                up=[s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_UPSCALER']
                self.assertEqual(bool(up),shown,gid)
                if up:self.assertEqual((up[0]['default'],up[0]['options']),('game',[['game',"Game's own"],['metalfx','MetalFX']]))

def hdr_rows(gid,platform,sdk,env='DM_HDR'):   # the rows for env settings.py offers for a game whose sdk module is sdk (bytes; None: no sdk module)
    with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']),patch.dict(os.environ,{'SETTINGS_PLATFORM':platform}):
        (Path(tmp)/'main.fixups.tsv').write_text('')
        if sdk is not None:(Path(tmp)/'sdk.patched.bin').write_bytes(sdk)
        return [s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']==env]

class HdrSetting(unittest.TestCase):
    def test_switch2_games_on_mac_and_ios(self):   # general, not per game: shown where the sdk module names the Switch 2 spec, on either platform
        ounce,nx=b'FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }',b'FS_ACCESS: { sdk_version: 12.0.0, spec: NX }'
        for platform in ('mac','ios'):
            for gid,sdk,shown in [('040074A01BF12000',ounce,True),('040074A01BF12000',nx,False),('010056B015FE8000',nx,False),('040074A01BF12000',None,False)]:   # Donkey Kong Bananza (Switch 2); the same game on a Switch 1 SDK; NieR:Automata (Switch 1); no sdk module
                hdr=hdr_rows(gid,platform,sdk)
                self.assertEqual(bool(hdr),shown,(platform,gid,sdk))
                if hdr:self.assertEqual((hdr[0]['kind'],hdr[0]['default']),('toggle','1'),platform)   # on by default (the host's own unset DM_HDR stays SDR)

    def test_paper_white_and_peak_are_not_offered(self):   # the game's white always lands at the screen's SDR white and the peak follows the screen (hle_vi.c): nothing for a player to set; DM_HDR_PAPER_WHITE and DM_HDR_PEAK stay diagnostic switches
        ounce=b'FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }'
        for platform in ('mac','ios'):
            for env in ('DM_HDR_PAPER_WHITE','DM_HDR_PEAK'):self.assertEqual(hdr_rows('040074A01BF12000',platform,ounce,env),[],(platform,env))

    def test_upscaling_is_hidden_while_hdr_is_on(self):   # MetalFX's spatial scaler takes no HDR picture (nvn_mtl.m present_hdr_source): the row is a sub-setting of HDR off; a Switch 1 game, with no HDR row, keeps it drawn
        ounce,nx=b'FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }',b'FS_ACCESS: { sdk_version: 12.0.0, spec: NX }'
        for platform in ('mac','ios'):
            for sdk in (ounce,nx):
                row,=hdr_rows('040074A01BF12000',platform,sdk,'DM_PRESENT_UPSCALE')
                self.assertEqual(row['needs'],'DM_HDR=0',(platform,sdk))

if __name__=='__main__':unittest.main()
