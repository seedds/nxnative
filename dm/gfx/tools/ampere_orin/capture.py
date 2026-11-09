#!/usr/bin/env python3
"""Records the fixture of dm/gfx/tests/test_dmsh_ampere_orin.py on a Linux machine with an Ampere GPU and its NVIDIA driver (a Jetson Orin, sm_87, or a GeForce
RTX 30, sm_86). Needs no CUDA toolkit there, only Python 3 and the driver's libcuda: the driver compiles each op's PTX itself.

For each op in ops.py: a PTX kernel that loads its inputs from global memory, does the op and stores its outputs. On the device (over ssh) the driver compiles
and runs it on the inputs made here, and prog.py rewrites the same machine code for the translator (LDG / STG become LDS / STS, the CUDA parameter reads and
the addressing they feed become NOPs), which the device's nvdisasm checks. The fixture keeps, per op, that program, its disassembly, the inputs and the GPU's
outputs.

  capture.py --host user@machine [--arch sm_87] [--cases 1024] [--only names,families] [--out dm/gfx/tests/ampere_orin]

The device needs nvdisasm in --device-dir (NVIDIA's cuda_nvdisasm redistributable for linux-aarch64 or linux-x86_64; no install). PTX is written here, so no
compiler is needed on either side. Needs numpy here (.github/requirements/tools.txt)."""
import argparse
import gzip
import json
import re
import subprocess
import sys
from pathlib import Path

import numpy as np  # pyright: ignore[reportMissingImports]  # numpy is in .github/requirements/tools.txt, not in the lint environment

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ops import OPS  # noqa: E402

FIXTURE = HERE.parents[1] / 'tests' / 'ampere_orin'

F32_SPECIAL = [0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x00000100, 0x3f800000, 0xbf800000, 0x3f000000, 0x7f7fffff, 0xff7fffff,
               0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0x7fffffff, 0x4b800000, 0x4b800001, 0x3effffff, 0x3f7fffff, 0x40490fdb,
               0x4f000000, 0xcf000000, 0x4f800000, 0x5f000000, 0x3fc00000, 0x40200000, 0xbfc00000, 0x33800000, 0x38800000, 0x477fe000, 0x477ff000,
               0x33000000, 0x3f800001, 0x4effffff, 0xceffffff, 0x3f7ffffe, 0x4b000000, 0xcb000000, 0x4b7fffff, 0x3ea2f983, 0x40c90fdb, 0x42b17218]
F16_SPECIAL = [0x0000, 0x8000, 0x0001, 0x03ff, 0x0400, 0x3c00, 0xbc00, 0x3800, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e00, 0x7c01, 0x7fff, 0xfe00,
               0x3555, 0x3bff, 0x3c01, 0x0200, 0x8001, 0x6400, 0x4248, 0x5640]
BF16_SPECIAL = [0x0000, 0x8000, 0x0001, 0x007f, 0x0080, 0x3f80, 0xbf80, 0x3f00, 0x7f7f, 0xff7f, 0x7f80, 0xff80, 0x7fc0, 0x7f81, 0x7fff, 0x4049]
U32_SPECIAL = [0, 1, 2, 3, 7, 0xffffffff, 0xfffffffe, 0x80000000, 0x7fffffff, 0x7ffffffe, 0x80000001, 0xff, 0x100, 0xffff, 0x10000, 0x55555555,
               0xaaaaaaaa, 31, 32, 33, 63, 64, 0xffffffe1, 0x00ff00ff, 0x12345678, 0x80808080, 0x7f7f7f7f]


def _halves(kind, m, rng, tame):
    def pack(v):
        return (v.astype(np.float16).view(np.uint16) if kind == 'h' else (v.astype(np.float32).view(np.uint32) >> 16).astype(np.uint16)).astype(np.uint32)
    if tame:
        return pack(rng.uniform(-1, 1, m))
    p = rng.random(m)
    a = rng.choice(np.array(F16_SPECIAL if kind == 'h' else BF16_SPECIAL, np.uint32), m)
    b = rng.integers(0, 2 ** 16, m).astype(np.uint32)
    c = pack(rng.uniform(-4, 4, m))
    d = pack(rng.choice([-1.0, 1.0], m) * np.exp2(rng.uniform(-14, 15, m)))
    return np.select([p < .3, p < .45, p < .75], [a, b, c], d).astype(np.uint32)


