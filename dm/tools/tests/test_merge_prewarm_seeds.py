import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
SCRIPT = Path('scripts/merge_prewarm_seeds.py')
TITLE = '0100000000000000'
VERTEX = '0000000000000001_b'
COMPUTE = '0000000000000002_b'
MESH = '0000000000000003_b'
TASK = '0000000000000004_b'
GEOMETRY = '0000000000000005_b'
SECOND_VERTEX = '0000000000000006_b'
UPDATE_COMPUTE = '0000000000000007_b'
UPDATE_BUILD = 'a' * 32


def key(shader, stage=0):
    # A single shader in the vertex slot of pso_skey, with zeroed render state.
    return (shader.encode().ljust(48, b'\0') + bytes([stage]) + bytes(463)).hex()


@unittest.skipUnless(shutil.which('clang'), 'the host parser needs clang')
class ImportSeeds(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.repo = self.root / 'repo'
        self.inbox = self.root / 'inbox'
        self.inbox.mkdir()
        for path in (SCRIPT, Path('dm/games/game.py'), Path('dm/tools/prewarm_skey.c'), Path('dm/tools/nacp.py'),
                     Path('dm/gfx/tools/dmsh_chain.py')):
            target = self.repo / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / path, target)
        shutil.copytree(REPO / 'dm/gfx/nvn', self.repo / 'dm/gfx/nvn',
                        ignore=shutil.ignore_patterns('*.m', '*.c', '*.inc'))
        profile = self.repo / 'dm/games' / TITLE
        profile.mkdir()
        (profile / 'game.tsv').write_text(
            f'title_id\t{TITLE}\nname\tExample Game\nbuild\tv0\t1.0.0\n'
            f'build\tv65536\t1.0.1\tmain={UPDATE_BUILD}\n'
            'build\tv131072\t1.0.2 b723\npackage\tv0\nshader_versions\tv0\nshader_covers\tv131072\n')
        (profile / 'shader_map.tsv').write_text(''.join(
            f'shaders\t{index}\t{stage}\t{shader[:-2]}\n'
            for index, (stage, shader) in enumerate([('vertex', VERTEX), ('compute', COMPUTE),
                                                     ('mesh', MESH), ('task', TASK), ('geometry', GEOMETRY),
                                                     ('vertex', SECOND_VERTEX)])))
        self.manifest = profile / 'pso_manifest.tsv'
        self.original = f'# Original header\nR\t{key(VERTEX)}\n'
        self.manifest.write_text(self.original)
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull, PYTHONDONTWRITEBYTECODE='1')
        self.env.pop('DM_GAMES_DIR', None)

    def add_local_profile(self, display='?', build=UPDATE_BUILD):
        root = self.root / 'local profiles'
        profile = root / TITLE
        profile.mkdir(parents=True, exist_ok=True)
        (profile / 'game.tsv').write_text(f'title_id\t{TITLE}\nbuild\tv65536\t{display}\tmain={build}\n'
                                          'package\tv65536\nshader_versions\tv65536\n')
        (profile / 'shader_map.tsv').write_text(f'cs.bin\t0\tcompute\t{UPDATE_COMPUTE[:-2]}\n')
        self.env['DM_GAMES_DIR'] = str(root)
        return profile

    def upload(self, *rows, title=TITLE, version='1.0.0', corrupt=False):
        data = ('\n'.join(rows) + '\n').encode()
        digest = '0' * 64 if corrupt else hashlib.sha256(data).hexdigest()
        path = self.inbox / f'{title}--{version}--{digest}.tsv'
        path.write_bytes(data)
        return path

    def run_import(self, *flags, check=True):
        return subprocess.run([sys.executable, self.repo / SCRIPT, self.inbox, *flags], env=self.env,
                              text=True, capture_output=True, check=check)

    def git(self, *args, cwd=None):
        return subprocess.run(['git', '-c', 'user.name=seedds', '-c', 'user.email=seedds@gmail.com', *args],
                              cwd=cwd or self.repo, env=self.env, text=True, capture_output=True, check=True).stdout.strip()

    def init_origin(self):
        bare = self.root / 'origin.git'
        self.git('init', '--bare', '--initial-branch=main', str(bare))
        self.git('init', '--initial-branch=main')
        self.git('add', '.')
        self.git('commit', '-m', 'initial profile')
        self.git('remote', 'add', 'origin', str(bare))
        self.git('push', 'origin', 'HEAD:main')
        return bare

    def test_preview_and_apply_deduplicate_and_validate(self):
        shader = f'S\t{COMPUTE}\t5'
        mesh = f'M\t{TASK}\t{key(MESH, 7)}'
        compute = f'C\t{COMPUTE}'
        self.upload(f'R\t{key(VERTEX)[:-16]}', shader, mesh, f'M\t{TASK}\t{key(MESH, 4)}', compute,
                    f'C\t{"0" * 16}_b', f'S\t{VERTEX}\t1', f'G\t{VERTEX[:-2]}\t{COMPUTE[:-2]}',
                    f'G\t{VERTEX[:-2]}\t{"0" * 16}', 'R\t00',
                    version='1.0.1')
        self.upload(f'S\t{VERTEX}\t0', corrupt=True)
        self.upload(compute, title='0100000000000001')
        preview = self.run_import().stdout
        for expected in ('2 added', '2 duplicate', '2 ignored S', 'invalid pipeline', '2 missing shader',
                         'stage mismatch', 'no matching version map', 'SHA-256 mismatch',
                         'no repository profile', 'Preview'):
            self.assertIn(expected, preview)
        self.assertEqual(self.manifest.read_text(), self.original)
        self.run_import('--apply')
        self.assertEqual(self.manifest.read_text(), self.original + '# Inbox captures from 1.0.1.\n'
                         + '\n'.join([mesh, compute]) + '\n')
        self.assertIn('No manifest updates.', self.run_import('--apply').stdout)
        self.assertEqual(len(list(self.inbox.glob('*.tsv'))), 3)

    def test_legacy_s_rows_are_ignored_and_captures_clean_up(self):
        s_only = self.upload(f'S\t{VERTEX}\t0', f'S\t{COMPUTE}\t5')
        mixed = self.upload(f'S\t{VERTEX}\t0', f'C\t{COMPUTE}')
        applied = self.run_import('--apply').stdout
        self.assertIn('2 ignored S', applied)
        self.assertIn('1 added, 1 ignored S', applied)
        self.assertIn('Deleted 2 inbox capture(s).', applied)
        self.assertFalse(s_only.exists())
        self.assertFalse(mixed.exists())
        text = self.manifest.read_text()
        self.assertNotIn('S\t', text)
        self.assertIn(f'C\t{COMPUTE}\n', text)

    def test_geometry_adds_valid_new_pairs_once_and_keeps_stage_mismatches(self):
        vertex, geometry = VERTEX[:-2], GEOMETRY[:-2]
        pair = f'G\t{vertex}\t{geometry}'
        capture = self.upload(pair, pair)
        wrong_stage = self.upload(f'G\t{vertex}\t{COMPUTE[:-2]}')

        preview = self.run_import().stdout
        self.assertIn('1 added, 1 duplicate', preview)
        self.assertIn('1 stage mismatch', preview)
        self.assertIn(f'Next shader build: mesh pair {vertex} {geometry}', preview)
        self.assertIn('1 inbox capture(s) would be deleted', preview)
        self.assertEqual(self.manifest.read_text(), self.original)

        self.assertIn('Deleted 1 inbox capture(s).', self.run_import('--apply').stdout)
        self.assertFalse(capture.exists())
        self.assertTrue(wrong_stage.is_file())
        self.assertEqual(self.manifest.read_text(), self.original + f'# Inbox captures from 1.0.0.\n{pair}\n')

        capture = self.upload(pair)
        self.assertIn('No manifest updates.', self.run_import('--apply').stdout)
        self.assertFalse(capture.exists())

    def test_geometry_pairs_in_build_tables_need_no_manifest_rows(self):
        generated = f'{VERTEX[:-2]}\t{GEOMETRY[:-2]}'
        manual = f'{SECOND_VERTEX[:-2]}\t{GEOMETRY[:-2]}'
        self.manifest.with_name('geo_pairs.tsv').write_text(f'# Generated table\n{generated}\n')
        self.manifest.with_name('geo_mesh.tsv').write_text(f'# Manually requested pairs\n{manual}\n')
        capture = self.upload(f'G\t{generated}', f'G\t{manual}')

        applied = self.run_import('--apply').stdout
        self.assertIn('2 duplicate', applied)
        self.assertIn('No manifest updates.', applied)
        self.assertIn('Deleted 1 inbox capture(s).', applied)
        self.assertFalse(capture.exists())
        self.assertEqual(self.manifest.read_text(), self.original)

    def test_apply_deletes_fully_imported_captures_including_duplicate_only(self):
        seed = f'C\t{COMPUTE}'
        capture = self.upload(seed)
        self.assertIn('1 inbox capture(s) would be deleted', self.run_import().stdout)
        self.assertTrue(capture.is_file())
        self.assertIn('Deleted 1 inbox capture(s).', self.run_import('--apply').stdout)
        self.assertFalse(capture.exists())
        imported = self.manifest.read_text()
        self.assertIn(seed, imported)

        self.upload(seed)
        retry = self.run_import('--apply').stdout
        self.assertIn('No manifest updates.', retry)
        self.assertIn('Deleted 1 inbox capture(s).', retry)
        self.assertFalse(capture.exists())
        self.assertEqual(self.manifest.read_text(), imported)

    def test_apply_keeps_captures_with_missing_shaders_even_without_additions(self):
        missing = f'C\t{"0" * 16}_b'
        captures = (self.upload(missing), self.upload(f'R\t{key(VERTEX)}', missing))

        self.assertNotIn('would be deleted', self.run_import().stdout)
        result = self.run_import('--apply').stdout
        self.assertIn('1 duplicate, 1 missing shader', result)
        self.assertIn('No manifest updates.', result)
        self.assertTrue(all(capture.is_file() for capture in captures))
        self.assertEqual(self.manifest.read_text(), self.original)

    def test_apply_retries_missing_rows_after_the_shader_map_gains_the_key(self):
        seed = f'C\t{COMPUTE}'
        shader = '0' * 16
        missing = f'C\t{shader}_b'
        capture = self.upload(seed, missing, version='1.0.1')
        original_capture = capture.read_bytes()

        self.run_import('--apply')
        partial = self.original + f'# Inbox captures from 1.0.1.\n{seed}\n'
        self.assertEqual(self.manifest.read_text(), partial)

        self.assertIn('No manifest updates.', self.run_import('--apply').stdout)
        self.assertEqual(capture.read_bytes(), original_capture)
        self.assertEqual(self.manifest.read_text(), partial)

        with self.manifest.with_name('shader_map.tsv').open('a') as shader_map:
            shader_map.write(f'shaders\t6\tcompute\t{shader}\n')
        result = self.run_import('--apply').stdout
        self.assertIn('1 added, 1 duplicate', result)
        self.assertIn('Deleted 1 inbox capture(s).', result)
        self.assertFalse(capture.exists())
        self.assertEqual(self.manifest.read_text(), partial + f'# Inbox captures from 1.0.1.\n{missing}\n')

    def test_unavailable_version_keeps_unresolved_capture_until_matching_map_is_available(self):
        shared = f'C\t{COMPUTE}'
        update = f'C\t{UPDATE_COMPUTE}'
        capture = self.upload(f'R\t{key(VERTEX)}', shared, update, version='1.0.1')

        applied = self.run_import('--apply').stdout
        self.assertIn('1 added, 1 duplicate, 1 missing shader', applied)
        self.assertTrue(capture.is_file())
        self.assertIn(shared, self.manifest.read_text())
        self.assertNotIn(update, self.manifest.read_text())

        self.add_local_profile(display='1.0.1', build='b' * 32)
        retry = self.run_import('--apply').stdout
        self.assertIn('no matching version map', retry)
        self.assertIn('2 duplicate, 1 missing shader', retry)
        self.assertTrue(capture.is_file())

        local = self.add_local_profile()
        applied = self.run_import('--apply').stdout
        self.assertIn('1 added, 2 duplicate', applied)
        self.assertFalse(capture.exists())
        self.assertIn(update, self.manifest.read_text())
        self.assertFalse((local / 'pso_manifest.tsv').exists())

    def test_mixed_versions_use_their_own_shader_coverage(self):
        self.add_local_profile()
        base = self.upload(f'C\t{COMPUTE}', f'C\t{UPDATE_COMPUTE}')
        update = self.upload(f'C\t{UPDATE_COMPUTE}', version='1.0.1')
        covered = self.upload(f'R\t{key(SECOND_VERTEX)}', version='1.0.2')

        applied = self.run_import('--apply').stdout
        self.assertIn(f'{base.name}: 1 added, 1 missing shader', applied)
        self.assertIn(f'{update.name}: 1 added', applied)
        self.assertIn(f'{covered.name}: 1 added', applied)
        self.assertFalse(update.exists())
        self.assertFalse(covered.exists())
        self.assertTrue(base.exists())

    def test_generated_geometry_pairs_follow_the_capture_version(self):
        local = self.add_local_profile()
        shutil.copyfile(self.manifest.with_name('shader_map.tsv'), local / 'shader_map.tsv')
        base_pair = f'{VERTEX[:-2]}\t{GEOMETRY[:-2]}'
        update_pair = f'{SECOND_VERTEX[:-2]}\t{GEOMETRY[:-2]}'
        self.manifest.with_name('geo_pairs.tsv').write_text(base_pair + '\n')
        (local / 'geo_pairs.tsv').write_text(update_pair + '\n')
        capture = self.upload(f'G\t{base_pair}', f'G\t{update_pair}', version='1.0.1')

        applied = self.run_import('--apply').stdout
        self.assertIn('1 added, 1 duplicate', applied)
        self.assertIn(f'G\t{base_pair}', self.manifest.read_text())
        self.assertNotIn(f'G\t{update_pair}', self.manifest.read_text())
        self.assertFalse(capture.exists())

    def test_push_updates_main_without_including_local_commits(self):
        bare = self.init_origin()
        peer = self.root / 'peer'
        self.git('clone', str(bare), str(peer))
        self.git('commit', '--allow-empty', '-m', 'upstream update', cwd=peer)
        upstream = self.git('rev-parse', 'HEAD', cwd=peer)
        self.git('push', 'origin', 'HEAD:main', cwd=peer)

        imported = self.upload(f'C\t{COMPUTE}')
        self.run_import('--push')
        self.assertFalse(imported.exists())
        self.assertEqual(self.git('rev-parse', 'HEAD^'), upstream)
        landed = self.git('rev-parse', 'HEAD')
        self.assertEqual(self.git('rev-parse', 'refs/heads/main', cwd=bare), landed)
        self.assertEqual(self.git('diff-tree', '--no-commit-id', '--name-only', '-r', 'HEAD'),
                         self.manifest.relative_to(self.repo).as_posix())

        self.git('commit', '--allow-empty', '-m', 'local change')
        pending = self.upload(f'R\t{key(SECOND_VERTEX)}')
        result = self.run_import('--push', check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn('local commits', result.stderr)
        self.assertEqual(self.git('rev-parse', 'refs/heads/main', cwd=bare), landed)
        self.assertNotIn(f'R\t{key(SECOND_VERTEX)}', self.manifest.read_text())
        self.assertTrue(pending.is_file())

    def test_without_shader_map_validates_format_and_cleans_up(self):
        self.manifest.with_name('shader_map.tsv').unlink()
        seed = f'C\t{COMPUTE}'
        bad = 'R\t00'
        ok_capture = self.upload(f'S\t{VERTEX}\t0', seed)
        bad_capture = self.upload(bad)
        applied = self.run_import('--apply').stdout
        self.assertIn('no shader map available, format-validated only', applied)
        self.assertIn('1 added, 1 ignored S', applied)
        self.assertIn('1 invalid pipeline', applied)
        self.assertIn('Deleted 1 inbox capture(s).', applied)
        self.assertFalse(ok_capture.exists())
        self.assertTrue(bad_capture.is_file())
        self.assertIn(seed + '\n', self.manifest.read_text())

    def test_rejected_push_keeps_capture(self):
        bare = self.init_origin()
        hook = bare / 'hooks/pre-receive'
        hook.write_text('#!/bin/sh\nexit 1\n')
        hook.chmod(0o755)
        seed = f'C\t{COMPUTE}'
        capture = self.upload(seed)

        result = self.run_import('--push', check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn('pre-receive hook declined', result.stderr)
        self.assertIn(seed, self.manifest.read_text())
        self.assertTrue(capture.is_file())
