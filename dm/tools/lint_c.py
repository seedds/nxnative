#!/usr/bin/env python3
# C and Objective-C lint of dm/host and dm/gfx, as CI runs it (.github/workflows/lint-c.yml). Three commands:
#   compdb  writes the compile database dm/host/gen/compile_commands.json from dry runs of the Makefiles (make -n -B): the host (dm/host), the texture tests (dm/gfx/tex/tests),
#           every graphics test (dm/gfx/tests), the host tests (dm/host/tests) and the Metal sample (dm/gfx/metal), all for one profile (--game, default GAME or Super Mario Bros. Wonder).
#           A source compiled by several of them keeps the first command (the host's). Third-party sources (dm/third_party) are left out.
#   tidy    runs clang-tidy (the checks in .clang-tidy) on every source of the database and counts its findings per file and check, without line numbers, so that moving code does not
#           change them. The counts are a ratchet: more findings than dm/tools/lint_c_baseline.tsv lists for a file and check fail; fewer only ask for a refresh. --update writes
#           the baseline from this run (after fixing findings, or when a change is meant to add one). --report FILE writes every finding with its line. Sources named on the
#           command line are checked alone: quicker while fixing, but such a run does not report fixed findings (a file's findings also come from the sources including it).
#   format  runs clang-format (.clang-format, .clang-format-ignore) on the lines changed since --base (default: the merge base with upstream/main, else origin/main) and lists the
#           lines whose spacing inside the line would change. clang-format's line breaks are not reported: it puts each statement on a line of its own, which the house style does
#           not. A report only; --strict fails when it lists a line. --diff FILE writes clang-format's whole diff of those lines.
# Under GitHub Actions, tidy and format also write to the job summary.
# LLVM's clang-tidy and clang-format are needed (Apple's toolchain has no clang-tidy): LLVM=<folder with bin/> (a release from github.com/llvm/llvm-project), else Homebrew's
# llvm@23 or llvm, else PATH. The baseline names the LLVM it was made with, the release CI downloads (LLVM_VERSION in .github/workflows/lint-c.yml; test_lint_c.py keeps
# the two in step); another major version finds other things, and tidy says so.
# Usage: lint_c.py compdb [--game ID] | tidy [--update] [--report FILE] [-j N] [FILE...] | format [--base REF] [--strict] [--diff FILE]
import argparse, collections, concurrent.futures, difflib, json, os, pathlib, re, shlex, shutil, subprocess, sys

REPO = pathlib.Path(__file__).resolve().parents[2]
DB_DIR = REPO / 'dm' / 'host' / 'gen'
BASELINE = REPO / 'dm' / 'tools' / 'lint_c_baseline.tsv'
LINT_DIRS = ('dm/host', 'dm/gfx')
SKIP = ('dm/third_party/', 'dm/host/gen/', 'dm/gfx/nvn/include/')   # third-party code, generated headers, the NVN API header
SOURCES = ('.c', '.m', '.mm', '.cpp')
DEFAULT_GAME = '010015100B514000'

def llvm_tool(name):
    if os.environ.get('LLVM'):
        p = pathlib.Path(os.environ['LLVM']) / 'bin' / name
        if p.exists(): return str(p)
        sys.exit(f'{p} not found (LLVM={os.environ["LLVM"]})')
    for f in ('llvm@23', 'llvm') if shutil.which('brew') else ():
        r = subprocess.run(['brew', '--prefix', f], capture_output=True, text=True)
        p = pathlib.Path(r.stdout.strip()) / 'bin' / name
        if r.returncode == 0 and p.exists(): return str(p)
    if shutil.which(name): return shutil.which(name)
    sys.exit(f'{name} not found: set LLVM=<LLVM folder> or brew install llvm@23')

def rel(p):
    try: return pathlib.Path(p).resolve().relative_to(REPO).as_posix()
    except ValueError: return None

def linted(r):
    return r is not None and r.startswith(LINT_DIRS) and not r.startswith(SKIP)

