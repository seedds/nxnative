#!/usr/bin/env python3
"""Audit a downloaded prewarm inbox, import seeds, and clean up processed captures."""

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import NamedTemporaryFile, TemporaryDirectory

REPO = Path(__file__).resolve().parents[1]
GAMES = REPO / 'dm/games'
NVN = REPO / 'dm/gfx/nvn'
SHADER_TOOLS = NVN.parent / 'tools'
PARSER_SOURCE = REPO / 'dm/tools/prewarm_skey.c'
sys.path[:0] = [str(GAMES), str(SHADER_TOOLS), str(PARSER_SOURCE.parent)]
from dmsh_chain import table_pairs
from game import Game, extra_dirs
from nacp import bundle_version

FILENAME = re.compile(r'([A-F0-9]{16})--([A-Za-z0-9][A-Za-z0-9 ._+()-]{0,63})--([a-f0-9]{64})\.tsv')
HASH = r'[a-f0-9]{16}'
LIBRARY = HASH + r'_b'
ROW = re.compile(rf'S\t{LIBRARY}\t[0-7]|C\t{LIBRARY}|G\t{HASH}\t{HASH}|(?:R\t|M\t(?:-|{LIBRARY})\t)[a-f0-9]+')
STAGES = {'vertex': '0', 'pixel': '1', 'geometry': '2', 'hull': '3', 'domain': '4',
          'compute': '5', 'task': '36', 'mesh': '47'}  # NVN2 task and mesh can use guest or host stage numbers.
HEADER = '# Prewarm seed list (shipped in the app, next to shader_map.tsv). See docs/development.md#prewarm-lists.\n'
IDENTITY = ('-c', 'user.name=seedds', '-c', 'user.email=seedds@gmail.com')
GEOMETRY_TABLES = ('geo_pairs.tsv', 'geo_mesh.tsv')


@dataclass
class Capture:
    path: Path
    title: str
    version: str
    rows: list[str]
    file_stat: os.stat_result


@dataclass
class Update:
    path: Path
    original: bytes | None
    content: bytes
    subject: str
    message: str


@dataclass
class ImportPlan:
    updates: list[Update] = field(default_factory=list)
    captures_to_delete: list[tuple[Path, os.stat_result]] = field(default_factory=list)


def load_capture(path: Path) -> Capture:
    match = FILENAME.fullmatch(path.name)
    if not match:
        raise ValueError('expected an inbox filename with title, version and SHA-256')
    title, version, digest = match.groups()
    with path.open('rb') as source:
        file_stat = os.fstat(source.fileno())
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
    return Capture(path, title, version, rows, file_stat)


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
    elif kind == 'G':
        refs = [(key + '_b', 'vertex', ''), (rest[0] + '_b', 'geometry', '')]
    else:
        refs = [(key, 'compute', '')]
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


def read_shader_map(profile: Game) -> dict[str, str]:
    shaders = {}
    data = optional_bytes(profile.table('shader_map.tsv')) or b''
    for line in data.decode('utf-8').splitlines():
        columns = line.split('\t', 4)
        if not line.startswith(('#', 'package')) and len(columns) >= 4:
            shaders[columns[3] + '_b'] = columns[2]
    return shaders


def covered_versions(profile: Game, registered: dict[str, dict[str, str]]) -> set[str]:
    builds = profile.builds()
    covered = profile.words('shader_versions', profile.get('package', '')) + profile.words('shader_covers', '')
    versions = set()
    for version in covered:
        if version not in builds:
            continue
        build = builds[version]
        known = registered.get(version)
        if known is not None:
            # Older derived profiles may have no display label. Recover it from the same registered modules.
            if (build | {'display': known['display']}) != known:
                continue
            versions.update((known['display'], bundle_version(known['display'])))
        versions.update((version, build['display'], bundle_version(build['display'])))
    return versions


