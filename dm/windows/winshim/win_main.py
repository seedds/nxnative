# The real build script, with its steps that need Apple's tools answered by a Mac: the Mac host (with its font), the optional pipeline archive, and the iPhone executable
# A step that needs the Mac leaves a marker in the work folder (NEED_HOST, NEED_IOS); the Mac's queue (queue.py) answers it. Nothing here starts anything on the Mac
import hashlib, importlib.util, json, os, shutil, sys, tarfile, time, zipfile
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); KIT = os.environ.get('DM_WIN_KIT') or os.path.join(HERE, 'kit')
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', os.path.join(HERE, 'repo', 'scripts', 'build_ipa_from_nsp.py')); m = importlib.util.module_from_spec(spec); sys.modules[spec.name] = m; spec.loader.exec_module(m)
def mark(work, name, text):
    with open(os.path.join(work, name + '.part'), 'w', encoding='utf-8') as f: f.write(text)
    os.replace(os.path.join(work, name + '.part'), os.path.join(work, name))
def wait_for(path, what, minutes):
    for _ in range(minutes * 6):
        if os.path.exists(path): return
        if os.path.exists(os.path.join(os.path.dirname(path), 'FAILED')): raise m.BuildError(f'{what}: the Mac could not make it')
        time.sleep(10)
    raise m.BuildError(f'{what}: nothing from the Mac after {minutes} minutes: {path}')
def mac_host(py, repo, work, env, logs, g):
    kit = json.load(open(os.path.join(KIT, 'kit.json')))
    if kit['game'] != g.id: raise m.BuildError(f'the kit is for {kit["game"]}, not {g.id}')
    m.PROG.stage('Mac host', step='from the kit')
    if not os.path.isfile(os.path.join(KIT, 'dmhost')):
        mark(work, 'NEED_HOST', str(g.dir)); wait_for(os.path.join(KIT, 'dmhost'), 'the Mac host', 45)
    host = work / 'tools/dmhost'; host.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(os.path.join(KIT, 'dmhost'), host)
    font = work / 'font'; font.mkdir(parents=True, exist_ok=True); shutil.copyfile(os.path.join(KIT, 'font', 'nintendo_ext.ttf'), font / 'nintendo_ext.ttf')
    m.PROG.end(str(host)); return host, font
m.mac_host = mac_host
m.mac_pso = lambda *a, **k: None   # an optimization: the app builds its pipeline bases on its first start
m.check_ios_sdk = lambda env: None
def capture_jit(*a, **k): raise m.BuildError('this game makes code at run time (nn::jit): its iPhone app needs captures from booting it on a Mac')
m.capture_jit = capture_jit
m.tree_tar.pack = m.tree_tar.unpack = lambda *a, **k: None   # the Mac keeps Xcode's data

COPY = ('.patched.bin', '.fixups.tsv', '.gates.tsv', '.cuts.tsv', '.obs.tsv', 'nro.tsv')
def ios_app(out_ipa, env):
    """Stands in for build_ipa.sh: what Xcode needs goes in ipa_ready for the Mac, its app comes back in ipa_app, and pack_ipa.py packs the IPA here with the same mappings"""
    w = str(m_work[0]); ready = os.path.join(w, 'ipa_ready'); back = os.path.join(w, 'ipa_app')
    shutil.rmtree(ready, ignore_errors=True); shutil.rmtree(back, ignore_errors=True); os.makedirs(ready); os.makedirs(back)
    sys.path.insert(0, os.path.join(HERE, 'repo', 'dm', 'games')); sys.path.insert(0, os.path.join(HERE, 'repo', 'dm', 'tools'))
    import game as gamemod, shader_lib
    gid, patched, romfs, msl, gfx = env['GAME'], env['PATCHED'], env['ROMFS'], env['MSL_IOS'], env['GFXAPI']; g = gamemod.load(gid)
    pd = os.path.join(ready, 'patched'); os.makedirs(pd)
    for n in os.listdir(patched):
        if n.endswith(COPY) and os.path.isfile(os.path.join(patched, n)): shutil.copyfile(os.path.join(patched, n), os.path.join(pd, n))
    shutil.copytree(str(g.dir), os.path.join(ready, 'profile', gid))
    meta = {k: v for k, v in env.items() if (k.startswith('DM_') and not k.startswith(('DM_WIN', 'DM_GAMES', 'DM_XCODE', 'DM_ROOT'))) or k in ('GAME', 'GFXAPI')}
    meta['marker'] = g.get('romfs_marker'); side = os.path.dirname(romfs)
    for key, name in (('DM_ICON', 'icon.jpg'), ('DM_ENV_FILE', 'env.txt'), ('CONTROL', 'control.nacp')):
        p = env.get(key) or (os.path.join(side, 'control.nacp') if key == 'CONTROL' else None)
        if p and os.path.isfile(p): shutil.copyfile(p, os.path.join(ready, name)); meta[key] = name
        else: meta.pop(key, None)
    v = open(os.path.join(HERE, 'repo', 'dm', 'gfx', 'nvn', 'dmsh_chain.h')).read().split('DMSH_CHAIN "')[1][:12]; msldir = {'vulkan': 'vk', 'gl': 'gl'}.get(gfx, 'b-' + v); meta['msldir'] = msldir
    lib = os.path.join(msl, msldir)
    if gfx == 'nvn':
        with open(os.path.join(ready, 'inputs.tsv'), 'wb') as f: f.write((shader_lib.view(lib) / 'inputs.tsv').read_bytes())
    with open(os.path.join(ready, 'meta.json'), 'w', encoding='utf-8') as f: json.dump(meta, f)
    return lib, ready, back, w, side
