#!/usr/bin/env python3
# The pipeline archive made offline: the base of every shader pair, as the host's pipeline prebuild makes it on the device (pre_desc / pre_pair in dm/gfx/nvn/nvn_mtl.m), compiled on the Mac
# by metal-tt into one Metal 4 pipeline archive per GPU architecture, so an iPhone takes the bases from the app bundle instead of compiling them on its first start.
# The host matches libraries by content, so the archive serves the libraries the app ships (msl/b-<chain>); measurements are in dm/games/01007EF00011E000/README.md.
# Per pair (vertex key, pixel key) of the profile's shader_pairs.tsv and pso_pairs.tsv, read in that order with repeated lines dropped, as the host reads them:
#   · the vertex shader's pulling library <key>_bp (a vertex shader without stage inputs: its own <key>_b), entry `entry`, or `entry_z` for the near = 0 depth mode, from its reflection
#     <key>_b.json; the pixel shader's <key>_b, entry `entry` (main0); no vertex descriptor, one sample, every colour attachment unspecialized
#   · left out as the host leaves them out (to the draws): a reflection of another chain version or none, a vertex shader with stage inputs and no pulling library, a vertex library that
#     writes the layer (its reflection's layer_out, dmsh_entry.py layer_out: its pipelines take an input primitive class), a pixel shader that reads the primitive ID (its reflection's
#     raster.primitive_id: the host builds its pipelines whole, since through a base every primitive reads 0), a missing library or entry point
#   · a function that declares function constants is specialized as the host's prebuild specializes it (pre_fn in nvn_mtl.m): the border colour corrections' dm_border_off set (true:
#     off), the specialization the draws that bind no sampler with a twin share (fn_off). metal-tt specializes a <key>_b or <key>_bp function, cut by dmsh_side.cut or not, and the host
#     finds that base by its specialized descriptor (checked on Ocarina of Time's libraries, macOS 27.2, metal-tt 32023). It cannot lower a function unspecialized ("cannot lower module
#     with unresolved function constants"), and failed with "cannot find private metadata" on a side library (<key>_bx), which no base uses. A pair whose functions declare any other
#     constant is left to the device (rest_pairs.tsv), as before (HOST_CONSTANTS)
# And a compute pipeline per compute shader, which depends on its function alone (the host makes it from the function, cps_new in nvn_mtl.m; the threadgroup size is the dispatch's):
# the C rows of the profile's prewarm list (pso_manifest.tsv, recorded, in order), then every compute shader of its shader map (shader_map.tsv, stage compute), so a game whose list
# names none (a provisional profile) still has them; per shader <key>_b, entry `entry`. Left out as the host would not find them: a reflection of another chain version or none, a
# missing library or entry point, a function with a function constant other than HOST_CONSTANTS (the host builds that pipeline through the MTLDevice). One with the border corrections'
# constant is specialized as for the bases, as the host's common dispatch and its prewarm take it (cps_new_b)
# Output folder (what the app bundles as pso/, the host's DM_PSO_BUNDLE):
#   <arch>.mtl4a     one archive per GPU architecture (applegpu_g18p = A19 / A19 Pro); the host opens the one named after its device's architecture
#   prebuilt.tsv     what the archives cover, in the host's stamp format (pso_prebuild): chain version, depth mode, the pair lists' size and SHA-256. The host skips its prebuild when
#                    it matches and a probe of a few bases finds them in the archive
#   rest_pairs.tsv   the pairs the device still builds (only when there are any)
#   archive.tsv      what was built: architectures, OS minimum, toolchain, bases, compute pipelines (for people and logs; the host does not read it)
# Each archive is made again only when its compiler inputs change (key.<arch> in the work folder): the complete pipeline configuration without library locations,
# the referenced libraries' bytes, architecture, platform, minimum OS and metal-tt version. Chain/profile stamps are validated and refreshed independently.
# More bases than --batch-size are compiled in sequential groups (batches.<arch>/<index>/ in the work folder, each cached by its own inputs) and merged with metal-pack: one metal-tt on
# Super Mario Odyssey's 32,864 bases runs an 8 GB Mac out of paging space (docs/reference/shaders.md, Bounded groups for the offline pipeline compiler). The output is the same single file.
# With compute pipelines, they and the bases are separate groups (batches.<arch>/c<index>/ and <index>/) merged the same way, so new pairs leave the compute pipelines compiled, and the
# reverse. The compute groups are small (COMPUTE_BATCH) and run --jobs at a time: one metal-tt keeps about three cores busy.
# Usage: pso_offline.py --msl <b-<chain> folder> --out <folder> --work <folder> [--game <id>] [--pairs <shader_pairs.tsv> [--pairs <pso_pairs.tsv>]] [--gpu g18p[,g17p]]
#          [--depth gl|zero] [--platform ios|macos] [--os-min 27.0] [--script-only] [--batch-size N] [--shader-map <shader_map.tsv>] [--manifest <pso_manifest.tsv>] [--no-compute] [--jobs N]
#   --game: the profile (DM_GAMES_DIR as for game.py) whose shader_pairs.tsv and pso_pairs.tsv are the pairs, whose shader_map.tsv and pso_manifest.tsv name the compute shaders, and
#   whose depth_mode key the depth mode, unless --pairs / --shader-map / --manifest / --depth say; --no-compute: bases only; --jobs: metal-tt runs at a time for compute groups;
#   --gpu: architectures, with or without the applegpu_ prefix (default g18p); --depth: the game's NVN depth mode (gl = near -w, NVN's default; zero = near 0, which the game sets
#   with nvnDeviceSetDepthMode: the host knows it only at run time, so the profile says it; a wrong one costs the device its own prebuild, as without the archive);
#   --os-min: the archives' system (default: for iOS the SDK's major version, e.g. 27.0; for macOS the major version this Mac runs, since the Mac app is built for the Mac that builds it).
#   A device opens only an archive of its own major version (macOS 27.2 refused archives made for 26.0 and 26.6 and opened those for 27.0 and 27.2), and builds the bases itself
#   otherwise: the SDK's version on a Mac whose system is older than its Xcode's SDK (macOS 26.6 with Xcode 27.1) left that Mac without them (GitHub issue #673)
import argparse, concurrent.futures, filecmp, hashlib, json, os, shutil, struct, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romfs, shader_lib   # the libraries may be one file (shader_lib.py), read in place; metal-tt alone needs files of its own (real_libraries)
import dmsh_abi as abi

