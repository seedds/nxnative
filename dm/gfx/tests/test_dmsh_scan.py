"""Literal candidate lines preserve the anchored regex's matches, spans and captures."""
import pathlib
import random
import re
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_side, dmsh_scan


class Entries(unittest.TestCase):
    def test_matches_original_scans_with_qualifiers_and_multiline_signatures(self):
        rng = random.Random(104)
        candidates = ['fragment main0_out main0(', '[[ early_fragment_tests ]] fragment main0_out main0(',
                      'extern "C" fragment\nmain0_out\nmain0(', 'fragment main0_out_u main0_u(',
                      'float discard_fragment_count = 0;', 'discard_fragment();', 'float x = 0;',
                      'fragment_count = 0;', 'fragment main0_out main0( ) {} fragment main0_out main0_again(']
        patterns = [dmsh_side._ENTRY,
                    re.compile(r'^[^\n]*?\bfragment\s+(main0_out(?:_[um])?)\s+(main0(?:_[um])?)\s*\(', re.M)]
        for _ in range(100):
            source = '\n'.join(rng.choices(candidates, k=rng.randrange(1, 30)))
            for pattern in patterns:
                expected = [(m.span(), m.groups()) for m in pattern.finditer(source)]
                actual = [(m.span(), m.groups()) for m in dmsh_scan.literal_lines(pattern, source, 'fragment')]
                self.assertEqual(actual, expected)


if __name__ == '__main__': unittest.main()
