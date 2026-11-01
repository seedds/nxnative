"""The profile's unity_tier keeps only the programs of the hardware tier the player runs, unity_skip_keywords and unity_skip_passes leave out variants and passes it never draws."""
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

os.environ.setdefault('DM_GAME', '010015100B514000')
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def legacy(*subs):   # (blob index, tier) per subprogram, Unity before 2022
    return {'m_ParsedForm': {'m_SubShaders': [{'m_Passes': [
        {'progVertex': {'m_SubPrograms': [{'m_BlobIndex': b, 'm_ShaderHardwareTier': t} for b, t in subs]}}]}]}}


def passes(*modes):   # (LightMode, blob index, tier) per pass, the tag in m_State as Unity 2019.4 keeps it
    return {'m_ParsedForm': {'m_SubShaders': [{'m_Passes': [
        {'m_State': {'m_Tags': {'tags': [{'first': 'LIGHTMODE', 'second': m}]}}, 'progVertex': {'m_SubPrograms': [{'m_BlobIndex': b, 'm_ShaderHardwareTier': t}]}}
        for m, b, t in modes]}]}}


def player(*tiers):   # blob indices per tier, Unity 2022's m_PlayerSubPrograms
    return {'m_ParsedForm': {'m_SubShaders': [{'m_Passes': [
        {'progVertex': {'m_ParameterBlobIndices': [], 'm_PlayerSubPrograms': [[{'m_BlobIndex': b} for b in t] for t in tiers]}}]}]}}


class UnityTierTests(unittest.TestCase):
    def setUp(self):   # the keys as a profile without them sets them, whichever profile DM_GAME names
        for k in ('UNITY_SKIP_KW', 'UNITY_SKIP_PASSES'):
            p = mock.patch.object(split, k, frozenset()); p.start(); self.addCleanup(p.stop)

    def blobs(self, obj, tier):
        with mock.patch.object(split, 'UNITY_TIER', tier): return split.unity_code_blobs(obj)

    def test_without_the_key_every_tier_is_read(self):
        self.assertIsNone(self.blobs(legacy((0, 0), (1, 1), (2, 2)), -1))
        self.assertEqual(self.blobs(player([0], [1], [2]), -1), {0, 1, 2})

    def test_legacy_keeps_the_tier_and_shared_blobs(self):
        # Overcooked's form: one blob per tier; another game shares one blob among all three
        self.assertEqual(self.blobs(legacy((0, 0), (1, 1), (2, 2)), 2), {2})
        self.assertEqual(self.blobs(legacy((5, 0), (5, 1), (5, 2)), 2), {5})

    def test_player_subprograms_by_tier(self):
        self.assertEqual(self.blobs(player([0], [1], [2, 3]), 2), {2, 3})
        self.assertEqual(self.blobs(player([0, 1]), 2), {0, 1})   # fewer tiers listed than asked for: all of them

    def test_skip_keywords_global_or_local(self):
        with mock.patch.object(split, 'UNITY_SKIP_KW', frozenset({'DIRLIGHTMAP_SEPARATE'})):
            self.assertTrue(split.unity_skipped(['DIRECTIONAL', 'DIRLIGHTMAP_SEPARATE'], []))   # Unity 2019.4: global keywords only
            self.assertTrue(split.unity_skipped([], ['DIRLIGHTMAP_SEPARATE']))
            self.assertFalse(split.unity_skipped(['DIRECTIONAL', 'DIRLIGHTMAP_OFF'], []))
        self.assertFalse(split.unity_skipped(['DIRLIGHTMAP_SEPARATE'], []))   # no key: nothing is left out

    def test_skip_passes_by_lightmode(self):
        o = passes(('FORWARDBASE', 0, 2), ('META', 1, 2), ('FORWARDBASE', 2, 1))
        with mock.patch.object(split, 'UNITY_SKIP_PASSES', frozenset({'META'})):
            self.assertEqual(self.blobs(o, 2), {0})
            self.assertEqual(self.blobs(o, -1), {0, 2})   # every tier, still without the Meta pass
        with mock.patch.object(split, 'UNITY_SKIP_PASSES', frozenset()):
            self.assertEqual(self.blobs(o, 2), {0, 1})
            self.assertIsNone(self.blobs(o, -1))


if __name__ == '__main__':
    unittest.main()
