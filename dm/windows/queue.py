#!/usr/bin/env python3
# queue.py <queue.tsv> [--only <short names, commas>]: the Mac's side of a build queue on the PC. For each row (short name, title ID, app name, file in the shared Switch folder): the game's kit, its files on the PC,
# the build (Mac app, then IPA), answers to what the build asks of a Mac (the host, the iPhone executable), the results here, and a short play test of the Mac app. It goes on after a game fails, and a game
# with a finished row in results.tsv is not built again. Results: <queue folder>/<short name>/, results.tsv, ipas/.
import hashlib, json, os, re, shlex, shutil, subprocess, sys, threading, time, zipfile
T = '/Volumes/2TB/Switch_NSP/windows-test'; Q = f'{T}/queue'; TOOLS = f'{T}/windows-tools'; BASE = f'{T}/kits/_base-d866'
REPO = '/Users/debdipghosh/nxnative/.claude/worktrees/macport-ipa-switch-nsp-fd89d3'; H = 'nxbuild@192.168.1.180'
O = ['-i', os.path.expanduser('~/.ssh/nxnative_laptop'), '-o', 'IdentitiesOnly=yes', '-o', 'UserKnownHostsFile=' + os.path.expanduser('~/.ssh/nxnative_laptop_known_hosts'), '-o', 'ConnectTimeout=20', '-o', 'BatchMode=yes', '-o', 'ServerAliveInterval=30', '-o', 'ServerAliveCountMax=20']
def say(*a): print(time.strftime('%H:%M'), *a, flush=True)
def ssh(cmd, check=False):
    r = subprocess.run(['ssh', *O, H, cmd], capture_output=True, text=True, errors='replace')
    if check and r.returncode: raise RuntimeError(f'ssh {cmd[:80]}: {r.stderr[-300:]}')
    return r.stdout.replace('\r', '')
def builders():   # how many of the build's programs run on the PC: a dropped connection does not always end a build, and two at once would fight over its folders
    m = re.findall(r'\d+', ssh('powershell -NoProfile -Command "(Get-Process python,metal,metallib,dotnet -ErrorAction SilentlyContinue | Measure-Object).Count"'))
    return int(m[-1]) if m else -1
def wait_idle(say_):
    for i in range(240):
        if builders() == 0: return True
        if i == 0: say_('a build is still running on the PC: waiting for it')
        time.sleep(30)
    return False
def scp(*a): subprocess.run(['scp', '-q', *O, *a], check=True, capture_output=True)
def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
def serve(n, gid, R, K, stop, log):
    """Answers the markers the build leaves (winshim/win_main.py). Each is deleted first, so it is answered once"""
    w = f'C:\\dmb\\w-{n}'; u = f'C:/dmb/w-{n}'
    while not stop.is_set():
        s = ssh(f'(if exist {w}\\NEED_HOST echo HOST)& (if exist {w}\\NEED_IOS echo IOS)')
        if 'HOST' in s:
            try:
                d = ssh(f'type {w}\\NEED_HOST & del {w}\\NEED_HOST').strip().replace('\\', '/'); P = f'{K}/profile'; shutil.rmtree(P, ignore_errors=True); os.makedirs(P)
                scp('-r', f'{H}:{d}', f'{P}/{gid}'); t = time.time()
                r = subprocess.run([sys.executable, f'{TOOLS}/kit_host.py', P, gid, K], env=dict(os.environ, DM_REPO=REPO), capture_output=True, text=True, errors='replace', cwd=REPO); open(f'{K}/host_build.log', 'w').write(r.stdout + r.stderr)
                if r.returncode: raise RuntimeError(r.stderr[-400:])
                scp(f'{K}/dmhost', f'{H}:C:/dmb/kits/{n}/dmhost.part'); ssh(f'move /y C:\\dmb\\kits\\{n}\\dmhost.part C:\\dmb\\kits\\{n}\\dmhost', True); log(f'host compiled and sent in {int(time.time() - t)} s')
            except Exception as e: log(f'[RED] host failed: {e}'); ssh(f'type nul > C:\\dmb\\kits\\{n}\\FAILED')
        if 'IOS' in s:
            try:
                ssh(f'del {w}\\NEED_IOS'); D = f'{R}/ios'; shutil.rmtree(D, ignore_errors=True); os.makedirs(D); scp('-r', f'{H}:{u}/ipa_ready', f'{D}/ready'); t = time.time()
                r = subprocess.run([sys.executable, f'{TOOLS}/ios_exe.py', REPO, f'{D}/ready', f'{D}/out'], capture_output=True, text=True, errors='replace', cwd=REPO)
                if r.returncode: raise RuntimeError((r.stdout + r.stderr)[-600:])
                scp(f'{D}/out/app.tar', f'{D}/out/made.json', f'{H}:{u}/ipa_app/'); ssh(f'type nul > {w}\\ipa_app\\DONE', True); log(f'iPhone executable compiled and sent in {int(time.time() - t)} s')
                for x in ('ready', 'out/app.tar', 'out/stub', 'out/w'):
                    p = f'{D}/{x}'; shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else (os.path.exists(p) and os.unlink(p))
            except Exception as e: log(f'[RED] iPhone executable failed: {e}'); ssh(f'(if not exist {w}\\ipa_app mkdir {w}\\ipa_app)& type nul > {w}\\ipa_app\\FAILED')
        stop.wait(10)
