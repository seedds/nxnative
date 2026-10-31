#!/usr/bin/env python3
# Cuts dm/gfx/nvn/nvn_mtl.m into topic parts (nvn_mtl_<part>.inc, same folder), mechanically: no renaming, reformatting or reordering.
#
#   python3 dm/tools/split_nvn_mtl.py              split dm/gfx/nvn/nvn_mtl.m in place (it must be the whole, unsplit file)
#   python3 dm/tools/split_nvn_mtl.py --join       put the parts back: nvn_mtl.m whole again, the part files removed (for merging or editing as one file)
#   python3 dm/tools/split_nvn_mtl.py --check      prove the cut: split in memory, put the parts back, compare byte for byte; changes nothing.
#                                                  On an already split folder it joins the parts and checks that cutting that gives the same parts again
#   python3 dm/tools/split_nvn_mtl.py --write-plan rewrite the plan (split_nvn_mtl_plan.tsv) with what this run assigned, new blocks included
#   options: --dir DIR (the folder of nvn_mtl.m, default dm/gfx/nvn), --plan FILE, --out DIR (write elsewhere; the input is left alone)
#
# Recutting on a newer main (after a conflict in nvn_mtl.m or a part):  --join, merge or rebase as usual in the whole file, then run the script again.
# Or take the file from main (git checkout upstream/main -- dm/gfx/nvn/nvn_mtl.m, and delete the parts) and cut it; the plan needs no care.
#
# How it cuts: the file is read as top-level blocks: a function, a variable or type definition, an Objective-C @interface/@implementation...@end, a preprocessor line (a
# #if...#endif group, a #define with its continuation lines) with the comment lines directly above it. A block is named by what it defines (the function or variable, or the
# directive); a name defined twice gets #2, #3 in file order. The plan (split_nvn_mtl_plan.tsv: part<TAB>name, in file order) assigns every block to a part, or to main (what
# nvn_mtl.m keeps). A block the plan does not know (new since the plan was written) goes to the part of the block before it; the run lists those. Every part must be one run of
# neighbouring blocks: it is included at the place its first block stood, so the order of the code does not change. The part files start with a three-line header comment; the
# include line replaces the run. Blank lines after the last block of a run stay in its part. A part whose `res_add(` sites would be on the same line numbers as another file's gets padding comment lines under its header.
#
# The split commit is listed in .git-blame-ignore-revs (GitHub's blame honours it; locally: git config blame.ignoreRevsFile .git-blame-ignore-revs). A recut commit has a new SHA:
# put it in that file in place of the old one.
#
# It refuses (exit 1, [RED] line) when it cannot read the file safely: braces or parentheses unbalanced at the end of a block, a comment, #if group or block that is still open at
# the end of the file, a #if group whose braces do not balance (it spans blocks), a part that would be in two pieces, or a file that is already split.
import argparse, collections, pathlib, re, sys

REPO = pathlib.Path(__file__).resolve().parents[2]
NVN = REPO / 'dm/gfx/nvn'
PLAN = pathlib.Path(__file__).resolve().with_name('split_nvn_mtl_plan.tsv')
MAIN = 'main'

