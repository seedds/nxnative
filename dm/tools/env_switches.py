#!/usr/bin/env python3
"""The environment switches the code reads, as one table: docs/env_switches.tsv (name, class, meaning; class and meaning kept by hand, docs/development.md says what
the classes are).
  env_switches.py           write the table: a new switch gets a row with a guessed class and no meaning, a switch nothing reads any more loses its row
  env_switches.py --check   exit 1 when the table does not list exactly the switches the sources read, a row has no meaning or an unknown class, the docs quote
                            a DM_* name that nothing reads and RETIRED does not list, a RETIRED name is read again, or a doc line quotes a RETIRED name without
                            saying it is gone (one of RETIRED_WORDS)
Which names are switches:
  DM_*    every DM_* name the code quotes (a helper such as envdir("DM_TEMP", ...) reads it for its caller), except the names it only defines as a macro or an enum
          constant; in a test, only the names it reads itself (READ)
  others  every other name the code reads in one of the forms of OTHER_READ (getenv, plat_getenv, SDL_getenv and std::getenv, the launcher's env.get, os.environ,
          os.getenv, .NET's GetEnvironmentVariable, Java's System.getenv, and $NAME / ${NAME / %NAME% in the run scripts that scripts/*.py write), unless
          EXTERNAL or EXTERNAL_PREFIXES lists it as another program's or the OS's. The shell scripts' own inputs (${NAME:-default} in dm/**/*.sh) other than
          DM_* are not covered: a pattern cannot tell them from the scripts' local variables."""
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
TABLE = REPO / 'docs/env_switches.tsv'
SELF = 'dm/tools/env_switches.py'
HEAD = 'name\tclass\tmeaning\n'
CLASSES = ('player', 'path', 'switch', 'diagnostic', 'test')
CODE = ('.c', '.m', '.h', '.inc', '.swift', '.py', '.sh', '.S', '.cpp', '.hpp', '.cc', '.cs', '.java')
C_FAMILY = ('.c', '.m', '.h', '.inc', '.cpp', '.hpp', '.cc')
NAME = r'DM_[A-Z0-9_]*[A-Z0-9]'
READ = r'(?:getenv\(|environ(?:\.get\(|\[))\s*["\'](' + NAME + r')["\']'   # a literal name in getenv(…), os.environ.get(…) or os.environ[…]
UP = r'([A-Z][A-Z0-9_]*)'
OTHER_READ = (r'getenv\(\s*L?"' + UP + '"',                         # C, C++, Objective-C, Java; also the C sources a test's .py writes
              r'GetEnvironmentVariable[AW]?\(\s*L?"' + UP + '"',     # .NET, Win32
              r'\benv_set\(\s*"' + UP + '"',                         # the Vulkan tests' helper
              r'environ(?:\.get\(|\[)\s*["\']' + UP + '["\']',       # Python
              r'os\.getenv\(\s*["\']' + UP + '["\']')
LAUNCHER_READ = r'\benv\.get\(\s*"' + UP + '"'                       # dm/launcher's Env (the process environment, or a test's map)
# the run scripts scripts/*.py write (run.sh and run.cmd: build_ipa_from_nsp.py; pack.sh and pack.cmd: package_port.py): $NAME or ${NAME, and %NAME% of two
# characters or more (strftime's %Y%m is not one)
RUN_SH_READ, RUN_CMD_READ = r'\$\{?' + UP + r'\b', r'%([A-Z][A-Z0-9_]+)%'
# Environment variables of the OS or of other programs that the code reads: not the project's switches, so no row
EXTERNAL = {
    'HOME': 'the user\'s home folder (POSIX)', 'USERPROFILE': 'the user\'s home folder (Windows)', 'APPDATA': 'the user\'s roaming application data (Windows)',
    'TMPDIR': 'the temporary folder (POSIX)', 'TEMP': 'the temporary folder (Windows)', 'PATH': 'the program search path', 'USER': 'the login name (xcodegen needs it)',
    'ORIGIN': '$ORIGIN in a run path is the dynamic loader\'s token for the program\'s folder, not a variable',
}
EXTERNAL_PREFIXES = {'XDG_': 'the XDG base directories (Linux)', 'VK_': 'the Vulkan loader\'s', 'SDL_': 'SDL\'s', 'MTL_': 'Metal\'s', 'GITHUB_': 'GitHub Actions\'',
                     'DOTNET_': '.NET\'s'}
