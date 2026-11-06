"""The launcher bridge and machine-readable roster, each run in a fresh host process."""
from contextlib import ExitStack, contextmanager
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import unittest

HOST = Path(__file__).resolve().parents[2] / 'host'
LEGACY = '53574e415449564578626c0000000001'
SECOND = '000000000000000000000000000000ab'
ALICE = {'id': LEGACY, 'nickname': 'Alice'}
BOB = {'id': SECOND, 'nickname': 'Bob'}


@unittest.skipUnless(sys.platform == 'darwin', 'Apple host library')
class LocalUsers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        root = Path(tmp.name)
        probe = root / 'probe.c'
        probe.write_text(r'''
#include "local_users.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
int main(int argc, char **argv) {
    // Launcher calls prepare before the first registry lookup. Raw host queries do not.
    if (argc == 6 && !dm_users_prepare_file(argv[1], argv[2])) return 1;
    if (dm_users_error()) { fprintf(stderr, "%s", dm_users_error()); return 2; }
    if (argc == 6) {
        const char *id = strcmp(argv[4], "-") ? argv[4] : NULL;
        if (!strcmp(argv[3], "await")) {
            // Keep this launcher's snapshot open while another launcher edits the file.
            puts("ready"); fflush(stdout);
            if (getchar() == EOF) return 1;
        }
        if (!strcmp(argv[3], "frozen") && !dm_users_begin_session()) return 1;
        if (!!dm_users_can_edit() != !dm_users_selection_locked()) return 1;
        const char *error = dm_users_edit(id, argv[5]);
        if (error) {
            fprintf(stderr, "%s", error);
            return dm_users_write_json(stdout) ? 7 : 1;
        }
        const dm_local_user *user = dm_selected_user();
        if (!user || strcmp(user->nickname, argv[5]) || (id && strcasecmp(user->id, id))) return 1;
        if (!dm_users_begin_session() || dm_users_can_edit() || !dm_users_edit(NULL, "After Play")) return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "pick")) {
        if (dm_users_selection_locked() || !dm_users_select_index(1)) return 3;
        if (strcmp(getenv("DM_USER_ID"), dm_selected_user()->id)) return 4;
        if (!dm_users_begin_session() || dm_users_select_index(0)) return 5;
    }
    if (argc == 2 && !strcmp(argv[1], "locked")) {
        if (!dm_users_selection_locked() || dm_users_select_index(0)) return 6;
    }
    return dm_users_write_json(stdout) ? 0 : 1;
}
''')
        cls.exe = root / 'probe'
        subprocess.run(['clang', '-Wall', '-Werror', '-fobjc-arc', '-I', HOST, probe,
                        HOST / 'local_users.m', '-framework', 'Foundation', '-o', cls.exe], check=True)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.path = self.root / 'users.json'
        self.env = {k: v for k, v in os.environ.items() if k not in ('DM_USERS_FILE', 'DM_USER_ID', 'DM_NICK')}

    def query(self, roster=None, *, selected=None, action=None, raw=None, path=None, nick=None):
        env = self.env.copy()
        if roster is not None or raw is not None:
            path = self.root / 'users.json'
            path.write_bytes(json.dumps(roster).encode() if raw is None else raw)
        if path is not None:
            env['DM_USERS_FILE'] = str(path)
        if selected is not None:
            env['DM_USER_ID'] = selected
        if nick is not None:
            env['DM_NICK'] = nick
        return subprocess.run([self.exe] + ([action] if action else []), env=env, capture_output=True, timeout=5)

    def edit(self, nickname, uid='-', *, mode='edit', **env):
        return subprocess.run([self.exe, self.root, self.path, mode, uid, nickname],
                              env={**self.env, **env}, capture_output=True, timeout=5)

    @contextmanager
    def pending_edit(self, nickname, uid='-'):
        with subprocess.Popen([self.exe, self.root, self.path, 'await', uid, nickname],
                              env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
            try:
                assert child.stdout is not None
                self.assertTrue(select.select([child.stdout], [], [], 5)[0], 'launcher did not initialize')
                self.assertEqual(child.stdout.readline(), b'ready\n')
                yield child
            finally:
                if child.poll() is None:
                    child.kill()

    def test_default_and_legacy_nickname(self):
        self.assertEqual(json.loads(self.query().stdout),
                         {'version': 1, 'users': [dict(ALICE, nickname='Player')], 'selected': LEGACY})
        self.assertEqual(json.loads(self.query(nick='Player One').stdout)['users'][0]['nickname'], 'Player One')

    def test_first_create_preserves_the_original_account(self):
        result = self.edit('New player', DM_NICK='Original player')
        self.assertEqual(result.returncode, 0, result.stderr)
        snapshot = json.loads(result.stdout)
        users = snapshot['users']
        self.assertEqual([u['nickname'] for u in users], ['Original player', 'New player'])
        self.assertEqual(users[0]['id'], LEGACY)
        new = users[1]['id']
        self.assertRegex(new, r'^[0-9a-f]{32}$')
        self.assertNotIn(new, [LEGACY, '0' * 32])
        self.assertEqual(snapshot, {'version': 1, 'users': users, 'selected': new})
        # The same JSON API used by dmhost --list-users in a separate launch.
        self.assertEqual(json.loads(self.query(path=self.path, selected=new).stdout), snapshot)
        self.assertEqual(json.loads(self.query(path=self.path).stdout)['selected'], LEGACY)

    def test_first_rename_preserves_the_original_id(self):
        result = self.edit('Original renamed', LEGACY)
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = {'version': 1, 'users': [dict(ALICE, nickname='Original renamed')], 'selected': LEGACY}
        self.assertEqual(json.loads(result.stdout), expected)
        self.assertEqual(json.loads(self.query(path=self.path).stdout), expected)

    def test_rename_then_create_preserve_frontend_metadata_and_custom_path(self):
        custom = self.root / 'family.json'
        custom.write_text(json.dumps({'version': 1, 'users': [ALICE, dict(BOB, icon='star')], 'theme': {'color': 'blue'}}))
        result = self.edit('Robert', SECOND, DM_USERS_FILE=custom.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {'version': 1, 'users': [ALICE, dict(BOB, nickname='Robert')], 'selected': SECOND})
        result = self.edit('Third', DM_USERS_FILE=custom.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        saved = json.loads(custom.read_text())
        self.assertEqual(saved['theme'], {'color': 'blue'})
        self.assertEqual(saved['users'][0:2], [ALICE, dict(BOB, nickname='Robert', icon='star')])
        self.assertFalse(self.path.exists())

    def test_nickname_limit_in_utf8_bytes_and_failed_edits_keep_state(self):
        self.path.write_text(json.dumps({'version': 1, 'users': [ALICE, BOB]}))
        valid = '\u00e9' * 16
        renamed = self.edit(valid, SECOND)
        self.assertEqual(renamed.returncode, 0, renamed.stderr)
        before = self.path.read_bytes()
        expected = {'version': 1, 'users': [ALICE, dict(BOB, nickname=valid)], 'selected': LEGACY}
        for nickname, uid in [('', '-'), (valid + 'a', SECOND), ('with\nnewline', '-'), (b'\xff', SECOND),
                              ('Name', '000000000000000000000000000000ff')]:
            with self.subTest(nickname=nickname, uid=uid):
                failed = self.edit(nickname, uid)
                self.assertEqual(failed.returncode, 7, failed.stderr)
                self.assertTrue(failed.stderr)
                self.assertEqual(json.loads(failed.stdout), expected)
                self.assertEqual(self.path.read_bytes(), before)

    def test_original_account_counts_toward_the_eight_user_limit(self):
        for i in range(1, 8):
            result = self.edit(f'Player {i}')
            self.assertEqual(result.returncode, 0, result.stderr)
        before = self.path.read_bytes()
        expected = json.loads(self.query(path=self.path).stdout)
        self.assertEqual(len({u['id'] for u in expected['users']}), 8)
        self.assertEqual(expected['users'][0]['id'], LEGACY)
        result = self.edit('Ninth')
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(json.loads(result.stdout), expected)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(self.edit('New name at capacity', expected['users'][-1]['id']).returncode, 0)

    def test_explicit_selection_and_game_start_prevent_edits(self):
        self.path.write_text(json.dumps({'version': 1, 'users': [ALICE, BOB]}))
        before = self.path.read_bytes()
        for mode, env, active in [('edit', {'DM_USER_ID': SECOND}, SECOND), ('frozen', {}, LEGACY)]:
            for uid in ['-', LEGACY]:
                with self.subTest(mode=mode, uid=uid):
                    result = self.edit('Bad edit', uid, mode=mode, **env)
                    self.assertEqual(result.returncode, 7, result.stderr)
                    self.assertEqual(json.loads(result.stdout), {'version': 1, 'users': [ALICE, BOB], 'selected': active})
                    self.assertEqual(self.path.read_bytes(), before)
        result = self.edit('First', DM_USERS_FILE='missing.json')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(result.stderr)
        self.assertFalse((self.root / 'missing.json').exists())

    def test_stale_launcher_merges_another_launchers_create(self):
        with self.pending_edit('First renamed', LEGACY) as child:
            created = self.edit('Second')
            self.assertEqual(created.returncode, 0, created.stderr)
            out, err = child.communicate(input=b'\n', timeout=5)
            self.assertEqual(child.returncode, 0, err)
        second = json.loads(created.stdout)['users'][1]
        expected = {'version': 1, 'users': [dict(ALICE, nickname='First renamed'), second], 'selected': LEGACY}
        self.assertEqual(json.loads(out), expected)
        self.assertEqual(json.loads(self.query(path=self.path).stdout), expected)

    def test_concurrent_creates_keep_every_account(self):
        names = ['Ann', 'Ben', 'Cal', 'Dee']
        with ExitStack() as stack:
            children = [stack.enter_context(self.pending_edit(name)) for name in names]
            # They all saw the same original account before the first writer starts.
            for child in children:
                assert child.stdin is not None
                child.stdin.write(b'\n')
                child.stdin.flush()
            selected = set()
            for child in children:
                out, err = child.communicate(timeout=5)
                self.assertEqual(child.returncode, 0, err)
                selected.add(json.loads(out)['selected'])
        saved = json.loads(self.query(path=self.path).stdout)['users']
        self.assertEqual(saved[0], dict(ALICE, nickname='Player'))
        self.assertCountEqual([u['nickname'] for u in saved[1:]], names)
        self.assertEqual({u['id'] for u in saved[1:]}, selected)
        self.assertEqual(len(selected), len(names))

    def test_conflicting_rename_preserves_the_other_launchers_change(self):
        self.path.write_text(json.dumps({'version': 1, 'users': [ALICE, BOB]}))
        with self.pending_edit('Conflicting', SECOND) as child:
            winner = self.edit('Already renamed', SECOND)
            self.assertEqual(winner.returncode, 0, winner.stderr)
            before = self.path.read_bytes()
            _, err = child.communicate(input=b'\n', timeout=5)
            self.assertEqual(child.returncode, 7, err)
            self.assertTrue(err)
        self.assertEqual(self.path.read_bytes(), before)

    def test_storage_failure_preserves_loaded_users_and_their_selection(self):
        # The selected roster's directory becomes unreachable between loading and saving.
        self.path = self.root / 'settings' / 'users.json'
        self.path.parent.mkdir()
        self.path.write_text(json.dumps({'version': 1, 'users': [ALICE, BOB]}))
        before = self.path.read_bytes()
        moved = self.root / 'moved'
        with self.pending_edit('New name', SECOND) as child:
            self.path.parent.rename(moved)
            self.path.parent.write_text('a file now occupies this path')
            out, err = child.communicate(input=b'\n', timeout=5)
            self.assertEqual(child.returncode, 7, err)
            self.assertTrue(err)
        self.assertEqual(json.loads(out), {'version': 1, 'users': [ALICE, BOB], 'selected': LEGACY})
        self.assertEqual((moved / 'users.json').read_bytes(), before)

    def test_external_and_launcher_selection(self):
        roster = {'version': 1, 'users': [ALICE, dict(BOB, id=SECOND.upper())]}
        for selected, action, expected in [(None, None, LEGACY), (SECOND, 'locked', SECOND), (None, 'pick', SECOND)]:
            with self.subTest(action=action):
                result = self.query(roster, selected=selected, action=action)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), {'version': 1, 'users': [ALICE, BOB], 'selected': expected})

    def test_invalid_configuration_does_not_select_someone_else(self):
        for roster in [[], {'version': True, 'users': [ALICE]}, {'version': 2, 'users': [ALICE]},
                       {'version': 1, 'users': []}, {'version': 1, 'users': [BOB, dict(BOB, id=SECOND.upper())]},
                       {'version': 1, 'users': [dict(BOB, id='0' * 32)]}, {'version': 1, 'users': [dict(BOB, id='../evil')]},
                       {'version': 1, 'users': [dict(BOB, nickname='x\0y')]},
                       {'version': 1, 'users': [dict(BOB, nickname='\u00e9' * 17)]},
                       {'version': 1, 'users': [dict(BOB, id=f'{i + 1:032x}') for i in range(9)]}]:
            with self.subTest(roster=roster):
                result = self.query(roster)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
                self.assertFalse(result.stdout)
        self.assertNotEqual(self.query({'version': 1, 'users': [ALICE]}, selected=SECOND).returncode, 0)
        self.assertNotEqual(self.query(raw=b'{').returncode, 0)
        self.assertNotEqual(self.query(path=self.root / 'missing').returncode, 0)
        fifo = self.root / 'fifo'
        os.mkfifo(fifo)
        self.assertNotEqual(self.query(path=fifo).returncode, 0)