def ios_finish(out_ipa, env, lib, back, side):
    made = json.load(open(os.path.join(back, 'made.json'), encoding='utf-8')); romfs = env['ROMFS']
    with tarfile.open(os.path.join(back, 'app.tar')) as t: t.extractall(back, filter='fully_trusted')
    app = os.path.join(back, 'DmIOS.app'); P = 'Payload/DmIOS.app'
    maps = [f'{P}/romfs.img={romfs}' if os.path.isfile(romfs) else f'{P}/romfs={romfs}']
    aoc = env.get('AOC')
    if aoc and os.path.isdir(aoc): maps.append(f'{P}/aoc={aoc}')
    maps.append(f'{P}/msl/{os.path.basename(lib)}={lib}')   # a folder or its image: pack_ipa.py reads either (shader_lib.py), as for build_ipa.sh
    maps.append(f'{P}={app}')
    for n in ('html', 'jit'):
        if os.path.isdir(os.path.join(side, n)): maps.append(f'{P}/{n}={os.path.join(side, n)}')
    skip = [x for n in ('_CodeSignature', 'embedded.mobileprovision', 'msl', 'aoc', 'romfs', 'romfs.img', 'romfs.id', 'html', 'jit') for x in ('--skip', f'{P}/{n}')]
    opts = ['--store', made['zip_store'], '--no-reuse'] + ([] if made['pull'] == 1 else ['--skip-suffix', '_bp.metallib'])
    m.subprocess.run([sys.executable, os.path.join(HERE, 'repo', 'dm', 'ios', 'tools', 'pack_ipa.py'), str(out_ipa), *opts, *skip, *maps], check=True, env=env)
    # Windows files have no Unix modes: the zip's directory gets them here, from the list the Mac's app came with
    ex = {f'{P}/{n}' for n in made['executable']}
    with zipfile.ZipFile(out_ipa, 'a', allowZip64=True) as z:
        for i in z.infolist():
            i.create_system = 3; d = i.filename.endswith('/')
            i.external_attr = ((0o040755 if d else 0o100755 if i.filename in ex else 0o100644) << 16) | (0x10 if d else 0)
        z._didModify = True
        missing = ex - {i.filename for i in z.infolist()}
    if missing: raise m.BuildError(f'not in the IPA: {sorted(missing)[:3]}')
    h = hashlib.sha256()
    with open(out_ipa, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    with open(str(out_ipa) + '.sha256', 'w') as f: f.write(h.hexdigest())
    print(f'iPhone app: {out_ipa}, {os.path.getsize(out_ipa)} bytes', flush=True)
def ios_step(out_ipa, env):
    lib, ready, back, w, side = ios_app(out_ipa, env)
    mark(w, 'NEED_IOS', env['GAME']); wait_for(os.path.join(back, 'DONE'), 'the iPhone executable', 60)
    ios_finish(out_ipa, env, lib, back, side)
m_work = [None]
_run = m.run
def run(cmd, log, *a, **k):
    if len(cmd) > 1 and str(cmd[0]) == 'bash' and str(cmd[1]).replace('\\', '/').endswith('build_ipa.sh'): return ios_step(cmd[2], k.get('env') or os.environ)
    return _run(cmd, log, *a, **k)
m.run = run
a = sys.argv[1:]; m_work[0] = a[a.index('--work-dir') + 1]
sys.exit(m.cli(a))