# Names the docs may still quote that nothing reads any more (or ever read): what became of each. Each doc line that quotes one must say it is gone.
RETIRED = {
    'DM_FRAME_STEP': 'removed in 79d82f7 (its `legacy` mode ran the frame-interval method alone; the host\'s frame step in whole vsyncs replaced it)',
    'DM_NVN_ALPHA_RED': 'removed in c76f679 with the alpha_from_red profile key (the nvnTextureCompare mapping fix replaced them)',
    'DM_REFL_DIR': 'removed in ba3b91e (Vulkan port step M6) with t_refl_cmp, which compared the two reflection parsers',
    'DM_UPSCALE': 'never read by any code: a game note\'s record of a 2x run (010045C014650000) that names no real switch',
}
RETIRED_WORDS = r'removed|retired|replaced|renamed|gone|never read|no longer'


def tracked():
    out = subprocess.run(['git', 'ls-files', 'dm', 'scripts', 'docs', 'README.md'], cwd=REPO, capture_output=True, text=True, check=True).stdout.split('\n')
    return [p for p in out if p and 'third_party' not in p.split('/')]


def external(name):
    return name in EXTERNAL or any(name.startswith(p) for p in EXTERNAL_PREFIXES)


def file_reads(p, text):
    """What one code file (p: its path from the repository root) holds: (the switches it reads, the DM_* names it reads in a READ form, the DM_* names it
    defines as a macro or an enum constant)"""
    # names the code defines as preprocessor macros or enum constants (the generated profile header's DM_GAME_*, host constants, the JIT's DM_JIT_GATE) are
    # not switches, unless something also reads them from the environment (DM_GAME_ENV, which overrides the profile's macro of that name)
    macros = set(re.findall(r'#\s*define\s+(' + NAME + r')', text))
    # an enum entry (the comma or brace after it a lookahead: consumed, it hid every second entry of a one-line enum from the next match)
    if p.endswith(C_FAMILY): macros |= set(re.findall(r'(?:^|[{,])\s*(' + NAME + r')\s*=\s*-?(?:0x)?[0-9A-Fa-f]+(?=\s*[,}])', text, re.M))
    if '/tests/' in p:   # a test's own switches only; the names in its fixtures are data
        found = re.findall(READ, text)
    elif p.endswith('.sh'): found = re.findall(r'\$\{?(' + NAME + r')\b', text) + re.findall(r'["\'](' + NAME + r')["\']', text)
    else: found = re.findall(r'["\'](' + NAME + r')=?["\']', text)   # any quoted name: helpers such as envdir("DM_TEMP", …) read it for their caller
    other = [n for r in OTHER_READ for n in re.findall(r, text)]
    if p.startswith('dm/launcher/'): other += re.findall(LAUNCHER_READ, text)
    if p.startswith('scripts/') and p.endswith('.py'):   # in the run scripts it writes, a %NAME% that the script sets itself (run.cmd's set "NXRUN_…=") is its own
        own = set(re.findall(r'set "' + UP + '=', text))
        other += re.findall(RUN_SH_READ, text) + [n for n in re.findall(RUN_CMD_READ, text) if n not in own]
    found = set(found) | {n for n in other if not n.startswith('DM_') and not external(n)}
    return found, set(re.findall(READ, text)), macros


