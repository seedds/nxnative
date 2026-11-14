#!/usr/bin/env python3
"""The recompiled corpus against the same C compiled natively for x86-64 (README.md, "Against native").

For every program: the native build (the corpus's C with tests/native_rt.c, clang -O2 -march=native and the corpus's flags that change code:
-ffreestanding, -fno-stack-protector, -ffp-contract=off, -fno-math-errno, -fPIE), its output checked against corpus/expected/<program>.out;
the packs of its NROs (the GCC and the clang build) at each tier (nxaot build --tier W, C, H, H+, no profile); then every variant run pinned to
one CPU in rounds (the order rotated each round), each sample the fastest of N runs in one process (nxaot_host --repeat N, the native
harness's --repeat N). On the development machine (a virtual machine on a hybrid CPU) a pinned process runs at one of two speeds, 1.7 to
2.4 times apart and changing from one run to the next, with no probe run beside it telling which (README.md, "Against native"): every
variant's samples fall in two groups, those within SPEED_GAP of its fastest sample (the fast speed) and the others; a variant's time is
the median of its fast group, the slow group's median is printed beside it, and ratios compare one speed with itself. Every run must print
the native build's lines. Printed: a Markdown table (program, native time, each tier's time and its ratio to native at the fast speed and
its ratio at the slow one, the .text of each against the native executable's), each variant's time at the slow speed and its sample counts,
the load; written to --work: results.md and samples.tsv (every sample).

  native_ratio.py --nxaot build/aot/nxaot --host build/aot/nxaot_host --work /tmp/ratio [--tiers W,C,H,H+] [--rounds 8] [--cpu 3]
                  [--args '--no-safepoints'] [--cc clang-18] [--workers 4] [program ...]

Programs: int_ops-gcc, int_ops-clang (int_ops_rounds = 20), switch-gcc, switch-clang, fnptr-gcc, fnptr-clang, bench (gen_bench.py
--functions 4000, built for the Switch with the corpus's clang flags, and natively). The others have no native build (NOT_NATIVE). int_ops is
built from a port of its source (INT_OPS_PORT): its six inline-assembly divisions and shifts become the C of the A64 results they check, and
arm_acle.h a header with the same CRC-32 (a 256-entry table a byte at a time, as nxaot lowers CRC32X / W / H / B), CRC-32C (SSE4.2's crc32),
rbit and rev16. Standard library only; it runs where nxaot, nxaot_host and clang run (the nxnative-aot image). Its unit test, without them:
python3 -m unittest dm/tools/aot/tests/test_native_ratio.py
"""
import argparse
import importlib.util
import os
import shlex
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
AOT = os.path.normpath(os.path.join(HERE, '..'))
CORPUS = os.path.join(AOT, 'corpus')
NATIVE_RT = os.path.join(HERE, 'native_rt.c')
GEN_BENCH = os.path.join(HERE, 'gen_bench.py')

_spec = importlib.util.spec_from_file_location('measure_packs', os.path.join(HERE, 'measure_packs.py'))
mp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mp)

TIERS = ('W', 'C', 'H', 'H+')

# The flags of corpus/compile.sh's COMMON that change code, for the host: -march=native for -mcpu=cortex-a57 (nxaot compiles for the host's CPU
# by default, design 9.3), -mno-outline-atomics left out (an AArch64 option)
NATIVE_CFLAGS = ['-O2', '-march=native', '-std=gnu11', '-ffreestanding', '-fPIE', '-fno-stack-protector', '-ffp-contract=off', '-fno-math-errno']
NATIVE_LDFLAGS = ['-pie', '-rdynamic', '-Wl,-z,now', '-Wl,-z,relro', '-ldl']

# The six inline-assembly statements of corpus/src/int_ops.c and the C of the A64 results they compute (udiv, sdiv by zero: 0; sdiv of the
# minimum by -1: the minimum; lslv / asrv / lsrv: the shift amount modulo the width); arm_acle.h becomes nxaot_native_acle.h (NATIVE_ACLE)
INT_OPS_PORT = [
    ('#include <arm_acle.h>', '#include "nxaot_native_acle.h"'),
    ('__asm__("udiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));', 'r = b ? a / b : 0;'),
    ('__asm__("sdiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));',
     'r = b == 0 ? 0 : (a == (int64_t)0x8000000000000000ull && b == -1) ? a : a / b;'),
    ('__asm__("sdiv %w0, %w1, %w2" : "=r"(r) : "r"(a), "r"(b));',
     'r = (int32_t)b == 0 ? 0 : ((int32_t)a == (int32_t)0x80000000u && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b);'),
    ('__asm__("lslv %0, %1, %2" : "=r"(r) : "r"(x), "r"(n));', 'r = x << (n & 63);'),
    ('__asm__("asrv %0, %1, %2" : "=r"(r) : "r"(x), "r"(n));', 'r = (uint64_t)((int64_t)x >> (n & 63));'),
    ('__asm__("lsrv %w0, %w1, %w2" : "=r"(r) : "r"(x), "r"(n));', 'r = x >> (n & 31);'),
]


