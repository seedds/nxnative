#!/usr/bin/env python3
"""Before a Mac game starts: rewrite its modules with the mods' code patches the host cannot make at load time, cached; prints the modules folder to start the host with (docs/development.md "Mods")."""
import argparse, hashlib, json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
CHECK_SECS, ROUNDS = 180, 4


def say(text): print(f'mods: {text}', file=sys.stderr, flush=True)
def red(text): print(f'[RED] mods: {text}', file=sys.stderr, flush=True)


def has_patch_files(folder):
    for root, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if d.lower() != 'romfs']
        if any(f.lower().endswith(('.pchtxt', '.ips')) for f in files): return True
    return False


def host_check(host, modules, env):
    """The host's verdict on every patch file of the mods that are on (DM_MODS_CHECK, hle_mods.c), in the order it applies them: the lowest priority first."""
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / 'check.tsv'
        run_env = {k: v for k, v in env.items() if k not in ('DM_WINDOW', 'DM_CTL', 'DM_CHEATS_WINDOW', 'DM_KEYS', 'DM_REC')} | {'DM_MODS_CHECK': str(out)}
        run = subprocess.run([str(host), str(modules)], env=run_env, cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=CHECK_SECS, text=True, errors='replace')
        if run.returncode or not out.is_file():
            raise RuntimeError(f'the host did not answer DM_MODS_CHECK (exit {run.returncode}): ' + ' | '.join(run.stdout.splitlines()[-3:]))
        files = []
        for line in out.read_text(encoding='utf-8', errors='replace').splitlines():
            c = line.split('\t')
            if c[0] == 'file' and len(c) >= 8:
                files.append({'module': c[1], 'build': c[2], 'status': c[3], 'mod': c[4], 'path': c[5], 'sha': c[6], 'why': c[7], 'recs': []})
            elif c[0] == 'rec' and len(c) == 3 and files:
                files[-1]['recs'].append((int(c[1], 16), bytes.fromhex(c[2])))
        return files


def overlap(a, b):
    return any(o < p + len(e) and p < o + len(d) for o, d in a['recs'] for p, e in b['recs'])


def bake_set(files):
    """The files the host refuses, and every file that shares bytes with one of them: applied by two paths, their order would be lost."""
    bake = [f for f in files if f['status'] == 'refused']
    grew = True
    while grew:
        grew = False
        for f in files:
            if f['status'] == 'applied' and f not in bake and any(overlap(f, g) for g in bake): bake.append(f); grew = True
    return [f for f in files if f in bake]


