#!/usr/bin/env python3
"""Find where a number shown by a game lives in its memory, through DM_CTL (the Mac host). It only reads.

Start the game with DM_CTL=<file> (an empty file), and the host runs each line appended to that file ([05-debugging.md](../../docs/agents/05-debugging.md), Scripted input).
This tool appends `memdump` and `peek` lines, waits for the host's answers in its log, and does the searching a cheat tool does:

  ctl_scan.py scan f64 1015.01 --log game.log --ctl ctl.txt   dump the game's main module, alias and heap in chunks and keep every aligned value in range
  ... play: the number changes ...
  ctl_scan.py filter f64 1018.06 --log game.log               read the candidates again and keep those that now hold the new number
  ctl_scan.py show                                            the candidates, as "heap + offset" (the host's cheat memory type 1) or "main + offset" (type 0)
  ctl_scan.py cheat 999999                                    the Atmosphere cheat words that write a value to every candidate left

Types: i32 and i64 (exact), f32 and f64 (a range: the number on the screen is rounded or cut, so --decimals 2 means +-0.01). The candidates are kept in --state
(default ctl_scan.json). They are only good for the run they came from: a game's heap starts somewhere else at every launch, so `filter` stops when the log
shows a new run. An offset that is the same at every launch is worth a cheat; check with `peek` after a fresh start before trusting one.

The searching is done by dm/tools/ctl_scan.c, which this tool compiles with cc once and keeps in the temporary folder.
"""
import argparse, hashlib, json, os, pathlib, re, struct, subprocess, sys, tempfile, time

HERE = pathlib.Path(__file__).resolve().parent
WIDTH = {'i32': 4, 'f32': 4, 'i64': 8, 'f64': 8}
CHUNK = 0x10000000   # bytes dumped at a time
LIMIT = 1_000_000    # candidates kept before the scan stops: a value this common (0, 1) says nothing
MEM_TYPE = {'main': 0, 'heap': 1, 'alias': 2}   # the cheat memory types of docs/reference/cheats.md


def value_range(kind, value, decimals):
    """(lo, hi) as strings for the scanner: exact for integers, value +- 10^-decimals for floats (a screen rounds or cuts)"""
    if kind.startswith('i'):
        v = int(float(value))
        return str(v), str(v)
    d = 10.0 ** -decimals
    v = float(value)
    return repr(v - d), repr(v + d)


def memory_map(log_text):
    """{'main': (base, size), 'alias': (...), 'heap': (...)} and the text of the run's address-space line, from the host's log (the last run in it)"""
    mains = re.findall(r'main: base 0x([0-9a-f]+) size 0x([0-9a-f]+)', log_text)
    spaces = re.findall(r'guest AS 0x([0-9a-f]+) \+0x([0-9a-f]+)\s+heap 0x([0-9a-f]+) alias 0x([0-9a-f]+) stack 0x([0-9a-f]+)', log_text)
    if not mains or not spaces:
        sys.exit('ctl_scan: the log has no "main: base" and "guest AS" lines: is it the host\'s log, and has the game started?')
    base, size, heap, alias, stack = (int(x, 16) for x in spaces[-1])
    regions = {'main': (int(mains[-1][0], 16), int(mains[-1][1], 16)), 'alias': (alias, max(stack - alias, 0)), 'heap': (heap, base + size - heap)}
    run = re.findall(r'guest AS [^\n]*', log_text)[-1]
    return regions, run


def describe(addr, regions):
    for name, (base, size) in regions.items():
        if base <= addr < base + size:
            return f'{name} + {addr - base:#x}'
    return f'{addr:#x}'