def crc32_table():
    out = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
        out.append(c)
    return out


def native_acle():
    table = ', '.join(f'0x{c:08x}u' for c in crc32_table())
    return f'''// Written by native_ratio.py: what int_ops.c takes from arm_acle.h, for the host
#include <stdint.h>
static const uint32_t nxaot_crc32_table[256] = {{{table}}};
static inline uint32_t nxaot_crc32_bytes(uint32_t crc, uint64_t data, unsigned bytes) {{
    for (unsigned i = 0; i < bytes; ++i) crc = nxaot_crc32_table[(crc ^ (uint32_t)(data >> (8 * i))) & 0xff] ^ (crc >> 8);
    return crc;
}}
static inline uint32_t __crc32b(uint32_t c, uint8_t d) {{ return nxaot_crc32_bytes(c, d, 1); }}
static inline uint32_t __crc32h(uint32_t c, uint16_t d) {{ return nxaot_crc32_bytes(c, d, 2); }}
static inline uint32_t __crc32w(uint32_t c, uint32_t d) {{ return nxaot_crc32_bytes(c, d, 4); }}
static inline uint32_t __crc32d(uint32_t c, uint64_t d) {{ return nxaot_crc32_bytes(c, d, 8); }}
static inline uint32_t __crc32cb(uint32_t c, uint8_t d) {{ return __builtin_ia32_crc32qi(c, d); }}
static inline uint32_t __crc32ch(uint32_t c, uint16_t d) {{ return __builtin_ia32_crc32hi(c, d); }}
static inline uint32_t __crc32cw(uint32_t c, uint32_t d) {{ return __builtin_ia32_crc32si(c, d); }}
static inline uint32_t __crc32cd(uint32_t c, uint64_t d) {{ return (uint32_t)__builtin_ia32_crc32di(c, d); }}
static inline uint64_t __rbitll(uint64_t x) {{ return __builtin_bitreverse64(x); }}
static inline uint64_t __rev16ll(uint64_t x) {{ return ((x & 0x00ff00ff00ff00ffull) << 8) | ((x >> 8) & 0x00ff00ff00ff00ffull); }}
'''


# program: its sources (relative to corpus/), its flags beside NATIVE_CFLAGS (corpus/compile.sh's flags()), the port of its source, the
# nxaot_host / native --set values and the runs per sample (each sample the fastest of them)
PROGRAMS = {
    'int_ops': {'sources': ['src/int_ops.c'], 'flags': ['-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-msse4.2'],
                'port': INT_OPS_PORT, 'sets': ['int_ops_rounds=20'], 'repeats': 100},
    'switch': {'sources': ['src/switch.c', 'src/switch_word.c'], 'flags': ['-fasynchronous-unwind-tables'], 'repeats': 5000},
    'fnptr': {'sources': ['src/fnptr.c'], 'flags': ['-fasynchronous-unwind-tables', '-fexceptions'], 'repeats': 20000},
    'bench': {'sources': None, 'flags': ['-fasynchronous-unwind-tables'], 'repeats': 1000},
}
DEFAULT_PROGRAMS = ['int_ops-gcc', 'int_ops-clang', 'switch-gcc', 'switch-clang', 'fnptr-gcc', 'fnptr-clang', 'bench']

NOT_NATIVE = {
    'simd': 'NEON intrinsics (arm_neon.h): no x86 build without a NEON-to-SSE layer',
    'fp': 'every operation under test is A64 inline assembly, and its run is the JIT fallback\'s (msr fpcr)',
    'sjlj': 'setjmp / longjmp of rt/setjmp.S (A64 assembly) and A64 register variables',
    'excl': 'exclusives in A64 inline assembly and Horizon\'s thread SVCs',
    'except': 'the corpus\'s own unwinder for A64 frames (rt/unwind.S)',
    'fiber': 'stack switching in A64 assembly (rt/fiber.S)',
}


class RatioError(Exception):
    pass