# part -> what it holds (the second header line of the part file)
PARTS = {
    'submit': 'Submission and its helpers: conditional rendering, occlusion queries, pending-write tracking, the submission\'s begin, the barrier and GPU-profile counters, the encoder wrappers (enc_r_*, enc_c_*, enc_b_*) and the blit helpers.',
    'ubuf': 'Inline uniform updates: the deferred-update shadow (ubd_*), partial uniform reads (ubp_*), UpdateUniformBuffer / fill / event / counter, boot and A/B instrumentation, and the submission\'s commit, wait and finish.',
    'tex': 'Textures: the NVN texture to MTLTexture conversion, the alias / sharing tables, upgrades, the swizzled and linear upload paths, zero fill, sparse views and release.',
    'pass': 'Render passes: opening and closing, zero-source skip, pass dumps and per-pass GPU time, the store lookahead and the cross-submission validity ledger, attachment views and rp_begin.',
    'shader': 'Shaders and descriptors: the shader mapping table, c1 banks, pipeline recording files, shader and program loading, vertex-conversion sets, texture and sampler registration (the descriptor pool), tex_by_id / samp_by_id and the argument buffer arenas.',
    'state': 'Draw state: vertex buffer binding, the pull layout, stage binding, vertex format conversion (vconv_*), the vertex range cache and pso_desc, the description of a pipeline.',
    'pso': 'Pipelines: the Metal 4 base pipelines and the per-game archive, parallel precompile, hybrid pipelines, the prebuild, pso_get and the warm-up worker.',
    'args': 'Per-draw support: depth-stencil states, dummy resources, scratch buffers, argument encoding (encode_args) and the quad / fan index buffers.',
    'lookahead': 'Lookahead and hoisting: depth / stencil use of a pass, the deferred-release and hoist helpers, state ops, shader prefetch and pipeline prediction, render-target range checks, the store lookahead index.',
    'merge': 'Pass merging and draw rewrites: the readable tile pass merge, deferred passes, fusing, zero-skip, kd / ms draws and feedback snapshots.',
    'indirect': 'Draws Metal cannot take as they are: indirect and 8-bit index draws, masked multi-draws, transform feedback and the last-use cache (lc_*).',
    'draw': 'Draw and dispatch, buffer / texture copies, clears a load action cannot express, the cut counters and the replay executor mtl_exec.',
    'shot': 'Presentation helpers and diagnostics: the swapchain blit state, screenshots, texture dumps and lists, presented-time bookkeeping.',
    'present': 'Present path: the blit pipeline and encoders, the display link, the present thread, external textures, snapshots, present queueing and waiting, and the small stats accessors.',
}
HEADER_LINES = 3
SITE = re.compile(r'\bres_add\(')   # res_add(r) is res_add_at(r, __LINE__): the line number keys the site in the residency statistics, so no two sites in the parts may share one
PAD = '// (padding line: res_add keys its sites by __LINE__, so no two sites in different files may be on the same line number)\n'
PLAN_HEAD = '# dm/tools/split_nvn_mtl.py: part<TAB>block name, in file order (main = stays in nvn_mtl.m). Rewritten by --write-plan; a block missing here goes to the part before it.\n'

def die(msg):
    sys.exit(f'[RED] split_nvn_mtl: {msg}')

def part_file(part): return f'nvn_mtl_{part}.inc'
def site_lines(lines):
    return {i for i, l in enumerate(lines, 1) if SITE.search(l) and not l.lstrip().startswith('#define')}

def include_line(part): return f'#include "{part_file(part)}"\n'
def header(part):
    return [f'// nvn_mtl.m, part: {part}\n', f'// {PARTS[part]}\n',
            '// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.\n']

# ---- reading C / Objective-C by line -------------------------------------------------------------------------------------------------------------------------------------

class Line:
    __slots__ = ('text', 'code', 'braces', 'parens', 'directive', 'comment_only', 'blank')

def scan(lines):
    """Per line: its code with comments, strings and character literals blanked, and the brace and paren change (not counted on preprocessor lines)."""
    out, in_bc, cont = [], False, False
    for text in lines:
        ln = Line(); ln.text = text
        code, i, n = [], 0, len(text)
        was_bc = in_bc
        while i < n:
            c = text[i]
            if in_bc:
                j = text.find('*/', i)
                if j < 0: i = n
                else: in_bc, i = False, j + 2
                code.append(' ')
            elif text.startswith('/*', i): in_bc, i = True, i + 2; code.append(' ')
            elif text.startswith('//', i): i = n; code.append(' ')
            elif c in '"\'':
                j = i + 1
                while j < n and text[j] != c: j += 2 if text[j] == '\\' else 1
                if j >= n: die(f'unterminated {c} literal: {text.strip()[:80]}')
                code.append(c + ' ' * (j - i - 1) + c); i = j + 1
            else: code.append(c); i += 1
        ln.code = ''.join(code).rstrip()
        ln.blank = not text.strip()
        ln.directive = cont or (not was_bc and ln.code.lstrip().startswith('#'))
        cont = ln.directive and text.rstrip('\n').endswith('\\')
        ln.comment_only = not ln.blank and not ln.code.strip()
        ln.braces = 0 if ln.directive else ln.code.count('{') - ln.code.count('}')
        ln.parens = 0 if ln.directive else ln.code.count('(') - ln.code.count(')')
        out.append(ln)
    if in_bc: die('a /* comment is still open at the end of the file')
    return out

SKIP_CALLS = {'_Atomic', 'typeof', '__typeof__', '__attribute__', 'sizeof', '_Alignas', 'alignas', '__declspec', 'defined'}

