"""run_saves.py: the save policy of a host run that is not the player's (regress.py, bench.py, the NSP script's tools and prebuild). Cloud saves end off and the launchers'
choice goes whatever the environment held; the caller's own options asking for sync are refused ('' and 0 are not a request); the last of several options wins, as when they are
merged. The three callers' own tests (test_regress, test_bench, test_nsp_env_checks) check that each applies it last."""
import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import run_saves


class CloudOff(unittest.TestCase):
    def test_inherited_values_end_off(self):
        env = {'DM_SAVE_SYNC': '/Users/p/Library/Mobile Documents/cloud', 'DM_SAVE_SYNC_CHOICE': 'cloud', 'DM_SAVE': '/tmp/run/save', 'OTHER': 'x'}
        self.assertIs(run_saves.cloud_off(env), env)
        self.assertEqual(env, {'DM_SAVE_SYNC': '0', 'DM_SAVE': '/tmp/run/save', 'OTHER': 'x'})   # the scratch save and everything else untouched

    def test_unset_and_off(self):
        self.assertEqual(run_saves.cloud_off({}), {'DM_SAVE_SYNC': '0'})
        for v in ('', '0'): self.assertEqual(run_saves.cloud_off({'DM_SAVE_SYNC': v}, v), {'DM_SAVE_SYNC': '0'})

    def test_asking_for_sync_is_refused(self):
        env = {'DM_SAVE_SYNC': '/cloud'}
        with self.assertRaises(run_saves.CloudSync) as e: run_saves.cloud_off(env, '/cloud')
        self.assertIn('DM_SAVE_SYNC=/cloud', str(e.exception)); self.assertIsInstance(e.exception, ValueError)

    def test_asked(self):
        self.assertIsNone(run_saves.asked([]))
        self.assertIsNone(run_saves.asked(['DM_SAVE=/s', 'DM_SAVE_SYNC_CHOICE=cloud']))
        self.assertEqual(run_saves.asked(['DM_SAVE_SYNC=/cloud', 'DM_X=1', 'DM_SAVE_SYNC=0']), '0')   # the last one, as when merged in order
        self.assertEqual(run_saves.asked(['DM_SAVE_SYNC=']), '')


if __name__ == '__main__':
    unittest.main()