# The function constants the host's prebuild sets in a base's functions (nvn_mtl.m pre_fn), by name: (index, bool value). A pair whose functions declare another constant is left to the
# device: the host leaves it at its default, which metal-tt can do too, but no other constant was tried there, and one function metal-tt fails fails its whole group
HOST_CONSTANTS = {'dm_border_off': (abi.BORDER_FC, True)}

BATCH = 1000   # bases per metal-tt run: Odyssey's largest compiler lifetime footprint at this size was 2.065 GiB on an 8 GB M2
COMPUTE_BATCH = 16   # compute pipelines per metal-tt run, JOBS runs at a time: one metal-tt keeps about 2-3 cores busy: Star Fox's 233 took 268 s in 16 groups against 453 s in one run (M5 Pro, background priority, a loaded Mac)
JOBS = max(1, (os.cpu_count() or 2) // 2)
COLOUR = ('pixel_format', 'write_mask', 'blending_state', 'alpha_blend_operation', 'rgb_blend_operation', 'destination_alpha_blend_factor', 'destination_rgb_blend_factor',
          'source_alpha_blend_factor', 'source_rgb_blend_factor')


def arch_name(a):   # 'g18p' → 'applegpu_g18p'
    a = a.strip()
    return a if a.startswith('applegpu_') else f'applegpu_{a}'


def pair_text(paths):
    """The pair lists as the host reads them: their bytes one after the other (shader_pairs.tsv, then pso_pairs.tsv when it exists)"""
    return b''.join(Path(p).read_bytes() for p in paths if p and Path(p).is_file())


def read_pairs(text):
    """(vertex key, pixel key) per line, in order, without empty lines, comments and repeated lines (pso_prebuild's reading)"""
    seen, out = set(), []
    for line in text.decode('utf-8').split('\n'):
        if not line or line.startswith('#') or line in seen: continue
        seen.add(line); c = line.split('\t')
        if len(c) >= 2: out.append((c[0], c[1]))
    return out


def read_computes(maps, manifests):
    """The compute shaders by the library the host loads each from (<key>_b, as shader_load names it): the prewarm lists' C rows in their order, then the shader maps' rows of stage
    compute (column 3; the key is column 4, as shmap_load reads it), each once. Missing files are skipped"""
    out = {}
    for path, kind in [*((p, 'C') for p in manifests or ()), *((p, 'map') for p in maps or ())]:
        if not path or not Path(path).is_file(): continue
        for line in Path(path).read_text(encoding='utf-8').split('\n'):
            c = line.rstrip('\r').split('\t')
            if kind == 'C' and len(c) >= 2 and c[0] == 'C' and c[1]: out.setdefault(c[1], None)
            elif kind == 'map' and len(c) > 3 and c[2] == 'compute' and c[3]: out.setdefault(c[3] + '_b', None)
    return list(out)


def stamp(chain, zero, text):
    """The host's prebuild stamp (pso_prebuild in nvn_mtl.m): chain version, depth mode, and the pair lists' length in bytes and SHA-256 (first 16 hex digits)"""
    return f'chain\t{chain}\ndepth\t{"near=0" if zero else "near=-w"}\npairs\t{len(text)}\t{hashlib.sha256(text).hexdigest()[:16]}\n'


def functions(path):
    """{entry name: declares function constants} of a .metallib (constants). Raises ValueError on a file it cannot read"""
    return {k: bool(v) for k, v in constants(path).items()}


def _constant_names(cnst):
    """The names in a CNST tag: a 16-bit count, then per constant its name (NUL-terminated), a type byte, a 16-bit index and a byte"""
    n, p, out = struct.unpack_from('<H', cnst, 0)[0], 2, []
    for _ in range(n):
        end = cnst.index(b'\0', p); out.append(cnst[p:end].decode()); p = end + 1 + 4
    if p > len(cnst): raise ValueError('a truncated CNST tag')
    return tuple(out)


def constants(path):
    """{entry name: the names of the function constants it declares, a tuple, empty for none} of a .metallib, from its header: the function list (NAME tags) and each function's public
    metadata (a CNST tag lists its constants). Raises ValueError on a file it cannot read"""
    with romfs.open(path, 'rb') as f: b = f.read(1 << 20)
    if b[:4] != b'MTLB' or len(b) < 0x58: raise ValueError(f'{path}: not a Metal library')
    fo, _, po, ps = struct.unpack_from('<QQQQ', b, 0x18)
    def tags(p, end):
        t = {}
        while p + 4 <= end:
            tag = b[p:p + 4]; p += 4
            if tag == b'ENDT': return t
            n = struct.unpack_from('<H', b, p)[0]; p += 2; t[tag] = b[p:p + n]; p += n
        raise ValueError(f'{path}: unterminated tag list')
    try:
        count = struct.unpack_from('<I', b, fo)[0]; p = fo + 4; names = []
        for _ in range(count):   # each entry: its size (counting the size field), then tags
            size = struct.unpack_from('<I', b, p)[0]; names.append(tags(p + 4, p + size)[b'NAME'].rstrip(b'\0').decode()); p += size
        consts, p = [], po
        for _ in range(count):   # each entry: its size (not counting the size field), then tags
            size = struct.unpack_from('<I', b, p)[0]; t = tags(p + 4, p + 4 + size); consts.append(_constant_names(t[b'CNST']) if b'CNST' in t else ()); p += 4 + size
        if p != po + ps: raise ValueError(f'{path}: public metadata does not match the function list')
    except (struct.error, KeyError, UnicodeDecodeError) as e: raise ValueError(f'{path}: unreadable function list ({e})') from e
    return dict(zip(names, consts))


def plan(lib_dir, chain, pairs, zero):
    """The bases the archive holds: [(vertex library, vertex entry, pixel library, pixel entry)], the pairs left to the device (function constants), and counts of the pairs left out"""
    lib_dir = romfs.Path(lib_dir); js, fns = {}, {}
    def refl(key):
        if key not in js:
            try: js[key] = json.loads((lib_dir / f'{key}_b.json').read_bytes())
            except (OSError, ValueError): js[key] = None
        return js[key]
    def fn(lib):
        if lib not in fns:
            try: fns[lib] = constants(lib_dir / f'{lib}.metallib')
            except (OSError, ValueError): fns[lib] = None
        return fns[lib]
    bases, rest, seen, left = [], [], set(), dict.fromkeys(('reflection', 'mesh', 'stage inputs', 'library', 'layer', 'primitive ID', 'entry'), 0)
    for vk, fk in pairs:
        vj, fj = refl(vk), refl(fk)
        if not vj or not fj or vj.get('chain') != chain or fj.get('chain') != chain: left['reflection'] += 1; continue
        if vj.get('mesh_stage'): left['mesh'] += 1; continue   # a mesh shader's pair (shader_split.py var_pairs, for its padding): its pipelines are mesh pipelines, built at the draw (nvn_geometry.inc mtk_pso)
        ventry = vj['entry_z'] if zero and vj.get('entry_z') else vj.get('entry'); fentry = fj.get('entry') or 'main0'
        pull = isinstance(vj.get('pull'), dict)
        if not pull and vj.get('vertex_inputs'): left['stage inputs'] += 1; continue
        if vj.get('layer_out'): left['layer'] += 1; continue
        if (fj.get('raster') or {}).get('primitive_id'): left['primitive ID'] += 1; continue
        vl, fl = vk + ('_bp' if pull else '_b'), fk + '_b'
        vf, ff = fn(vl), fn(fl)
        if vf is None or ff is None: left['library'] += 1; continue
        if ventry not in vf or fentry not in ff: left['entry'] += 1; continue
        if any(c not in HOST_CONSTANTS for c in vf[ventry] + ff[fentry]): rest.append((vk, fk)); continue   # script specializes HOST_CONSTANTS as the host does
        b = (vl, ventry, fl, fentry)
        if b not in seen: seen.add(b); bases.append(b)
    return bases, rest, left


def plan_compute(lib_dir, chain, shaders):
    """The compute pipelines the archive holds, [(library, entry)]: each of shaders (<key>_b) with its reflection's entry; and counts of
    the shaders left out as the host would not find their pipeline (reflection, library, entry point, function constants)"""
    lib_dir = romfs.Path(lib_dir); pipes, seen = [], set(); left = dict.fromkeys(('reflection', 'library', 'entry', 'constants'), 0)
    for pc in shaders:
        try: j = json.loads((lib_dir / f'{pc}.json').read_bytes())
        except (OSError, ValueError): j = None
        if not isinstance(j, dict) or j.get('chain') != chain or j.get('stage', 'cs') != 'cs': left['reflection'] += 1; continue
        entry = j.get('entry') or 'main0'
        try: fs = constants(lib_dir / f'{pc}.metallib')
        except (OSError, ValueError): left['library'] += 1; continue
        if entry not in fs: left['entry'] += 1; continue
        if any(c not in HOST_CONSTANTS for c in fs[entry]): left['constants'] += 1; continue   # script specializes HOST_CONSTANTS, as cps_new looks the pipeline up
        if (pc, entry) not in seen: seen.add((pc, entry)); pipes.append((pc, entry))
    return pipes, left


def script(lib_dir, bases, computes=()):
    """A Metal 4 pipelines script (metal4-pipelines-script(5), the .mtl4-json format metal-tt reads) with one render pipeline descriptor per base, and one compute pipeline descriptor per
    compute pipeline (library, entry) that names the function alone, as the host's lookup does (cps_bundle in nvn_mtl.m: no threadgroup size, no indirect command buffers).
    A function that declares constants is named through its specialization by HOST_CONSTANTS (a pair with other constants is not among the bases: plan)"""
    libs, fns, specs, declared, pipes = {}, {}, {}, {}, []
    def fd(lib, entry):
        if lib not in libs:
            libs[lib] = {'label': lib, 'path': str(Path(lib_dir).resolve() / f'{lib}.metallib')}
            try: declared[lib] = constants(romfs.Path(lib_dir) / f'{lib}.metallib')
            except (OSError, ValueError): declared[lib] = {}   # plan read every base's libraries; one it could not read is not among them
        label = f'{lib}.{entry}'
        if label not in fns: fns[label] = {'label': label, 'library': f'lib:{lib}', 'name': entry}
        cs = declared[lib].get(entry, ())
        if not cs: return f'fnd:{label}'
        if label not in specs:
            specs[label] = {'label': f'{label}.host', 'function_descriptor': f'fnd:{label}',
                            'constant_values': [{'id_type': 'FunctionConstantIndex', 'id': {'data': i}, 'value_type': 'ConstantBool', 'value': {'data': v}}
                                                for i, v in sorted(HOST_CONSTANTS[c] for c in cs if c in HOST_CONSTANTS)]}
        return f'fnd:{label}.host'
    for vl, ve, fl, fe in bases:
        pipes.append({'vertex_function_descriptor': fd(vl, ve), 'fragment_function_descriptor': fd(fl, fe),
                      'color_attachments': [dict.fromkeys(COLOUR, 'Unspecialized') for _ in range(8)]})
    descriptors = {'render_pipeline_descriptors': pipes}
    if computes: descriptors['compute_pipeline_descriptors'] = [{'compute_function_descriptor': fd(lib, entry)} for lib, entry in computes]   # none: the script of bases alone, as before
    functions_ = {'library_function_descriptors': list(fns.values())}
    if specs: functions_['specialized_function_descriptors'] = list(specs.values())   # none: the script as before, so other games' archive keys hold
    return {'libraries': list(libs.values()), 'function_descriptors': functions_, 'pipeline_descriptors': descriptors}


def write_script(path, configuration):
    """Stream the script to disk beside the previous one (<name>.partial, written again by the next run if this one is killed), which it replaces only when the two differ: an unchanged file keeps its time"""
    part = path.with_name(path.name + '.partial')
    try:
        with part.open('w', encoding='utf-8', newline='\n') as out: json.dump(configuration, out, indent=1)
        if not path.is_file() or not filecmp.cmp(path, part, shallow=False): os.replace(part, path)
    finally: part.unlink(missing_ok=True)


def library_digest(lib_dir, bases, computes=()):
    """The bytes metal-tt consumes: a profile/chain stamp does not change when the same shaders are rebuilt by another Metal compiler."""
    h = hashlib.sha256()
    for name in sorted({lib for vl, _, fl, _ in bases for lib in (vl, fl)} | {lib for lib, _ in computes}):
        h.update(name.encode() + b'\0')
        lib = hashlib.sha256()
        with romfs.open(os.path.join(lib_dir, f'{name}.metallib'), 'rb') as f:
            while chunk := f.read(1 << 20): lib.update(chunk)
        h.update(lib.digest())
    return h.hexdigest()


def space_needed(bases):   # what metal-tt is let start with, an estimate: its temporary files (TMPDIR, one .airp per base) and the archive (about 41 KB per base at 15,767 bases); Cruis'n Blast's 33,858 bases failed with 366 MB free
    return max(2 << 30, bases * (96 << 10))


def clear_temporaries(work):   # what a failed or killed metal-tt leaves in its TMPDIR (the work folder): translate-*, *.airp, multiple-functions-input-*.metallib
    for f in [*work.glob('translate-*'), *work.glob('*.airp'), *work.glob('multiple-functions-input-*.metallib')]:
        shutil.rmtree(f) if f.is_dir() else f.unlink(missing_ok=True)


def archive_key(configuration, libraries, arch, platform, osmin, toolchain):
    """Identity of metal-tt's inputs, independent of chain labels and library locations.

    The generated configuration includes all entry points and pipeline settings; the
    library digest covers every referenced file's name and bytes. Only absolute
    library paths are replaced: moving identical inputs cannot change the GPU code.
    Version this format if a future input is not represented here. Legacy keys miss
    once; runtime stamps are rewritten even when the compiled archive is reused.
    """
    config = dict(configuration, libraries=[dict(lib, path=lib['label'] + '.metallib') for lib in configuration['libraries']])
    inputs = {'format': 1, 'configuration': config, 'libraries': libraries,
              'arch': arch, 'platform': platform, 'os_min': osmin, 'toolchain': toolchain}
    digest = hashlib.sha256()
    for chunk in json.JSONEncoder(sort_keys=True, separators=(',', ':')).iterencode(inputs):
        digest.update(chunk.encode())
    return digest.hexdigest()


def sdk(platform):
    return 'iphoneos' if platform == 'ios' else 'macosx'


def tool_version(platform):
    r = subprocess.run(['xcrun', '-sdk', sdk(platform), 'metal-tt', '-version'], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'[RED] pso_offline: metal-tt is not available ({(r.stderr or r.stdout).strip()}); install the Metal toolchain: xcodebuild -downloadComponent MetalToolchain')
    return r.stdout.strip().splitlines()[0]


def os_min_default(platform):
    cmd = ['sw_vers', '-productVersion'] if platform == 'macos' else ['xcrun', '-sdk', sdk(platform), '--show-sdk-version']
    v = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()
    return v.split('.')[0] + '.0'


def compile_archive(js, part, work, arch, platform, osmin, count):
    """One compiler invocation, with its temporaries on the work volume. Never publish a failed output."""
    part.unlink(missing_ok=True)
    clear_temporaries(work)
    need, free = space_needed(count), shutil.disk_usage(work).free
    if free < need: sys.exit(f'[RED] pso_offline: {free / 2**30:.1f} GB free on the volume of {work}, metal-tt needs about {need / 2**30:.0f} GB for {count} bases: free some space and run again')
    cmd = ['xcrun', '-sdk', sdk(platform), 'metal-tt', '-arch', arch, f'-{platform}-version-min', osmin, str(js), '-o', str(part)]
    log_path = work / f'metal-tt.{arch}.log'
    with log_path.open('w') as log:
        r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, TMPDIR=str(work)))
    if r.returncode or not part.is_file() or not part.stat().st_size:
        free = shutil.disk_usage(work).free
        clear_temporaries(work); part.unlink(missing_ok=True)
        full = f' ({free / 2**30:.1f} GB free on the volume of {work} when it failed: likely out of disk space)' if free < 1 << 30 else ''
        sys.exit(f'[RED] pso_offline: metal-tt failed for {arch} (exit {r.returncode}){full}: {log_path.read_text()[-2000:]}')


