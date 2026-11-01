#!/usr/bin/env python3
"""Reads the "Device and OS" field of a bug report (an issue body made by .github/ISSUE_TEMPLATE/bug_report.yml) and decides whether the device is clearly outside
README's Requirements: `unsupported` (with a short reason), `ok`, or `unknown`. Only clear cases are `unsupported`; anything ambiguous (several device families in one
text, a model and a chip that disagree, two OS versions on both sides of the line, no model or version at all) is `unknown`, because a false alarm on a supported device
is worse than a missed one. The workflow .github/workflows/issue-device-check.yml acts on `unsupported` only.

Supported: iPhone with A17 Pro or later (15 Pro and newer), iPad with M1 or later, iOS / iPadOS 26 or later, Mac with Apple silicon on macOS 26 or later.
The build host (macOS 26.6, Xcode 27.1) is not read: the field asks for the device the problem happens on, and a build line would need free-text guessing about which
machine a version belongs to.

  issue_device_check.py [--event FILE | --body FILE] [--comment-file FILE]    # default --event $GITHUB_EVENT_PATH
Prints a JSON object {status, device, reason}; with $GITHUB_OUTPUT set it also appends status=... for the workflow."""
import argparse
import json
import os
import re
import sys

MARKER = '<!-- device-check -->'
REQUIREMENTS = 'https://github.com/seedds/nxnative/blob/main/README.md#requirements'
MIN_OS = 26
MAC_NAMES = {'tahoe': 26, 'sequoia': 15, 'sonoma': 14, 'ventura': 13, 'monterey': 12, 'big sur': 11, 'catalina': 10, 'mojave': 10}


def section(body):
    """The text under the '### Device and OS' heading, or None."""
    m = re.search(r'^#{2,4}[ \t]*Device and OS[ \t]*\r?\n(.*?)(?=^#{1,4}[ \t]|\Z)', body or '', re.M | re.S | re.I)
    if not m:
        return None
    text = ' '.join(m.group(1).split())
    return None if text.lower() in ('', '_no response_', 'no response') else text


def families(t):
    f = set()
    if re.search(r'\biphone\b', t): f.add('iphone')
    if re.search(r'\bipad\b', t): f.add('ipad')
    if re.search(r'\bmac\s?os\b|\bmacbook\b|\bimac\b|\bmac\b|\bmac\s?(mini|studio|pro)\b|\bmac[ -]?os[ -]?x\b', t): f.add('mac')
    if not f and re.search(r'\bios\b|\bios\s?\d', t): f.add('iphone')   # a bare "iOS 18.3" names no model: read as an iPhone
    if not f and re.search(r'\bipados\b', t): f.add('ipad')
    return f


def os_versions(t, pattern):
    return [int(v) for v in re.findall(pattern, t)]


def mac_versions(t):
    v = os_versions(t, r'mac\s?os(?:\s?x)?\s*(?:version\s*)?(?:[a-z]+\s+)?(\d+)')
    v += [n for name, n in MAC_NAMES.items() if re.search(r'\b' + name + r'\b', t)]
    return v


def below_line(versions):
    """True: every version is below 26; False: every one is at or above; None: none named; 'mixed': both sides."""
    low = [v < MIN_OS for v in versions]
    if not low:
        return None
    return low[0] if any(low) == all(low) else 'mixed'


def iphone_model(t):
    """Verdicts ('old' or 'new') of every iPhone model named, with a reason for 'old'."""
    out = []
    for m in re.finditer(r'\biphone[ -]?(?:(se|xs|xr|x|air)(?![a-z])|(\d+)(e?))(\s*(plus|pro|max|mini)?)', t):
        name, rest = m.group(1) or m.group(2), m.group(4)
        if name in ('se', 'xs', 'xr', 'x'):
            out.append(('old', 'iPhone ' + name.upper()))
        elif name == 'air':
            out.append(('new', ''))
        else:
            n = int(name)
            pro = re.match(r'\s*pro', rest) is not None
            if n <= 14 or (n == 15 and not pro and not m.group(3)):
                out.append(('old', 'an iPhone 15 or 15 Plus, which has the A16 chip' if n == 15 else 'an iPhone %d, older than the iPhone 15 Pro' % n))
            else:
                out.append(('new', ''))
    return out