def validation_tables(profile: Game, versions: set[str]):
    repository_shaders = read_shader_map(profile)
    registered = profile.builds()
    tables: dict[str, tuple[dict[str, str], set[tuple[str, ...]]]] = {}
    sources = [profile]
    for root in extra_dirs():
        folder = root / profile.id
        if (folder / 'game.tsv').is_file() and folder.resolve() != profile.dir:
            sources.append(Game(folder))
    for source in sources:
        if source.get('title_id', '').upper() != profile.id:
            continue
        matches = (covered_versions(source, registered) & versions) - tables.keys()
        if not matches:
            continue
        shaders = repository_shaders if source is profile else read_shader_map(source)
        if shaders:
            pairs = {('G', *pair) for pair in table_pairs(source.table('geo_pairs.tsv'))}
            tables.update(dict.fromkeys(matches, (shaders, pairs)))
    return repository_shaders, tables


def plan_profile(parser: Path, profile: Game, captures: list[Capture], result: ImportPlan) -> None:
    repository_shaders, tables = validation_tables(profile, {capture.version for capture in captures})
    path = profile.table('pso_manifest.tsv')
    original = optional_bytes(path)
    seed = original.decode('utf-8') if original is not None else HEADER
    existing = [row for row in seed.splitlines() if ROW.fullmatch(row)]
    parsed = parse_pipelines(parser, existing)
    seen = {row_key(row, parsed) for row in existing}
    seen.update(('G', *pair) for pair in table_pairs(profile.table('geo_mesh.tsv')))
    additions = []
    kinds: Counter[str] = Counter()
    versions: dict[str, None] = {}
    for capture in captures:
        exact = tables.get(capture.version)
        shaders, pairs = exact if exact is not None else (repository_shaders, set())
        parsed = parse_pipelines(parser, capture.rows)
        counts: Counter[str] = Counter()
        for row in capture.rows:
            if row.startswith('S\t'):
                counts['ignored S'] += 1
                continue
            key = row_key(row, parsed)
            if not key:
                counts['invalid pipeline'] += 1
            elif key in seen or key in pairs:
                counts['duplicate'] += 1
            elif shaders and (reason := check_stages(row, parsed, shaders)):
                counts[reason] += 1
            else:
                seen.add(key)
                additions.append(row + '\n')
                kinds[row[0]] += 1
                counts['added'] += 1
                versions[capture.version] = None
        note = ''
        if exact is None:
            note = ' (no matching version map, using repository shader map)' if repository_shaders else ' (no shader map available, format-validated only)'
        summary = ', '.join(f'{count:,} {kind}' for kind, count in sorted(counts.items()))
        print(f'{capture.path.name}{note}: {summary}')
        if counts['added'] + counts['duplicate'] + counts['ignored S'] == len(capture.rows):
            result.captures_to_delete.append((capture.path, capture.file_stat))
    if not additions:
        return
    # The generated placeholder is obsolete once the manifest contains a recording.
    seed = ''.join(line for line in seed.splitlines(keepends=True)
                   if not line.startswith('# Provisional profile: empty until recorded on the Mac'))
    if seed and not seed.endswith('\n'):
        seed += '\n'
    content = seed + f'# Inbox captures from {", ".join(versions)}.\n' + ''.join(additions)
    name = profile.get('name', profile.id)
    breakdown = ', '.join(f'{n:,} {kind}' for kind, n in sorted(kinds.items()))
    print(f'{name} ({profile.id}): {len(additions):,} new rows ({breakdown})')
    for row in additions:
        if row.startswith('G\t'):
            _, vertex, geometry = row.rstrip('\n').split('\t')
            print(f'  Next shader build: mesh pair {vertex} {geometry}')
    subject = f'game: {name} adds prewarm seeds'
    if len(subject) > 72:
        subject = f'game/{profile.id}: add prewarm seeds'
    message = f'Import {len(additions):,} rows ({breakdown}) from inbox recordings of {", ".join(versions)}.'
    result.updates.append(Update(path, original, content.encode('utf-8'), subject, message))


