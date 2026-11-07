#!/usr/bin/env python3
# watchdog.py: runs beside queue.py and stops a shader compiler or linker (metal, metallib) on the laptop that has used no CPU time for STALL minutes.
# Seen on 2026-11-07: a metallib sat idle for 12 minutes during Mario Tennis Aces' iPhone shaders and the build waited on it; once stopped, the build went on.
# A process that does work uses CPU time, so an unchanged CPU total for minutes means it is hung, not slow. It leaves python and everything else alone.
# Exits when queue.py does. Log: ../watchdog.log
import base64, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
O = ['-i', os.path.expanduser('~/.ssh/nxnative_laptop'), '-o', 'IdentitiesOnly=yes', '-o', 'UserKnownHostsFile=' + os.path.expanduser('~/.ssh/nxnative_laptop_known_hosts'), '-o', 'ConnectTimeout=20', '-o', 'BatchMode=yes']
H = 'nxbuild@192.168.1.180'; STALL = 8 * 60; EVERY = 60
LOG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'watchdog.log')
def say(*a):
    with open(LOG, 'a') as f: print(time.strftime('%m-%d %H:%M'), *a, file=f, flush=True)
def ps(script):
    enc = base64.b64encode(script.encode('utf-16-le')).decode()
    r = subprocess.run(['ssh', *O, H, f'powershell -NoProfile -EncodedCommand {enc}'], capture_output=True, text=True, errors='replace', timeout=120)
    return r.stdout
def queue_running():
    return subprocess.run(['pgrep', '-f', 'queue.py '], capture_output=True).returncode == 0
seen = {}   # pid -> (name, cpu, time the cpu last changed)
say('started: stops metal/metallib idle for', STALL // 60, 'minutes')
while queue_running():
    try:
        out = ps('Get-Process metal,metallib -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Id) $($_.Name) $($_.CPU)" }')
    except Exception as e:
        say('laptop did not answer:', e); time.sleep(EVERY); continue
    now = time.time(); live = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) != 3: continue
        pid, name, cpu = int(p[0]), p[1], float(p[2] or 0)
        old = seen.get(pid)
        live[pid] = (name, cpu, now if not old or old[1] != cpu else old[2])
    seen = live
    for pid, (name, cpu, since) in list(seen.items()):
        if now - since >= STALL:
            ps(f'Stop-Process -Id {pid} -Force -ErrorAction SilentlyContinue')
            say(f'stopped {name} {pid}: no CPU used for {int((now - since) / 60)} minutes (CPU total {cpu} s)')
            seen.pop(pid)
    time.sleep(EVERY)
say('queue.py ended: watchdog stops')