def port_source(text, port):
    """Apply each (old, new) replacement, which must match exactly once."""
    for old, new in port:
        n = text.count(old)
        if n != 1:
            raise RatioError(f'the port expects {old!r} once in the source, found it {n} times')
        text = text.replace(old, new)
    return text


def parse_fastest(stderr):
    """(runs, fastest seconds) from nxaot_host's or the native harness's standard error."""
    m = mp.FASTEST_RE.search(stderr)
    if not m:
        raise RatioError('no "fastest of N: T s" in the standard error')
    return int(m.group(1)), float(m.group(2))


def split_label(label):
    """int_ops-gcc -> (int_ops, gcc); bench -> (bench, None)."""
    for tc in ('-gcc', '-clang'):
        if label.endswith(tc):
            return label[:-len(tc)], tc[1:]
    return label, None


SPEED_GAP = 1.3  # a variant's samples at the fast speed lie within 1.3 times its fastest; the slow speed is 1.7 to 2.4 times slower


def split_speeds(samples):
    """A variant's samples -> (at the fast speed, at the slow speed): within SPEED_GAP of the fastest sample, and the others (none when every
    sample is within it: one speed, taken for the fast one, which a variant meets in most runs of a few rounds)."""
    lo = min(samples)
    return [t for t in samples if t <= lo * SPEED_GAP], [t for t in samples if t > lo * SPEED_GAP]


def summarize(samples):
    """samples: {variant: [seconds]} -> {variant: {'fast': median, 'slow': median or None, 'n': {speed: count}}}."""
    out = {}
    for v, s in samples.items():
        if not s:
            continue
        fast, slow = split_speeds(s)
        out[v] = {'fast': mp.median(fast), 'slow': mp.median(slow) if slow else None, 'n': {'fast': len(fast), 'slow': len(slow)}}
    return out


def ratio_row(label, stats, texts, tiers):
    """One Markdown row: program, native, each tier's time and ratio at the fast speed and its ratio at the slow one, the .text ratios."""
    nat = stats['native']
    ratio = lambda st, sp: f'{st[sp] / nat[sp]:.2f}x' if st.get(sp) and nat.get(sp) else '-'
    cells = [label, mp.human(nat['fast'])]
    for t in tiers:
        st = stats.get(t)
        if st:
            cells.append(f'{mp.human(st["fast"])} ({ratio(st, "fast")} / {ratio(st, "slow")})')
        else:
            cells.append('-')
    ntext = texts.get('native')
    cells.append(f'{ntext:,}' if ntext else '-')
    cells.append(' / '.join(f'{texts[t] / ntext:.1f}' if ntext and texts.get(t) else '-' for t in tiers))
    return '| ' + ' | '.join(cells) + ' |'


def table_header(tiers):
    head = ['Program', 'native'] + [f'tier {t} (ratio / at the slow speed)' for t in tiers] + ['native .text', '.text ratio ' + ' / '.join(tiers)]
    return ['| ' + ' | '.join(head) + ' |', '|---|' + '---:|' * (len(head) - 1)]


# Builds --------------------------------------------------------------------------------------------------------------------------------------

def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, **kw)
    if p.returncode != 0:
        raise RatioError(f'{shlex.join(cmd)} exited {p.returncode}: {(p.stderr or p.stdout).decode(errors="replace").strip()[-2000:]}')
    return p


def build_bench(opts):
    out = os.path.join(opts.work, 'bench')
    nro = os.path.join(out, 'bench.nro')
    if not os.path.exists(nro):
        run([sys.executable, GEN_BENCH, '--functions', str(opts.bench_functions), out])
    return nro, os.path.join(out, 'bench.c')


def build_native(opts, program, bench_c=None):
    spec = PROGRAMS[program]
    d = os.path.join(opts.work, 'native', program)
    os.makedirs(d, exist_ok=True)
    srcs = []
    if program == 'bench':
        srcs = [bench_c]
    for s in spec['sources'] or []:
        path = os.path.join(CORPUS, s)
        if spec.get('port'):
            with open(path) as f:
                text = port_source(f.read(), spec['port'])
            path = os.path.join(d, os.path.basename(s))
            with open(path, 'w') as f:
                f.write(text)
            with open(os.path.join(d, 'nxaot_native_acle.h'), 'w') as f:
                f.write(native_acle())
        srcs.append(path)
    exe = os.path.join(d, program)
    objs = []
    for s in srcs:
        o = os.path.join(d, os.path.basename(s) + '.o')
        run([opts.cc] + NATIVE_CFLAGS + spec['flags'] + ['-I', d, '-I', os.path.join(CORPUS, 'rt'), '-Dmain=nxaot_corpus_main', '-c', s,
                                                          '-o', o])
        objs.append(o)
    o = os.path.join(d, 'native_rt.o')
    run([opts.cc, '-O2', '-march=native', '-fPIE', '-I', os.path.join(CORPUS, 'rt'), '-c', NATIVE_RT, '-o', o])
    run([opts.cc] + NATIVE_LDFLAGS[:1] + ['-o', exe] + objs + [o] + NATIVE_LDFLAGS[1:])
    return exe