def gen_kind(kind, n, rng):
    """n input words of one kind (ops.py): special values, random bits and ordinary values mixed; an upper-case kind gives ordinary values only"""
    tame, k = kind.isupper(), kind.lower()
    pick = rng.random(n)
    rand = rng.integers(0, 2 ** 32, n, dtype=np.uint64).astype(np.uint32)
    if k == 'f':
        if tame:
            return rng.uniform(-2, 2, n).astype(np.float32).view(np.uint32)
        a = rng.choice(np.array(F32_SPECIAL, np.uint32), n)
        c = rng.uniform(-8, 8, n).astype(np.float32).view(np.uint32)
        d = (rng.choice([-1.0, 1.0], n) * np.exp2(rng.uniform(-30, 30, n))).astype(np.float32).view(np.uint32)
        e = (rng.integers(-4, 5, n) * 0.5).astype(np.float32).view(np.uint32)   # halves and whole numbers: the ties of the roundings
        return np.select([pick < .25, pick < .4, pick < .65, pick < .85], [a, rand, c, d], e).astype(np.uint32)
    if k in 'hb':
        return (_halves(k, n, rng, tame) | _halves(k, n, rng, tame) << 16).astype(np.uint32)
    if k == 'u':
        if tame:
            return rand
        a = rng.choice(np.array(U32_SPECIAL, np.uint32), n)
        c = rng.integers(-1000, 1000, n).astype(np.int32).view(np.uint32)
        return np.select([pick < .3, pick < .7], [a, rand], c).astype(np.uint32)
    if k == 's':
        a = rng.integers(0, 41, n).astype(np.uint32)
        c = rng.choice(np.array([0, 1, 31, 32, 33, 63, 64, 255, 256, 0xffffffff, 0x80000000, 0xffffffe0], np.uint32), n)
        return np.select([pick < .7, pick < .85], [a, rand], c).astype(np.uint32)
    if k == 'l':
        return np.where(pick < .8, rng.integers(0, 32, n).astype(np.uint32), rand).astype(np.uint32)
    if k == 'p':   # whole warps of one value now and then, so all / any / uni see both answers
        same = np.repeat(rng.random(n // 32) < .3, 32)
        return np.where(same, np.repeat(rng.integers(0, 2 ** 32, n // 32, dtype=np.uint64).astype(np.uint32), 32), rand).astype(np.uint32)
    raise ValueError(kind)


PTX_HEAD = '''.version 8.5
.target {arch}
.address_size 64
.visible .entry k(.param .u64 pin, .param .u64 pout)
{
  .reg .b32 %i<{nin}>, %o<{nout}>, %t<8>, %r<8>, %x<8>;
  .reg .b16 %y<8>, %w<8>;
  .reg .f32 %f<8>;
  .reg .b64 %d<8>, %a<4>;
  .reg .f64 %g<4>;
  .reg .pred %p<4>;
  mov.u32 %r0, %tid.x; mov.u32 %r1, %ctaid.x; shl.b32 %r1, %r1, 5; add.u32 %r2, %r1, %r0;
  ld.param.u64 %a0, [pin]; cvta.to.global.u64 %a0, %a0; mul.wide.u32 %a1, %r2, {inb}; add.u64 %a0, %a0, %a1;
'''


def ptx_for(o, arch):
    """the op's kernel: volatile loads and stores, so the compiler keeps one 32-bit LDG / STG a word, which prog.py replaces one for one"""
    nin, nout = len(o['ins']), len(o['outs'])
    s = PTX_HEAD.replace('{arch}', arch).replace('{nin}', str(max(nin, 1))).replace('{nout}', str(nout)).replace('{inb}', str(nin * 4))
    s += ''.join(f'  ld.volatile.global.b32 %i{k}, [%a0+{k * 4}];\n' for k in range(nin))
    s += '  ' + o['body'] + '\n'
    s += f'  ld.param.u64 %a2, [pout]; cvta.to.global.u64 %a2, %a2; mul.wide.u32 %a3, %r2, {nout * 4}; add.u64 %a2, %a2, %a3;\n'
    s += ''.join(f'  st.volatile.global.b32 [%a2+{k * 4}], %o{k};\n' for k in range(nout))
    return s + '  ret;\n}\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', required=True, help='ssh destination of the machine with the Ampere GPU')
    ap.add_argument('--arch', default='sm_87', help='the GPU\'s PTX target: sm_87 (Jetson Orin), sm_86 (GeForce RTX 30)')
    ap.add_argument('--cases', type=int, default=1024, help='cases an op, a multiple of 32 (one warp a group, as on the Metal side)')
    ap.add_argument('--only', default='', help='op names or families, comma-separated')
    ap.add_argument('--out', default=str(FIXTURE))
    ap.add_argument('--device-dir', default='ampere_orin', help='folder on the device (relative to its home) holding nvdisasm; the run uses <it>/run')
    ap.add_argument('--work', default='', help='local folder for the device\'s files (default: a temporary one)')
    args = ap.parse_args()
    if args.cases % 32:
        sys.exit('--cases must be a multiple of 32')
    want = set(args.only.split(',')) if args.only else None
    ops = [o for o in OPS if not want or o['name'] in want or o['family'] in want]
    import tempfile
    with tempfile.TemporaryDirectory(prefix='ampere_orin_') as tmp:
        work = Path(args.work or tmp)
        work.mkdir(parents=True, exist_ok=True)
        man = []
        for idx, o in enumerate(ops):
            rng = np.random.default_rng(0x5eed + idx * 7919 + sum(map(ord, o['name'])))
            data = np.stack([gen_kind(k, args.cases, rng) for k in o['ins']], 1)
            (work / f"{o['name']}.ptx").write_text(ptx_for(o, args.arch))
            data.astype('<u4').tofile(work / f"{o['name']}.in.bin")
            man.append(dict(name=o['name'], family=o['family'], ins=o['ins'], outs=o['outs'], nin=len(o['ins']), nout=len(o['outs']), groups=args.cases // 32))
        (work / 'manifest.json').write_text(json.dumps(man))
        run = f'{args.device_dir}/run'
        ssh = ['ssh', '-o', 'BatchMode=yes', args.host]
        subprocess.run(ssh + [f'rm -rf {run} && mkdir -p {run} && cp {args.device_dir}/nvdisasm {run}/'], check=True)
        files = [str(HERE / f) for f in ('cu.py', 'prog.py', 'batch.py')] + [str(work / 'manifest.json')]
        files += [str(work / f"{o['name']}{e}") for o in man for e in ('.ptx', '.in.bin')]
        subprocess.run(['scp', '-q', *files, f'{args.host}:{run}/'], check=True)
        r = subprocess.run(ssh + [f'cd {run} && python3 batch.py manifest.json'], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        tar = subprocess.Popen(ssh + [f'cd {run} && tar cf - *.orin.bin *.prog.bin *.prog.sass *.err device.info 2>/dev/null'], stdout=subprocess.PIPE)
        subprocess.run(['tar', 'xf', '-', '-C', str(work)], stdin=tar.stdout, check=False)
        tar.wait()
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        write_fixture(out, work, man, ops, args.arch)


def trimmed_sass(text):
    """a program's disassembly without the hex column and the NOP padding after its last EXIT"""
    lines = [ln for ln in text.splitlines() if ln.startswith('//') or '/*0' in ln]
    lines = [re.sub(r'\s*/\* 0x[0-9a-f]+ \*/', '', ln).rstrip() for ln in lines]
    while lines and lines[-1].rstrip(' ;').endswith('NOP'):
        lines.pop()
    out, run = [], []
    for ln in lines + ['']:
        if ln.rstrip(' ;').endswith('NOP'):
            run.append(ln)
            continue
        out += run if len(run) < 3 else [run[0], f'        ...   ({len(run) - 2} more NOPs)', run[-1]]
        run = []
        out.append(ln)
    return out[:-1]


def write_fixture(out, work, man, ops, arch):
    """out/manifest.json (the device; per op its kinds, PTX body, case count and where its words lie), out/ampere_orin.u32.gz (per op: the program, then its
    cases, a row of input words and the GPU's output words each) and out/ampere_orin.sass (each program's disassembly). Ops this run did not record keep their
    old entries"""
    info = json.loads((work / 'device.info').read_text()) if (work / 'device.info').exists() else {}
    old_doc = json.loads((out / 'manifest.json').read_text()) if (out / 'manifest.json').exists() else {'ops': []}
    old_blob = gzip.decompress((out / 'ampere_orin.u32.gz').read_bytes()) if (out / 'ampere_orin.u32.gz').exists() else b''
    old_sass = {}
    if (out / 'ampere_orin.sass').exists():
        for part in (out / 'ampere_orin.sass').read_text().split('\n// == ')[1:]:
            name, _, body = part.partition('\n')
            old_sass[name.strip()] = body.rstrip('\n').splitlines()
    entries = {}
    for o in old_doc.get('ops', []):
        if 'prog' not in o:
            continue
        prog = old_blob[o['prog'][0]:o['prog'][0] + o['prog'][1]]
        rows = old_blob[o['rows'][0]:o['rows'][0] + o['rows'][1]]
        entries[o['name']] = ({k: v for k, v in o.items() if k not in ('prog', 'rows')}, prog, rows, old_sass.get(o['name'], []))
    for o in man:
        n = o['name']
        if not (work / f'{n}.orin.bin').exists() or not (work / f'{n}.prog.bin').exists():
            err = (work / f'{n}.err').read_text().splitlines()[0] if (work / f'{n}.err').exists() else 'no result'
            print(f'{n}: left out ({err})', file=sys.stderr)
            continue
        ins = np.fromfile(work / f'{n}.in.bin', '<u4').reshape(-1, o['nin'])
        outs = np.fromfile(work / f'{n}.orin.bin', '<u4').reshape(-1, o['nout'])
        meta = {**{k: o[k] for k in ('name', 'family', 'ins', 'outs')}, 'cases': len(ins), 'ptx': next(x['body'] for x in ops if x['name'] == n)}
        entries[n] = (meta, (work / f'{n}.prog.bin').read_bytes(), np.concatenate([ins, outs], 1).astype('<u4').tobytes(),
                      trimmed_sass((work / f'{n}.prog.sass').read_text()))
    order = [o['name'] for o in OPS]
    blob, doc_ops, sass = bytearray(), [], []
    for n in sorted(entries, key=lambda k: order.index(k) if k in order else len(order)):
        meta, prog, rows, lines = entries[n]
        doc_ops.append({**meta, 'prog': [len(blob), len(prog)], 'rows': [len(blob) + len(prog), len(rows)]})
        blob += prog + rows
        sass.append(f'// == {n}\n' + '\n'.join(lines))
    doc = {'device': info or old_doc.get('device', {}), 'arch': arch, 'ops': doc_ops}
    (out / 'manifest.json').write_text(json.dumps(doc, indent=1) + '\n')
    (out / 'ampere_orin.u32.gz').write_bytes(gzip.compress(bytes(blob), 9, mtime=0))
    (out / 'ampere_orin.sass').write_text("// Each op's program as the translator gets it (dm/gfx/tools/ampere_orin/capture.py): the GPU compiler's machine code with its global\n"
                                           "// loads and stores made shared memory ones, disassembled by nvdisasm on the device. Thread t's words lie at shared memory word t * W\n"
                                           '// (W = inputs + outputs): its inputs, then its outputs.\n\n' + '\n\n'.join(sass) + '\n')
    print(f'{len(doc_ops)} ops in {out}')


if __name__ == '__main__':
    main()