# ---------- compile database ----------
def make_targets(d):
    # every t_* target of a test Makefile (the graphics and host tests), without the sanitizer variants (their sources are the plain tests')
    ts = re.findall(r'^(t_[a-z0-9_]+):', (REPO / d / 'Makefile').read_text(), re.M)
    return sorted({t for t in ts if not re.search(r'tsan|asan|sanitize', t)})

def dry_run(d, targets, game):
    r = subprocess.run(['make', '-n', '-B', '-k', '-w', f'GAME={game}', *targets], cwd=REPO / d, capture_output=True, text=True)
    dirs, out = [REPO / d], []   # make -w names the folder each recursive make enters and leaves
    for line in r.stdout.split('\n'):
        m = re.match(r"make(?:\[\d+\])?: (Entering|Leaving) directory [`']([^']+)'", line)
        if m:
            if m.group(1) == 'Entering': dirs.append(pathlib.Path(m.group(2)))
            elif len(dirs) > 1: dirs.pop()
            continue
        out.append((dirs[-1], line))
    return out

def entries(cwd, line, sysroot):
    try: argv = shlex.split(line)
    except ValueError: return []
    if not argv or os.path.basename(argv[0]) not in ('clang', 'cc', 'clang++', 'gcc'): return []
    flags, srcs, i = [], [], 1
    while i < len(argv):
        a = argv[i]
        if a in ('-o', '-framework', '-MF', '-MT', '-arch'): i += 2; continue
        if a in ('-c', '-Werror') or a.startswith(('-fsanitize', '-l', '-L', '-Wl,')): i += 1; continue
        if a.startswith('-'):
            flags.append(a)
            if a in ('-I', '-D', '-include', '-isystem', '-x', '-isysroot', '-iquote'): flags.append(argv[i + 1]); i += 1
        elif a.endswith(SOURCES): srcs.append(a)
        i += 1
    if sysroot and '-isysroot' not in flags: flags = ['-isysroot', sysroot] + flags
    return [{'directory': str(cwd), 'file': str((cwd / s).resolve()), 'arguments': ['clang', *flags, '-c', str((cwd / s).resolve())]} for s in srcs]

def compdb(game):
    # the profile header and dmsh_chain.h must exist for the sources to parse: make writes the first (game_profile), the second is committed
    subprocess.run(['make', '-s', '-C', str(REPO / 'dm/host'), f'GAME={game}', 'game_profile'], check=True)
    sysroot = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip() if sys.platform == 'darwin' else ''
    runs = [('dm/host', ['dmhost']), ('dm/gfx/tex/tests', make_targets('dm/gfx/tex/tests')), ('dm/gfx/tests', make_targets('dm/gfx/tests')),
            ('dm/host/tests', make_targets('dm/host/tests')), ('dm/gfx/metal', ['all'])]
    db = merge_entries(e for d, targets in runs for cwd, line in dry_run(d, targets, game) for e in entries(cwd, line, sysroot))
    DB_DIR.mkdir(parents=True, exist_ok=True)
    (DB_DIR / 'compile_commands.json').write_text(json.dumps(db, indent=1) + '\n')
    print(f'{DB_DIR.relative_to(REPO)}/compile_commands.json: {len(db)} sources')
    return db

def merge_entries(es):
    # one entry per linted source, the first command that compiles it (the runs go host first), sorted by file
    db, seen = [], set()
    for e in es:
        r = rel(e['file'])
        if linted(r) and r not in seen: seen.add(r); db.append(e)
    return sorted(db, key=lambda e: e['file'])

# ---------- clang-tidy ----------
DIAG = re.compile(r'^(.+?):(\d+):(\d+): (warning|error): (.*?) \[([^\]]+)\]$')

def tidy_one(tool, f):
    r = subprocess.run([tool, '-p', str(DB_DIR), '--quiet', f], capture_output=True, text=True, cwd=REPO)
    if r.returncode < 0 or 'PLEASE submit a bug report' in r.stderr:   # a crash would otherwise read as a file without findings
        sys.exit(f'[RED] lint_c: clang-tidy crashed on {rel(f)} (exit {r.returncode}):\n{r.stderr[-4000:]}')
    return f, parse_diags(r.stdout)