def plan(folder: Path) -> ImportPlan:
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
    result = ImportPlan()
    if captures:
        with TemporaryDirectory(prefix='nxnative-prewarm-') as work:
            parser = Path(work) / 'prewarm_skey'
            subprocess.run(['clang', '-std=c11', '-I', NVN, '-I', NVN / 'include',
                            PARSER_SOURCE, '-o', parser], check=True)
            for title, sources in captures.items():
                plan_profile(parser, Game(GAMES / title), sources, result)
    return result


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


def delete_captures(captures: list[tuple[Path, os.stat_result]]) -> None:
    deleted = 0
    for path, original in captures:
        try:
            current = path.stat()
            if (not os.path.samestat(current, original) or current.st_size != original.st_size
                    or current.st_mtime_ns != original.st_mtime_ns):
                print(f'{path.name}: kept capture (file changed during the import)')
                continue
            path.unlink()
        except FileNotFoundError:
            continue
        deleted += 1
    if deleted:
        print(f'Deleted {deleted:,} inbox capture(s).')


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


def push(plan: ImportPlan, base: str) -> None:
    paths = []
    for update in plan.updates:
        path = update.path.relative_to(REPO).as_posix()
        paths.append(path)
        if update.path.read_bytes() != update.content:
            raise ValueError(f'{path}: changed after the import')
        git('add', '--', path)
        git('diff', '--cached', '--check', '--', path)
        git(*IDENTITY, 'commit', '--only', '-m', update.subject, '-m', update.message, '--', path)
    git('fetch', 'origin', 'main')
    inputs = ['dm/gfx/nvn/nvn_pso_skey.h', 'dm/gfx/nvn/nvn_pso_ds.h', 'dm/gfx/nvn/nvn_obj.h',
              str(PARSER_SOURCE.relative_to(REPO)), str((SHADER_TOOLS / 'dmsh_chain.py').relative_to(REPO))]
    titles = {update.path.parent.name for update in plan.updates}
    titles.update(path.name.split('--', 1)[0] for path, _ in plan.captures_to_delete)
    for title in sorted(titles):
        directory = GAMES.relative_to(REPO) / title
        inputs.extend(str(directory / name) for name in ('game.tsv', 'shader_map.tsv', 'pso_manifest.tsv', *GEOMETRY_TABLES))
    if git('diff', '--name-only', base, 'origin/main', '--', *inputs, capture=True):
        raise ValueError('origin/main changed import inputs during the run. The local commits need a fresh audit before pushing')
    git(*IDENTITY, 'rebase', '--no-autostash', 'origin/main')
    change = 'origin/main..HEAD'
    changed = git('diff', '--name-only', change, capture=True).splitlines()
    commits = git('rev-list', '--count', change, capture=True).strip()
    if (sorted(changed) != sorted(paths) or int(commits) != len(plan.updates)
            or any(update.path.read_bytes() != update.content for update in plan.updates)):
        raise ValueError('the commits to push differ from this import')
    git('diff', '--check', change)
    git('push', 'origin', 'HEAD:main')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path, help='folder containing downloaded inbox TSVs')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--apply', action='store_true', help='update local manifests and clean up the inbox')
    modes.add_argument('--push', action='store_true', help='update, commit and push to GitHub main, then clean up the inbox')
    args = parser.parse_args()
    folder = args.folder.resolve()
    if not folder.is_dir():
        parser.error(f'not a folder: {folder}')
    try:
        base = prepare_main() if args.push else None
        result = plan(folder)
        write = args.apply or args.push
        if not result.updates:
            print('No manifest updates.')
        elif write:
            apply(result.updates)
            print(f'Updated {len(result.updates)} manifest(s).', flush=True)
            if base is not None:
                push(result, base)
        else:
            print(f'Preview: {len(result.updates)} manifest(s) would change.')
        if write:
            delete_captures(result.captures_to_delete)
        elif result.captures_to_delete:
            print(f'Preview: {len(result.captures_to_delete):,} inbox capture(s) would be deleted.')
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'[RED] {parser.prog}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
