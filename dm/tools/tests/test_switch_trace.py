#!/usr/bin/env python3
# dm/tools/switch_trace: the console module against its mock NVN driver (every capture mode, NVN and NVN2 keys; needs clang++), and the two joiners on synthetic host logs:
# join.py's host table (GPUPROF times, PASSDUMP keys) and seqdiff.py's token streams (a console sequence and a host pass dump of the same work line up in full)
import contextlib, importlib.util, io, os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
TOOL = REPO / 'dm/tools/switch_trace'


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOL / f'{name}.py'); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


join, seqdiff, dump2png = load('join'), load('seqdiff'), load('dump2png')
P = '[dm] [nvn-mtl] [gpuprof] '


def rp(f, i, us, pas, w, h, nc, draws):
    return f'{P}F{f} {i:4d} {us:8.1f} us render pass {pas} {w}x{h} colors {nc} depth 1 loads 0 draws {draws} start +0.0 seg 0'


def cp(f, i, us, after, disp):
    return f'{P}F{f} {i:4d} {us:8.1f} us compute / copy after pass {after} start +0.0 seg 0 copies 0 (uniform updates 0) dispatches {disp}'


HOST_LOG = [
    rp(100, 0, 500.0, 10, 1920, 1080, 1, 40), cp(100, 1, 80.0, 10, 2), rp(100, 2, 120.0, 11, 960, 540, 1, 1),
    rp(160, 0, 700.0, 20, 1920, 1080, 1, 40), cp(160, 1, 60.0, 20, 2), rp(160, 2, 100.0, 21, 960, 540, 1, 1),
    rp(220, 0, 600.0, 30, 1920, 1080, 1, 40), cp(220, 1, 70.0, 30, 2), rp(220, 2, 110.0, 31, 960, 540, 1, 1),
    # frame 160's pass dump, in encode order
    '[pass] F160 P 20 c0 ser=1 fmt=70 1920x1080 ms1 lv0 sl0 clear mtl=0x1', '[pass] F160 P 20 d ser=2 fmt=252 1920x1080 clear',
    '[pass] F160 K 20 st0 key=aaaaaaaaaaaaaaaa nb=1', '[pass] F160 K 20 st1 key=bbbbbbbbbbbbbbbb nb=1',
    '[pass] F160 I 20 barrier bits 0x28',
    '[pass] F160 X 20 dispatch cccccccccccccccc_b groups 4x4x1 after a barrier', '[pass] F160 X 20 dispatch dddddddddddddddd groups 0x0x0 indirect beside the one before',
    '[pass] F160 Y signal_event cmd=0x1 dst=0x2 mode=0 value=3',
    '[pass] F160 P 21 c0 ser=3 fmt=115 960x540 ms1 lv0 sl0 load mtl=0x2',
    '[pass] F160 K 21 st1 key=eeeeeeeeeeeeeeee nb=1', '[pass] F160 E 21 conditional render passed addr=0x10 values 1 / 0', '[pass] F160 E 21 render enable 1',
]
SEQ = '''# switch_trace sequence log, capture 1; frames 7-7
frame\tidx\tqueue\tcb\tn\top\targs\tkeys
7\t0\t0\t0\t\tsubmit\t\t
7\t1\t0\t0\t0\tbegin\t\t
7\t2\t0\t0\t1\ttargets\t1440x810 colors 1 cfmt 37 dfmt 52\t
7\t3\t0\t0\t2\tdraw\t40\taaaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbb
7\t4\t0\t0\t3\tbarrier\tbits 0x28\t
7\t5\t0\t0\t4\tdispatch\t4x4x1\tcccccccccccccccc
7\t6\t0\t0\t5\twait_event\tevent 0 mode 1 value 2\t
7\t7\t0\t0\t6\tdispatch\tindirect\tdddddddddddddddd
7\t8\t0\t0\t7\tsignal_event\tevent 1 mode 0 location 1 flags 0 value 3\t
7\t9\t0\t0\t8\ttargets\t720x405 colors 1 cfmt 41 dfmt 0\t
7\t10\t0\t0\t9\ttargets\t720x405 colors 1 cfmt 41 dfmt 0\t
7\t11\t0\t0\t10\trender_cond\tmode 1 addr 0x10\t
7\t12\t0\t0\t11\tdraw\t1\teeeeeeeeeeeeeeee
7\t13\t0\t0\t12\trender_enable\t1\t
7\t14\t0\t0\t13\tend\t\t
'''


