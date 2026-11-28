#!/usr/bin/env python3
"""The NVN / NVN2 entry points as the host handles them, as one table: docs/reference/nvn-api.tsv (one row per name in dm/gfx/nvn/names.txt).
  nvn_api_map.py            write the table and the picture of it (docs/reference/nvn-api.svg; --svg <file> writes the picture elsewhere) and print the totals per class
  nvn_api_map.py --check    exit 1, with the problems and a diff, when the table or the picture is not what the sources give or the judgments table has a gap or a stale row
  nvn_api_map.py --gaps ... print, for one game, the names it calls or carries that the host does not fully handle (nothing is written; the game's inputs are not kept)
docs/reference/nvn-api.md says what the columns and classes mean.

Read from the repository only (names, handlers, recording helpers, ops, replay cases): no game, no GPU.
  dm/gfx/nvn/names.txt      every name the host can hand out from nvnBootstrapLoader / nvnDeviceGetProcAddress
  dm/gfx/nvn/hle_nvn.c      the NVN(...) / NVN_NOPROTO(...) handlers and the g_nops[] empty implementations (dm/gfx/nvn/nvn_queue.inc: helpers)
  dm/gfx/nvn/nvn_obj.h      the OP_* enum and the OPC_* compact forms
  dm/gfx/nvn/nvn_mtl.m      the replay: the case OP_* labels of mtl_exec
The class comes from the code where the code can say it: no handler is `missing`; a handler that records an OP_* is `recorded`; an empty, constant-return or log-only handler
(or an entry of g_nops[]) is a stub; every other handler is `immediate`. What code cannot say comes from docs/reference/nvn-api-judgments.tsv (name, class, anchor, note):
  stub-ok / stub-risky   whether the ignored call loses anything: stub-ok needs an anchor, a phrase of the code that says why it is safe
  partial                a handler or its replay that works for some inputs: the note says what it does not do, the anchor is the code's own text for the gap
An anchor is a literal phrase that must occur in dm/gfx/nvn (or docs/reference), so a judgment whose code was rewritten or removed fails --check instead of going stale.
The table holds names, not line numbers, so an edit that moves a handler does not change it.

--gaps takes the game's side as optional inputs, never committed: --game-log <file or folder> (a run log: the `[RED] NVN unimplemented function called: <name>` lines),
--game-names <file> (any text in which the names the game called appear, e.g. the call lines of a DM_NVN_COUNT run), --game-binary <file> (a guest module: the nvn* strings in it are the names it carries; carried is not called)."""
import argparse
import difflib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NVN = 'dm/gfx/nvn'
TABLE = 'docs/reference/nvn-api.tsv'
SVG = 'docs/reference/nvn-api.svg'
JUDGMENTS = 'docs/reference/nvn-api-judgments.tsv'
HEAD = ['name', 'category', 'class', 'op', 'handler', 'replay', 'notes']
JHEAD = ['name', 'class', 'anchor', 'note']
CLASSES = ('immediate', 'recorded', 'partial', 'stub-ok', 'stub-risky', 'missing')
JUDGED = ('stub-ok', 'stub-risky', 'partial')
# recorded ops with no `case` in mtl_exec: nvn_queue.inc turns them into scheduler nodes (NQ_EVENT, NQ_SYNC) when it flattens a submission, so the replay never meets them
NO_CASE_OK = {'OP_WAIT_EVENT', 'OP_WAIT_SYNC'}
GROUPS = [  # name prefix -> category; the first match wins
    ('nvnDevice', 'device'), ('nvnQueue', 'queue'), ('nvnCommandBuffer', 'command buffer'), ('nvnProgram', 'program/shader'), ('nvnMemoryPool', 'buffers/memory pools'),
    ('nvnBuffer', 'buffers/memory pools'), ('nvnTexturePool', 'textures/samplers/pools'), ('nvnSamplerPool', 'textures/samplers/pools'), ('nvnTexture', 'textures/samplers/pools'),
    ('nvnSampler', 'textures/samplers/pools'), ('nvnSync', 'sync/fences'), ('nvnEvent', 'sync/fences'), ('nvnWindow', 'window/present'), ('nvnBlend', 'pipeline state blocks'),
    ('nvnColor', 'pipeline state blocks'), ('nvnChannelMask', 'pipeline state blocks'), ('nvnMultisample', 'pipeline state blocks'), ('nvnPolygon', 'pipeline state blocks'),
    ('nvnDepthStencil', 'pipeline state blocks'), ('nvnVertex', 'pipeline state blocks'), ('nvnDebug', 'debug'), ('nvnDlss', 'NVN2: DLSS, ray tracing, cooperative vectors'),
    ('nvnDLSS', 'NVN2: DLSS, ray tracing, cooperative vectors'),
]
CB_SUB = [  # nvnCommandBuffer* by what the call does (the part of the name after the prefix); the first match wins
    (r'Draw|Multi', 'command buffer: draw'), (r'Dispatch|ComputeDirtyTileMap', 'command buffer: compute'),
    (r'Bind|Push(Graphics|Compute)Constants|SetProgramSubroutines', 'command buffer: bindings'),
    (r'Fence|Wait|Signal|Report|Reset|Counter|Barrier|Event|ComputeEnable|CopyEnable', 'command buffer: sync and queries'),
    (r'Memory|Callback|Debug|Label|Checkpoint|Recording|Begin|End|Call|Initialize|Finalize|Control|Insert|Push|Pop|SubmitDeviceCommands', 'command buffer: lifecycle, memory, debug'),
    (r'Set|Enable|Override|Discard|ZCull', 'command buffer: fixed-function state'), (r'Clear|Copy|Downsample|Tiled|Update|Fill|Resolve', 'command buffer: clears and copies'),
]


