#!/usr/bin/env python3
"""Measure two builds of a pack against each other (README.md, "Measuring two packs").

Two variants, A and B, of the same program's pack: two nxaot binaries, or one binary with an environment switch (`--b-env
NXAOT_PROFILE_WEIGHTS=0`), or with other `nxaot build` arguments (`--b-args=--profile x.tsv`). For every program the tool builds both packs under
--layouts file names of different lengths (the soname is the file name, and its length shifts the code by a few bytes, so the layout alone moves
the time), checks that they differ (identical bytes: said, and the runs skipped), then runs them natively through nxaot_host pinned to one CPU
(taskset -c <cpu>) in alternating rounds: in each round, for each layout, A then B. Each run is the fastest of --repeats (nxaot_host --repeat N,
or N processes with --repeat-mode process); a variant's time in a layout is the median over the rounds, and its time is the median over the
layouts. The two variants must print the same guest output, and every run must exit 0: otherwise an error naming the program.

Printed: a Markdown table (program, A, B, change, the spread over layouts, the rounds' spread, .text, file size, pack time and peak memory A -> B),
a verdict per program by the rule below, the machine's load at start and end. Written to --work: the packs, samples.tsv (every run), builds.tsv
(every build) and results.md (what was printed).

The verdict: noise = the largest of the layout spread of A, the layout spread of B (each (max - min) / median of that variant's per-layout medians)
and the rounds' spread (the median over every variant and layout of (max - min) / median of its rounds' samples). "faster" when B's change is
below -noise and B's median is below A's in every layout; "slower" when the change is above +noise and B's median is above A's in every layout;
otherwise "within noise". With one layout and one round there is no estimate of the noise: "no noise estimate".

Standard library only. Linux packs (an ELF shared object); --dry-run prints the commands without running them.
"""
import argparse
import os
import re
import shlex
import shutil
import statistics
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS_NRO = os.path.normpath(os.path.join(HERE, '..', 'corpus', 'nro'))
VARIANTS = ('a', 'b')

NOISE_RULE = ('noise = max(layout spread A, layout spread B, rounds spread); layout spread = (max - min) / median of the per-layout medians; '
              'rounds spread = median over variants and layouts of (max - min) / median of the rounds; verdict "faster" if change < -noise and '
              'B < A in every layout, "slower" if change > +noise and B > A in every layout, else "within noise" '
              '("no noise estimate" with one layout and one round)')

FASTEST_RE = re.compile(r'fastest of (\d+): ([0-9.eE+-]+) s')


class MeasureError(Exception):
    pass


# Statistics ------------------------------------------------------------------------------------------------------------------------------------

def fastest(values):
    return min(values)


def median(values):
    return statistics.median(values)


def change_pct(a, b):
    return (b - a) / a * 100.0


def spread_pct(values):
    """(max - min) / median, in percent; 0 for fewer than two values."""
    if len(values) < 2:
        return 0.0
    m = median(values)
    return (max(values) - min(values)) / m * 100.0 if m else 0.0


def summarize(samples, layouts):
    """samples: {(variant, layout): [per-round fastest seconds]} -> the program's statistics and verdict."""
    per_layout = {v: [median(samples[(v, l)]) for l in range(layouts)] for v in VARIANTS}
    a = median(per_layout['a'])
    b = median(per_layout['b'])
    change = change_pct(a, b)
    lspread = {v: spread_pct(per_layout[v]) for v in VARIANTS}
    rounds = len(samples[('a', 0)])
    rspread = median([spread_pct(samples[(v, l)]) for v in VARIANTS for l in range(layouts)])
    noise = max(lspread['a'], lspread['b'], rspread)
    if layouts < 2 and rounds < 2:
        verdict = 'no noise estimate'
    elif change < -noise and all(pb < pa for pa, pb in zip(per_layout['a'], per_layout['b'])):
        verdict = 'faster'
    elif change > noise and all(pb > pa for pa, pb in zip(per_layout['a'], per_layout['b'])):
        verdict = 'slower'
    else:
        verdict = 'within noise'
    return {'a': a, 'b': b, 'change': change, 'per_layout': per_layout, 'layout_spread': lspread, 'rounds_spread': rspread,
            'noise': noise, 'verdict': verdict}