class SwitchTrace(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_mock_console(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run(['sh', str(TOOL / 'test/run.sh')], capture_output=True, text=True, env={**os.environ, 'TMPDIR': d})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            for case in ('timing (run)', 'seq:', 'timing (each)', 'dump-key', 'dump-all'):
                self.assertIn(case, r.stdout)
            self.assertNotIn('mismatch', r.stdout)

    def test_host_table(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / 'game.log'; log.write_text('\n'.join(HOST_LOG) + '\n')
            rows = join.host_table(str(log), 160)
            self.assertEqual([(r['kind'], r['us'], r['keys']) for r in rows],
                             [('R', 600.0, 'aaaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbb'), ('C', 70.0, 'cccccccccccccccc,dddddddddddddddd'), ('R', 110.0, 'eeeeeeeeeeeeeeee')])
            con = Path(d) / 'capture1.tsv'
            con.write_text('frame\tidx\tus\tkind\tqueue\tw\th\tcolors\tkeys\n7\t0\t300\tR\t0\t1440\t810\t1\taaaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbb\n'
                           '7\t1\t100\tC\t0\t0\t0\t0\tcccccccccccccccc,dddddddddddddddd\n7\t2\t100\tR\t0\t720\t405\t1\teeeeeeeeeeeeeeee\n')
            out = io.StringIO()
            with contextlib.redirect_stdout(out): join.main([str(con), '--log', str(log), '--frame', '160'])
            self.assertIn('console: 1 frames, median interval sum 0.50 ms; host: 0.78 ms', out.getvalue())
            self.assertIn('| eeeeeeeeeeeeeeee | 0.100 | 0.110 | 20.0 | 14.1 | 1.10 | 0.71 |', out.getvalue())

    def test_seqdiff_same_order(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / 'game.log'; log.write_text('\n'.join(HOST_LOG) + '\n')
            seq = Path(d) / 'seq1.tsv'; seq.write_text(SEQ)
            f, ctoks, waits = seqdiff.console_tokens(str(seq))
            self.assertEqual((f, waits), (7, 1))
            htoks = seqdiff.host_tokens(str(log), 160)
            want = ['T full c1 d', 'B 0x28', 'C cccccccccccccccc', 'C dddddddddddddddd', 'S', 'T half c1', 'RC', 'RE 1']
            self.assertEqual([t[0] for t in ctoks if t[0] != 'W'], want)   # the first 720x405 targets are set twice and drawn once
            self.assertEqual([t[0] for t in htoks], want)
            out = io.StringIO()
            with contextlib.redirect_stdout(out): seqdiff.main([str(seq), '--log', str(log), '--frame', '160'])
            self.assertIn('8 tokens (1 waits left out); host frame 160: 8 tokens; in the same order: 8', out.getvalue())


    def test_dump2png(self):
        import struct
        self.assertEqual(dump2png.decode(0x25, 2, 1, bytes([1, 2, 3, 4, 5, 6, 7, 8])), bytes([1, 2, 3, 4, 5, 6, 7, 8]))                 # RGBA8
        self.assertEqual(dump2png.decode(0x9A, 1, 1, bytes([1, 2, 3, 4])), bytes([3, 2, 1, 4]))                                       # BGRA8
        self.assertEqual(dump2png.decode(0x29, 1, 1, struct.pack('<4e', 1.0, 0.5, 0.0, 2.0)), bytes([255, 128, 0, 255]))             # RGBA16F, clamped
        u = (15 << 6) | (30 << 17) | (14 << 27)   # R11G11B10F, mantissas zero: r = 2^(15-15) = 1.0, g = 2^(30-15) (clamped), b = 2^(14-15) = 0.5
        self.assertEqual(dump2png.decode(0x3F, 1, 1, struct.pack('<I', u))[:4], bytes([255, 255, 128, 255]))
        self.assertEqual(dump2png.decode(0x34, 2, 1, struct.pack('<2f', 0.25, 0.75)), bytes([0, 0, 0, 255, 255, 255, 255, 255]))   # depth, stretched
        self.assertIsNone(dump2png.decode(0x79, 1, 1, b'\0' * 16))                                                                 # ASTC: left as .bin
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'dump1.tsv').write_text('n\tframe\tkind\tw\th\tnvn_format\tbpp\tbytes\tbuffer\tfile\n0\t9\tcolor\t2\t1\t37\t4\t8\t0\tdump1_0.bin\n')
            (Path(d) / 'dump1_0.bin').write_bytes(bytes(range(8)))
            with contextlib.redirect_stdout(io.StringIO()): dump2png.main([d])
            self.assertTrue((Path(d) / 'dump1_0_f9_color_2x1.png').read_bytes().startswith(b'\x89PNG'))


if __name__ == '__main__':
    unittest.main()
