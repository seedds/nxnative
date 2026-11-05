"""LAN launcher save selection and build validation, without launching a game."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('run_mac_lan', REPO / 'scripts/run_mac_lan.py')
assert spec and spec.loader
lan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lan)
TITLE = '0400C3F00006E000'
SETTINGS = [
    {'env': 'DM_UPSCALER', 'kind': 'choice', 'options': [['game', 'Game'], ['metalfx', 'MetalFX']], 'default': 'game'},
    {'env': 'DM_OPMODE', 'kind': 'choice', 'options': [['1', 'Docked'], ['0', 'Handheld']], 'default': '1'},
    {'env': 'DM_PSO_MTL4', 'kind': 'toggle', 'on': '1', 'off': '0', 'default': '1'},
    {'env': 'DM_RENDER_SCALE', 'kind': 'range', 'min': 0.5, 'max': 1.0, 'default': '1.0'},
]


class Launcher(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve()
        self.build = self.root / f'{TITLE}_build'
        self.home = self.root / 'home'
        self.app_save = self.home / 'Library/Application Support/nxnative' / TITLE / 'save'
        for name in ('patched_v917504/main.patched.bin', f'profile/{TITLE}/shader_map.tsv',
                     'extract/v917504/RomFS.img', 'tools/dmhost'):
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'test input')
        for name in ('shader/cache', 'font'):
            (self.build / name).mkdir(parents=True)

    def invoke(self, *args, env=None, build=None, choose=False, peer_args=None, local_ip='192.0.2.10'):
        route = mock.MagicMock()
        route.__enter__.return_value.getsockname.return_value = (local_ip, 12345)
        output = io.StringIO()
        glob = Path.glob
        def fixture_glob(path, pattern):
            # A developer may have this game installed globally; tests use only
            # their own synthetic app and never read that app's preferences.
            return iter(()) if path == Path('/Applications') else glob(path, pattern)
        with mock.patch.object(lan.Path, 'home', return_value=self.home), \
                mock.patch.object(lan.Path, 'glob', fixture_glob), \
                mock.patch.object(lan.socket, 'socket', return_value=route), \
                mock.patch.object(lan.os, 'execve') as execute, \
                mock.patch.object(lan.os, 'chdir'), \
                mock.patch.dict(os.environ, env or {}, clear=True), \
                mock.patch.object(sys, 'argv', ['run_mac_lan.py', *([] if choose else [str(build or self.build)]),
                                              *(peer_args or ['--peer', '198.51.100.20']), *args]), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            lan.main()
        return execute, output.getvalue()

    def make_app(self, parent=None):
        app = (parent or self.root) / 'Mario Kart World.app'
        resources = app / 'Contents/Resources'
        (resources / 'dm').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({
            'DMTitleID': TITLE, 'CFBundleIdentifier': f'org.nxnative.{TITLE.lower()}', 'DMLanguage': 'en-US'}))
        (resources / 'dm/launcher.json').write_text(json.dumps({'settings': SETTINGS}))
        (resources / 'pso').mkdir()
        (resources / 'pso/prebuilt.tsv').write_text('local GPU archive')
        (resources / 'pso/applegpu_g13x.mtl4a').write_bytes(b'M1 Pro local archive')
        return app, resources

    def test_regular_app_settings_and_local_gpu_archive_are_reused(self):
        app, resources = self.make_app()
        saved = {'launcher.DM_UPSCALER': 'metalfx', 'launcher.DM_OPMODE': '0'}
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=plistlib.dumps(saved))) as preferences:
            execute, output = self.invoke()
        env = execute.call_args.args[2]
        self.assertEqual(env['DM_UPSCALER'], 'metalfx')
        self.assertEqual(env['DM_OPMODE'], '0')
        self.assertEqual(env['DM_PSO_MTL4'], '1')
        self.assertEqual(env['DM_PSO_BUNDLE'], str(resources / 'pso'))
        self.assertEqual(env['DM_ROOT'], str(self.app_save.parent))
        self.assertIn(f'Regular app: {app}', output)
        self.assertEqual(preferences.call_args.args[0], ['/usr/bin/defaults', 'export', f'org.nxnative.{TITLE.lower()}', '-'])

    def test_unrelated_app_with_invalid_metadata_is_skipped(self):
        app, _ = self.make_app()
        invalid = self.root / 'Unrelated.app/Contents/Info.plist'
        invalid.parent.mkdir(parents=True)
        invalid.write_text('<?xml version="1.0"?><plist><bad & XML></plist>')
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=1)):
            _, output = self.invoke('--check')
        self.assertIn(f'Regular app: {app}', output)

    def test_process_and_env_files_override_saved_settings_and_defaults(self):
        _, resources = self.make_app()
        self.app_save.parent.mkdir(parents=True)
        (self.app_save.parent / 'env.txt').write_text('DM_OPMODE=0\nDM_PSO_MTL4=0\n')
        (resources / 'dm/env.txt').write_text('DM_OPMODE=1\nDM_PSO_MTL4=1\nDM_RENDER_SCALE=0.75\n')
        saved = {'launcher.DM_UPSCALER': 'metalfx', 'launcher.DM_OPMODE': '1', 'launcher.DM_RENDER_SCALE': '0.5'}
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=plistlib.dumps(saved))):
            execute, _ = self.invoke(env={'DM_UPSCALER': 'game', 'DM_PSO_BUNDLE': 'off'})
        env = execute.call_args.args[2]
        self.assertEqual(env['DM_UPSCALER'], 'game')
        self.assertEqual(env['DM_OPMODE'], '0')
        self.assertEqual(env['DM_PSO_MTL4'], '0')
        self.assertEqual(env['DM_RENDER_SCALE'], '0.75')
        self.assertEqual(env['DM_PSO_BUNDLE'], 'off')

    def test_invalid_or_unoffered_preferences_use_app_defaults(self):
        self.make_app()
        saved = {'launcher.DM_UPSCALER': 'unknown', 'launcher.DM_OPMODE': 0,
                 'launcher.DM_RENDER_SCALE': 'nan', 'launcher.UNKNOWN': 'do not import'}
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=plistlib.dumps(saved))):
            execute, _ = self.invoke()
        env = execute.call_args.args[2]
        self.assertEqual(env['DM_UPSCALER'], 'game')
        self.assertEqual(env['DM_OPMODE'], '1')
        self.assertEqual(env['DM_RENDER_SCALE'], '1.0')
        self.assertNotIn('UNKNOWN', env)

    def test_explicit_app_can_be_outside_build_folder(self):
        outside = self.root / 'different location'
        app, resources = self.make_app(outside)
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=1)):
            execute, _ = self.invoke('--app', str(app))
        self.assertEqual(execute.call_args.args[2]['DM_PSO_BUNDLE'], str(resources / 'pso'))

    def test_build_settings_are_used_if_app_is_not_found(self):
        settings = self.build / 'app/launcher.json'
        settings.parent.mkdir()
        settings.write_text(json.dumps({'settings': SETTINGS}))
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=plistlib.dumps({'launcher.DM_OPMODE': '0'}))):
            execute, _ = self.invoke()
        self.assertEqual(execute.call_args.args[2]['DM_OPMODE'], '0')
        self.assertNotIn('DM_PSO_BUNDLE', execute.call_args.args[2])

    def test_saved_choices_can_be_disabled(self):
        self.make_app()
        with mock.patch.object(lan.subprocess, 'run') as preferences:
            execute, _ = self.invoke(env={'DM_SAVED_CHOICES': '0'})
        preferences.assert_not_called()
        self.assertEqual(execute.call_args.args[2]['DM_UPSCALER'], 'game')

    def test_regular_app_save_is_used_without_copying_or_replacing_it(self):
        self.app_save.mkdir(parents=True)
        account = self.app_save / 'account.sav'
        account.write_bytes(b'existing progress')
        build_save = self.build / 'run/save'
        build_save.mkdir(parents=True)
        (build_save / 'account.sav').write_bytes(b'other progress')
        execute, output = self.invoke()
        env = execute.call_args.args[2]
        self.assertEqual(env['DM_SAVE'], str(self.app_save))
        self.assertEqual(env['DM_TEMP'], str(self.app_save.parent / 'temp'))
        self.assertIn(f'Save: {self.app_save}', output)
        self.assertEqual(account.read_bytes(), b'existing progress')
        self.assertEqual((build_save / 'account.sav').read_bytes(), b'other progress')

    def test_username_option_sets_account_nickname_for_this_launch(self):
        execute, output = self.invoke('--username', 'Racer One', env={'DM_NICK': 'Old name'})
        env = execute.call_args.args[2]
        self.assertEqual(env['DM_NICK'], 'Racer One')
        self.assertEqual(env['DM_SAVE'], str(self.app_save))
        self.assertIn('Username: Racer One', output)
        self.assertFalse((self.app_save.parent / 'lan-user.json').exists())

    def test_username_utf8_limit_preserves_complete_characters(self):
        execute, _ = self.invoke('--username', '\u00dcber Racer')
        self.assertEqual(execute.call_args.args[2]['DM_NICK'], '\u00dcber Racer')
        for invalid in (' ', 'A\nB', 'x' * 33, '\U0001f3ce' * 9, '\udcff'):
            with self.subTest(name=repr(invalid)), self.assertRaises(SystemExit) as error:
                self.invoke('--username', invalid)
            self.assertEqual(error.exception.code, 2)

    def test_username_dialog_remembers_choice_only_on_this_mac(self):
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout='Racer Two')):
            execute, _ = self.invoke('--choose-username')
        self.assertEqual(execute.call_args.args[2]['DM_NICK'], 'Racer Two')
        saved = self.app_save.parent / 'lan-user.json'
        self.assertEqual(json.loads(saved.read_text()), {'username': 'Racer Two'})
        execute, _ = self.invoke()
        self.assertEqual(execute.call_args.args[2]['DM_NICK'], 'Racer Two')
        execute, _ = self.invoke(env={'DM_NICK': 'Environment name'})
        self.assertEqual(execute.call_args.args[2]['DM_NICK'], 'Environment name')

    def test_username_dialog_cancel_and_check_do_not_write_settings(self):
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=1, stdout='')):
            execute, _ = self.invoke('--choose-username')
        execute.assert_not_called()
        self.assertFalse(self.home.exists())
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout='Racer')):
            execute, _ = self.invoke('--choose-username', '--check')
        execute.assert_not_called()
        self.assertFalse(self.home.exists())

    def test_username_is_passed_to_dialog_as_data(self):
        default = 'Racer "Q" $(id)'
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout='Racer')) as dialog:
            self.invoke('--choose-username', env={'DM_NICK': default})
        command = dialog.call_args.args[0]
        self.assertEqual(command[-1], default)
        self.assertNotIn(default, command[-2])

    def test_invalid_saved_username_is_ignored(self):
        saved = self.app_save.parent / 'lan-user.json'
        saved.parent.mkdir(parents=True)
        saved.write_text('{invalid JSON')
        execute, _ = self.invoke()
        self.assertEqual(execute.call_args.args[2]['DM_NICK'], 'Player')

    def test_explicit_save_overrides_environment(self):
        isolated = self.root / 'isolated/save'
        execute, _ = self.invoke('--save', str(isolated), env={'DM_SAVE': str(self.root / 'env-save')})
        self.assertEqual(execute.call_args.args[2]['DM_SAVE'], str(isolated))
        self.assertFalse(self.app_save.exists())

    def test_environment_save_is_honored(self):
        custom = self.root / 'custom-save'
        execute, _ = self.invoke(env={'DM_SAVE': str(custom)})
        self.assertEqual(execute.call_args.args[2]['DM_SAVE'], str(custom))

    def test_check_reports_save_without_creating_it_or_starting_game(self):
        execute, output = self.invoke('--check')
        execute.assert_not_called()
        self.assertIn(f'Save: {self.app_save}', output)
        self.assertIn('main.patched.bin', output)
        self.assertFalse(self.home.exists())
        self.assertFalse((self.build / 'run').exists())

    def test_raw_rom_folder_needs_a_build_first(self):
        raw = self.root / 'raw-rom'
        raw.mkdir()
        (raw / 'game.nup').write_bytes(b'raw ROM')
        with self.assertRaises(SystemExit) as error:
            self.invoke(build=raw)
        self.assertEqual(error.exception.code, 2)

    def test_registered_profile_without_a_derived_profile_is_supported(self):
        registered = self.root / 'checkout/dm/games' / TITLE / 'shader_map.tsv'
        registered.parent.mkdir(parents=True)
        registered.write_bytes(b'registered profile')
        derived = self.build / 'profile' / TITLE / 'shader_map.tsv'
        derived.unlink()
        with mock.patch.object(lan, '__file__', str(self.root / 'checkout/scripts/run_mac_lan.py')):
            execute, _ = self.invoke()
        self.assertEqual(execute.call_args.args[2]['DM_NVN_SHMAP'], str(registered))

    def test_explicit_profile_supports_a_custom_build_folder(self):
        custom = self.root / 'custom build path'
        self.build.rename(custom)
        profile = custom / 'profile' / TITLE
        execute, _ = self.invoke('--profile', str(profile), build=custom)
        self.assertEqual(execute.call_args.args[2]['DM_NVN_SHMAP'], str(profile / 'shader_map.tsv'))

    def test_kit_uses_its_host_without_replacing_build_host(self):
        kit = self.root / 'kit'
        kit.mkdir()
        host = kit / 'dmhost'
        host.write_bytes(b'LAN host')
        with mock.patch.object(lan, '__file__', str(kit / 'run_mac_lan.py')):
            execute, _ = self.invoke()
        self.assertEqual(execute.call_args.args[0], host)
        self.assertEqual((self.build / 'tools/dmhost').read_bytes(), b'test input')

    def test_folder_picker_uses_selected_build(self):
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=f'{self.build}\n')):
            execute, _ = self.invoke('--choose-build', choose=True)
        self.assertEqual(execute.call_args.args[1][1], str(self.build / 'patched_v917504'))

    def test_cancel_folder_picker_does_not_launch_or_create_saves(self):
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=1, stdout='')):
            execute, _ = self.invoke('--choose-build', choose=True)
        execute.assert_not_called()
        self.assertFalse(self.home.exists())

    def test_kit_does_not_enforce_compatibility_manifest(self):
        kit = self.root / 'kit'
        kit.mkdir()
        (kit / 'dmhost').write_bytes(b'LAN host')
        (kit / 'compatibility.json').write_text('{"title_id": "different version", "shader_chain": "another chain"}')
        with mock.patch.object(lan, '__file__', str(kit / 'run_mac_lan.py')):
            execute, _ = self.invoke('--check')
            execute.assert_not_called()
        self.assertFalse(self.home.exists())

    def test_portable_kit_uses_its_matching_shaders_and_tables(self):
        kit = self.root / 'kit'
        kit.mkdir()
        host = kit / 'dmhost'
        host.write_bytes(b'LAN host')
        runtime = kit / 'runtime'
        shaders = runtime / 'shaders'
        shaders.mkdir(parents=True)
        shader_map = runtime / 'tables' / TITLE / 'shader_map.tsv'
        shader_map.parent.mkdir(parents=True)
        shader_map.write_bytes(b'kit map')
        # A different or absent development shader cache must not affect the kit.
        (self.build / 'shader/cache').rmdir()
        (self.build / 'profile' / TITLE / 'shader_map.tsv').unlink()
        with mock.patch.object(lan, '__file__', str(kit / 'run_mac_lan.py')):
            execute, output = self.invoke()
        env = execute.call_args.args[2]
        self.assertEqual(env['DM_MSL_DIR'], str(shaders))
        self.assertEqual(env['DM_NVN_SHMAP'], str(shader_map))
        self.assertIn(f'Shaders: {shaders}', output)
        self.assertEqual(env['DM_ROMFS'], str(self.build / 'extract/v917504/RomFS.img'))
        self.assertEqual(env['DM_SAVE'], str(self.app_save))

    def test_explicit_host_uses_build_shaders_instead_of_kit_runtime(self):
        kit = self.root / 'kit'
        (kit / 'runtime').mkdir(parents=True)
        host = self.build / 'tools/dmhost'
        with mock.patch.object(lan, '__file__', str(kit / 'run_mac_lan.py')):
            execute, _ = self.invoke('--host', str(host))
        self.assertEqual(execute.call_args.args[2]['DM_MSL_DIR'], str(self.build / 'shader/cache'))

    def test_shared_kit_uses_the_ip_entered_on_either_mac(self):
        for local, remote in (('192.0.2.10', '198.51.100.20'), ('198.51.100.20', '192.0.2.10')):
            with self.subTest(local=local), mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=remote)):
                execute, output = self.invoke(peer_args=['--choose-peer'], local_ip=local)
                env = execute.call_args.args[2]
                self.assertEqual(env['DM_LAN_PEERS'], remote)
                self.assertEqual(env['DM_LAN_IP'], local)
                self.assertIn(f'discovery peer {remote}', output)

    def test_cancel_ip_dialog_does_not_launch_or_create_saves(self):
        with mock.patch.object(lan.subprocess, 'run', return_value=mock.Mock(returncode=1, stdout='')):
            execute, _ = self.invoke(peer_args=['--choose-peer'])
        execute.assert_not_called()
        self.assertFalse(self.home.exists())

    def test_invalid_ip_is_requested_again(self):
        answers = [mock.Mock(returncode=0, stdout='invalid'), mock.Mock(returncode=0, stdout='198.51.100.20')]
        with mock.patch.object(lan.subprocess, 'run', side_effect=answers) as dialogs:
            execute, _ = self.invoke(peer_args=['--choose-peer'])
        self.assertEqual(dialogs.call_count, 2)
        self.assertEqual(execute.call_args.args[2]['DM_LAN_PEERS'], '198.51.100.20')

    def test_double_click_dialogs_select_ip_then_build(self):
        answers = [mock.Mock(returncode=0, stdout='198.51.100.20'), mock.Mock(returncode=0, stdout=str(self.build))]
        with mock.patch.object(lan.subprocess, 'run', side_effect=answers) as dialogs:
            execute, _ = self.invoke('--choose-build', peer_args=['--choose-peer'], choose=True)
        self.assertIn('display dialog', dialogs.call_args_list[0].args[0][-1])
        self.assertIn('choose folder', dialogs.call_args_list[1].args[0][-1])
        self.assertEqual(execute.call_args.args[2]['DM_LAN_PEERS'], '198.51.100.20')


if __name__ == '__main__':
    unittest.main()