def parse_diags(text):
    # clang-tidy's findings in dm/host and dm/gfx as (file, line, column, check, message); a finding of several checks counts for each
    out = []
    for line in text.split('\n'):
        m = DIAG.match(line)
        if not m: continue
        path, ln, col, _, msg, checks = m.groups()
        r = rel(path)
        if not linted(r): continue
        for check in checks.split(','):
            # the compiler's own warnings are the build's to report; a parse error is kept (the file was not fully checked)
            if check.startswith('clang-diagnostic-') and check != 'clang-diagnostic-error': continue
            out.append((r, int(ln), int(col), check, msg))
    return out

def parse_baseline(text):
    base = collections.Counter()
    for line in text.split('\n'):
        if line and not line.startswith('#'):
            f, check, n = line.split('\t'); base[(f, check)] = int(n)
    return base

def baseline_llvm(text):
    # the LLVM version a baseline was made with (its header), or None
    m = re.search(r'^#.*Made with LLVM (\d+\.\d+\.\d+)', text, re.M)
    return m.group(1) if m else None

def read_baseline():
    return parse_baseline(BASELINE.read_text()) if BASELINE.exists() else collections.Counter()

def compare(counts, base, partial=False):
    # the file/check pairs above the baseline (worse: fail) and below it (better: refresh the baseline), each as ((file, check), now, baseline). A partial run (some
    # sources) finds nothing fixed: a file's findings can come from other sources too (a header, or a test that includes the .c or .m file it tests)
    worse = [(k, n, base.get(k, 0)) for k, n in sorted(counts.items()) if n > base.get(k, 0)]
    better = [] if partial else [(k, counts.get(k, 0), b) for k, b in sorted(base.items()) if counts.get(k, 0) < b]
    return worse, better

def summary(text):
    # GitHub's job summary, when running there
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f: f.write(text + '\n')

def write_baseline(counts, version):
    lines = ['# clang-tidy findings of dm/host and dm/gfx per file and check (dm/tools/lint_c.py tidy; checks in .clang-tidy), the ratchet CI holds pull requests to: more',
             '# findings for a file and check than listed here fail. Refresh after fixing findings, or when a change is meant to add one: python3 dm/tools/lint_c.py compdb &&',
             f'# python3 dm/tools/lint_c.py tidy --update. Made with {version}.',
             '# file\tcheck\tfindings']
    lines += [f'{f}\t{c}\t{n}' for (f, c), n in sorted(counts.items())]
    BASELINE.write_text('\n'.join(lines) + '\n')