def real_libraries(lib_dir, bases, work, computes=()):
    """Where metal-tt finds the bases' libraries: it takes file paths, so the libraries of one file get a temporary folder of what this compile reads, which clear_real_libraries removes after it.
    An archive whose key still holds compiles nothing and gets none"""
    if os.path.isdir(lib_dir): return lib_dir
    real = Path(work) / 'libraries'; shutil.rmtree(real, ignore_errors=True); real.mkdir(parents=True)
    for name in sorted({lib for vl, _, fl, _ in bases for lib in (vl, fl)} | {lib for lib, _ in computes}):
        with romfs.open(os.path.join(lib_dir, f'{name}.metallib'), 'rb') as f, open(real / f'{name}.metallib', 'wb') as o: shutil.copyfileobj(f, o)
    return real


def clear_real_libraries(work): shutil.rmtree(Path(work) / 'libraries', ignore_errors=True)


def compile_batch(lib_dir, bases, work, arch, platform, osmin, toolchain, computes=()):
    """Cache one bounded group by its actual libraries and descriptors; also used for an isolated pilot."""
    work.mkdir(parents=True, exist_ok=True)
    configuration = script(lib_dir, bases, computes)
    js = work / 'bases.mtl4-json'; write_script(js, configuration)
    key = archive_key(configuration, library_digest(lib_dir, bases, computes), arch, platform, osmin, toolchain)
    del configuration
    dst, kf = work / f'{arch}.mtl4a', work / f'key.{arch}'
    if dst.is_file() and dst.stat().st_size and kf.is_file() and kf.read_text().strip() == key: return dst, True
    kf.unlink(missing_ok=True)
    part = work / f'{arch}.mtl4a.partial'
    write_script(js, script(real_libraries(lib_dir, bases, work, computes), bases, computes))
    compile_archive(js, part, work, arch, platform, osmin, len(bases) + len(computes))
    clear_real_libraries(work)
    os.replace(part, dst); kf.write_text(key + '\n')
    return dst, False


