#!/usr/bin/env python3
"""Audit a downloaded prewarm inbox, append compatible seeds, and optionally push to main."""

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import subprocess
import sys
from tempfile import NamedTemporaryFile, TemporaryDirectory

REPO = Path(__file__).resolve().parents[1]
GAMES = REPO / 'dm/games'
NVN = REPO / 'dm/gfx/nvn'
PARSER_SOURCE = REPO / 'dm/tools/prewarm_skey.c'
sys.path.insert(0, str(GAMES))
from game import Game

FILENAME = re.compile(r'([A-F0-9]{16})--([A-Za-z0-9][A-Za-z0-9 ._+()-]{0,63})--([a-f0-9]{64})\.tsv')
HASH = r'[a-f0-9]{16}'
LIBRARY = HASH + r'_b'
ROW = re.compile(rf'S\t{LIBRARY}\t[0-7]|C\t{LIBRARY}|G\t{HASH}\t{HASH}|(?:R\t|M\t(?:-|{LIBRARY})\t)[a-f0-9]+')
STAGES = {'vertex': '0', 'pixel': '1', 'geometry': '2', 'hull': '3', 'domain': '4',
          'compute': '5', 'task': '36', 'mesh': '47'}  # NVN2 task and mesh can use guest or host stage numbers.
HEADER = '# Prewarm seed list (shipped in the app, next to shader_map.tsv). See docs/development.md#prewarm-lists.\n'
IDENTITY = ('-c', 'user.name=seedds', '-c', 'user.email=seedds@gmail.com')


@dataclass
class Capture:
    path: Path
    title: str
    version: str
    rows: list[str]


@dataclass
class Update:
    path: Path
    original: bytes | None
    content: bytes
    subject: str
    message: str


def load_capture(path: Path) -> Capture:
    match = FILENAME.fullmatch(path.name)
    if not match:
        raise ValueError('expected an inbox filename with title, version and SHA-256')
    title, version, digest = match.groups()
    with path.open('rb') as source:
        data = source.read(32 * 1024 * 1024 + 1)
    if len(data) > 32 * 1024 * 1024:
        raise ValueError('exceeds the inbox size limit')
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError('SHA-256 mismatch')
    if not data.endswith(b'\n'):
        raise ValueError('expected LF-terminated rows')
    rows = data.decode('ascii')[:-1].split('\n')
    for number, row in enumerate(rows, 1):
        if not ROW.fullmatch(row):
            raise ValueError(f'line {number}: invalid inbox row')
    return Capture(path, title, version, rows)


def parse_pipelines(parser: Path, rows: list[str]) -> dict[str, list[str]]:
    # A mesh pipeline's task key is separate from the state the native parser normalizes.
    queries = {row: row[0] + '\t' + row.rsplit('\t', 1)[1] for row in rows if row[:2] in ('R\t', 'M\t')}
    if not queries:
        return {}
    output = subprocess.run([parser], input=''.join(query + '\n' for query in queries.values()),
                            text=True, stdout=subprocess.PIPE, check=True)
    return {row: parsed.split('\t') for row, parsed in zip(queries, output.stdout.splitlines(), strict=True)}


def row_key(row: str, parsed: dict[str, list[str]]) -> tuple[str, ...]:
    fields = row.split('\t')
    if row in parsed:
        return (*fields[:-1], parsed[row][-1]) if parsed[row] != ['-'] else ()
    if fields[0] == 'S':
        return tuple(fields[:2])  # The host loads an S row by key and gets its stage from the map.
    return tuple(fields)


def check_stages(row: str, parsed: dict[str, list[str]], shaders: dict[str, str]) -> str:
    kind, key, *rest = row.split('\t')
    if kind in ('R', 'M'):
        vertex, vs_stage, fragment, fs_stage, _ = parsed[row]
        refs = [(vertex, 'mesh' if kind == 'M' else 'vertex', vs_stage)]
        if fragment:
            refs.append((fragment, 'pixel', fs_stage))
        if kind == 'M' and key != '-':
            refs.append((key, 'task', ''))
    else:
        refs = [(key, 'compute' if kind == 'C' else '', rest[0] if kind == 'S' else '')]
    if any(key not in shaders for key, _, _ in refs):
        return 'missing shader'
    if any((stage and shaders[key] != stage) or (number and number not in STAGES.get(shaders[key], ''))
           for key, stage, number in refs):
        return 'stage mismatch'
    return ''


