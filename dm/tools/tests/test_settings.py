"""Offer the resolution control only when the rewritten build supports it, the game's upscaler choice only to games whose shader map holds their own upscaler (an NDSR pack's network: game.py upscaler_keys), and the HDR setting only to Switch 2 games that ask for their HDR status (their sdk module names the Ounce spec, and a module of their own imports the status query), on the Mac and iOS."""
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

    def test_40_only_where_the_frame_step_takes_ticks_of_1_120_s(self):
        """40 fps needs a frame step stub in ticks of 1/120 s (FRAME_STEP_120 in main's fixups table: Ocarina of Time's ratio stub); both platforms offer it, the host and iOS launcher check the screen"""
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            p=Path(tmp)/'main.fixups.tsv'
            for header,platform,has in [('# FRAME_STEP_120\t1\n','ios',True),('# FRAME_STEP_120\t1\n','mac',True),('','ios',False),('# DYNAMIC_RESOLUTION\t1\n','mac',False)]:
                p.write_text(header)
                with patch.dict(settings.os.environ,{'SETTINGS_PLATFORM':platform}):
                    fps=next(s for s in settings.build('0400486024DEC000',tmp,'unused')['settings'] if s['env']=='DM_FPS_TARGET')
                self.assertEqual(fps['default'],'60',(header,platform))
                self.assertEqual(fps['options'],[['30','30 fps']]+([['40','40 fps']] if has else [])+[['60','60 fps']],(header,platform))

class UpscalerSetting(unittest.TestCase):
    def test_only_for_games_naming_their_upscaler(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']):
            (Path(tmp)/'main.fixups.tsv').write_text('')
            for gid,shown in [('040074A01BF12000',True),('040077201BE50000',True),('010056B015FE8000',False)]:   # Donkey Kong Bananza (agl's NDSR pack), Kirby Air Riders (the pack under its own name), NieR:Automata (none)
                up=[s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']=='DM_UPSCALER']
                self.assertEqual(bool(up),shown,gid)
                if up:self.assertEqual((up[0]['default'],up[0]['options']),('game',[['game',"Game's own"],['metalfx','MetalFX']]))

def hdr_rows(gid,platform,sdk,env='DM_HDR',asks=True):   # the rows for env settings.py offers for a game whose sdk module is sdk (bytes; None: no sdk module) and whose own modules import the HDR status query (asks)
    with tempfile.TemporaryDirectory() as tmp,patch.object(settings.nacp,'languages',return_value=['en-US']),patch.dict(os.environ,{'SETTINGS_PLATFORM':platform}),patch.object(settings.nso_syms,'any_imports',return_value=asks) as imp:
        (Path(tmp)/'main.fixups.tsv').write_text('')
        if sdk is not None:(Path(tmp)/'sdk.patched.bin').write_bytes(sdk)
        rows=[s for s in settings.build(gid,tmp,'unused')['settings'] if s['env']==env]
        assert all(c.args[1]=='_ZN2nn2vi21QueryDisplayHdrStatus' for c in imp.call_args_list),imp.call_args_list   # the query hle_vi.c dm_hdr_boot looks for
        return rows

class HdrSetting(unittest.TestCase):
    def test_switch2_games_that_ask_on_mac_and_ios(self):   # general, not per game: the host's own rule (hle_vi.c dm_hdr_boot), a Switch 2 sdk and a game module importing the status query
        ounce,nx=b'FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }',b'FS_ACCESS: { sdk_version: 12.0.0, spec: NX }'
        for platform in ('mac','ios'):
            for gid,sdk,asks,shown in [('040074A01BF12000',ounce,True,True),('040074A01BF12000',ounce,False,False),('040074A01BF12000',nx,True,False),('010056B015FE8000',nx,False,False),('040074A01BF12000',None,True,False)]:   # Donkey Kong Bananza (Switch 2); one that never asks; the same game on a Switch 1 SDK; NieR:Automata (Switch 1); no sdk module
                hdr=hdr_rows(gid,platform,sdk,asks=asks)
                self.assertEqual(bool(hdr),shown,(platform,gid,sdk,asks))
                if hdr:self.assertEqual((hdr[0]['kind'],hdr[0]['default']),('toggle','1'),platform)

    def test_upscaling_stays_with_hdr(self):   # the present scales an HDR picture bilinearly by itself (nvn_mtl.m present_hdr_source): MetalFX stays a choice for the SDR picture
        ounce=b'FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }'
        for platform in ('mac','ios'):
            row,=hdr_rows('040074A01BF12000',platform,ounce,'DM_PRESENT_UPSCALE')
            self.assertNotIn('needs',row,platform)

if __name__=='__main__':unittest.main()