def classify(device):
    """(status, reason) for the text of the Device and OS field."""
    if device is None:
        return 'unknown', 'no Device and OS field'
    t = device.lower()
    fam = families(t)
    if len(fam) != 1:
        return 'unknown', 'no single device family' if not fam else 'several device families'
    fam = next(iter(fam))
    m_chip = re.search(r'\bm[1-9]\b', t) is not None
    a_chips = [int(n) for n in re.findall(r'\ba(\d{1,2})(?:x|z|\s*bionic|\s*pro|\b)', t) if 4 <= int(n) <= 19]
    reasons, seen = [], False
    if fam == 'mac':
        intel = re.search(r'\bintel\b|\bx86|\bcore[ -]?i[3579]\b|\bi[3579]\b|\bi[3579]-\d', t) is not None
        if intel and m_chip:
            return 'unknown', 'Intel and Apple silicon both named'
        if intel:
            reasons.append('an Intel Mac')
        low = below_line(mac_versions(t))
        if low == 'mixed':
            return 'unknown', 'macOS versions on both sides of the line'
        seen = intel or m_chip or low is not None
        if low:
            reasons.append('macOS below %d' % MIN_OS)
    else:
        low = below_line(os_versions(t, r'(?:i|ipad)os\s*(?:version\s*)?(\d+)'))
        if low == 'mixed':
            return 'unknown', 'OS versions on both sides of the line'
        seen = low is not None or m_chip
        if low:
            reasons.append('%s below %d' % ('iPadOS' if fam == 'ipad' else 'iOS', MIN_OS))
        chip_old = None
        if fam == 'iphone':
            models = iphone_model(t)
            verdicts = {v for v, _ in models}
            if len(verdicts) > 1:
                return 'unknown', 'iPhone models on both sides of the line'
            model_old = verdicts == {'old'}
            seen = seen or bool(models)
            if a_chips:
                chip_old = max(a_chips) < 17
                if len(verdicts) == 1 and chip_old != model_old:
                    return 'unknown', 'iPhone model and chip disagree'
                if len({c < 17 for c in a_chips}) > 1:
                    return 'unknown', 'chips on both sides of the line'
                seen = True
            if model_old:
                reasons.append(next(r for v, r in models if v == 'old'))
            elif chip_old:
                reasons.append('an A%d chip, older than the A17 Pro' % max(a_chips))
        elif a_chips and not m_chip:
            if max(a_chips) < 17:   # an A17 Pro iPad mini is left alone: unclear enough
                reasons.append('an iPad with an A%d chip, while iPads need an M1 or later' % max(a_chips))
            seen = True
    if reasons:
        return 'unsupported', ' and '.join(reasons[:2])
    return ('ok' if seen else 'unknown'), 'no clear match'


def comment(device, reason):
    shown = device.replace('`', "'")
    if len(shown) > 80:
        shown = shown[:79] + '...'
    return (MARKER + '\n'
            'Thanks for the report! I read your device as `%s`, which looks outside the devices in the [Requirements](%s) table: %s. '
            'nxnative is tested only on those devices, so the problem may well come from the device. I added the `unsupported device` label so this is easy to spot.\n\n'
            'If I misread your device, say so here and a maintainer will remove the label.\n' % (shown, REQUIREMENTS, reason))


def main(argv=None):
    ap = argparse.ArgumentParser(description='Decide whether a bug report names an unsupported device')
    ap.add_argument('--event', help="a GitHub issue event JSON (default: $GITHUB_EVENT_PATH)")
    ap.add_argument('--body', help='a file with the issue body')
    ap.add_argument('--comment-file', help='write the comment for an unsupported device here')
    a = ap.parse_args(argv)
    if a.body:
        with open(a.body, encoding='utf-8') as f: body = f.read()
    else:
        path = a.event or os.environ.get('GITHUB_EVENT_PATH')
        if not path:
            sys.exit('no --event, --body or GITHUB_EVENT_PATH')
        with open(path, encoding='utf-8') as f: body = (json.load(f).get('issue') or {}).get('body')
    device = section(body)
    status, reason = classify(device)
    if status == 'unsupported' and a.comment_file:
        with open(a.comment_file, 'w', encoding='utf-8') as f: f.write(comment(device or '', reason))
    print(json.dumps({'status': status, 'device': device, 'reason': reason}))
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as f: f.write('status=%s\n' % status)


if __name__ == '__main__':
    main()