# Packs -------------------------------------------------------------------------------------------------------------------------------------------

def elf_text_size(path):
    """The size of the ELF64 shared object's .text sections (.text and .text.*), or None when it is not ELF64."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return None
    if len(data) < 64 or data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        return None
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
    if not shoff or shentsize < 64 or shstrndx >= shnum or shoff + shnum * shentsize > len(data):
        return None

    def section(i):
        return struct.unpack_from('<IIQQQQ', data, shoff + i * shentsize)
    stroff = section(shstrndx)[4]
    total = 0
    for i in range(shnum):
        name_off, _, _, _, _, size = section(i)
        end = data.find(b'\0', stroff + name_off)
        name = data[stroff + name_off:end].decode('ascii', 'replace')
        if name == '.text' or name.startswith('.text.'):
            total += size
    return total


def module_path(program):
    """A corpus name (int_ops-gcc) or a path to an NRO or a manifest (name.aot.tsv), as nxaot build takes them."""
    if os.path.sep in program or program.endswith('.nro') or program.endswith('.aot.tsv'):
        return os.path.abspath(program)
    return os.path.join(CORPUS_NRO, program + '.nro')


def program_label(program):
    base = os.path.basename(program)
    for suffix in ('.nro', '.aot.tsv'):
        if base.endswith(suffix):
            return base[:-len(suffix)]
    return base


def pack_name(label, layout):
    # One file name (the soname) for A and B in one layout, in folders a/ and b/, so that equal code gives equal bytes; one character longer
    # per layout
    return f'{label}.L{layout}' + 'x' * layout + '.so'


def build_cmd(opts, variant, module, so):
    nxaot = opts.a_nxaot if variant == 'a' else (opts.b_nxaot or opts.a_nxaot)
    extra = shlex.split(opts.args) + shlex.split(opts.a_args if variant == 'a' else opts.b_args)
    return [nxaot, 'build', module, '--shared', so] + extra


def build_env(opts, variant):
    env = dict(os.environ)
    for kv in (opts.a_env if variant == 'a' else opts.b_env):
        k, _, v = kv.partition('=')
        env[k] = v
    return env


def run_build(cmd, env, log_path):
    """Run one nxaot build with its output in log_path; (status, wall seconds, peak RSS KiB) from wait4's per-child rusage."""
    with open(log_path, 'wb') as log:
        t0 = time.monotonic()
        p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
        _, status, ru = os.wait4(p.pid, 0)
        wall = time.monotonic() - t0
        p.returncode = os.waitstatus_to_exitcode(status)
    return p.returncode, wall, ru.ru_maxrss


def host_cmd(opts, so, module, program_sets, repeat):
    cmd = [opts.host, so, module, '--repeat', str(repeat)]
    for s in program_sets:
        cmd += ['--set', s]
    return opts.pin + cmd


def sets_for(opts, label):
    """--set name=value applies to every program; --set program:name=value to that program only."""
    out = []
    for s in opts.set:
        head, eq, _ = s.partition('=')
        if ':' in head and eq:
            prog, _, rest = s.partition(':')
            if prog == label:
                out.append(rest)
        else:
            out.append(s)
    return out


def human(seconds):
    if seconds >= 1:
        return f'{seconds:.3f} s'
    if seconds >= 1e-3:
        return f'{seconds * 1e3:.3f} ms'
    return f'{seconds * 1e6:.1f} us'


def size_arrow(a, b):
    if a is None or b is None:
        return '-'
    return f'{a:,} -> {b:,}'


# The run -------------------------------------------------------------------------------------------------------------------------------------

class Report:
    def __init__(self, path):
        self.lines = []
        self.path = path

    def __call__(self, line=''):
        print(line, flush=True)
        self.lines.append(line)

    def save(self):
        if self.path:
            with open(self.path, 'w') as f:
                f.write('\n'.join(self.lines) + '\n')


