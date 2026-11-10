"""dm/tools/audio_channel_check.py: the channel mapping and downmix checks accept a correct output and reject a wrong one (synthetic signals, no device)."""
import math, os, sys, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import audio_channel_check as m

RATE = 48000
SECONDS = 1.6


def tone(f, amp, n):
    return [amp * math.sin(2 * math.pi * f * i / RATE) for i in range(n)]


def mix(parts, n):
    out = [0.0] * n
    for f, amp in parts:
        for i, v in enumerate(tone(f, amp, n)):
            out[i] += v
    return out


def six_channels(order):
    """Six channels, each carrying the tone of `order[c]` at 0.2 full scale."""
    n = int(SECONDS * RATE)
    return [tone(order[c], 0.2, n) for c in range(6)]


def levels_of(chans):
    win = m.steady_window(chans, RATE)
    assert win is not None
    return [m.levels(RATE, c, *win) for c in chans]


class SixChannels(unittest.TestCase):
    def test_correct_mapping_passes(self):
        results = m.check_six(levels_of(six_channels(m.TONES)))
        self.assertTrue(all(ok for ok, _ in results), results)

    def test_swapped_front_pair_fails(self):
        order = list(m.TONES); order[0], order[1] = order[1], order[0]
        results = m.check_six(levels_of(six_channels(order)))
        self.assertFalse(results[0][0]); self.assertFalse(results[1][0])

    def test_center_on_lfe_fails(self):
        order = list(m.TONES); order[2], order[3] = order[3], order[2]
        results = m.check_six(levels_of(six_channels(order)))
        self.assertFalse(results[2][0]); self.assertFalse(results[3][0])

    def test_leak_below_the_separation_passes_and_above_fails(self):
        # the 12 dB separation the check asks for: a leak of the next tone at -20 dB passes, one at -6 dB fails
        n = int(SECONDS * RATE)
        for gain, ok in ((0.1, True), (0.5, False)):
            chans = six_channels(m.TONES)
            chans[0] = [a + gain * b for a, b in zip(chans[0], tone(m.TONES[1], 0.2, n))]
            self.assertEqual(m.check_six(levels_of(chans))[0][0], ok)


class Stereo(unittest.TestCase):
    def downmix(self, c_coef=0.707):
        """Left and right of the host's downmix for the six-channel test tones (the same coefficients as the host)."""
        n = int(SECONDS * RATE)
        six = [tone(f, 0.2, n) for f in m.TONES]
        left = [a + c_coef * c + 0.251 * l + 0.707 * b for a, c, l, b in zip(six[0], six[2], six[3], six[4])]
        right = [a + c_coef * c + 0.251 * l + 0.707 * b for a, c, l, b in zip(six[1], six[2], six[3], six[5])]
        return [left, right]

    def test_host_coefficients_pass(self):
        results = m.check_stereo(levels_of(self.downmix()))
        self.assertTrue(all(ok for ok, _ in results), results)

    def test_wrong_center_coefficient_fails(self):
        results = m.check_stereo(levels_of(self.downmix(c_coef=1.0)))
        self.assertFalse(all(ok for ok, _ in results))

    def test_missing_back_channels_fail(self):
        n = int(SECONDS * RATE)
        front_only = [mix([(440.0, 0.2), (880.0, 0.1414)], n), mix([(660.0, 0.2), (880.0, 0.1414)], n)]
        self.assertFalse(all(ok for ok, _ in m.check_stereo(levels_of(front_only))))


class Capture(unittest.TestCase):
    def test_gap_in_the_window_is_refused(self):
        chans = six_channels(m.TONES)
        for c in chans:
            for i in range(int(0.9 * RATE), int(1.1 * RATE)):
                c[i] = 0.0   # a silent 200 ms inside the window
        self.assertIsNone(m.steady_window(chans, RATE))


if __name__ == '__main__':
    unittest.main()
