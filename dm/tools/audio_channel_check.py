#!/usr/bin/env python3
"""Check the host's multichannel output end to end on a real loopback device.

The probe (dm/host/tests/t_audio_live.c, built by `make -C dm/host/tests GAME=<id> audio-live`) plays a test signal through the host's
AudioOut on the named device and captures that device's input in the same process (DM_AUDIO_CAPTURE), so what it measures is what the
device received from the host.

Six channels (a six-channel device such as BlackHole 16ch): channels 1..6 must carry the tones 440, 660, 880, 60, 1320, 1760 Hz
(front-left, front-right, center, LFE, back-left, back-right), each at least 12 dB above the others.
Stereo (a two-channel device such as BlackHole 2ch, the six-channel guest downmixed by the host): left carries 440 Hz at 1.0, 880 Hz and 1320 Hz
at 0.707 (center and back-left), 60 Hz at 0.251 (LFE); right the same with 660 and 1760 Hz. Each within 1.5 dB.

Usage: audio_channel_check.py [--device "BlackHole 16ch"] [--stereo] [--seconds 4]
Exit status 0 when every check passes, 1 when one fails, 2 when the probe is missing or the capture is unusable.
"""
import argparse, array, math, os, shutil, subprocess, sys, tempfile

RATE = 48000
TONES = [440.0, 660.0, 880.0, 60.0, 1320.0, 1760.0]   # front-left, front-right, center, LFE, back-left, back-right
SIX_ORDER = ['front-left', 'front-right', 'center', 'LFE', 'back-left', 'back-right']
# The host's downmix (hle_audio.c pull_device, hle_audren.c): L = FL + 0.707 C + 0.251 LFE + 0.707 BL, R = FR + 0.707 C + 0.251 LFE + 0.707 BR
STEREO_EXPECT = {
    'L': {440.0: 1.0, 880.0: 0.707, 60.0: 0.251, 1320.0: 0.707},
    'R': {660.0: 1.0, 880.0: 0.707, 60.0: 0.251, 1760.0: 0.707},
}
STEREO_TOL_DB = 1.5
SIX_SEPARATION_DB = 12.0
PROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'host', 'tests', 'build', 't_audio_live')


def amplitude(x, rate, start, n, f):
    """Amplitude of a sine at exactly f in x[start:start+n]: Hann-windowed correlation (the window's leakage at the test tones' 220 Hz spacing is negligible)."""
    re = im = wsum = 0.0
    for i in range(n):
        w = 0.5 - 0.5 * math.cos(2 * math.pi * i / n)
        v = x[start + i] * w
        a = 2 * math.pi * f * i / rate
        re += v * math.cos(a); im -= v * math.sin(a); wsum += w
    return 2 * math.hypot(re, im) / wsum