def measure_program(opts, program, say, samples_tsv, builds_tsv, seq):
    label = program_label(program)
    module = module_path(program)
    sets = sets_for(opts, label)
    pdir = os.path.join(opts.work, label)
    packs = {}
    for v in VARIANTS:
        for l in range(opts.layouts):
            packs[(v, l)] = os.path.join(pdir, v, pack_name(label, l))
    repeat_host = opts.repeats if opts.repeat_mode == 'host' else 1
    procs = 1 if opts.repeat_mode == 'host' else opts.repeats

    if opts.dry_run:
        for l in range(opts.layouts):
            for v in VARIANTS:
                envs = ' '.join(shlex.quote(e) for e in (opts.a_env if v == 'a' else opts.b_env))
                say(f'build {v}: ' + (f'env {envs} ' if envs else '') + shlex.join(build_cmd(opts, v, module, packs[(v, l)])))
        for r in range(opts.rounds):
            for l in range(opts.layouts):
                for v in VARIANTS:
                    for _ in range(procs):
                        say(f'run round {r + 1} layout {l} {v}: ' + shlex.join(host_cmd(opts, packs[(v, l)], module, sets, repeat_host)))
        return None

    if not os.path.exists(module):
        raise MeasureError(f'{label}: no module at {module}')
    for v in VARIANTS:
        os.makedirs(os.path.join(pdir, v), exist_ok=True)
    builds = {}
    for l in range(opts.layouts):
        for v in VARIANTS:
            so = packs[(v, l)]
            if os.path.exists(so):
                os.remove(so)
            cmd = build_cmd(opts, v, module, so)
            status, wall, peak = run_build(cmd, build_env(opts, v), so + '.build.log')
            if status != 0 or not os.path.exists(so):
                raise MeasureError(f'{label}: nxaot build of variant {v.upper()} exited {status} (log {so}.build.log): {shlex.join(cmd)}')
            size = os.path.getsize(so)
            text = elf_text_size(so)
            builds[(v, l)] = (wall, peak, size, text)
            builds_tsv.write(f'{label}\t{v}\t{l}\t{os.path.relpath(so, pdir)}\t{wall:.6f}\t{peak}\t{size}\t{text if text is not None else "-"}\n')
    with open(packs[('a', 0)], 'rb') as fa, open(packs[('b', 0)], 'rb') as fb:
        if fa.read() == fb.read():
            say(f'{label}: the packs of A and B are identical bytes ({os.path.getsize(packs[("a", 0)]):,} bytes): no runs')
            return {'label': label, 'identical': True, 'builds': builds}

    reference = None
    samples = {(v, l): [] for v in VARIANTS for l in range(opts.layouts)}
    for r in range(opts.rounds):
        for l in range(opts.layouts):
            for v in VARIANTS:
                times = []
                for i in range(procs):
                    cmd = host_cmd(opts, packs[(v, l)], module, sets, repeat_host)
                    p = subprocess.run(cmd, capture_output=True)
                    seq[0] += 1
                    if p.returncode != 0:
                        tail = (p.stdout.decode(errors='replace').strip().splitlines() or ['-'])[-1]
                        raise MeasureError(f'{label}: variant {v.upper()} (layout {l}, round {r + 1}) exited {p.returncode}: {tail}')
                    if reference is None:
                        reference = p.stdout
                    elif p.stdout != reference:
                        got = p.stdout.decode(errors='replace').splitlines()
                        want = reference.decode(errors='replace').splitlines()
                        where = next((k for k, (g, w) in enumerate(zip(got, want)) if g != w), min(len(got), len(want)))
                        raise MeasureError(f'{label}: variant {v.upper()} (layout {l}, round {r + 1}) printed a different guest output from '
                                           f'variant A\'s first run (line {where + 1}: {got[where] if where < len(got) else "<none>"!r}, '
                                           f'want {want[where] if where < len(want) else "<none>"!r})')
                    m = FASTEST_RE.search(p.stderr.decode(errors='replace'))
                    if not m:
                        raise MeasureError(f'{label}: variant {v.upper()}: no "fastest of N: T s" in nxaot_host\'s standard error')
                    t = float(m.group(2))
                    times.append(t)
                    rep = f'fastest of {m.group(1)}' if opts.repeat_mode == 'host' else str(i + 1)
                    samples_tsv.write(f'{label}\t{r + 1}\t{seq[0]}\t{l}\t{os.path.relpath(packs[(v, l)], pdir)}\t{v}\t{rep}\t{t:.9f}\n')
                samples[(v, l)].append(fastest(times))
    samples_tsv.flush()
    stats = summarize(samples, opts.layouts)
    stats.update({'label': label, 'identical': False, 'builds': builds})
    return stats


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='measure_packs.py',
        description='Measure two builds (A, B) of nxaot packs against each other: alternating rounds, pinned to one CPU, several layouts.',
        epilog='Values that start with "-" take "=": --b-args=\'--profile p.tsv\'. ' + NOISE_RULE + '.')
    ap.add_argument('programs', nargs='+', metavar='program',
                    help='a corpus name (int_ops-gcc: corpus/nro/<name>.nro) or a path to an NRO or a manifest (<name>.aot.tsv)')
    ap.add_argument('--a-nxaot', required=True, help='nxaot of variant A')
    ap.add_argument('--b-nxaot', help='nxaot of variant B (default: A\'s)')
    ap.add_argument('--a-env', action='append', default=[], metavar='K=V', help='environment of A\'s nxaot build (repeatable)')
    ap.add_argument('--b-env', action='append', default=[], metavar='K=V', help='environment of B\'s nxaot build (repeatable)')
    ap.add_argument('--args', default='', help='extra nxaot build arguments for both variants (one string, shell-split)')
    ap.add_argument('--a-args', default='', help='extra nxaot build arguments for A (one string, shell-split)')
    ap.add_argument('--b-args', default='', help='extra nxaot build arguments for B (one string, shell-split)')
    ap.add_argument('--host', required=True, help='nxaot_host')
    ap.add_argument('--rounds', type=int, default=6, help='alternating rounds (default 6)')
    ap.add_argument('--repeats', type=int, default=10, help='runs per sample, the fastest kept (default 10)')
    ap.add_argument('--repeat-mode', choices=('host', 'process'), default='host',
                    help='host: one nxaot_host --repeat N per sample (default); process: N nxaot_host processes, each --repeat 1')
    ap.add_argument('--cpu', default=None, help='the CPU to pin every run to (default: the last one this process may use); "none": unpinned')
    ap.add_argument('--layouts', type=int, default=4, help='file names of different lengths for each pack (default 4)')
    ap.add_argument('--set', action='append', default=[], metavar='[program:]name=value',
                    help='nxaot_host --set for every program, or for one (repeatable): int_ops-gcc:int_ops_rounds=200')
    ap.add_argument('--work', required=True, help='work folder: packs, build logs, samples.tsv, builds.tsv, results.md')
    ap.add_argument('--dry-run', action='store_true', help='print the commands, run nothing')
    opts = ap.parse_args(argv)
    if opts.rounds < 1 or opts.repeats < 1 or opts.layouts < 1:
        ap.error('--rounds, --repeats and --layouts must be at least 1')
    for kv in opts.a_env + opts.b_env:
        if '=' not in kv:
            ap.error(f'--a-env / --b-env take K=V, not {kv!r}')
    opts.work = os.path.abspath(opts.work)

    say = Report(None if opts.dry_run else os.path.join(opts.work, 'results.md'))
    taskset = shutil.which('taskset')
    if opts.cpu is None:
        try:
            opts.cpu = str(max(os.sched_getaffinity(0)))
        except (AttributeError, OSError):
            opts.cpu = '0'
    if opts.cpu == 'none':
        opts.pin = []
        pin_note = 'runs NOT pinned (--cpu none)'
    elif taskset:
        opts.pin = [taskset, '-c', opts.cpu]
        pin_note = f'runs pinned to CPU {opts.cpu} (taskset)'
    else:
        opts.pin = []
        pin_note = 'runs NOT pinned: taskset not found, the times are less stable'

    if not opts.dry_run:
        os.makedirs(opts.work, exist_ok=True)
    load0 = os.getloadavg()
    say(f'measure_packs: A = {opts.a_nxaot} {" ".join(opts.a_env)} {opts.args} {opts.a_args}'.rstrip())
    say(f'measure_packs: B = {opts.b_nxaot or opts.a_nxaot} {" ".join(opts.b_env)} {opts.args} {opts.b_args}'.rstrip())
    say(f'measure_packs: {pin_note}; {opts.rounds} rounds (A B A B ...), fastest of {opts.repeats} ({opts.repeat_mode}), {opts.layouts} layouts; '
        f'load at start {load0[0]:.2f} {load0[1]:.2f} {load0[2]:.2f}')
    if opts.dry_run:
        seq = [0]
        for program in opts.programs:
            measure_program(opts, program, say, None, None, seq)
        return 0

    results, errors = [], []
    seq = [0]
    with open(os.path.join(opts.work, 'samples.tsv'), 'w') as samples_tsv, open(os.path.join(opts.work, 'builds.tsv'), 'w') as builds_tsv:
        samples_tsv.write('# program\tround\torder\tlayout\tfile\tvariant\trepeat\tseconds\n')
        builds_tsv.write('# program\tvariant\tlayout\tfile\twall_seconds\tpeak_rss_kib\tbytes\ttext_bytes\n')
        for program in opts.programs:
            try:
                results.append(measure_program(opts, program, say, samples_tsv, builds_tsv, seq))
            except MeasureError as e:
                errors.append(str(e))
                say(f'ERROR {e}')
    load1 = os.getloadavg()

    say()
    say('| Program | A | B | change | layout spread A / B | rounds spread | .text A -> B | bytes A -> B | pack time A -> B | pack peak MiB A -> B |')
    say('|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|')
    for res in results:
        bld = res['builds']
        med = {v: {k: median([bld[(v, l)][k] for l in range(opts.layouts)]) for k in (0, 1)} for v in VARIANTS}
        text = size_arrow(bld[('a', 0)][3], bld[('b', 0)][3])
        size = size_arrow(bld[('a', 0)][2], bld[('b', 0)][2])
        ptime = f'{med["a"][0]:.2f} -> {med["b"][0]:.2f} s'
        peak = f'{med["a"][1] / 1024:.0f} -> {med["b"][1] / 1024:.0f}'
        if res['identical']:
            say(f'| {res["label"]} | - | - | identical packs | - | - | {text} | {size} | {ptime} | {peak} |')
            continue
        say(f'| {res["label"]} | {human(res["a"])} | {human(res["b"])} | {res["change"]:+.2f} % | {res["layout_spread"]["a"]:.2f} / '
            f'{res["layout_spread"]["b"]:.2f} % | {res["rounds_spread"]:.2f} % | {text} | {size} | {ptime} | {peak} |')
    say()
    for res in results:
        if res['identical']:
            say(f'verdict {res["label"]}: identical packs, not run')
        else:
            say(f'verdict {res["label"]}: {res["verdict"]} (change {res["change"]:+.2f} %, noise {res["noise"]:.2f} %; per layout A '
                + ' '.join(human(x) for x in res['per_layout']['a']) + ', B ' + ' '.join(human(x) for x in res['per_layout']['b']) + ')')
    for e in errors:
        say(f'verdict ERROR {e}')
    say(f'rule: {NOISE_RULE}')
    say(f'measure_packs: {pin_note}; load at start {load0[0]:.2f}, at end {load1[0]:.2f} (1-minute); samples {os.path.join(opts.work, "samples.tsv")}, '
        f'builds {os.path.join(opts.work, "builds.tsv")}')
    say.save()
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