def block_name(body):
    """The name of what a block defines. body: the code lines of the block, comments and strings blanked, and the first raw line for directives."""
    first = body[0].strip()
    if first.startswith('#'):
        m = re.match(r'#\s*(\w*)\s*(\w+)?', first)
        kw = m.group(1) if m else ''
        if kw in ('define', 'undef') and m: return f'#{kw} {m.group(2)}'
        return '#' + re.sub(r'\s+', ' ', first[1:].strip())
    m = re.match(r'@(interface|implementation|protocol)\s+(\w+)', first)
    if m: return f'@{m.group(1)} {m.group(2)}'
    text = re.sub(r'__attribute__\s*\(\(.*?\)\)', ' ', ' '.join(body))
    head = re.split(r'[{=;]', text, maxsplit=1)[0]                   # the declaration before its body or initializer
    head = re.sub(r'<[^<>;]*>', ' ', head)                 # id<MTLBuffer>, NSArray<X *> -> id, NSArray
    if text.lstrip().startswith('_Static_assert'): return '_Static_assert ' + re.sub(r'\s+', ' ', text).strip()[:60]
    # a function (or a function pointer): the name before the first '(' that is not a type-like call
    for m in re.finditer(r'(\w+)\s*\(', head):
        if m.group(1) in SKIP_CALLS: continue
        rest = head[m.end():].lstrip()
        if rest.startswith(('*', '^')):    # void (*fp)(int): the name is inside
            m2 = re.match(r'[*^\s]*(\w+)', rest)
            if m2: return m2.group(1)
        proto = ';' if text.rstrip().endswith(';') and '{' not in text and '=' not in text and not re.match(r'\s*typedef\b', text) else ''
        return m.group(1) + proto                            # name; is a prototype, so it is not the same block as the definition
    head = re.split(r'[,\[]', head, maxsplit=1)[0]
    ids = re.findall(r'\w+', head)
    if ids and ids[-1] in ('struct', 'union', 'enum') and '{' in text:          # no tag: named by its variable, or by its first enumerator
        tail = re.findall(r'\w+', re.split(r'[,\[;=]', text.rsplit('}', 1)[-1], maxsplit=1)[0])
        if tail: return tail[-1]
        return 'enum ' + re.findall(r'\w+', text.split('{', 1)[1])[0]
    if re.match(r'\s*typedef\b', head) or not ids:
        tail = re.findall(r'\w+', re.split(r'[{]', text)[-1])  # typedef struct {...} name;
        if tail: return tail[-1]
    if ids and ids[0] in ('struct', 'enum', 'union') and len(ids) == 2 and '{' in text: return ' '.join(ids)
    if not ids: die(f'no name for the block starting {first[:80]}')
    return ids[-1]

class Block:
    def __init__(self, start, end, name):
        self.start, self.end, self.name = start, end, name     # lines [start, end): the comment lines above, the code, the blank lines after it

def parse(text):
    """Cut the file into top-level blocks; return (lines, blocks)."""
    lines = text.splitlines(keepends=True)
    info = scan(lines)
    blocks, i, n = [], 0, len(lines)
    pending = 0           # first line of the comment / blank lines before the next block
    while i < n:
        ln = info[i]
        if ln.blank or ln.comment_only:
            i += 1; continue
        # a block starts here; its preamble is the comment run directly above (a comment run split from the code by a blank line goes with it too)
        start = pending if blocks == [] else _preamble(info, blocks[-1].end if blocks else 0, i)
        j = _block_end(info, i)
        k = j
        while k < n and info[k].blank: k += 1
        name = block_name([lines[i].rstrip('\n')] if ln.code.lstrip().startswith('#') else [info[x].code for x in range(i, j)])
        b = Block(start, k, name)
        blocks.append(b)
        i = k
    # trailing comments / blanks after the last block belong to it
    if blocks: blocks[-1].end = n
    return lines, blocks

def _preamble(info, lo, i):
    """The first line of the comment run (with blank lines inside it) that ends at line i, not before lo; blank lines right after the previous block stay with it."""
    s = i
    while s > lo and (info[s - 1].comment_only or info[s - 1].blank): s -= 1
    while s < i and info[s].blank: s += 1
    return s