def cheat_words(kind, region, offset, value):
    """The Atmosphere words (code type 0, store a static value) that write `value` at `region + offset`"""
    width = WIDTH[kind]
    words = [f'0{width:X}{MEM_TYPE[region]:X}000{(offset >> 32) & 0xff:02X}', f'{offset & 0xffffffff:08X}']
    if kind == 'f32': raw = struct.unpack('>I', struct.pack('>f', float(value)))[0]
    elif kind == 'f64': raw = struct.unpack('>Q', struct.pack('>d', float(value)))[0]
    else: raw = int(float(value)) & ((1 << (8 * width)) - 1)
    if width == 8: words += [f'{raw >> 32:08X}', f'{raw & 0xffffffff:08X}']
    else: words.append(f'{raw:08X}')
    return ' '.join(words)


def scanner():
    """Path of the compiled dm/tools/ctl_scan.c, built on first use into the temporary folder"""
    src = HERE / 'ctl_scan.c'
    out = pathlib.Path(tempfile.gettempdir()) / ('nxnative_ctl_scan_' + hashlib.sha1(src.read_bytes()).hexdigest()[:12])
    if not out.is_file():
        part = out.with_suffix('.part')
        try: subprocess.run(['cc', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-o', str(part), str(src)], check=True, capture_output=True, text=True)
        except (OSError, subprocess.CalledProcessError) as e: sys.exit(f'ctl_scan: cc could not build {src}: {getattr(e, "stderr", e)}')
        os.replace(part, out)
    return out


class Host:
    """The host's two files: lines appended to the DM_CTL file go in, and its answers come back as log lines"""
    def __init__(self, ctl, log, poll=0.1):
        self.ctl, self.log, self.poll = pathlib.Path(ctl), pathlib.Path(log), poll

    def send(self, *lines):
        with open(self.ctl, 'a') as f: f.write(''.join(l + '\n' for l in lines))

    def mark(self):
        return self.log.stat().st_size

    def text_since(self, mark):
        with open(self.log, 'rb') as f:
            f.seek(mark)
            return f.read().decode('utf-8', 'replace')

    def wait(self, pattern, mark, timeout=120.0, count=1):
        """The matches of `pattern` in the log after `mark`, once there are `count` of them (or the time is up)"""
        end = time.time() + timeout
        while True:
            found = re.findall(pattern, self.text_since(mark))
            if len(found) >= count or time.time() > end: return found
            time.sleep(self.poll)


def load_state(state_path):
    try: return json.loads(pathlib.Path(state_path).read_text())
    except OSError: sys.exit(f'ctl_scan: no scan state in {state_path}: run `scan` first (or name the file with --state)')


def scan(host, kind, lo, hi, state_path, only=None, chunk=CHUNK, limit=LIMIT):
    regions, run = memory_map(host.log.read_text(errors='replace'))
    helper, cands, dump = scanner(), [], pathlib.Path(tempfile.gettempdir()) / 'nxnative_ctl_scan.dump'
    for name, (base, size) in regions.items():
        if only and name not in only: continue
        off = 0
        while off < size:
            n, addr = min(chunk, size - off), base + off
            mark = host.mark()
            host.send(f'memdump {addr:x} {n} {dump}')
            got = host.wait(rf'memdump {addr:#x}: (\d+) of', mark)
            if not got: sys.exit(f'ctl_scan: the host did not answer a memdump at {addr:#x}: is the game running with DM_CTL={host.ctl}?')
            if int(got[0]):
                out = subprocess.run([str(helper), str(dump), f'{addr:x}', kind, lo, hi, str(limit - len(cands))], capture_output=True, text=True, check=True).stdout
                cands += [(int(a, 16), float(v) if kind[0] == 'f' else int(v)) for a, v in (line.split() for line in out.splitlines())]
            if len(cands) >= limit:
                print(f'ctl_scan: stopped at {limit} candidates: a value this common tells nothing. Scan for a value that is rarer (not 0 or 1), or raise --limit.')
                break
            if int(got[0]) < n: break   # the rest is not mapped
            off += n
        print(f'{name}: {len(cands)} candidates so far', flush=True)
        if len(cands) >= limit: break
    dump.unlink(missing_ok=True)
    pathlib.Path(state_path).write_text(json.dumps({'kind': kind, 'run': run, 'regions': {k: list(v) for k, v in regions.items()}, 'candidates': cands}))
    return cands


def decode(kind, words):
    raw = struct.pack('<IIII', *words)
    return {'i32': lambda: struct.unpack_from('<i', raw)[0], 'i64': lambda: struct.unpack_from('<q', raw)[0],
            'f32': lambda: struct.unpack_from('<f', raw)[0], 'f64': lambda: struct.unpack_from('<d', raw)[0]}[kind]()


def filter_candidates(host, state_path, lo, hi, timeout=120.0):
    state = load_state(state_path)
    kind, cands = state['kind'], state['candidates']
    regions, run = memory_map(host.log.read_text(errors='replace'))
    if run != state['run']: sys.exit('ctl_scan: the game was started again since the scan: its addresses are different now. Scan again.')
    mark = host.mark()
    host.send(*(f'peek {a:x} 4' for a, _ in cands))
    answers = host.wait(r'peek 0x([0-9a-f]+): (?:not readable|[^(]*\(([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})\))', mark, timeout, len(cands))
    now = {int(a, 16): decode(kind, [int(w, 16) for w in ws]) for a, *ws in answers if ws[0]}
    keep = [(a, now[a]) for a, _ in cands if a in now and float(lo) <= now[a] <= float(hi)]
    state['candidates'] = keep
    pathlib.Path(state_path).write_text(json.dumps(state))
    print(f'read {len(now)} of {len(cands)}, kept {len(keep)}')
    return keep


def show(state_path, limit=40):
    state = load_state(state_path)
    regions = {k: tuple(v) for k, v in state['regions'].items()}
    for a, v in state['candidates'][:limit]: print(f'{a:#x}  {describe(a, regions):<22} {v}')
    extra = len(state['candidates']) - limit
    print(f'{len(state["candidates"])} candidates' + (f' ({extra} more not shown)' if extra > 0 else ''))


def cheat(state_path, value):
    state = load_state(state_path)
    kind = state['kind']
    for a, _ in state['candidates']:
        for name, (base, size) in state['regions'].items():
            if base <= a < base + size:
                print(f'# {name} + {a - base:#x}')
                print(cheat_words(kind, name, a - base, value))
                break


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or '').split('\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ctl', default=os.environ.get('DM_CTL'), help='the DM_CTL file the game was started with (default: $DM_CTL)')
    ap.add_argument('--log', help='the host\'s log (Mac app: ~/Library/Logs/nxnative/<title ID>.log)')
    ap.add_argument('--state', default='ctl_scan.json')
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('scan', 'filter'):
        p = sub.add_parser(name)
        p.add_argument('kind', choices=sorted(WIDTH))
        p.add_argument('value')
        p.add_argument('--decimals', type=int, default=2, help='decimals the screen shows, for f32 and f64 (default 2: matches +-0.01)')
        if name == 'scan':
            p.add_argument('--regions', default='main,alias,heap', help='comma separated (default main,alias,heap)')
            p.add_argument('--limit', type=int, default=LIMIT, help=f'stop at this many candidates (default {LIMIT})')
    sub.add_parser('show')
    p = sub.add_parser('cheat')
    p.add_argument('value')
    a = ap.parse_args()
    if a.cmd in ('show', 'cheat'):
        if a.cmd == 'show': show(a.state)
        else: cheat(a.state, a.value)
        return
    if not a.ctl or not a.log: sys.exit('ctl_scan: --ctl (or $DM_CTL) and --log are needed')
    lo, hi = value_range(a.kind, a.value, a.decimals)
    host = Host(a.ctl, a.log)
    if a.cmd == 'scan': scan(host, a.kind, lo, hi, a.state, set(a.regions.split(',')), limit=a.limit)
    else: filter_candidates(host, a.state, lo, hi)


if __name__ == '__main__':
    main()