def blank(src):
    """The source with comments and the insides of string and character literals replaced by spaces (newlines kept)."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            j = src.find('\n', i); j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', src[i:j])); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            j += 1
            out.append(c + re.sub(r'[^\n]', ' ', src[i + 1:j - 1]) + c); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def blank_pp(text):
    """The text with preprocessor lines (and their continuations) blanked."""
    out, cont = [], False
    for ln in text.split('\n'):
        if cont or ln.lstrip().startswith('#'):
            cont = ln.rstrip().endswith('\\'); out.append(' ' * len(ln))
        else:
            out.append(ln)
    return '\n'.join(out)


def match(text, i, o, c):
    d = 0
    while i < len(text):
        if text[i] == o: d += 1
        elif text[i] == c:
            d -= 1
            if d == 0: return i
        i += 1
    return -1


def functions(raw):
    """({name: body} for the top-level functions, [(name, body without comments, raw body, has no prototype)] for the NVN()/NVN_NOPROTO() handlers) of one C file."""
    t = blank_pp(blank(raw))
    funcs, handlers = {}, []
    for m in re.finditer(r'^(NVN|NVN_NOPROTO)\(\s*(\w+)\s*,', t, re.M):
        pc = match(t, t.index('(', m.start()), '(', ')')
        bo = t.index('{', pc); bc = match(t, bo, '{', '}')
        handlers.append((m.group(2), t[bo:bc + 1], raw[bo:bc + 1], m.group(1) == 'NVN_NOPROTO'))
    depth, i, start = 0, 0, 0
    while i < len(t):
        ch = t[i]
        if ch == '{' and depth == 0:
            hdr = t[start:i]; bc = match(t, i, '{', '}')
            if bc < 0: break
            m = re.search(r'(\w+)\s*\(', hdr)
            if m and not re.match(r'\s*(typedef|struct|union|enum)\b', hdr) and '=' not in hdr.split('(')[0] and m.group(1) not in ('NVN', 'NVN_NOPROTO', '__attribute__'):
                funcs.setdefault(m.group(1), t[i:bc + 1])
            i = bc + 1; start = i; continue
        if ch == ';' and depth == 0: start = i + 1
        i += 1
    return funcs, handlers


def log_only(raw_body):
    """True for a body that only logs (LOG / ONCE_LOG, behind ifs and a once flag)."""
    b = re.sub(r'\(void\)\s*\w+\s*;', '', raw_body[1:-1])
    if not re.search(r'\b(ONCE_LOG|LOG)\(', b): return False
    b = re.sub(r'\b(?:ONCE_LOG|LOG)\((?:"(?:[^"\\]|\\.)*"|[^;"])*\)\s*;', '', b)
    for _ in range(4): b = re.sub(r'\bif\s*\((?:[^()]|\([^()]*\))*\)', '', b)
    return re.sub(r'static int once;|once\+\+|[{}!;\s]|\bonce\b', '', b) == ''


def category(n):
    cat = 'other'
    for p, c in GROUPS:
        if n.startswith(p): cat = c; break
    if n.startswith('nvnCommandBuffer'):
        rest = n[len('nvnCommandBuffer'):]
        cat = next((c for pat, c in CB_SUB if re.match(pat, rest)), 'command buffer: other')
    if re.search(r'NVX$|Interception|Private|Devtools|CUDA|GLInterop|ExternalDebugger|NvRm', n): cat = 'debug and vendor (NVX etc.)'
    if re.search(r'Dlss|DLSS|Cooperative|RayTracing|AccelerationStructure|DispatchRays|DeviceMessageBuffer', n): cat = 'NVN2: DLSS, ray tracing, cooperative vectors'
    return cat


def read_judgments(repo):
    """({name: (class, anchor, note)}, problems) from docs/reference/nvn-api-judgments.tsv."""
    path = Path(repo) / JUDGMENTS
    out, bad = {}, []
    if not path.is_file(): return out, ['%s is missing' % JUDGMENTS]
    lines = path.read_text(encoding='utf-8').split('\n')
    if lines[0].split('\t') != JHEAD: bad.append('%s: the header must be %s' % (JUDGMENTS, '\t'.join(JHEAD)))
    for k, l in enumerate(lines[1:], 2):
        if not l.strip(): continue
        f = l.split('\t')
        if len(f) != 4: bad.append('%s:%d: %d fields, want 4' % (JUDGMENTS, k, len(f))); continue
        n, cls, anchor, note = f
        if n in out: bad.append('%s:%d: %s is judged twice' % (JUDGMENTS, k, n)); continue
        if cls not in JUDGED: bad.append('%s:%d: %s has class %r, want one of %s' % (JUDGMENTS, k, n, cls, ', '.join(JUDGED))); continue
        if not note.strip(): bad.append('%s:%d: %s has no note' % (JUDGMENTS, k, n)); continue
        out[n] = (cls, anchor, note)
    return out, bad


def analyze(repo):
    """(rows, problems): one row per name of names.txt, in its order, and what is wrong with the sources or the judgments."""
    repo = Path(repo); g = repo / NVN
    problems = []
    names = [l.strip() for l in (g / 'names.txt').read_text(encoding='utf-8').split('\n') if l.strip()]
    nameset = set(names)
    hle_raw = (g / 'hle_nvn.c').read_text(encoding='utf-8', errors='replace')
    funcs, handlers = functions(hle_raw)
    qf, _ = functions((g / 'nvn_queue.inc').read_text(encoding='utf-8', errors='replace')) if (g / 'nvn_queue.inc').is_file() else ({}, [])
    for k, v in qf.items(): funcs.setdefault(k, v)
    hdl = {n: (body, raw, noproto) for n, body, raw, noproto in handlers}
    m = re.search(r'g_nops\[\]\s*=\s*\{(.*?)\};', hle_raw, re.S)
    nops = set(re.findall(r'"(nvn\w+)"', m.group(1))) if m else set()
    # the OP_* numbers are the enum's order; OPC_* compact forms name the op they stand for in a comment
    obj = (g / 'nvn_obj.h').read_text(encoding='utf-8', errors='replace')
    en = re.search(r'enum \{\s*OP_END,(.*?)OP_COUNT_?', obj, re.S)
    ops = ['OP_END'] + re.findall(r'\bOP_[A-Z0-9_]+', blank(en.group(1)))
    opset = set(ops)
    opc = {mm.group(1): mm.group(2) for mm in re.finditer(r'^\s*(OPC_\w+)[,=\s][^\n]*?(?:→|->)\s*(OP_\w+)', obj, re.M)}
    # the replay: the case labels inside mtl_exec
    mtl = (g / 'nvn_mtl.m').read_text(encoding='utf-8', errors='replace').split('\n')
    s = next(i for i, l in enumerate(mtl) if l.startswith('void mtl_exec(const uint8_t *'))
    e = next(i for i in range(s + 1, len(mtl)) if mtl[i].startswith('}'))
    replay = set(re.findall(r'\bcase (OP_\w+):', '\n'.join(mtl[s:e])))
    # an NVN handler is also a function its siblings may call (h_<name>)
    for n_, (body_, _raw, _np) in hdl.items(): funcs.setdefault('h_' + n_, body_)
    tok = lambda b: set(re.findall(r'\b(OP_[A-Z0-9_]+|OPC_[A-Z0-9_]+)\b', b))

    def reach(body, seen, depth=0):
        r = tok(body)
        if depth < 4:
            for f in set(re.findall(r'\b(\w+)\s*\(', body)):
                if f in funcs and f not in seen:
                    seen.add(f); r |= reach(funcs[f], seen, depth + 1)
        return r
    # the OP_ tokens the handler names itself; only when it names none, those of the helpers it calls (a generic helper names every op it can be given)
    ops_of = lambda body: tok(body) or reach(body, set())
    for n in hdl:
        if n not in nameset: problems.append('handler %s is not in names.txt: nothing can reach it' % n)
    for n in sorted(nops):
        if n not in nameset: problems.append('g_nops lists %s, which is not in names.txt' % n)
        if n in hdl: problems.append('%s has a handler and is also in g_nops' % n)
    judged, jbad = read_judgments(repo)
    problems += jbad
    corpus = ''.join(p.read_text(encoding='utf-8', errors='replace') for p in sorted(g.iterdir()) if p.suffix in ('.c', '.h', '.m', '.inc')) \
        + ''.join(p.read_text(encoding='utf-8', errors='replace') for p in sorted((repo / 'docs/reference').glob('*.md')))
    rows = []
    for n in names:
        row = dict(name=n, category=category(n), cls='', op='', handler='', replay='', notes=[])
        stub = False
        if n in hdl:
            body, raw, noproto = hdl[n]
            row['handler'] = 'h_' + n
            if noproto: row['notes'].append('no prototype in nvn.h (no signature check)')
            found = ops_of(body) if n.startswith('nvnCommandBuffer') else set()
            canon = sorted({opc.get(o, o) for o in found if opc.get(o, o) in opset}, key=ops.index)
            compact = sorted(o for o in found if o.startswith('OPC_'))
            core = re.sub(r'\(void\)\s*\w+\s*;', '', body[1:-1]).strip()
            logonly = log_only(raw)
            if canon:
                row['cls'] = 'recorded'; row['op'] = ','.join(canon); row['replay'] = ','.join(o for o in canon if o in replay)
                if compact: row['notes'].append('compact form %s, expanded before the replay' % '/'.join(compact))
                for o in canon:
                    if o not in replay and o not in NO_CASE_OK: problems.append('%s records %s, which has no case in mtl_exec' % (n, o))
            elif core == '' or re.fullmatch(r'return\s*[^;{}()]*;', core) or logonly:
                stub = True
                row['notes'].append('code: ' + ('only logs' if logonly else 'empty body' if core == '' else 'returns a constant'))
            else:
                row['cls'] = 'immediate'
            red = re.search(r'LOG\("(\[RED\][^"]{0,100})', raw)
            if red and row['cls'] in ('immediate', 'recorded'): row['notes'].append('code reports ' + re.sub(r'\s+', ' ', red.group(1)).replace('%', ''))
        elif n in nops:
            stub = True; row['handler'] = 'nvn_nop (g_nops)'; row['notes'].append('code: empty implementation (g_nops)')
        else:
            row['cls'] = 'missing'; row['notes'].append('no handler: nvn_unimpl_c logs "[RED] NVN unimplemented function called" once and returns 0')
        j = judged.get(n)
        if j:
            cls, anchor, note = j
            if cls in ('stub-ok', 'stub-risky') and not stub:
                problems.append('stale judgment: %s is judged %s but its handler is %s' % (n, cls, 'missing' if row['cls'] == 'missing' else 'no longer an empty, constant or log-only one'))
            elif cls == 'partial' and row['cls'] == 'missing':
                problems.append('stale judgment: %s is judged partial but has no handler' % n)
            if cls in ('stub-ok', 'partial') and not anchor:
                problems.append('%s is judged %s without an anchor (a phrase of the code that says why)' % (n, cls))
            if anchor and anchor not in corpus:
                problems.append('stale judgment: the anchor of %s is not in the code any more: %r' % (n, anchor))
            if cls != 'partial' or row['cls'] != 'missing': row['cls'] = cls
            row['notes'].append('judged: ' + note)
        elif stub:
            row['cls'] = 'unjudged'; problems.append('stub without a judgment: %s (add a row to %s)' % (n, JUDGMENTS))
        rows.append(row)
    for n in judged:
        if n not in nameset: problems.append('judgment for %s, which is not in names.txt' % n)
    return rows, problems


def render(rows):
    clean = lambda s: re.sub(r'[\t\n]+', ' ', s)
    out = ['\t'.join(HEAD)]
    for r in rows:
        out.append('\t'.join(clean(x) for x in (r['name'], r['category'], r['cls'], r['op'], r['handler'], r['replay'], '; '.join(r['notes']))))
    return '\n'.join(out) + '\n'


def totals(rows):
    t = {c: 0 for c in CLASSES}
    for r in rows: t[r['cls']] = t.get(r['cls'], 0) + 1
    return t


def summary(rows):
    t = totals(rows)
    return '%d names: %s' % (len(rows), ', '.join('%s %d' % (c, t[c]) for c in list(CLASSES) + [k for k in t if k not in CLASSES] if t.get(c)))


# the picture: one cell per name, grouped by category, on its own dark background so that it reads the same on GitHub's light and dark pages
COLORS = {'immediate': '#2ea043', 'recorded': '#2ea043', 'partial': '#7ee787', 'stub-ok': '#2d9a8c', 'stub-risky': '#d29922', 'missing': '#5b6472', 'unjudged': '#f85149'}
BG, FG, DIM = '#0d1117', '#e6edf3', '#9da7b3'
FONT = "-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"


def svg(rows):
    """The coverage map as an SVG string: plain string building, no scripts, fonts or references; the same rows give the same bytes."""
    esc = lambda x: x.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')
    W, PAD, GAP, CELL, PITCH = 960, 16, 24, 12, 14
    t = totals(rows); n = len(rows)
    translated = t.get('immediate', 0) + t.get('recorded', 0)
    out = []
    text = lambda x, y, size, fill, body, anchor='start', weight='normal': out.append(
        '<text x="%d" y="%d" font-family="%s" font-size="%d" font-weight="%s" fill="%s" text-anchor="%s">%s</text>' % (x, y, FONT, size, weight, fill, anchor, esc(body)))
    text(PAD, 34, 22, FG, 'NVN / NVN2 API coverage in the nxnative host', weight='bold')
    text(PAD, 58, 13, DIM, '%d entry points: %d translated (%.1f%%), %d partial, %d stubbed, %d missing' % (n, translated, 100.0 * translated / n, t.get('partial', 0),
                                                                                                    t.get('stub-ok', 0) + t.get('stub-risky', 0), t.get('missing', 0)))
    # the overall bar: translated / partial / stub-ok / stub-risky / missing as proportions of the names
    x, bw = float(PAD), W - 2 * PAD
    for key, cnt in (('translated', translated), ('partial', t.get('partial', 0)), ('stub-ok', t.get('stub-ok', 0)), ('stub-risky', t.get('stub-risky', 0)), ('missing', t.get('missing', 0))):
        w = bw * cnt / n
        if cnt: out.append('<rect x="%.2f" y="72" width="%.2f" height="14" fill="%s"><title>%s: %d of %d</title></rect>' % (x, w, COLORS['immediate' if key == 'translated' else key], key, cnt, n))
        x += w
    # the legend
    lx = PAD
    for key, label, cnt in (('immediate', 'immediate', t.get('immediate', 0)), ('recorded', 'recorded', t.get('recorded', 0)), ('partial', 'partial', t.get('partial', 0)),
                            ('stub-ok', 'stub-ok', t.get('stub-ok', 0)), ('stub-risky', 'stub-risky', t.get('stub-risky', 0)), ('missing', 'missing', t.get('missing', 0))):
        out.append('<rect x="%d" y="102" width="12" height="12" fill="%s"/>' % (lx, COLORS[key]))
        text(lx + 18, 112, 12, FG, '%s %d' % (label, cnt))
        lx += 154
    # the groups: two columns, each group goes under the shorter one; a group is a header line and rows of cells ordered by name
    cats = {}
    for r in rows: cats.setdefault(r['category'], []).append(r)
    colw = (W - 2 * PAD - GAP) // 2
    per_row = (colw + CELL - PITCH) // PITCH
    heights, top = [0, 0], 134
    for cat in sorted(cats):
        members = sorted(cats[cat], key=lambda r: r['name'])
        c = 0 if heights[0] <= heights[1] else 1
        gx, gy = PAD + c * (colw + GAP), top + heights[c]
        done = sum(1 for r in members if r['cls'] in ('immediate', 'recorded'))
        text(gx, gy + 14, 13, FG, cat, weight='bold')
        text(gx + colw, gy + 14, 13, DIM, '%d/%d' % (done, len(members)), anchor='end')
        out.append('<g>')
        for i, r in enumerate(members):
            out.append('<rect x="%d" y="%d" width="%d" height="%d" rx="2" fill="%s"><title>%s: %s</title></rect>' % (gx + i % per_row * PITCH, gy + 24 + i // per_row * PITCH, CELL, CELL, COLORS.get(r['cls'], COLORS['unjudged']), esc(r['name']), r['cls']))
        out.append('</g>')
        heights[c] += 24 + (len(members) + per_row - 1) // per_row * PITCH + 18
    H = top + max(heights) + 4
    text(PAD, H + 12, 11, DIM, 'translated = immediate or recorded. A cell is one name of dm/gfx/nvn/names.txt; hover for its name and class. Generated by dm/tools/nvn_api_map.py.')
    H += 24
    head = '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" role="img" aria-label="NVN / NVN2 API coverage map">\n<rect width="%d" height="%d" fill="%s"/>\n' % (W, H, W, H, W, H, BG)
    return head + '\n'.join(out) + '\n</svg>\n'


def gaps(rows, args):
    """Print the names one game calls (from logs or a names file) or carries (from a module) that the host does not fully handle."""
    called, carried = set(), set()
    for p in args.game_log:
        p = Path(p)
        for f in sorted(p.rglob('*.log')) + sorted(p.rglob('out.log')) if p.is_dir() else [p]:
            called |= set(re.findall(r'NVN unimplemented function called: (nvn\w+)', f.read_text(encoding='utf-8', errors='replace')))
    for p in args.game_names: called |= set(re.findall(r'\bnvn[A-Z][A-Za-z0-9]+', Path(p).read_text(encoding='utf-8', errors='replace')))
    for p in args.game_binary: carried |= {x.decode() for x in re.findall(rb'nvn[A-Z][A-Za-z0-9]+(?=\x00)', Path(p).read_bytes())}
    rank = {'missing': 0, 'partial': 1, 'stub-risky': 2}
    print('name\tclass\tevidence\tcategory\tnotes')
    for r in sorted((r for r in rows if r['cls'] in rank and (r['name'] in called or r['name'] in carried)), key=lambda r: (r['name'] not in called, rank[r['cls']], r['name'])):
        print('\t'.join([r['name'], r['cls'], 'called' if r['name'] in called else 'carried (not known to be called)', r['category'], '; '.join(r['notes'])]))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog='\n'.join(__doc__.split('\n')[1:]))
    ap.add_argument('--check', action='store_true', help='exit 1 when the table is stale or the judgments have a gap or a stale row')
    ap.add_argument('--gaps', action='store_true', help='print one game\'s gaps (with --game-log, --game-names, --game-binary); writes nothing')
    ap.add_argument('--svg', metavar='FILE', help='write the picture of the table here instead of docs/reference/nvn-api.svg (not with --check)')
    ap.add_argument('--repo', default=str(REPO), help='the repository to read (default: this one)')
    ap.add_argument('--game-log', action='append', default=[], metavar='PATH', help='a run log, or a folder of them: the names the host reported as unimplemented')
    ap.add_argument('--game-names', action='append', default=[], metavar='FILE', help='any text in which the nvn names a game called appear')
    ap.add_argument('--game-binary', action='append', default=[], metavar='FILE', help='a guest module: the nvn* strings in it are the names the game carries')
    a = ap.parse_args(argv)
    repo = Path(a.repo)
    rows, problems = analyze(repo)
    text = render(rows)
    if a.gaps:
        gaps(rows, a); return 0
    table, picture = repo / TABLE, repo / SVG
    if a.check:
        for path, rel, want in ((table, TABLE, text), (picture, SVG, svg(rows))):
            have = path.read_text(encoding='utf-8') if path.is_file() else ''
            if have != want:
                problems.append('%s is not what the sources give: run dm/tools/nvn_api_map.py and commit the result' % rel)
                d = list(difflib.unified_diff(have.split('\n'), want.split('\n'), 'committed ' + rel, 'generated', lineterm='', n=0))
                print('\n'.join(d[:60] + (['... %d more diff lines' % (len(d) - 60)] if len(d) > 60 else [])))
        for p in problems: print('nvn_api_map: ' + p)
        if not problems: print('nvn_api_map: ok, ' + summary(rows))
        return 1 if problems else 0
    Path(a.svg).write_text(svg(rows), encoding='utf-8') if a.svg else picture.write_text(svg(rows), encoding='utf-8')
    table.write_text(text, encoding='utf-8')
    for p in problems: print('nvn_api_map: ' + p)
    print(summary(rows))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