def scan():
    """({switch: files that read it}, {DM_* name quoted in the docs: docs})"""
    reads, docs, macros, direct = {}, {}, set(), set()
    for p in tracked():
        path = REPO / p
        if not path.is_file() or p == SELF: continue   # this file quotes RETIRED's names
        text = path.read_text(errors='replace')
        if p.endswith('.md'):
            for n in set(re.findall(r'`(' + NAME + r')', text)): docs.setdefault(n, set()).add(p)
            continue
        if not p.endswith(CODE) and not p.startswith('scripts/'): continue
        found, d, m = file_reads(p, text)
        direct |= d; macros |= m
        for n in found: reads.setdefault(n, set()).add(p)
    for n in macros - direct: reads.pop(n, None); docs.pop(n, None)
    return reads, docs


def guess(name, files, player):   # a new row's class, for whoever writes its meaning to correct
    if name in player: return 'player'
    if all('/tests/' in f for f in files): return 'test'
    if re.search(r'_(DIR|PATH|FILE|ROOT)$', name): return 'path'
    return 'diagnostic'


def retired_problems(reads):
    """RETIRED names that something reads again, and doc lines that quote a RETIRED name without saying it is gone (file:line)"""
    out = [f'{n} is in RETIRED but {", ".join(sorted(reads[n]))} reads it: drop it from RETIRED and give it a row' for n in sorted(RETIRED) if n in reads]
    for p in tracked():
        if not p.endswith('.md') or not (REPO / p).is_file(): continue
        for i, line in enumerate((REPO / p).read_text(errors='replace').split('\n'), 1):
            for n in sorted(set(re.findall(r'`(' + NAME + r')', line)) & set(RETIRED)):
                if not re.search(RETIRED_WORDS, line): out.append(f'{p}:{i} quotes {n}, which is {RETIRED[n].split(" (")[0]}, without saying so ({RETIRED_WORDS})')
    return out


def rows():
    """(table text, rows without a meaning or with an unknown class, names the docs quote that nothing reads and RETIRED does not list, RETIRED problems)"""
    reads, docs = scan()
    player = {l.split('\t')[0] for l in (REPO / 'dm/games/settings.tsv').read_text().split('\n') if l.startswith('DM_')}
    kept = {}
    if TABLE.exists():
        for l in TABLE.read_text().split('\n')[1:]:
            f = l.split('\t')
            if len(f) == 3: kept[f[0]] = f[1:]
    out, bad = [], []
    for n in sorted(reads):
        cls, meaning = kept.get(n, [guess(n, reads[n], player), ''])
        if cls not in CLASSES or not meaning.strip(): bad.append(n)
        out.append(f'{n}\t{cls}\t{meaning}\n')
    unread = sorted(n for n in docs if n not in reads and n not in RETIRED and not n.endswith('_'))
    return HEAD + ''.join(out), bad, unread, retired_problems(reads)


if __name__ == '__main__':
    text, bad, unread, retired = rows()
    if sys.argv[1:] == ['--check']:
        problems = []
        if not TABLE.exists() or TABLE.read_text() != text: problems.append('docs/env_switches.tsv does not list the switches the code reads: run dm/tools/env_switches.py, then write the new rows\' meanings')
        if bad: problems.append('switches without a meaning or a class (one of ' + ', '.join(CLASSES) + '): ' + ' '.join(bad))
        if unread: problems.append('quoted in the docs but read nowhere (a removed or renamed switch: fix the mention, then list it in RETIRED with what became of it): ' + ' '.join(unread))
        problems += retired
        if problems: print('\n'.join(problems))
        sys.exit(1 if problems else 0)
    if unread: print('quoted in the docs but read nowhere and not in RETIRED: ' + ' '.join(unread))
    for r in retired: print(r)
    TABLE.write_text(text)
    print(f'{TABLE.relative_to(REPO)}: {text.count(chr(10)) - 1} switches, {len(bad)} without a meaning')