def steady_window(chans, rate, start_offset=0.3, length=1.0, block_ms=10):
    """(start, n) of a `length`-second window that starts `start_offset` after the first signal and has signal in every channel, or None."""
    block = max(1, rate * block_ms // 1000)
    nb = min(len(c) for c in chans) // block
    rms = [[math.sqrt(sum(v * v for v in c[i * block:(i + 1) * block]) / block) for i in range(nb)] for c in chans]
    active = [max(r[i] for r in rms) > 0.01 for i in range(nb)]
    if not any(active):
        return None
    start = active.index(True) * block + int(start_offset * rate)
    n = int(length * rate)
    if start + n > min(len(c) for c in chans) or any(rms[c][(start + i) // block] <= 1e-4 for c in range(len(chans)) for i in range(0, n, block)):
        return None
    return start, n


def levels(rate, x, start, n, part=0.25):
    """Amplitude of each test tone over the window: the median over quarter-second parts."""
    m = max(1, int(part * rate))
    per = [{f: amplitude(x, rate, start + k * m, m, f) for f in TONES} for k in range(n // m)]
    return {f: sorted(p[f] for p in per)[len(per) // 2] for f in TONES}


def db(a, b):
    return 20 * math.log10(max(a, 1e-12) / max(b, 1e-12))


def check_six(chan_levels):
    """chan_levels: six dicts (tone -> amplitude), one per channel. Returns a list of (ok, message)."""
    out = []
    for c, lv in enumerate(chan_levels):
        if lv is None:
            out.append((False, 'channel %d: no signal' % (c + 1))); continue
        want = TONES[c]
        others = max(v for f, v in lv.items() if f != want)
        sep = db(lv[want], others)
        ok = sep >= SIX_SEPARATION_DB and max(lv, key=lv.get) == want
        out.append((ok, 'channel %d (%s): %g Hz at %.3f, %.1f dB above the rest' % (c + 1, SIX_ORDER[c], want, lv[want], sep)))
    return out


def check_stereo(chan_levels):
    """chan_levels: [left, right] dicts. Returns a list of (ok, message)."""
    out = []
    for name, lv, expect, unit_f in zip(('L', 'R'), chan_levels, (STEREO_EXPECT['L'], STEREO_EXPECT['R']), (440.0, 660.0)):
        if lv is None:
            out.append((False, '%s: no signal' % name)); continue
        unit = lv[unit_f]
        for f, rel in expect.items():
            if f == unit_f:
                continue
            got = lv[f] / unit
            err = db(got, rel)
            out.append((abs(err) <= STEREO_TOL_DB, '%s %g Hz: %.3f of %g Hz (expected %.3f, %+.1f dB)' % (name, f, got, unit_f, rel, err)))
        leak = max(lv[f] for f in TONES if f not in expect) / unit
        out.append((db(leak, 1) < -30, '%s: the other tones at most %.1f dB below the unit' % (name, db(leak, 1))))
    return out


def read_capture(path, nch):
    a = array.array('f')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    n = len(a) // nch
    return [list(a[c:n * nch:nch]) for c in range(nch)]


def unusable(msg):
    print(msg, file=sys.stderr)
    sys.exit(2)


def run(device, stereo, seconds):
    """Returns the result lines' pass state, or None when the capture is unusable."""
    if not os.path.exists(PROBE):
        unusable('missing %s: make -C dm/host/tests GAME=<id> audio-live' % PROBE)
    nch = 2 if stereo else 6
    tmp = tempfile.mkdtemp(prefix='audiocheck-')
    try:
        return checked(device, stereo, seconds, nch, os.path.join(tmp, 'capture.f32'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def checked(device, stereo, seconds, nch, cap):
    env = dict(os.environ, DM_AUDIO_DEVICE=device, DM_AUDIO_CAPTURE=cap, DM_AUDIO_CAPTURE_CH=str(nch))
    try:
        play = subprocess.run([PROBE, str(seconds)], env=env, capture_output=True, text=True, timeout=seconds + 60)
    except subprocess.TimeoutExpired:
        unusable('the probe did not finish within %d s' % (seconds + 60))
    print(play.stdout.strip())
    if play.returncode:
        unusable('the probe failed (status %d): %s' % (play.returncode, play.stderr.strip().splitlines()[-1:]))
    route = [l for l in play.stdout.splitlines() if l.startswith('opened:')]
    want_unit = '2 channels' if stereo else '6 channels'
    results = [(bool(route) and ('unit %s' % want_unit) in route[0], 'host unit: %s' % (route[0] if route else 'not opened'))]
    if not os.path.exists(cap):
        unusable('no capture written (the probe said: %s)' % play.stdout.strip().splitlines()[-1:])
    chans = read_capture(cap, nch)
    win = steady_window(chans, RATE)
    if win is None:
        return None
    lv = [levels(RATE, chans[c], *win) for c in range(nch)]
    results += check_stereo(lv) if stereo else check_six(lv)
    for ok, msg in results:
        print('%s  %s' % ('ok  ' if ok else 'FAIL', msg))
    return all(ok for ok, _ in results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--device', default='BlackHole 16ch')
    ap.add_argument('--stereo', action='store_true', help='a two-channel device: check the downmix')
    ap.add_argument('--seconds', type=int, default=4)
    a = ap.parse_args()
    ok = run(a.device, a.stereo, a.seconds)
    if ok is None:
        print('the capture has no steady tone in every channel: check the device and the probe output above')
        sys.exit(2)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
