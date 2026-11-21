"""docs/reference/nvn-api.tsv lists every NVN name the host can hand out, with how the host handles it (dm/tools/nvn_api_map.py writes it from the sources; docs/reference/nvn-api-judgments.tsv
keeps what code cannot say). Fails on a name the table has no row for, a handler no name reaches, a stub with no judgment, a judgment that went stale (its handler is no longer a stub, or the
code it quotes is gone), a recorded op with no replay case, or a table that is not what the sources give. The cases other than the repository's own run on a small fixture repository."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('nvn_api_map', REPO / 'dm/tools/nvn_api_map.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

NAMES = 'nvnImm\nnvnStub\nnvnCommandBufferRec\nnvnNop\nnvnGone\n'
HLE = '''// header
NVN(nvnImm, void, int *x) { *x = 1; }
NVN(nvnStub, void, int *x) { (void)x; }   // nothing to do: a hint only
NVN(nvnCommandBufferRec, void, int *c) { REC(c, OP_A, p_a)->n = 1; }
static const char *const g_nops[] = {
    "nvnNop",
};
'''
OBJ = 'enum { OP_END, OP_A, OP_B, OP_COUNT_ };\n'
MTL = 'void mtl_exec(const uint8_t *p) {\n    switch (op) {\n    case OP_A: break;\n    }\n}\n'
JUDG = ('name\tclass\tanchor\tnote\n'
        'nvnNop\tstub-ok\tnothing to do: a hint only\tempty implementation\n'
        'nvnStub\tstub-ok\tnothing to do: a hint only\ta hint\n')


def fixture(root, names=NAMES, hle=HLE, obj=OBJ, mtl=MTL, judg=JUDG, table=True):
    g = Path(root) / 'dm/gfx/nvn'
    g.mkdir(parents=True)
    (Path(root) / 'docs/reference').mkdir(parents=True)
    for f, t in (('names.txt', names), ('hle_nvn.c', hle), ('nvn_obj.h', obj), ('nvn_mtl.m', mtl)): (g / f).write_text(t, encoding='utf-8')
    (Path(root) / tool.JUDGMENTS).write_text(judg, encoding='utf-8')
    if table:
        rows, _ = tool.analyze(root)
        (Path(root) / tool.TABLE).write_text(tool.render(rows), encoding='utf-8')
        (Path(root) / tool.SVG).write_text(tool.svg(rows), encoding='utf-8')
    return root


def check(root):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = tool.main(['--check', '--repo', str(root)])
    return rc, out.getvalue()


class Repository(unittest.TestCase):
    def test_table_is_current_and_complete(self):
        r = subprocess.run([sys.executable, '-I', str(REPO / 'dm/tools/nvn_api_map.py'), '--check'], cwd=REPO, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_every_name_has_one_row_and_a_known_class(self):
        names = [l.strip() for l in (REPO / 'dm/gfx/nvn/names.txt').read_text(encoding='utf-8').split('\n') if l.strip()]
        rows = [l.split('\t') for l in (REPO / tool.TABLE).read_text(encoding='utf-8').split('\n')[1:] if l]
        self.assertEqual([r[0] for r in rows], names)
        self.assertEqual({r[2] for r in rows} - set(tool.CLASSES), set())

    def test_generation_is_deterministic(self):
        a, _ = tool.analyze(REPO)
        b, _ = tool.analyze(REPO)
        self.assertEqual(tool.render(a), tool.render(b))
        self.assertEqual(tool.svg(a), tool.svg(b))

    def test_picture_is_plain_and_has_one_cell_per_name(self):
        rows, _ = tool.analyze(REPO)
        pic = tool.svg(rows)
        self.assertEqual(pic.count('<title>' + rows[0]['name'] + ': '), 1)
        self.assertEqual(sum(pic.count('<title>%s: ' % r['name']) for r in rows), len(rows))
        for bad in ('<script', 'href=', 'url(', '@import', 'currentColor', 'http://www.w3.org/1999/xlink'):
            self.assertNotIn(bad, pic)


class Fixture(unittest.TestCase):
    def run_case(self, **kw):
        with tempfile.TemporaryDirectory() as d:
            root = fixture(Path(d) / 'repo', **kw)
            return check(root)

    def test_clean_fixture_passes_and_classes_are_read_from_the_code(self):
        with tempfile.TemporaryDirectory() as d:
            root = fixture(Path(d) / 'repo')
            rc, out = check(root)
            self.assertEqual(rc, 0, out)
            rows = {r['name']: r for r in tool.analyze(root)[0]}
            self.assertEqual({n: r['cls'] for n, r in rows.items()},
                             {'nvnImm': 'immediate', 'nvnStub': 'stub-ok', 'nvnCommandBufferRec': 'recorded', 'nvnNop': 'stub-ok', 'nvnGone': 'missing'})
            self.assertEqual((rows['nvnCommandBufferRec']['op'], rows['nvnCommandBufferRec']['replay']), ('OP_A', 'OP_A'))

    def test_name_without_a_row(self):
        with tempfile.TemporaryDirectory() as d:
            root = fixture(Path(d) / 'repo')
            names = Path(root) / 'dm/gfx/nvn/names.txt'
            names.write_text(names.read_text(encoding='utf-8') + 'nvnNew\n', encoding='utf-8')
            rc, out = check(root)
            self.assertEqual(rc, 1)
            self.assertIn('nvnNew', out)

    def test_handler_not_in_the_names(self):
        rc, out = self.run_case(hle=HLE + 'NVN(nvnStray, void, int *x) { *x = 2; }\n')
        self.assertEqual(rc, 1)
        self.assertIn('handler nvnStray is not in names.txt', out)

    def test_stub_without_a_judgment(self):
        rc, out = self.run_case(judg='name\tclass\tanchor\tnote\nnvnNop\tstub-ok\tnothing to do: a hint only\tempty implementation\n')
        self.assertEqual(rc, 1)
        self.assertIn('stub without a judgment: nvnStub', out)

    def test_judgment_of_a_handler_that_is_no_longer_a_stub(self):
        rc, out = self.run_case(hle=HLE.replace('{ (void)x; }', '{ *x = 3; }'))
        self.assertEqual(rc, 1)
        self.assertIn('stale judgment: nvnStub is judged stub-ok', out)

    def test_judgment_whose_code_is_gone(self):
        rc, out = self.run_case(hle=HLE.replace('nothing to do: a hint only', 'removed'))
        self.assertEqual(rc, 1)
        self.assertIn('the anchor of nvnNop is not in the code any more', out)

    def test_stub_ok_without_an_anchor(self):
        rc, out = self.run_case(judg=JUDG.replace('nvnNop\tstub-ok\tnothing to do: a hint only', 'nvnNop\tstub-ok\t'))
        self.assertEqual(rc, 1)
        self.assertIn('nvnNop is judged stub-ok without an anchor', out)

    def test_stub_risky_needs_no_anchor_but_a_note(self):
        rc, out = self.run_case(judg=JUDG.replace('nvnNop\tstub-ok\tnothing to do: a hint only\tempty implementation', 'nvnNop\tstub-risky\t\tnot verified'))
        self.assertEqual(rc, 0, out)
        rc, out = self.run_case(judg=JUDG.replace('nvnNop\tstub-ok\tnothing to do: a hint only\tempty implementation', 'nvnNop\tstub-risky\t\t'))
        self.assertEqual(rc, 1)
        self.assertIn('nvnNop has no note', out)

    def test_judgment_of_a_missing_handler(self):
        rc, out = self.run_case(judg=JUDG + 'nvnGone\tstub-risky\t\tnot verified\n')
        self.assertEqual(rc, 1)
        self.assertIn('stale judgment: nvnGone', out)

    def test_recorded_op_without_a_replay_case(self):
        rc, out = self.run_case(hle=HLE.replace('OP_A, p_a', 'OP_B, p_a'))
        self.assertEqual(rc, 1)
        self.assertIn('nvnCommandBufferRec records OP_B, which has no case in mtl_exec', out)

    def test_stale_committed_table_shows_a_diff(self):
        with tempfile.TemporaryDirectory() as d:
            root = fixture(Path(d) / 'repo')
            hle = Path(root) / 'dm/gfx/nvn/hle_nvn.c'
            hle.write_text(hle.read_text(encoding='utf-8').replace('{ *x = 1; }', '{ *x = 9; }'), encoding='utf-8')
            self.assertEqual(check(root)[0], 0)   # an edit that changes no name, class or op leaves the table alone: no line numbers in it
            hle.write_text(hle.read_text(encoding='utf-8').replace('NVN(nvnCommandBufferRec', 'static int unrelated;\nNVN(nvnCommandBufferRec'), encoding='utf-8')
            self.assertEqual(check(root)[0], 0)
            (Path(root) / tool.TABLE).write_text((Path(root) / tool.TABLE).read_text(encoding='utf-8').replace('\tmissing\t', '\timmediate\t'), encoding='utf-8')
            rc, out = check(root)
            self.assertEqual(rc, 1)
            self.assertIn('-nvnGone', out)
            self.assertIn('+nvnGone', out)

    def test_stale_picture(self):
        with tempfile.TemporaryDirectory() as d:
            root = fixture(Path(d) / 'repo')
            (Path(root) / tool.SVG).write_text('<svg/>\n', encoding='utf-8')
            rc, out = check(root)
            self.assertEqual(rc, 1)
            self.assertIn(tool.SVG + ' is not what the sources give', out)

    def test_gaps_lists_what_a_game_calls_before_what_it_carries(self):
        with tempfile.TemporaryDirectory() as d:
            root = fixture(Path(d) / 'repo')
            log = Path(d) / 'game.log'
            log.write_text('[RED] NVN unimplemented function called: nvnGone (x0=0x0)\n', encoding='utf-8')
            binary = Path(d) / 'main.bin'
            binary.write_bytes(b'\x00nvnStub\x00nvnImm\x00')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tool.main(['--gaps', '--repo', str(root), '--game-log', str(log), '--game-binary', str(binary)])
            lines = out.getvalue().strip().split('\n')[1:]
            self.assertEqual([l.split('\t')[0:3] for l in lines], [['nvnGone', 'missing', 'called']])   # nvnStub is stub-ok, nvnImm immediate: no gap


if __name__ == '__main__':
    unittest.main()
