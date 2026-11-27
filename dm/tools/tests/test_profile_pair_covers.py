# Every profile's pair tables are laid out as shader_split.py writes them now: synthetic padding covers under a "# cover:" section, after the real pairs (faf91a9df). A profile whose tables
# come from an earlier splitter has its cover rows among the real ones; the NSP script compares the profile's tables with its own split and stops with "the same rows in another order"
# (issue #3422, Pentiment), and pso_offline.py and pso_prebuild would build the covers as pipeline bases. These checks read the profiles only, so they run without game files:
#   shader_pairs.tsv: separable_pairs pairs every single-stage vertex shader (a source tagged in shader_split.SINGLE_STAGE, not a '#var' GLSLC program) with every pixel shader of
#     one cover, so the cover rows are exactly (single-stage vertex) x (cover pixel), less any that are real pairs, and the cover pixels are those every single-stage vertex shader pairs with.
#   pso_pairs.tsv: its real rows are BAHS program variations; all others (legacy Unity pass-input covers) sit under "# cover:", so a profile with no BAHS shader has no row outside it.
#   Both: each section is sorted, as the splitter writes it (the NSP script's check sees the order).
# A table with no cover section whose single-stage vertex shaders all share a real pixel partner would be flagged, as would one whose covers are all real pairs too: no profile has such a table.
from pathlib import Path
import ast
import unittest

REPO = Path(__file__).resolve().parents[3]
SPLIT = REPO / 'dm/gfx/tools/shader_split.py'
SINGLE_STAGE = next(ast.literal_eval(n.value) for n in ast.parse(SPLIT.read_text()).body
                    if isinstance(n, ast.Assign) and getattr(n.targets[0], 'id', None) == 'SINGLE_STAGE')
AGAIN = 'its tables come from a shader_split.py before the # cover: sections (faf91a9df): split them again'


def shader_map(text):
    """{key: (stage, source)}: the first row of each key, whose source is where the split first found it (the label separable_pairs reads)"""
    out, head = {}, None
    for line in text.splitlines():
        if line.startswith('#') or not line.strip(): continue
        f = line.split('\t')
        if head is None: head = f; continue
        r = dict(zip(head, f))
        out.setdefault(r['dm_ctl_sha16'], (r['stage'], r.get('source', '')))
    return out


def sections(text):
    """(real rows, cover rows) as pso_offline.read_pairs reads them: a "# cover" line starts a cover section, a "# kind:" line ends it"""
    real, cover = [], []
    into = real
    for line in text.splitlines():
        if line.startswith('# cover') and (len(line) == 7 or line[7] in ':\t '): into = cover; continue
        if line.startswith('# kind:'): into = real; continue
        if line.startswith('#') or not line.strip(): continue
        into.append(tuple(line.split('\t')[:2]))
    return real, cover


def single_stage(keys):
    return {k for k, (stage, src) in keys.items() if stage == 'vertex' and any(s in src for s in SINGLE_STAGE) and '#var' not in src}


def unsorted(real, cover):
    for name, rows in (('real pairs', real), ('cover section', cover)):
        if rows != sorted(rows): return f'the {name} are not sorted as the splitter writes them'
    return None


def shader_pairs_problem(keys, text):
    """why a shader_pairs.tsv is not laid out as the splitter writes it, or None"""
    real, cover = sections(text)
    if why := unsorted(real, cover): return why
    single = single_stage(keys)
    if cover:
        if bad := [v for v, _ in cover if v not in single]: return f'{len(bad)} cover row(s) of a vertex shader that is not single-stage, first {bad[0]}'
        pix = {p for _, p in cover}
        want = {(v, p) for v in single for p in pix} - set(real)
        if want != set(cover): return f'the cover section is not every single-stage vertex shader x every cover pixel shader ({len(want - set(cover))} missing, {len(set(cover) - want)} extra)'
        return None
    by = {}
    for v, p in real: by.setdefault(v, set()).add(p)
    if single and (common := set.intersection(*(by.get(v, set()) for v in single))):
        n = sum(1 for v, p in real if v in single and p in common)
        return f'{n} single-stage padding row(s) (pixel shader(s) {" ".join(sorted(common))}) are among the real pairs, with no # cover: section; {AGAIN}'
    return None


