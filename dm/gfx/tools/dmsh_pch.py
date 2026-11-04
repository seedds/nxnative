#!/usr/bin/env python3
# A precompiled header for the shader libraries' Metal compiles (dmsh_build.py mac_compile, ios_metal.compile). Each compile is a new metal process, and most of its front end was instantiating
# Metal's texture classes again: about 90 ms for each of the argument buffer's texture types and for its memory_coherence_device copy, 80% of a typical shader's front end (GitHub issue #1036).
# The module cache holds those templates, not their instantiations; this header holds the instantiations, so a compile reads them instead of making them.
# The translated source stays as it was: the header only adds the two includes the sources start with and completes classes, and declares no name. A class left out of CLASSES costs only its
# time. The output is byte-identical (checked on a shader of every kind in make(), so a compiler that behaves otherwise builds without the header), so -include-pch is not in the compile records
# (dmsh_build.py MAC_RECORD, ios_metal.RECORD), and like dmsh_mtlcache.py this file is not part of the chain version: it never changes the code in an output, it only makes one faster and keeps the source's name out of it (_run).
# One header per compile convention, made at the first compile of a process (about 2 s) and removed when it ends. Environment: DMSH_PCH=0 compiles without it.
import atexit, os, shutil, subprocess, sys, tempfile, threading

SAMPLED = ('texture2d', 'texture2d_array', 'texture3d', 'texturecube', 'texturecube_array', 'depth2d', 'depth2d_array', 'depthcube', 'depthcube_array')
WRITTEN = ('texture2d', 'texture2d_array', 'texture3d')
CLASSES = ([f'metal::{k}<float, metal::access::sample, metal::memory_coherence_{c}>' for k in SAMPLED for c in ('threadgroup', 'device')] +
           [f'metal::{k}<float, metal::access::write, metal::memory_coherence_threadgroup>' for k in WRITTEN])   # what shaders of eight games instantiated (-ftime-trace): float, sampled or written
HEADER = '#include <metal_stdlib>\n#include <simd/simd.h>\n' + ''.join(f'static_assert(sizeof({c}) > 0);\n' for c in CLASSES)
PROBE = '''#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;
struct set0 { texture2d<float> a [[id(0)]]; texture2d_array<float> b [[id(1)]]; texture3d<float> c [[id(2)]]; texturecube<float> d [[id(3)]]; depth2d_array<float> e [[id(4)]]; sampler s [[id(5)]]; };
struct main0_in { float4 uv [[user(locn0)]]; };
fragment float4 main0(main0_in in [[stage_in]], constant set0& t [[buffer(0)]])
{
    return t.a.sample(t.s, in.uv.xy) + t.b.sample(t.s, in.uv.xy, 0u) + t.c.sample(t.s, in.uv.xyz) + t.d.sample(t.s, in.uv.xyz) + t.e.sample(t.s, in.uv.xy, 0u);
}
'''
_lock, _headers = threading.Lock(), {}


def make(metal):
    """The header for a compile convention (the metal command up to its source, with -c), checked on PROBE. Returns its path, or (None, why it is not used)"""
    d = tempfile.mkdtemp(prefix='pch_', dir=os.environ.get('WORK'))   # not dmsh_*: that is the build's own temporary folder, which the NSP script's progress looks for
    atexit.register(shutil.rmtree, d, ignore_errors=True)
    h, pch, src = os.path.join(d, 'prefix.h'), os.path.join(d, 'prefix.pch'), os.path.join(d, 'probe.metal')
    with open(h, 'w') as f: f.write(HEADER)
    with open(src, 'w') as f: f.write(PROBE)
    r = subprocess.run([*(a for a in metal if a != '-c'), '-x', 'metal-header', h, '-o', pch], capture_output=True, text=True)
    if r.returncode: return None, f'it does not build: {r.stderr.strip()[:200]}'
    outs = []
    for extra, name in (((), 'plain.air'), (('-include-pch', pch), 'header.air')):
        out = os.path.join(d, name)
        r = subprocess.run([*metal, *extra, src, '-o', out], capture_output=True, text=True)
        if r.returncode: return None, f'a test shader does not compile: {r.stderr.strip()[:200]}'
        with open(out, 'rb') as f: outs.append(f.read())
    if outs[0] != outs[1]: return None, 'a test shader compiled with it differs'
    return pch, None


def header(metal):   # the path of the convention's header, None when it is not used (said once)
    if os.environ.get('DMSH_PCH') == '0': return None
    with _lock:   # the other workers wait for it: compiling without it would be slower than the wait
        if tuple(metal) not in _headers:
            pch, why = make(metal)
            if why: print(f'note: no precompiled Metal header for {" ".join(metal)}, so each compile takes longer: {why}', flush=True)
            _headers[tuple(metal)] = pch
        return _headers[tuple(metal)]


def _run(metal, msl, air, **kw):
    # The source on standard input, so no file name reaches the compiler: clang names a source's static initializer after its file (_GLOBAL__sub_I_<name>, which any function constant brings),
    # so the same text under two names gave two libraries, and shaders sharing a source (3,936 of Breath of the Wild's 10,560 pixel shaders, in their _bl) took the bytes of whichever compiled first
    with open(msl, 'rb') as f: return subprocess.run([*metal, '-x', 'metal', '-', '-o', air], stdin=f, **kw)


def compile(metal, msl, air):
    """metal (the command up to its source, with -c) on msl → air. A compile that fails with the header runs again without it, so what a failure reports and raises (CalledProcessError) is the plain compile's"""
    pch = header(metal)
    if pch:
        r = _run([*metal, '-include-pch', pch], msl, air, capture_output=True)
        if not r.returncode:
            sys.stderr.buffer.write(r.stderr); sys.stderr.flush()
            return
    try: _run(metal, msl, air, check=True)
    except subprocess.CalledProcessError: print(f'{msl}: does not compile (the messages above say <stdin>)', file=sys.stderr, flush=True); raise