def module_of(label, bench_nro):
    return bench_nro if label == 'bench' else os.path.join(CORPUS, 'nro', label + '.nro')


def build_pack(opts, label, module, tier):
    d = os.path.join(opts.work, 'packs', tier.replace('+', 'p'))
    os.makedirs(d, exist_ok=True)
    so = os.path.join(d, label + '.so')
    cmd = [opts.nxaot, 'build', module, '--shared', so, '--tier', tier, '--workers', str(opts.workers)] + shlex.split(opts.args)
    t0 = time.monotonic()
    with open(so + '.build.log', 'wb') as log:
        p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
        _, status, ru = os.wait4(p.pid, 0)
    if os.waitstatus_to_exitcode(status) != 0:
        raise RatioError(f'{shlex.join(cmd)} failed (log {so}.build.log)')
    return so, time.monotonic() - t0, ru.ru_maxrss


# Runs ------------------------------------------------------------------------------------------------------------------------------------------

def run_sample(opts, cmd):
    try:
        p = subprocess.run(opts.pin + cmd, capture_output=True, timeout=600)
    except subprocess.TimeoutExpired:
        raise RatioError(f'{shlex.join(cmd)} still running after 600 s')
    if p.returncode != 0:
        tail = (p.stdout.decode(errors='replace').strip().splitlines() or ['-'])[-1]
        raise RatioError(f'{shlex.join(cmd)} exited {p.returncode}: {tail}')
    return p.stdout, parse_fastest(p.stderr.decode(errors='replace'))[1]


def first_difference(got, want):
    g = got.decode(errors='replace').splitlines()
    w = want.decode(errors='replace').splitlines()
    k = next((i for i, (a, b) in enumerate(zip(g, w)) if a != b), min(len(g), len(w)))
    return f'line {k + 1}: {g[k] if k < len(g) else "<none>"!r}, want {w[k] if k < len(w) else "<none>"!r}'