def pso_pairs_problem(keys, text):
    real, cover = sections(text)
    if why := unsorted(real, cover): return why
    if real and not any('BAHS' in src for _, src in keys.values()):
        return f'{len(real)} row(s) outside a # cover: section, and no shader comes from a BAHS archive (the only real pairs this table has); {AGAIN}'
    return None


H = '# kind: evidence  pairs\n'
MAP = ('package\tindex\tstage\tdm_ctl_sha16\tsource\n'
       'a.bin\t0\tvertex\tv1\t/shaders#GLSLC@0x10\n'
       'a.bin\t1\tvertex\tv2\t/shaders#GLSLC@0x90\n'
       'a.bin\t2\tvertex\tv3\t/fx.bnsh#BNSH@0x0#var0\n'
       'a.bin\t3\tpixel\tp1\t/shaders#GLSLC@0x20\n'
       'a.bin\t4\tpixel\tp2\t/fx.bnsh#BNSH@0x0#var0\n')
COVER = '# cover: single-stage padding pairs\n'


class Synthetic(unittest.TestCase):
    keys = shader_map(MAP)

    def test_current_layout_passes(self):
        self.assertIsNone(shader_pairs_problem(self.keys, H + 'v3\tp2\n' + COVER + 'v1\tp1\nv2\tp1\n'))
        self.assertIsNone(shader_pairs_problem(self.keys, H + 'v1\tp2\nv3\tp2\n' + COVER + 'v1\tp1\nv2\tp1\n'))   # a real pair of a single-stage shader stays above
        self.assertIsNone(shader_pairs_problem(shader_map(MAP.replace('#GLSLC@0x10', '#BNSH@0x10#var1').replace('#GLSLC@0x90', '#BNSH@0x90#var1')), H + 'v3\tp2\n'))

    def test_covers_among_the_real_pairs_fail(self):
        why = shader_pairs_problem(self.keys, H + 'v1\tp1\nv2\tp1\nv3\tp2\n')   # the layout before faf91a9df: one sorted list
        self.assertIn('2 single-stage padding row(s) (pixel shader(s) p1)', why)
        self.assertIn('split them again', why)

    def test_unsorted_sections_fail(self):
        self.assertIn('cover section are not sorted', shader_pairs_problem(self.keys, H + 'v3\tp2\n' + COVER + 'v2\tp1\nv1\tp1\n'))
        self.assertIn('real pairs are not sorted', shader_pairs_problem(self.keys, H + 'v3\tp2\nv1\tp2\n' + COVER + 'v1\tp1\nv2\tp1\n'))
        self.assertIn('cover section are not sorted', pso_pairs_problem(self.keys, H + '# cover: legacy\nv3\tp2\nv1\tp2\n'))

    def test_a_cover_section_that_is_not_the_whole_cover_fails(self):
        self.assertIn('1 missing', shader_pairs_problem(self.keys, H + 'v3\tp2\n' + COVER + 'v1\tp1\n'))
        self.assertIn('not single-stage', shader_pairs_problem(self.keys, H + COVER + 'v1\tp1\nv2\tp1\nv3\tp1\n'))

    def test_pso_pairs(self):
        self.assertIsNone(pso_pairs_problem(self.keys, H + '# cover: legacy Unity pass-input padding pairs\nv3\tp2\n'))
        self.assertIsNone(pso_pairs_problem(self.keys, H))
        self.assertIn('1 row(s) outside', pso_pairs_problem(self.keys, H + 'v3\tp2\n'))
        bahs = shader_map(MAP + 'a.bin\t5\tvertex\tv4\t/x.bfsha#BAHS@0x40\n')
        self.assertIsNone(pso_pairs_problem(bahs, H + 'v4\tp2\n'))


class Repository(unittest.TestCase):
    def test_profiles_pair_tables_have_their_covers_apart(self):
        problems = []
        for d in sorted((REPO / 'dm/games').iterdir()):
            if not (d / 'shader_map.tsv').is_file(): continue
            keys = shader_map((d / 'shader_map.tsv').read_text())
            for name, check in (('shader_pairs.tsv', shader_pairs_problem), ('pso_pairs.tsv', pso_pairs_problem)):
                if (d / name).is_file() and (why := check(keys, (d / name).read_text())): problems.append(f'{d.name}/{name}: {why}')
        self.assertEqual(problems, [])


if __name__ == '__main__': unittest.main()