def _block_end(info, i):
    """The line after the block that starts at line i."""
    n = len(info)
    ln = info[i]
    code = ln.code.lstrip()
    if ln.directive and code.startswith('#'):
        m = re.match(r'#\s*(\w+)', code)
        kw = m.group(1) if m else ''
        j = i
        while info[j].text.rstrip('\n').endswith('\\') and j + 1 < n: j += 1
        j += 1
        if kw in ('if', 'ifdef', 'ifndef'):
            depth, br, par = 1, 0, 0
            for x in range(i + 1, n):
                c = info[x].code.lstrip()
                if info[x].directive and c.startswith('#'):
                    k = re.sub(r'^#\s*(\w*).*', r'\1', c)
                    if k in ('if', 'ifdef', 'ifndef'): depth += 1
                    elif k == 'endif':
                        depth -= 1
                        if depth == 0:
                            j = x + 1
                            if br != 0 or par != 0: die(f'the #if group at line {i + 1} spans blocks (braces/parens do not balance inside it): {info[i].text.strip()[:80]}')
                            return j
                else:
                    br += info[x].code.count('{') - info[x].code.count('}'); par += info[x].code.count('(') - info[x].code.count(')')
            die(f'the #if at line {i + 1} has no #endif')
        return j
    if code.startswith(('@interface', '@implementation', '@protocol')):
        # one-line forms end with @end on the same line
        for x in range(i, n):
            if re.search(r'@end\b', info[x].code): return x + 1
        die(f'no @end for line {i + 1}')
    br = par = 0
    head_open = None
    for x in range(i, n):
        l = info[x]
        if l.directive: continue
        if head_open is None and '{' in l.code:
            head_open = ' '.join(info[y].code for y in range(i, x)) + ' ' + l.code.split('{')[0]
        br += l.braces; par += l.parens
        if br < 0 or par < 0: die(f'unbalanced }}/) at line {x + 1}: {l.text.strip()[:80]}')
        if br == 0 and par == 0 and l.code.strip():
            last = l.code.rstrip()[-1]
            if last == ';': return x + 1
            if last == '}':
                h = re.sub(r'__attribute__\s*\(\(.*?\)\)', '', head_open or '')
                if '(' in h and '=' not in h and not re.match(r'\s*typedef\b', h): return x + 1   # a function body
                if '=' in h or re.match(r'\s*(typedef|struct|union|enum)\b', h): continue         # the ; follows
                return x + 1
    die(f'the block starting at line {i + 1} is not closed at the end of the file (unbalanced braces, parens or a missing ;): {info[i].text.strip()[:80]}')

def keys(blocks):
    seen = collections.Counter(); out = []
    for b in blocks:
        seen[b.name] += 1
        out.append(b.name if seen[b.name] == 1 else f'{b.name}#{seen[b.name]}')
    return out

# ---- the plan ----------------------------------------------------------------------------------------------------------------------------------------------------------

def read_plan(path):
    plan = {}
    for ln in pathlib.Path(path).read_text().splitlines():
        if not ln or ln.startswith('#'): continue
        part, _, name = ln.partition('\t')
        if part != MAIN and part not in PARTS: die(f'{path}: unknown part {part!r}')
        plan[name] = part
    return plan

def assign(blocks, plan):
    """The part of every block; the blocks the plan does not know, and those known blocks that stand away from their part (moved by an edit): both take the part of the
    block before them. A part keeps its longest run of neighbouring blocks."""
    ks = keys(blocks)
    first, prev = [], MAIN
    for k in ks:
        prev = plan.get(k, prev); first.append(prev)
    core, i = {}, 0
    while i < len(first):
        j = i
        while j + 1 < len(first) and first[j + 1] == first[i]: j += 1
        if first[i] != MAIN and (first[i] not in core or j - i > core[first[i]][1] - core[first[i]][0]): core[first[i]] = (i, j)
        i = j + 1
    labels, new = [], []
    for idx, k in enumerate(ks):
        p = plan.get(k)
        if p is None: p = labels[-1] if labels else MAIN; new.append((k, blocks[idx].start + 1, p, 'new'))
        elif p != MAIN and not core[p][0] <= idx <= core[p][1]:
            was, p = p, labels[-1] if labels else MAIN; new.append((k, blocks[idx].start + 1, p, f'was {was}'))
        labels.append(p)
    runs = {}
    for idx, p in enumerate(labels):
        if p != MAIN: runs[p] = (runs[p][0], idx) if p in runs else (idx, idx)
    for p, (a, z) in runs.items():
        if any(labels[x] != p for x in range(a, z + 1)):
            die(f'part {p!r} would be in two pieces: blocks {ks[a]!r} (line {blocks[a].start + 1}) .. {ks[z]!r} are not all its own; fix the plan')
    return ks, labels, runs, new

# ---- cutting and joining -------------------------------------------------------------------------------------------------------------------------------------------------