def groups_of(bases, computes, batch_size, compute_size=COMPUTE_BATCH):
    """The groups compile_batches compiles, [(folder name, bases, compute pipelines)]: the bases in groups of batch_size (00001, ...), then the compute pipelines in groups of compute_size
    of their own (c00001, ...), so that a change to either leaves the other's groups as they are. 0: one group of each"""
    groups = []
    for prefix, items, size in (('', bases, batch_size), ('c', computes, compute_size)):
        size = size or len(items) or 1
        for index, start in enumerate(range(0, len(items), size), 1):
            group = items[start:start + size]
            groups.append((f'{prefix}{index:05d}', group if not prefix else [], group if prefix else []))
    return groups


def compile_batches(lib_dir, bases, work, part, arch, platform, osmin, toolchain, batch_size, computes=(), jobs=1, compute_size=COMPUTE_BATCH):
    """Limit compiler input size, then combine every completed group into the usual single archive.

    This bounds input count, not memory: a single pipeline can still exceed Apple's compiler resources.
    On M2, merged archives passed archive-only lookups of two synthetic pipelines and all 32,864 Odyssey bases.
    Other workloads and compiler versions can have different resource use.
    The groups of bases run one after the other, as before; the compute groups, small, jobs at a time, each in its own folder (its TMPDIR).
    """
    groups = groups_of(bases, computes, batch_size, compute_size); total = len(groups); names = {name for name, _, _ in groups}
    for old in [*(work / f'batches.{arch}').glob('[0-9]*'), *(work / f'batches.{arch}').glob('c[0-9]*')]:   # the groups of a longer list than this one
        if old.name not in names: shutil.rmtree(old)
    def one(index, name, group, cgroup):
        print(f'pso_offline: {arch}: batch {index}/{total}: {f"{len(group)} bases" if group else f"{len(cgroup)} compute pipelines"}, compiling or checking cache', flush=True)
        piece, reused = compile_batch(lib_dir, group, work / f'batches.{arch}' / name, arch, platform, osmin, toolchain, cgroup)
        print(f'pso_offline: {arch}: batch {index}/{total}: ready ({"reused" if reused else "compiled"})', flush=True)
        return piece
    pieces = [one(i, *g) for i, g in enumerate(groups, 1) if g[1]]
    with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as pool:   # a failed group stops the build once the others under way have finished (their pieces stay cached)
        futures = [pool.submit(one, i, *g) for i, g in enumerate(groups, 1) if not g[1]]
        pieces += [f.result() for f in futures]
    part.unlink(missing_ok=True)
    need, free = max(2 << 30, sum(p.stat().st_size for p in pieces)), shutil.disk_usage(work).free
    if free < need: sys.exit(f'[RED] pso_offline: {free / 2**30:.1f} GB free before merging; need at least {need / 2**30:.1f} GB')
    print(f'pso_offline: {arch}: merging {total} batches', flush=True)
    cmd = ['xcrun', '-sdk', sdk(platform), 'metal-pack', '-nthreads', '1', *map(str, pieces), '-o', str(part)]
    log_path = work / f'metal-pack.{arch}.log'
    with log_path.open('w') as log:
        r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, TMPDIR=str(work)))
    if r.returncode or not part.is_file() or not part.stat().st_size:
        part.unlink(missing_ok=True)
        sys.exit(f'[RED] pso_offline: metal-pack failed for {arch} (exit {r.returncode}): {log_path.read_text()[-2000:]}')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Build the pipeline archive of a game\'s shader pairs offline (metal-tt)')
    ap.add_argument('--msl', required=True, help='the shader libraries, b-<chain version> (with their reflection <key>_b.json)')
    ap.add_argument('--game', help='the game profile: its pair lists and depth_mode key')
    ap.add_argument('--pairs', action='append', help='shader_pairs.tsv, then pso_pairs.tsv (missing files are skipped; default: the profile\'s)')
    ap.add_argument('--out', required=True, help='the folder the app bundles (archives and stamp)')
    ap.add_argument('--work', required=True, help='the script, keys and logs')
    ap.add_argument('--gpu', default='g18p', help='GPU architectures, comma-separated (default g18p)')
    ap.add_argument('--depth', choices=('gl', 'zero'), help='the game\'s NVN depth mode (default: the profile\'s depth_mode, else gl: near = -w)')
    ap.add_argument('--platform', choices=('ios', 'macos'), default='ios')
    ap.add_argument('--os-min', help='minimum OS of the archives (default: iOS, the SDK\'s major version; macOS, this Mac\'s)')
    ap.add_argument('--script-only', action='store_true', help='write the script and the stamp, run no metal-tt')
    ap.add_argument('--batch-size', type=int, default=BATCH, help=f'compile sequential groups of at most N bases and merge with metal-pack, when there are more (default {BATCH}; 0: one compiler invocation)')
    ap.add_argument('--shader-map', action='append', help='shader_map.tsv: its compute shaders get pipelines (default: the profile\'s)')
    ap.add_argument('--manifest', action='append', help='pso_manifest.tsv: its C rows first (default: the profile\'s)')
    ap.add_argument('--no-compute', action='store_true', help='no compute pipelines, the bases alone')
    ap.add_argument('--jobs', type=int, default=JOBS, help=f'metal-tt runs at a time for the compute pipelines (default {JOBS}, half the logical CPUs)')
    a = ap.parse_args(argv)
    if a.batch_size < 0: ap.error('--batch-size must be zero or positive')
    if a.game:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'games')); import game
        g = game.load(a.game)
        a.pairs = a.pairs or [str(g.table('shader_pairs.tsv')), str(g.table('pso_pairs.tsv'))]
        a.shader_map = a.shader_map or [str(g.table('shader_map.tsv'))]; a.manifest = a.manifest or [str(g.table('pso_manifest.tsv'))]
        a.depth = a.depth or g.get('depth_mode', 'gl')
        if a.depth not in ('gl', 'zero'): sys.exit(f'[RED] pso_offline: {g.id}: depth_mode must be gl or zero: {a.depth}')
    if not a.pairs: sys.exit('[RED] pso_offline: no pair lists (--pairs or --game)')
    a.depth = a.depth or 'gl'
    lib_dir, out, work = shader_lib.view(a.msl), Path(a.out), Path(a.work)
    chain = shader_lib.name_of(lib_dir)[2:] if shader_lib.name_of(lib_dir).startswith('b-') else sys.exit(f'[RED] pso_offline: {lib_dir} is not a b-<chain version> folder')
    archs = sorted({arch_name(x) for x in a.gpu.split(',') if x.strip()})
    if not archs: sys.exit('[RED] pso_offline: no GPU architecture (--gpu)')
    text = pair_text(a.pairs)
    if not text: sys.exit(f'[RED] pso_offline: no pair list ({", ".join(a.pairs)})')
    zero = a.depth == 'zero'
    t0 = time.time()
    pairs = read_pairs(text)
    bases, rest, left = plan(lib_dir, chain, pairs, zero)
    shaders = [] if a.no_compute else read_computes(a.shader_map, a.manifest)
    computes, cleft = plan_compute(lib_dir, chain, shaders)
    work.mkdir(parents=True, exist_ok=True); out.mkdir(parents=True, exist_ok=True)
    js = work / 'bases.mtl4-json'   # metal-tt takes the format from the extension: .mtl4-json is Metal 4's (.mtlp-json is the Metal 3 format)
    configuration = script(lib_dir, bases, computes)
    write_script(js, configuration)
    st = stamp(chain, zero, text)
    print(f'pso_offline: {len(pairs)} pairs: {len(bases)} bases, {len(rest)} left to the device (function constants), left out as the host leaves them: '
          + ', '.join(f'{k} {v}' for k, v in left.items()) + f' ({time.time() - t0:.1f} s)')
    if shaders: print(f'pso_offline: {len(shaders)} compute shaders: {len(computes)} compute pipelines, left out: '
                      + ', '.join(f'{k} {v}' for k, v in cleft.items()))
    if a.script_only: (out / 'prebuilt.tsv').write_text(st); return 0
    if pairs and not bases and not rest: sys.exit('[RED] pso_offline: no base to build')   # pairs, but the host would leave out every one: wrong or missing libraries
    if not pairs: print('pso_offline: no pairs (only a header): nothing to build, no archive')   # a profile whose split found no pair (GitHub issue #338)
    if not bases and not computes: archs = []   # no pair, or every eligible pair needs device specialization, and no compute pipeline; clear old archives and still write the stamp and the remainder list
    osmin = a.os_min or os_min_default(a.platform); tv = tool_version(a.platform) if bases or computes else 'not used (function constants only)'
    libraries = library_digest(lib_dir, bases, computes)
    keys = {arch: archive_key(configuration, libraries, arch, a.platform, osmin, tv) for arch in archs}
    del configuration   # the compiler reads the script from disk
    for arch, key in keys.items():
        dst, kf = out / f'{arch}.mtl4a', work / f'key.{arch}'
        if dst.is_file() and kf.is_file() and kf.read_text().strip() == key: print(f'pso_offline: {arch}: reused ({dst.stat().st_size / 2**20:.0f} MB)'); continue
        kf.unlink(missing_ok=True); dst.unlink(missing_ok=True)
        part = work / f'{arch}.mtl4a.partial'; part.unlink(missing_ok=True)
        t1 = time.time(); print(f'pso_offline: {arch}: metal-tt, {len(bases)} bases, {len(computes)} compute pipelines (minutes) ...', flush=True)
        if a.batch_size and len(groups_of(bases, computes, a.batch_size)) > 1:
            compile_batches(lib_dir, bases, work, part, arch, a.platform, osmin, tv, a.batch_size, computes, a.jobs)
        else:
            write_script(js, script(real_libraries(lib_dir, bases, work, computes), bases, computes))
            compile_archive(js, part, work, arch, a.platform, osmin, len(bases) + len(computes))
            clear_real_libraries(work)
        os.replace(part, dst); kf.write_text(key + '\n')
        print(f'pso_offline: {arch}: {dst.stat().st_size / 2**20:.0f} MB in {time.time() - t1:.0f} s')
    for f in out.glob('*.mtl4a'):   # architectures no longer asked for
        if f.stem not in archs: f.unlink()
    (out / 'prebuilt.tsv').write_text(st)
    rp = out / 'rest_pairs.tsv'
    if rest: rp.write_text(''.join(f'{v}\t{f}\n' for v, f in rest))
    else: rp.unlink(missing_ok=True)
    (out / 'archive.tsv').write_text(f'# the pipeline archive made offline (dm/gfx/tools/pso_offline.py); the host reads prebuilt.tsv, not this file\narch\t{" ".join(archs)}\n'
                                     f'platform\t{a.platform}\t{osmin}\ntoolchain\t{tv}\nbases\t{len(bases)}\nrest\t{len(rest)}\ncompute\t{len(computes)}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
