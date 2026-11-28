"""shader_split.yaz0 against the item-by-item decoder it replaced, kept here as the reference: the same bytes and the same red() text on generated streams
(a small random encoder, overlapping copies at every short distance, both length forms up to 0x111, n = 0, n ending inside a group or a copy, truncated streams,
back-references before the start), each as bytes, bytearray and memoryview. No game files needed."""
import random
import sys
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz
ss = fz.ss


def reference(s, label='Yaz0'):
    """shader_split.yaz0 up to upstream 002724b8: one item at a time"""
    n = int.from_bytes(s[4:8], 'big'); out = bytearray(); si = 16
    try:
        while len(out) < n:
            h = s[si]; si += 1
            for _ in range(8):
                if h & 0x80:
                    out.append(s[si]); si += 1
                else:
                    b1, b2 = s[si], s[si + 1]; si += 2
                    dist, ln = ((b1 & 15) << 8 | b2) + 1, b1 >> 4
                    if ln: ln += 2
                    else: ln = s[si] + 0x12; si += 1
                    st = len(out) - dist
                    if st < 0: ss.red(f'{label}: a back-reference at {si:#x} reaches {-st:#x} bytes before the start')
                    out += out[st:st + ln] if dist >= ln else (out[st:] * (ln // dist + 1))[:ln]
                if len(out) >= n: break
                h <<= 1
    except IndexError: ss.red(f'{label}: the Yaz0 stream ends at {len(s):#x} with {len(out):#x} of its {n:#x} bytes decoded')
    return bytes(out[:n])


def encode(items, n=None):
    """A Yaz0 stream of items: an int is a literal byte, (distance, length) a back-reference (lengths 3..0x11 in 2 bytes, 0x12..0x111 in 3). n: the header's size,
    by default what the items decode to"""
    body, size = bytearray(), 0
    for g in range(0, len(items), 8):
        head = len(body); body.append(0)
        for bit, it in enumerate(items[g:g + 8]):
            if isinstance(it, int): body[head] |= 0x80 >> bit; body.append(it); size += 1; continue
            dist, ln = it; size += ln
            if ln <= 0x11: body += bytes(((ln - 2) << 4 | (dist - 1) >> 8, (dist - 1) & 0xff))
            else: body += bytes(((dist - 1) >> 8, (dist - 1) & 0xff, ln - 0x12))
    return b'Yaz0' + (size if n is None else n).to_bytes(4, 'big') + bytes(8) + body


def random_items(rnd, size, p_lit, near):
    """Items that decode to about size bytes: literals with probability p_lit, else a back-reference within what is out (distance 1..near more often than not)"""
    items, o = [], 0
    while o < size:
        if o < 3 or rnd.random() < p_lit: items.append(rnd.randrange(256)); o += 1; continue
        top = min(o, 0x1000)
        dist = rnd.randint(1, min(near, top)) if rnd.random() < 0.5 else rnd.randint(1, top)
        ln = rnd.choice((rnd.randint(3, 0x11), rnd.randint(3, 0x11), rnd.randint(0x12, 0x111), 0x111))
        items.append((dist, ln)); o += ln
    return items


def run(fn, s):
    try: return 'ok', fn(s)
    except SystemExit as e: return 'red', e.code


def differ(got, want):
    """'' when they agree, else a short account (assertEqual would diff whole outputs)"""
    if got == want: return ''
    if got[0] != want[0] or got[0] == 'red': return f'{got[0]} {got[1] if got[0] == "red" else len(got[1])!r}, the reference {want[0]} {want[1] if want[0] == "red" else len(want[1])!r}'
    at = next((i for i, (a, b) in enumerate(zip(got[1], want[1])) if a != b), min(len(got[1]), len(want[1])))
    return f'{len(got[1]):#x} bytes, the reference {len(want[1]):#x}, first difference at {at:#x}'


class Yaz0(unittest.TestCase):
    streams = 0

    def same(self, s, expect=None):
        """yaz0 and the reference agree on s as bytes, bytearray and memoryview; expect: 'ok' or 'red', what the reference must give"""
        want = run(reference, bytes(s)); Yaz0.streams += 1
        if expect: self.assertEqual(want[0], expect, want[1] if want[0] == 'red' else None)
        for kind in (bytes, bytearray, memoryview):
            if d := differ(run(ss.yaz0, kind(s)), want): self.fail(f'{kind.__name__}, {len(s):#x} bytes: {d}')
        return want

    def test_random_streams(self):
        rnd = random.Random(640)
        for i in range(240):
            size = rnd.choice((rnd.randint(1, 0x1000), rnd.randint(0x1000, 0x3000), rnd.randint(0x3000, 0x20000)))
            p_lit = rnd.choice((0.05, 0.3, 0.6, 0.8, 0.95, 1.0))
            with self.subTest(i=i, size=size, p_lit=p_lit):
                self.same(encode(random_items(rnd, size, p_lit, rnd.choice((1, 4, 16, 300)))), 'ok')

    def test_every_short_distance_and_length(self):
        """Every length 3..0x111 at distances 1..32 and 0xffe..0x1000, from the first bytes (item by item) and past 0x1000 (in batches)"""
        rnd = random.Random(2)
        for dist in (*range(1, 33), 0xffe, 0xfff, 0x1000):
            for lead in (dist, 0x1200):
                refs = [(dist, ln) for ln in range(3, 0x112)]; rnd.shuffle(refs)
                items = [rnd.randrange(256) for _ in range(lead)] + [x for r in refs for x in (r, rnd.randrange(256))] + [rnd.randrange(256) for _ in range(0x900)]
                with self.subTest(dist=dist, lead=lead): self.same(encode(items), 'ok')

    def test_empty_and_short(self):
        for s in (b'Yaz0', b'Yaz0' + bytes(4), b'Yaz0' + bytes(12), encode([]), encode([1, 2, 3], n=0), b'Yaz0\0\0\0\1', b'Yaz0\0\0\0\1' + bytes(8)):
            with self.subTest(s=s): self.same(s)
        self.assertEqual(ss.yaz0(encode([1, 2, 3], n=0)), b'')

    def test_n_ends_inside_a_group_or_a_copy(self):
        """Every n in the last 0xa00 bytes (the last batches and the item-by-item end), and a spread before them; n past the end is a stream that ends early"""
        rnd = random.Random(3)
        for p_lit in (0.2, 0.9):
            items = random_items(rnd, 0x3800, p_lit, 8); full = reference(encode(items)); total = len(full)
            for n in sorted({*range(total - 0xa00, total + 1), *rnd.sample(range(total), 300)}):
                with self.subTest(p_lit=p_lit, n=n): self.assertFalse(differ(self.same(encode(items, n), 'ok'), ('ok', full[:n])))
            for n in (total + 1, total + 0x111, total + 0x10000):
                with self.subTest(p_lit=p_lit, n=n): self.same(encode(items, n), 'red')

    def test_truncated(self):
        """The stream cut at every length, the 3-byte length form and the long copies included (p_lit 0: groups of back-references only, the most a group reads)"""
        rnd = random.Random(4)
        for p_lit in (0.0, 0.3, 0.9):
            s = encode(random_items(rnd, 0x2000, p_lit, 8))
            for cut in range(len(s)):
                with self.subTest(p_lit=p_lit, cut=cut): self.same(s[:cut], 'red' if 16 <= cut < len(s) else None)

    def test_before_the_start(self):
        rnd = random.Random(5); lit = lambda k: [rnd.randrange(256) for _ in range(k)]
        cases = {
            'first item': [(1, 3)],
            'first item, 3-byte form': [(1, 0x111)],
            'second group': lit(8) + [(9, 3)],
            'after 0xfff bytes': lit(0xfff) + [(0x1000, 3)] + lit(0x2000),
            'after 0xfff bytes, 3-byte form': lit(0xfff) + [(0x1000, 0x80)] + lit(0x2000),
            'after copies, just under 0x1000': lit(0x10) + [(0x10, 0x111)] * 14 + [(0x1000, 4)] + lit(0x2000),
        }
        for name, items in cases.items():
            with self.subTest(name):
                self.assertIn('bytes before the start', str(self.same(encode(items), 'red')[1]))
        self.assertIn('stream ends', str(self.same(encode([(1, 0x111)])[:-1], 'red')[1]))   # its length byte is missing: the stream ends first

    def test_batches_run(self):
        """The streams above reach the batches: only past 0x1000 bytes and 0x888 before n, which a short stream never does"""
        class Counted(tuple):
            def __getitem__(self, k): Counted.calls += 1; return tuple.__getitem__(self, k)
        rnd = random.Random(6)
        for size, want in ((0x1887, False), (0x2000, True)):
            Counted.calls = 0
            with mock.patch.object(ss, 'YAZ0_RUNS', Counted(ss.YAZ0_RUNS)): self.same(encode([rnd.randrange(256) for _ in range(size)]), 'ok')
            with self.subTest(size=size): self.assertEqual(Counted.calls > 0, want)

    @classmethod
    def tearDownClass(cls): print(f'\n{cls.streams} streams compared', file=sys.stderr)


if __name__ == '__main__':
    unittest.main()