def tidy(args):
    tool = llvm_tool('clang-tidy')
    version = re.search(r'LLVM version (\S+)', subprocess.run([tool, '--version'], capture_output=True, text=True).stdout)
    version = f'LLVM {version.group(1)}' if version else 'an unknown LLVM'
    dbf = DB_DIR / 'compile_commands.json'
    if not dbf.exists(): sys.exit(f'{dbf.relative_to(REPO)} is missing: run lint_c.py compdb first')
    if args.update and args.files: sys.exit('--update writes the whole baseline: run it without files')
    files = [e['file'] for e in json.loads(dbf.read_text())]
    if args.files: files = [str((REPO / f).resolve()) for f in args.files]
    found = set()
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for i, (f, out) in enumerate(ex.map(lambda f: tidy_one(tool, f), files)):
            found.update(out)   # a header's finding comes once from every source that includes it
            print(f'[{i + 1}/{len(files)}] {rel(f)}: {sum(1 for o in out if o[0] == rel(f))} findings', flush=True)
    found = sorted(found)
    counts = collections.Counter((f, c) for f, _, _, c, _ in found)
    if args.report:
        pathlib.Path(args.report).write_text(''.join(f'{f}:{l}:{c}: {msg} [{chk}]\n' for f, l, c, chk, msg in found))
    print(f'{version}: {len(found)} findings in {len({f for f, _ in counts})} files, {len(counts)} file/check pairs')
    if args.update:
        write_baseline(counts, version); print(f'wrote {BASELINE.relative_to(REPO)}'); return 0
    base = read_baseline()
    made = baseline_llvm(BASELINE.read_text()) if BASELINE.exists() else None
    if made and made.split('.')[0] != version.removeprefix('LLVM ').split('.')[0]:
        print(f'::warning::the baseline was made with LLVM {made}, this is {version}: another major version finds other things')
    worse, better = compare(counts, base, partial=bool(args.files))
    md = []
    for (f, c), n, b in worse:
        print(f'{f}: {c}: {n} findings, the baseline has {b}')
        md.append(f'- `{f}` {c}: {n} findings, the baseline has {b}')
        for ff, l, col, cc, msg in found:   # each finding of the pair, annotated on its line: which of them is new, counts cannot tell
            if ff == f and cc == c: print(f'::error file={f},line={l},col={col}::{msg} [{c}] ({n} in this file, the baseline has {b})'); md.append(f'  - line {l}: {msg}')
    for (f, c), n, b in better:
        print(f'::notice file={f}::{c}: {n} findings, the baseline has {b}: refresh it (python3 dm/tools/lint_c.py tidy --update)')
    line = f'baseline {sum(base.values())} findings; {len(worse)} file/check pairs above it, {len(better)} below it'
    print(line)
    summary(f'### clang-tidy ({version})\n\n{len(found)} findings, {line}.'
            + ('\n\nNew findings (fix them, or refresh the baseline when one is meant: `python3 dm/tools/lint_c.py tidy --update`):\n\n' + '\n'.join(md) if worse else '')
            + (f'\n\n{len(better)} file/check pairs are below the baseline: `python3 dm/tools/lint_c.py tidy --update` lowers it.' if better else ''))
    return 1 if worse else 0

# ---------- clang-format ----------
def merge_base(ref):
    for r in ([ref] if ref else ['upstream/main', 'origin/main']):
        p = subprocess.run(['git', 'merge-base', 'HEAD', r], cwd=REPO, capture_output=True, text=True)
        if p.returncode == 0: return p.stdout.strip()
    sys.exit(f'no merge base with {ref or "upstream/main or origin/main"}: pass --base')

def gaps(text):
    # the non-whitespace characters of text, each with the whitespace before it and its line; a backslash line continuation counts as a line break
    out, ws, line = [], '', 1
    for ch in text.replace('\\\n', '\0'):
        if ch == '\0' or ch == '\n': ws += '\n'; line += 1
        elif ch.isspace(): ws += ch
        else: out.append((ch, ws, line)); ws = ''
    return out

def spacing_lines(old, new):
    # lines of old where clang-format adds or removes a space inside a line (not a line break, and not the width of a space that stays)
    a, b = gaps(old), gaps(new)
    if [c for c, _, _ in a] != [c for c, _, _ in b]: return None
    return sorted({l for (_, w1, l), (_, w2, _) in zip(a, b) if '\n' not in w1 + w2 and bool(w1) != bool(w2)})

def hunk_lines(diff):
    # the added and changed lines of a unified diff with -U0, per file (the new side); a hunk that only deletes adds none
    out, f = collections.defaultdict(set), None
    for line in diff.split('\n'):
        if line.startswith('+++ '): f = line[6:] if line.startswith('+++ b/') else None
        m = re.match(r'@@ -\S+ \+(\d+)(?:,(\d+))? @@', line)
        if m and f: s, n = int(m.group(1)), int(m.group(2) or 1); out[f].update(range(s, s + n))
    return {f: ls for f, ls in out.items() if ls and f.endswith(SOURCES + ('.h',)) and linted(f)}