def play(R, gid, name):
    """A short driven run: pictures every 30 s, buttons from 40 s; the game's save is put back by drive.sh"""
    P = f'{R}/play'; A = f'{R}/app/{name}.app'
    env = dict(os.environ, DRIVE_ID=gid, DM_PSO_ARCHIVE=f'{R}/pso', DM_PSO_USER=f'{R}/user.tsv'); os.makedirs(f'{R}/pso', exist_ok=True)
    d = subprocess.Popen([f'{T}/handoff-scripts/drive.sh', P, A, '230'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(8)
    keys = ['A', 'Plus', 'A', 'LUp~0.3']; t = 0
    while t < 210 and d.poll() is None:
        time.sleep(10); t += 10
        with open(f'{P}/ctl', 'a') as f:
            if t % 30 == 0: f.write(f'shot {P}/shots/t{t:03d}.png\n')
            if t >= 40: f.write(keys[(t // 10) % 4] + '\n')
    open(f'{P}/stop', 'w').close(); d.wait()
    txt = open(f'{P}/log.txt', errors='replace').read() if os.path.exists(f'{P}/log.txt') else ''
    fr = re.findall(r'up to frame (\d+)', txt); reds = {}
    for l in txt.split('\n'):
        if '[RED]' in l: k = re.sub(r'0x[0-9a-f]+|\b[0-9a-f]{8,}\b', '#', l.strip())[:100]; reds[k] = reds.get(k, 0) + 1
    shots = [f for f in os.listdir(f'{P}/shots') if f.endswith('.png')] if os.path.isdir(f'{P}/shots') else []
    return dict(alive=open(f'{P}/result.txt').readline().strip() if os.path.exists(f'{P}/result.txt') else 'no result', frames=int(fr[-1]) if fr else 0, gpu_errors=len(re.findall(r'command buffer.*error', txt)), crashes=txt.count('CRASH'), missing=len(re.findall('not in the shader map', txt)), shots=len(shots), reds=sorted(reds.items(), key=lambda x: -x[1])[:4])
def one(n, gid, name, src, mode=''):   # mode 'ipa': only the IPA (the game's Mac app already exists), no Mac app and no play test
    R = f'{Q}/{n}'; K = f'{T}/kits/q-{n}'; shutil.rmtree(R, ignore_errors=True); os.makedirs(f'{R}/app'); lines = []; res = dict(short=n, id=gid, name=name, app=False, ipa=False); t0 = time.time()
    def log(*a): m = ' '.join(map(str, a)); lines.append(m); say(n, m); open(f'{R}/queue.log', 'a').write(m + '\n')
    if not wait_idle(log): raise RuntimeError('the PC is still busy after 2 hours')
    subprocess.run([sys.executable, f'{TOOLS}/new_kit.py', K, gid, name, BASE], check=True, capture_output=True)
    ssh(f'rmdir /s /q C:\\dmb\\kits\\{n} & mkdir C:\\dmb\\kits\\{n}\\font & rmdir /s /q C:\\dmb\\w-{n} & rmdir /s /q C:\\dmb\\g-{n} & rmdir /s /q C:\\dmb\\app-{n} & del C:\\dmb\\ipa-{n}.ipa C:\\dmb\\ipa-{n}.ipa.sha256 C:\\dmb\\app.zip C:\\dmb\\results.zip & mkdir C:\\dmb\\g-{n}')
    scp(f'{K}/nxnative', f'{K}/kit.json', f'{H}:C:/dmb/kits/{n}/'); scp(f'{K}/font/nintendo_ext.ttf', f'{H}:C:/dmb/kits/{n}/font/')
    log('copying', src); pats = ' '.join(f'"{x}"' for x in src.split('|')); out = ssh(f'robocopy \\\\192.168.1.244\\Shared\\Switch C:\\dmb\\g-{n} {pats} /J /NP /NJH /NDL /R:2 /W:5 >nul & dir /b C:\\dmb\\g-{n}')   # a game and its update: names joined with |
    if src.lower().endswith('.zip'):
        ssh(f'cd /d C:\\dmb\\g-{n} && C:\\dmb\\python\\python.exe -m zipfile -e "{src}" . && del "{src}"', True); out = ssh(f'dir /b C:\\dmb\\g-{n}')
    log('files:', out.strip().replace('\n', ', ')[:120])
    stop = threading.Event(); th = threading.Thread(target=serve, args=(n, gid, R, K, stop, log), daemon=True); th.start()
    with open(f'{R}/console.log', 'w') as f:
        for attempt in range(2):   # a dropped connection ends the build; its compile cache lets it resume
            r = subprocess.run(['ssh', *O, H, f'cd /d C:\\dmb && python\\python.exe run_all.py --game C:\\dmb\\g-{n} --kit C:\\dmb\\kits\\{n} --work C:\\dmb\\w-{n} --app-dir C:\\dmb\\app-{n} --ipa C:\\dmb\\ipa-{n}.ipa' + (' --ipa-only' if mode == 'ipa' else '')], stdout=f, stderr=subprocess.STDOUT)
            if r.returncode != 255: break
            log('connection lost: waiting for the PC, then building again'); time.sleep(60)
            if not wait_idle(log): log('[RED] the PC is still busy after 2 hours'); break
    stop.set(); th.join(60); txt = open(f'{R}/console.log', errors='replace').read().replace('\r', ''); res['minutes'] = round((time.time() - t0) / 60)
    try: scp(f'{H}:C:/dmb/results.zip', f'{R}/results.zip')
    except Exception as e: log('no results.zip', e)
    has = ssh(f'(if exist C:\\dmb\\app.zip echo APP)& (if exist C:\\dmb\\ipa-{n}.ipa.sha256 echo IPA)')
    if 'APP' in has:
        scp(f'{H}:C:/dmb/app.zip', f'{R}/app.zip'); subprocess.run(['unzip', '-q', f'{R}/app.zip', '-d', f'{R}/app'], check=True); os.unlink(f'{R}/app.zip')
        z = zipfile.ZipFile(f'{R}/results.zip'); nm = {x.replace('\\', '/'): x for x in z.namelist()}; bad = 0; cnt = 0
        for l in z.read(nm['results/app_hashes.tsv']).decode().splitlines():
            x = l.split('\t')
            if len(x) == 3: cnt += 1; bad += sha(os.path.join(R, 'app', x[2])) != x[0]
        res['app'] = bad == 0 and os.path.isdir(f'{R}/app/{name}.app'); log(f'Mac app: {cnt} files, {bad} wrong')
    if 'IPA' in has:
        os.makedirs(f'{Q}/ipas', exist_ok=True); I = f'{Q}/ipas/{name}.ipa'; scp(f'{H}:C:/dmb/ipa-{n}.ipa', I); w = ssh(f'type C:\\dmb\\ipa-{n}.ipa.sha256').strip(); res['ipa'] = sha(I) == w; res['ipa_gb'] = round(os.path.getsize(I) / 1e9, 2); log(f'IPA: {res["ipa_gb"]} GB, hash {"good" if res["ipa"] else "WRONG"}')
    res['why'] = next((l.strip()[:200] for l in txt.split('\n') if '[RED]' in l and 'provisional game profile' not in l and 'could not be read' not in l and 'pipeline' not in l), '')
    res['shaders'] = next(iter(re.findall(r'(\d[\d,]*) of \d[\d,]* shaders ready', txt)), '')
    ssh(f'rmdir /s /q C:\\dmb\\w-{n} & rmdir /s /q C:\\dmb\\app-{n} & rmdir /s /q C:\\dmb\\g-{n} & del C:\\dmb\\ipa-{n}.ipa C:\\dmb\\ipa-{n}.ipa.sha256 C:\\dmb\\app.zip')
    if res['app']:
        try: res['play'] = play(R, gid, name); log('play:', res['play'])
        except Exception as e: log('[RED] play test failed', e)
    return res
def main():
    os.makedirs(Q, exist_ok=True); rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1], encoding='utf-8') if l.strip() and not l.startswith('#')]
    only = sys.argv[sys.argv.index('--only') + 1].split(',') if '--only' in sys.argv else None
    done_path = f'{Q}/results.jsonl'; done = {json.loads(l)['short'] for l in open(done_path, encoding='utf-8')} if os.path.exists(done_path) else set()
    subprocess.Popen([sys.executable, '-u', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'watchdog.py')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)   # stops a hung metal/metallib on the PC; ends with this queue
    for row in rows:
        n, gid, name, src = row[:4]; mode = row[4] if len(row) > 4 else ''
        if n in done or (only and n not in only): continue
        if shutil.disk_usage(T).free < 80e9: say('[RED] under 80 GB free on the 2TB drive: stopping'); break
        free = re.search(r'([\d,]+) bytes free', ssh('dir C:\\ | findstr /c:"bytes free"'))
        if not free: say('the laptop does not answer: waiting 5 minutes'); time.sleep(300); free = re.search(r'([\d,]+) bytes free', ssh('dir C:\\ | findstr /c:"bytes free"'))
        if not free: say('[RED] the laptop is gone: stopping'); break
        say('=====', name, 'laptop free GB', int(free[1].replace(',', '')) // 10 ** 9)
        try: res = one(n, gid, name, src, mode)
        except Exception as e: res = dict(short=n, id=gid, name=name, app=False, ipa=False, why=f'queue: {e}'[:300]); say('[RED]', n, e)
        with open(done_path, 'a', encoding='utf-8') as f: f.write(json.dumps(res) + '\n')
        say('RESULT', json.dumps({k: res.get(k) for k in ('short', 'app', 'ipa', 'minutes', 'shaders', 'why')})[:300])
    say('QUEUE-END')
main()