def split(text, plan):
    """Returns (nvn_mtl.m text, {part: file text}, new blocks, labels info)."""
    if any(include_line(p) in text for p in PARTS): die('nvn_mtl.m already includes its parts: --join first')
    lines, blocks = parse(text)
    ks, labels, runs, new = assign(blocks, plan)
    main, parts, cursor = [], {}, 0
    for p, (a, z) in sorted(runs.items(), key=lambda kv: kv[1][0]):
        first, last = blocks[a].start, blocks[z].end
        main += lines[cursor:first]
        parts[p] = header(p) + lines[first:last]
        main.append(include_line(p))
        cursor = last
    main += lines[cursor:]
    used = site_lines(main)       # the sites of nvn_mtl.m itself keep their lines; a part whose sites would meet one gets padding lines under its header
    for p in sorted(parts, key=lambda q: runs[q][0]):
        while site_lines(parts[p]) & used: parts[p].insert(HEADER_LINES, PAD)
        used |= site_lines(parts[p])
    parts = {p: ''.join(t) for p, t in parts.items()}
    return ''.join(main), parts, new, (ks, labels, runs)

def join(main_text, read_part):
    out = main_text
    for p in PARTS:
        inc = include_line(p)
        if inc not in out: continue
        body = read_part(p)
        hl = body.splitlines(keepends=True)
        if hl[:HEADER_LINES] != header(p): die(f'{part_file(p)}: the three header lines are not the ones this script writes')
        k = HEADER_LINES
        while hl[k] == PAD: k += 1
        out = out.replace(inc, ''.join(hl[k:]), 1)
    return out

def roundtrip(text, plan):
    main, parts, new, info = split(text, plan)
    back = join(main, lambda p: parts[p])
    if back != text: die('the parts put back are not the original file')
    return main, parts, new, info

def read_tree(d):
    main = (d / 'nvn_mtl.m').read_text()
    return join(main, lambda p: (d / part_file(p)).read_text())

def report(new, info, parts, main):
    ks, labels, runs = info
    for p in sorted(parts, key=lambda q: runs[q][0]):
        print(f'  {part_file(p):26s} {parts[p].count(chr(10)):6d} lines')
    print(f'  {"nvn_mtl.m":26s} {main.count(chr(10)):6d} lines')
    for k, ln, p, why in new: print(f'  {why} block {k!r} (line {ln}) -> {p}')

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0] if __doc__ else None, usage='split_nvn_mtl.py [--join | --check | --write-plan] [--dir DIR] [--plan FILE] [--out DIR]')
    ap.add_argument('--dir', default=str(NVN)); ap.add_argument('--plan', default=str(PLAN)); ap.add_argument('--out')
    g = ap.add_mutually_exclusive_group(); g.add_argument('--join', action='store_true'); g.add_argument('--check', action='store_true'); g.add_argument('--write-plan', action='store_true')
    a = ap.parse_args()
    d = pathlib.Path(a.dir); out = pathlib.Path(a.out) if a.out else d
    plan = read_plan(a.plan)
    src = d / 'nvn_mtl.m'
    text = src.read_text()
    is_split = any(include_line(p) in text for p in PARTS)
    if a.join:
        if not is_split: die('nothing to join: nvn_mtl.m includes none of its parts')
        whole = read_tree(d); out.mkdir(parents=True, exist_ok=True); (out / 'nvn_mtl.m').write_text(whole)
        if out == d:
            for p in PARTS:
                (d / part_file(p)).unlink(missing_ok=True)
        print(f'joined: nvn_mtl.m {whole.count(chr(10))} lines'); return
    if is_split:
        if not a.check: die('nvn_mtl.m is already split: --join first')
        whole = read_tree(d)
        main, parts, new, info = roundtrip(whole, plan)
        if main != text or any(parts[p] != (d / part_file(p)).read_text() for p in parts): die('cutting the joined parts again gives other files than the ones in the folder')
        print('check: the folder joins to a file that cuts into the same files'); report(new, info, parts, main); return
    main, parts, new, info = roundtrip(text, plan)     # always proves the round trip before anything is written
    if a.check:
        print('check: the parts put back in place give the original file byte for byte'); report(new, info, parts, main); return
    ks, labels, runs = info
    if a.write_plan:
        pathlib.Path(a.plan).write_text(PLAN_HEAD + ''.join(f'{p}\t{k}\n' for k, p in zip(ks, labels)))
    out.mkdir(parents=True, exist_ok=True)
    (out / 'nvn_mtl.m').write_text(main)
    for p, t in parts.items(): (out / part_file(p)).write_text(t)
    print(f'cut {src}: {len(parts)} parts'); report(new, info, parts, main)

if __name__ == '__main__':
    main()