def changed_lines(base):
    # the added and changed lines of the working tree against base, per file
    d = subprocess.run(['git', 'diff', '-U0', '--no-color', '--no-ext-diff', base, '--', *LINT_DIRS], cwd=REPO, capture_output=True, text=True, check=True).stdout
    return {f: ls for f, ls in hunk_lines(d).items() if (REPO / f).exists()}

def line_ranges(ls):
    # sorted line numbers as runs of consecutive lines, for clang-format --lines
    ranges = []
    for l in sorted(ls):
        if ranges and ranges[-1][1] == l - 1: ranges[-1][1] = l
        else: ranges.append([l, l])
    return ranges

def report_lines(src, new, changed):
    # the changed lines of src whose spacing clang-format's output new changes, with their text; none when it moved code across lines (line numbers no longer pair up)
    lines = spacing_lines(src, new)
    text = src.split('\n')
    return [(l, text[l - 1].strip()) for l in lines or () if l in changed]

def fmt(args):
    cf = llvm_tool('clang-format')
    if args.base:
        p = subprocess.run(['git', 'rev-parse', '--verify', args.base + '^{commit}'], cwd=REPO, capture_output=True, text=True)
        if p.returncode: sys.exit(f'--base {args.base}: no such commit')
        base = p.stdout.strip()
    else: base = merge_base(None)
    files = changed_lines(base)
    report, diffs = [], []
    for f, ls in sorted(files.items()):
        src = (REPO / f).read_text(errors='replace')
        new = subprocess.run([cf, f'--assume-filename={f}', *[f'--lines={a}:{b}' for a, b in line_ranges(ls)]], input=src, capture_output=True, text=True, cwd=REPO).stdout
        if not new or new == src: continue   # unchanged, or ignored (.clang-format-ignore)
        diffs.append(unified(src, new, f))
        report += [(f, l, t) for l, t in report_lines(src, new, ls)]
    if args.diff: pathlib.Path(args.diff).write_text(''.join(diffs))
    for f, l, t in report: print(f'::{"error" if args.strict else "warning"} file={f},line={l}::clang-format would change the spacing of this line: {t[:200]}')
    n = sum(len(v) for v in files.values())
    print(f'{n} changed lines in {len(files)} files since {base[:12]}; {len(report)} with spacing clang-format would change'
          + ('' if not report else f' (python3 dm/tools/lint_c.py format --diff FILE shows the changes; `git clang-format {base[:12]}` applies them)'))
    summary(f'### clang-format (changed lines, a report)\n\n{n} changed lines in {len(files)} files since `{base[:12]}`; {len(report)} with spacing clang-format would change.'
            + ('\n\n' + '\n'.join(f'- `{f}:{l}`: `{t[:160]}`' for f, l, t in report[:200]) if report else '')
            + ('\n\n<details><summary>clang-format\'s changes to those lines (with the line breaks, which are not reported)</summary>\n\n```diff\n' + ''.join(diffs)[:60000]
               + '\n```\n\n</details>' if diffs else ''))
    return 1 if report and args.strict else 0

def unified(old, new, f):
    return ''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True), f'a/{f} (as written)', f'b/{f} (clang-format)', n=0))

def main():
    ap = argparse.ArgumentParser(description='C and Objective-C lint of dm/host and dm/gfx (clang-tidy ratchet, clang-format on changed lines)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('compdb'); p.add_argument('--game', default=os.environ.get('GAME') or DEFAULT_GAME)
    p = sub.add_parser('tidy'); p.add_argument('--update', action='store_true'); p.add_argument('--report'); p.add_argument('-j', '--jobs', type=int, default=os.cpu_count())
    p.add_argument('files', nargs='*')
    p = sub.add_parser('format'); p.add_argument('--base'); p.add_argument('--strict', action='store_true'); p.add_argument('--diff')
    a = ap.parse_args()
    if a.cmd == 'compdb': compdb(a.game); return 0
    return tidy(a) if a.cmd == 'tidy' else fmt(a)

if __name__ == '__main__':
    sys.exit(main())