def optional_bytes(path: Path) -> bytes | None:
    try:
        return path.read_bytes()
    except FileNotFoundError:
        return None


def plan_profile(parser: Path, profile: Game, captures: list[Capture]) -> Update | None:
    shader_map = profile.table('shader_map.tsv')
    if not shader_map.is_file():
        for capture in captures:
            print(f'{capture.path.name}: skipped {len(capture.rows):,} rows (profile has no shader map)')
        return None
    shaders = {}
    for line in shader_map.read_text(encoding='utf-8').splitlines():
        columns = line.split('\t', 4)
        if not line.startswith(('#', 'package')) and len(columns) >= 4:
            shaders[columns[3] + '_b'] = columns[2]
    path = profile.table('pso_manifest.tsv')
    original = optional_bytes(path)
    seed = original.decode('utf-8') if original is not None else HEADER
    existing = [row for row in seed.splitlines() if ROW.fullmatch(row)]
    parsed = parse_pipelines(parser, existing)
    seen = {row_key(row, parsed) for row in existing}
    package = profile.builds().get(profile.get('package', ''), {}).get('display')
    additions = []
    kinds: Counter[str] = Counter()
    versions: dict[str, None] = {}
    for capture in captures:
        parsed = parse_pipelines(parser, capture.rows)
        counts: Counter[str] = Counter()
        for row in capture.rows:
            key = row_key(row, parsed)
            if not key:
                counts['invalid pipeline'] += 1
            elif key in seen:
                counts['duplicate'] += 1
            elif row.startswith('G\t'):
                counts['geometry needs shader build'] += 1
            elif reason := check_stages(row, parsed, shaders):
                counts[reason] += 1
            else:
                seen.add(key)
                additions.append(row + '\n')
                kinds[row[0]] += 1
                counts['added'] += 1
                versions[capture.version] = None
        note = f' (profile package {package})' if package and package != capture.version else ''
        summary = ', '.join(f'{count:,} {kind}' for kind, count in sorted(counts.items()))
        print(f'{capture.path.name}{note}: {summary}')
    if not additions:
        return None
    # The generated placeholder is obsolete once the manifest contains a recording.
    seed = ''.join(line for line in seed.splitlines(keepends=True)
                   if not line.startswith('# Provisional profile: empty until recorded on the Mac'))
    if seed and not seed.endswith('\n'):
        seed += '\n'
    content = seed + f'# Inbox captures from {", ".join(versions)}.\n' + ''.join(additions)
    name = profile.get('name', profile.id)
    breakdown = ', '.join(f'{n:,} {kind}' for kind, n in sorted(kinds.items()))
    print(f'{name} ({profile.id}): {len(additions):,} new rows ({breakdown})')
    subject = f'game: {name} adds prewarm seeds'
    if len(subject) > 72:
        subject = f'game/{profile.id}: add prewarm seeds'
    message = f'Import {len(additions):,} rows ({breakdown}) from inbox recordings of {", ".join(versions)}.'
    return Update(path, original, content.encode('utf-8'), subject, message)


def plan(folder: Path) -> list[Update]:
    captures: dict[str, list[Capture]] = defaultdict(list)
    files = sorted((p for p in folder.glob('*.tsv') if p.is_file()), key=lambda p: (p.stat().st_mtime_ns, p.name))
    if not files:
        raise ValueError(f'{folder}: no TSV files')
    for path in files:
        try:
            capture = load_capture(path)
        except (OSError, ValueError) as error:
            print(f'{path.name}: skipped file ({error})')
            continue
        if (GAMES / capture.title / 'game.tsv').is_file():
            captures[capture.title].append(capture)
        else:
            print(f'{path.name}: skipped {len(capture.rows):,} rows (no repository profile)')
    if not captures:
        return []
    with TemporaryDirectory(prefix='nxnative-prewarm-') as work:
        parser = Path(work) / 'prewarm_skey'
        subprocess.run(['clang', '-std=c11', '-I', NVN, '-I', NVN / 'include',
                        PARSER_SOURCE, '-o', parser], check=True)
        return [update for title, sources in captures.items()
                if (update := plan_profile(parser, Game(GAMES / title), sources)) is not None]