def registered_sites(modules, name):
    sites = []
    for ext, width in (('cuts', None), ('obs', 4)):
        try: lines = (Path(modules) / f'{name}.{ext}.tsv').read_text(encoding='utf-8', errors='replace').splitlines()
        except OSError: continue
        for line in lines:
            c = line.split('\t')
            if line.startswith('#') or len(c) < 4: continue
            sites.append((int(c[0], 16), width or len(c[1]) // 2, f'a cut ({c[3]})' if ext == 'cuts' else f'an observation gate ({c[3]})'))
    return sites


def tools_digest(modules):
    h = hashlib.sha256(Path(__file__).read_bytes())
    try: listed = [line.split(None, 1)[1].strip() for line in (Path(modules) / 'build_src.sha256').read_text().splitlines() if ' ' in line]
    except OSError: listed = ['patch_nso.py']
    for rel in listed:
        try: h.update(rel.encode() + (TOOLS / rel).read_bytes())
        except OSError: pass
    return h.hexdigest()


def rewrite(py, exefs, out, name, bake, env, game):
    """patch_nso.py on one original module with the files' patches. Returns the files the rewriter refused, by path, or None when it made the module."""
    patches = out / f'{name}.modpatches.tsv'
    patches.write_text(''.join(f'{o:x}\t{d.hex()}\t{f["path"]}\n' for f in bake for o, d in f['recs']), encoding='utf-8')
    log = out / f'{name}.rewrite.log'
    with open(log, 'w') as lf:
        code = subprocess.run([str(py), str(TOOLS / 'patch_nso.py'), str(Path(exefs) / name), str(out), name], env=env | {'DM_GAME': game, 'DM_MOD_PATCHES': str(patches)},
                              stdout=lf, stderr=subprocess.STDOUT).returncode
    if code == 0: return None
    refused = dict(line.split('\t')[1:3] for line in log.read_text(errors='replace').splitlines() if line.startswith('MOD_REFUSED\t') and line.count('\t') >= 2)
    if code != 4 or not refused:
        raise RuntimeError(f'the rewriter failed on {name} with the mods\' patches (exit {code}): ' + ' | '.join(log.read_text(errors='replace').splitlines()[-3:]))
    return refused


def clear_module(out, name):
    for p in out.glob(f'{name}.*'):
        if p.name != f'{name}.rewrite.log': p.unlink()


def prune(cache, keep, final):
    done = sorted((d for d in cache.iterdir() if d.is_dir() and d != final), key=lambda d: d.stat().st_mtime, reverse=True)
    for d in done[max(0, keep - 1):]: shutil.rmtree(d, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game', required=True, help='the game profile (title ID), as DM_GAME')
    ap.add_argument('--host', required=True, help='dmhost, built for the game')
    ap.add_argument('--modules', required=True, help='the rewritten modules the game starts with (patched_v<version>)')
    ap.add_argument('--exefs', required=True, help='the original modules (extract/v<version>/ExeFS)')
    ap.add_argument('--cache', required=True, help='where rewritten modules are kept between starts')
    ap.add_argument('--python', default=sys.executable, help='the Python that runs the rewriter (it needs capstone 5)')
    ap.add_argument('--keep', type=int, default=2, help='cached sets to keep')
    a = ap.parse_args()
    modules, cache, env = Path(a.modules).resolve(), Path(a.cache).resolve(), dict(os.environ)
    a.host, a.exefs = Path(a.host).resolve(), Path(a.exefs).resolve()
    mods = env.get('DM_MODS') or (os.path.join(env['DM_ROOT'], 'mods') if env.get('DM_ROOT') else '')
    plain = lambda: print(modules)
    if not mods or not os.path.isdir(mods) or not has_patch_files(mods): return plain()
    try: files = host_check(a.host, modules, env)
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        red(f'{e}: the game starts without a rewrite'); return plain()
    names = sorted({f['module'] for f in files})
    bake = {n: bake_set([f for f in files if f['module'] == n]) for n in names}
    refused = {n: {} for n in names}
    for n in names:
        sites = registered_sites(modules, n)
        for f in list(bake[n]):
            hit = next((what for off, size, what in sites for o, d in f['recs'] if o < off + size and off < o + len(d)), None)
            if hit: refused[n][f['path']] = f'it patches a place in {n} where the rewriter has {hit}'; bake[n].remove(f)
    if not any(bake.values()) and not any(refused.values()): return plain()
    key = hashlib.sha256(json.dumps([tools_digest(modules), a.game, [(n, [(f['build'], f['path'], f['sha'], [(o, d.hex()) for o, d in f['recs']]) for f in bake[n]], sorted(refused[n])) for n in names]]).encode()).hexdigest()[:16]
    final = cache / key
    if (final / 'ok').is_file():
        os.utime(final); say(f'the modules rewritten with the mods\' patches are reused ({final})'); print(final); return
    by_path = {f['path']: f for f in files}
    missing = [n for n in names if bake[n] and not (Path(a.exefs) / n).is_file()]
    if missing:
        red(f'no original {", ".join(missing)} in {a.exefs}: the mods\' patches that need a rewrite are not applied (a packed app has no original modules)'); return plain()
    out = cache / (key + '.partial'); shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
    try:
        for _ in range(ROUNDS):
            for n in names:
                clear_module(out, n)
                while bake[n]:
                    say(f'rewriting {n} with {len(bake[n])} patch file(s) of the mods (main takes about half a minute; later starts reuse it)')
                    t = time.time(); gone = rewrite(a.python, a.exefs, out, n, bake[n], env, a.game)
                    if gone is None: say(f'{n} rewritten in {time.time() - t:.0f} s'); break
                    refused[n].update(gone); bake[n] = [f for f in bake[n] if f['path'] not in gone]
                    clear_module(out, n)
                if bake[n] or refused[n]:
                    (out / f'{n}.mods.tsv').write_text(''.join(f'baked\t{f["sha"]}\t{f["path"]}\n' for f in bake[n])
                                                      + ''.join(f'refused\t{by_path[p]["sha"]}\t{p}\t{why}\n' for p, why in sorted(refused[n].items())), encoding='utf-8')
            for src in modules.iterdir():
                dst = out / src.name
                if dst.is_symlink(): dst.unlink()
                if not dst.exists(): dst.symlink_to(src)
            late = {(f['module'], f['path']) for f in host_check(a.host, out, env) if f['status'] == 'refused'}
            if not late: break
            for f in files:
                if (f['module'], f['path']) in late: f['status'] = 'refused'
            for n in names: bake[n] = [f for f in bake_set([f for f in files if f['module'] == n]) if f['path'] not in refused[n]]
        else: raise RuntimeError('the patch files the host refuses keep changing after each rewrite')
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        red(f'{e}: the game starts without the rewrite (its log, if any, is in {out})'); return plain()
    for n in names:
        for p, why in sorted(refused[n].items()): red(f'{by_path[p]["mod"]}: {p} is not applied: {why}')
    (out / 'ok').write_text(f'{time.strftime("%Y-%m-%d %H:%M:%S")}\n')
    shutil.rmtree(final, ignore_errors=True); out.rename(final); prune(cache, a.keep, final)
    print(final)


if __name__ == '__main__': main()