def main(argv=None):
    ap = argparse.ArgumentParser(prog='native_ratio.py', description=__doc__.splitlines()[0])
    ap.add_argument('programs', nargs='*', help='corpus NRO names (int_ops-gcc) or bench; default: ' + ' '.join(DEFAULT_PROGRAMS))
    ap.add_argument('--nxaot', required=True)
    ap.add_argument('--host', required=True, help='nxaot_host')
    ap.add_argument('--work', required=True)
    ap.add_argument('--cc', default='clang-18')
    ap.add_argument('--tiers', default=','.join(TIERS), help='comma-separated tiers (default W,C,H,H+)')
    ap.add_argument('--args', default='', help='extra nxaot build arguments (one string, shell-split): --no-safepoints for an ablation')
    ap.add_argument('--workers', type=int, default=4, help='nxaot build --workers (default 4)')
    ap.add_argument('--rounds', type=int, default=8)
    ap.add_argument('--repeats-scale', type=float, default=1.0, help='scale every program\'s runs per sample')
    ap.add_argument('--cpu', default=None, help='the CPU to pin to (default: the last one this process may use); "none": unpinned')
    ap.add_argument('--bench-functions', type=int, default=4000)
    opts = ap.parse_args(argv)
    labels = opts.programs or DEFAULT_PROGRAMS
    tiers = [t for t in opts.tiers.split(',') if t]
    for t in tiers:
        if t not in TIERS:
            ap.error(f'unknown tier {t}')
    for label in labels:
        program, _ = split_label(label)
        if program in NOT_NATIVE:
            ap.error(f'{label}: no native build: {NOT_NATIVE[program]}')
        if program not in PROGRAMS:
            ap.error(f'{label}: unknown program')
    opts.work = os.path.abspath(opts.work)
    os.makedirs(opts.work, exist_ok=True)
    if opts.cpu is None:
        opts.cpu = str(max(os.sched_getaffinity(0)))
    taskset = shutil.which('taskset')
    opts.pin = [] if opts.cpu == 'none' or not taskset else [taskset, '-c', opts.cpu]
    say = mp.Report(os.path.join(opts.work, 'results.md'))
    load0 = os.getloadavg()
    say(f'native_ratio: nxaot {opts.nxaot} {opts.args}'.rstrip() + f'; {opts.cc} ' + ' '.join(NATIVE_CFLAGS))
    say(f'native_ratio: {"pinned to CPU " + opts.cpu if opts.pin else "NOT pinned"}; {opts.rounds} rounds; load at start '
        f'{load0[0]:.2f} {load0[1]:.2f} {load0[2]:.2f}')

    bench_nro = bench_c = None
    if 'bench' in labels:
        bench_nro, bench_c = build_bench(opts)
    natives, references = {}, {}
    for label in labels:
        program, _ = split_label(label)
        if program in natives:
            continue
        exe = build_native(opts, program, bench_c)
        natives[program] = exe
        expected = os.path.join(CORPUS, 'expected', program + '.out')
        if os.path.exists(expected):
            out, _ = run_sample(opts, [exe])
            with open(expected, 'rb') as f:
                want = f.read()
            if out != want:
                raise RatioError(f'{program}: the native build prints other lines than {expected} ({first_difference(out, want)})')
            say(f'native_ratio: {program}: the native build prints {os.path.relpath(expected, AOT)}')

    packs, builds, texts = {}, {}, {}
    for label in labels:
        program, _ = split_label(label)
        texts[label] = {'native': mp.elf_text_size(natives[program])}
        for t in tiers:
            so, wall, peak = build_pack(opts, label, module_of(label, bench_nro), t)
            packs[(label, t)] = so
            builds[(label, t)] = (wall, peak)
            texts[label][t] = mp.elf_text_size(so)

    samples = {label: {v: [] for v in ['native'] + tiers} for label in labels}
    native_samples = {}
    tsv = open(os.path.join(opts.work, 'samples.tsv'), 'w')
    tsv.write('# program\tround\tvariant\truns\tfastest seconds\n')
    for r in range(opts.rounds):
        for label in labels:
            program, _ = split_label(label)
            spec = PROGRAMS[program]
            reps = max(1, int(spec['repeats'] * opts.repeats_scale))
            sets = sum((['--set', s] for s in spec.get('sets', [])), [])
            order = ['native'] + tiers
            order = order[r % len(order):] + order[:r % len(order)]
            for v in order:
                if v == 'native':
                    if (program, r) not in native_samples:
                        out, t = run_sample(opts, [natives[program], '--repeat', str(reps)] + sets)
                        native_samples[(program, r)] = t
                        references.setdefault(program, out)
                        tsv.write(f'{program}\t{r + 1}\tnative\t{reps}\t{t:.9f}\n')
                    samples[label]['native'].append(native_samples[(program, r)])
                    continue
                out, t = run_sample(opts, [opts.host, packs[(label, v)], module_of(label, bench_nro), '--repeat', str(reps)] + sets)
                if program not in references:
                    references[program] = out
                elif out != references[program]:
                    raise RatioError(f'{label} at tier {v} prints other lines than the native build ({first_difference(out, references[program])})')
                samples[label][v].append(t)
                tsv.write(f'{label}\t{r + 1}\t{v}\t{reps}\t{t:.9f}\n')
            tsv.flush()
    tsv.close()
    load1 = os.getloadavg()

    say()
    for line in table_header(tiers):
        say(line)
    stats_all = {}
    for label in labels:
        stats = summarize(samples[label])
        stats_all[label] = stats
        say(ratio_row(label, stats, texts[label], tiers))
    say()
    for label in labels:
        stats = stats_all[label]
        slow = ', '.join(f'{v} {mp.human(stats[v]["slow"]) if stats[v]["slow"] else "-"}' for v in ['native'] + tiers)
        counts = ', '.join(f'{v} {stats[v]["n"]["fast"]} / {stats[v]["n"]["slow"]}' for v in ['native'] + tiers)
        pack = ', '.join(f'{t} {builds[(label, t)][0]:.2f} s {builds[(label, t)][1] / 1024:.0f} MiB' for t in tiers)
        say(f'native_ratio: {label}: at the slow speed {slow}; samples fast / slow {counts}; packs (wall, peak) {pack}')
    say(f'native_ratio: load at start {load0[0]:.2f}, at end {load1[0]:.2f} (1-minute)')
    say.save()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RatioError as e:
        print(f'native_ratio: ERROR {e}', file=sys.stderr)
        sys.exit(1)