def apply(updates: list[Update]) -> None:
    for update in updates:
        if update.path.is_symlink() or optional_bytes(update.path) != update.original:
            raise ValueError(f'{update.path}: changed during the audit, run the import again')
    for update in updates:
        with NamedTemporaryFile(dir=update.path.parent, delete=False) as out:
            temporary = Path(out.name)
            try:
                out.write(update.content)
                out.close()
                temporary.chmod(update.path.stat().st_mode & 0o777 if update.original is not None else 0o644)
                temporary.replace(update.path)
            finally:
                temporary.unlink(missing_ok=True)


def git(*args: str, capture: bool = False) -> str:
    return subprocess.run(['git', *args], cwd=REPO, text=True, check=True,
                          stdout=subprocess.PIPE if capture else None).stdout or ''


def prepare_main() -> str:
    if git('status', '--porcelain', '--untracked-files=normal', capture=True):
        raise ValueError('--push needs a clean worktree')
    git('fetch', 'origin', 'main')
    if git('log', '--format=%h', 'origin/main..HEAD', capture=True):
        raise ValueError('--push would include local commits that are outside this import')
    git(*IDENTITY, 'rebase', '--no-autostash', 'origin/main')
    return git('rev-parse', 'HEAD', capture=True).strip()


def push(updates: list[Update], base: str) -> None:
    paths = []
    for update in updates:
        path = update.path.relative_to(REPO).as_posix()
        paths.append(path)
        if update.path.read_bytes() != update.content:
            raise ValueError(f'{path}: changed after the import')
        git('add', '--', path)
        git('diff', '--cached', '--check', '--', path)
        git(*IDENTITY, 'commit', '--only', '-m', update.subject, '-m', update.message, '--', path)
    git('fetch', 'origin', 'main')
    inputs = ['dm/gfx/nvn/nvn_pso_skey.h', 'dm/gfx/nvn/nvn_pso_ds.h', 'dm/gfx/nvn/nvn_obj.h',
              str(PARSER_SOURCE.relative_to(REPO))]
    for update in updates:
        directory = update.path.parent.relative_to(REPO)
        inputs.extend(str(directory / name) for name in ('game.tsv', 'shader_map.tsv', 'pso_manifest.tsv'))
    if git('diff', '--name-only', base, 'origin/main', '--', *inputs, capture=True):
        raise ValueError('origin/main changed import inputs during the run. The local commits need a fresh audit before pushing')
    git(*IDENTITY, 'rebase', '--no-autostash', 'origin/main')
    change = 'origin/main..HEAD'
    changed = git('diff', '--name-only', change, capture=True).splitlines()
    commits = git('rev-list', '--count', change, capture=True).strip()
    if (sorted(changed) != sorted(paths) or int(commits) != len(updates)
            or any(update.path.read_bytes() != update.content for update in updates)):
        raise ValueError('the commits to push differ from this import')
    git('diff', '--check', change)
    git('push', 'origin', 'HEAD:main')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path, help='folder containing downloaded inbox TSVs')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--apply', action='store_true', help='update local manifests without committing')
    modes.add_argument('--push', action='store_true', help='update, commit and push to GitHub main')
    args = parser.parse_args()
    folder = args.folder.resolve()
    if not folder.is_dir():
        parser.error(f'not a folder: {folder}')
    try:
        base = prepare_main() if args.push else None
        updates = plan(folder)
        if not updates:
            print('No manifest updates.')
            return 0
        if args.apply or base is not None:
            apply(updates)
            print(f'Updated {len(updates)} manifest(s).', flush=True)
        else:
            print(f'Preview: {len(updates)} manifest(s) would change.')
        if base is not None:
            push(updates, base)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'[RED] {parser.prog}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
