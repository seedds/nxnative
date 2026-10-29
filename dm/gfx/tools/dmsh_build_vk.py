#!/usr/bin/env python3
# The shader chain for the Vulkan target (docs/design/vulkan-backend.md, section 2): original Switch shaders → SPIR-V (dmsh_xlate_vk, the Ryujinx translator with an accessor that
# answers for Vulkan) → SPIR-V passes (dmsh_spv.py) → <output cache dir>/vk-<V>/, V being DMSH_CHAIN_VK (dmsh_chain_vk.py). SPIRV-Cross is not on the output path: the decisions the
# Metal chain takes from its MSL (integer vertex inputs, integer fragment outputs, raster facts, driver bank reads, the c1 bounds and the uniform read extents) are taken by the same
# code on an MSL rendering of the Vulkan SPIR-V made for analysis only (2.1), kept in vk-<V>/analysis/. The Metal chain is not changed and none of its outputs is read.
# Usage: DM_GAME=<id> dmsh_build_vk.py <Switch package dir> <output cache dir> [key...]   (no keys = all; a full build also stamps vk-<V>/inputs.tsv with the profile inputs digest
#                                                                                      and writes the profile's c1_full_vk.tsv and cb_extent_vk.tsv; one output directory per game)
#        DM_GAME=<id> dmsh_build_vk.py --census <output cache dir> [<out.tsv>]           (the census below over vk-<V>/, by default into vk-<V>/census.tsv)
# Outputs in vk-<V>/: <key>.spv (entry point main; main_u and main_m for a pixel shader that writes integer bits, dmsh_build.py fs_uint), <key>_bv.spv (a vertex shader with integer
#   inputs: their float and other-sign shadows at locations 16-30, dmsh_spv.py shadow_inputs), <key>_fp32.spv (a module with doubles, FP64 lowered for devices without shaderFloat64),
#   <key>.json (the reflection: the neutral binding kinds of nvn_refl.h as KINDS names them, each with its set and binding; the entry points; the vertex inputs; the fragment
#   interpolation mask; the raster facts; chain), analysis/<key>_b.metal and analysis/<key>_b.json (the analysis rendering, as c1_guard.py and cb_extent.py read a Metal library),
#   _vk_build.json (each key's result), inputs.tsv (a full build's stamp)
# The census (section 2.2's last paragraph, section 11): Apple GPUs return the bits of an integer or stencil texture read through a float declaration, Vulkan requires the sampled
#   type to match the format's. For every built shader, each image read whose declared sampled type is float, with how its result's bits are used (dmsh_spv.py bit_use: 'bits' when
#   only bit casts to integers use it) and the slot's answer-table format where the table has one; a read is listed when its bits are used as an integer or the table names an
#   integer format. The host decides per slot later (a translator answer, or a second entry point).
# The job model is dmsh_build.py's: shader keys (with the target added), the content-addressed cache (DMSH_CAS, default <output cache dir>/cas; outputs under cas/vk/), a translator
#   process per batch of J keys, pixel shaders before the vertex shaders paired with them (vs_pad reads their inputs).
# Environment: DM_GAME (required), XLATE_VK, SPIRV_CROSS_VK, DOTNET (dmsh_chain_vk.py), DMSH_CAS, J (concurrency, default 8), WORK (the temporary folder's parent), XLATE_DEBUG /
#   XLATE_REDUCED (dmsh_chain.py options)
import ast, collections, concurrent.futures as cf, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile, threading
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import dmsh_chain, dmsh_chain_vk, dmsh_spv, dmsh_atomic   # noqa: E402

# The binding kinds the reflection names, and the nvn_refl.h kind each is (the strings are the ones nvn_refl.c reads from the Metal reflection, plus tex_dyn / samp_dyn, which the
# Metal reflection marks by the binding's MSL name and type instead); '?' is an unrecognized binding, reported in the summary as the Metal build does
KINDS = {'cbuf': 'NVN_RB_CBUF', 'samp': 'NVN_RB_SAMP', 'tex': 'NVN_RB_TEX', 'img': 'NVN_RB_IMG', 'tbuf': 'NVN_RB_TBUF', 'sbuf': 'NVN_RB_SBUFP', 'support': 'NVN_RB_SUPPORT',
         'c1': 'NVN_RB_C1', 'drvcb': 'NVN_RB_DRV0', 'samp_tex': 'NVN_RB_SAMPT', 'ubo_ptr': 'NVN_RB_UBOP', 'tex_ubo': 'NVN_RB_TEXU', 'samp_ubo': 'NVN_RB_SAMPU', 'samp_id': 'NVN_RB_SAMPI',
         'sbuf_ubo': 'NVN_RB_SBUFU', 'tex_dyn': 'NVN_RB_TEXD', 'samp_dyn': 'NVN_RB_SAMPD', 'tex_c2a': 'NVN_RB_TEXA', 'samp_c2a': 'NVN_RB_SAMPA'}   # drvcb: NVN_RB_DRV2 for bank 2

# Code of dmsh_build.py, executed from its source: importing it would run the Metal build (it parses its arguments and checks the Metal tools at import). The same functions decide
# for both targets; the Metal chain's tests take them the same way (dm/gfx/tests/test_dmsh_reflect.py, test_subgroup.py)
SHARED_FUNCTIONS = ('records', 'vertex_ab', 'job_for', 'tf_layout', 'int_inputs', 'fs_uint', 'raster_facts', 'drv_reads', 'unfillable', 'decode_tex', 'const_sampler', 'decode_ubo_tex',
                    'c2_array', 'load_pairs', 'shader_key', 'pair_keyed', 'shader_rule_source')
SHARED_NAMES = ('TEX_BASE', 'SSBO', 'DRV_FILL', '_CAST', 'XC', 'RULES')


def shared(scope):
    """Run SHARED_FUNCTIONS and SHARED_NAMES of dmsh_build.py into scope (a dict that also holds the globals they read); → scope"""
    path = os.path.join(HERE, 'dmsh_build.py')
    with open(path) as f: tree = ast.parse(f.read())
    def names(t): return [n.id for n in ast.walk(t) if isinstance(n, ast.Name)]
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in SHARED_FUNCTIONS
             or isinstance(n, ast.Assign) and any(x in SHARED_NAMES for t in n.targets for x in names(t))]
    found = {n.name for n in nodes if isinstance(n, ast.FunctionDef)} | {x for n in nodes if isinstance(n, ast.Assign) for t in n.targets for x in names(t)}
    missing = (set(SHARED_FUNCTIONS) | set(SHARED_NAMES)) - found
    if missing: sys.exit(f'[RED] dmsh_build.py no longer has {", ".join(sorted(missing))}: dmsh_build_vk.py takes them from it')
    exec(compile(ast.Module(body=nodes, type_ignores=[]), path, 'exec'), scope)
    return scope


def code_fingerprint():   # the part of every shader key the chain's code makes (dmsh_build.py CODE_FP's counterpart)
    h = hashlib.sha256()
    for f in ('dmsh_build_vk.py', 'dmsh_spv.py', 'dmsh_build.py', 'dmsh_chain.py', 'dmsh_chain_vk.py', 'dmsh_atomic.py'):
        with open(os.path.join(HERE, f), 'rb') as fh: h.update(f.encode() + b'\0' + fh.read() + b'\0')
    return h.hexdigest()


def tool_fingerprint():   # the part of every shader key the tools make (dmsh_build.py TOOL_FP's counterpart): the lock, the options, the translator source, the analysis renderer
    with open(dmsh_chain_vk.LOCK) as f: lock = f.read()
    return ('lock\0' + lock + '\0options\0' + dmsh_chain.options() + '\0xlate\0' + dmsh_chain_vk.xlate_source_sha() + '\0spirv-cross\0' +
            dmsh_chain_vk._first_line([dmsh_chain_vk.spirv_cross(), '--revision'])).encode()


def atomic_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    with open(tmp, 'w') as f: json.dump(value, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)


def read_json(path):
    with open(path, encoding='utf-8') as f: return json.load(f)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    shutil.copyfile(src, tmp); os.replace(tmp, dst)


class Build:
    """One run of the Vulkan chain over a game's package (see the file header)"""
    def __init__(self, dm_dir, cache_root, want):
        bad = dmsh_chain_vk.check_tools()
        if bad: sys.exit('\n'.join('[RED] Vulkan translation chain tools do not match the lock file: ' + b for b in bad) +
                         '\n(build the translator as docker/README.md says, in the chain image; if the tools changed, run dmsh_chain_vk.py --update-lock there and commit the lock)')
        self.chain = dmsh_chain_vk.version()
        with open(dmsh_chain_vk.HEADER) as f:
            if f'"{self.chain}"' not in f.read(): sys.exit(f'[RED] {dmsh_chain_vk.HEADER} does not hold the current Vulkan chain version {self.chain}: run dmsh_chain_vk.py --header first and commit it')
        import dmsh_facts   # reads the profile (DM_GAME) when imported
        self.game = dmsh_chain.load_game(); self.inputs = dmsh_chain.inputs(self.game); self.facts = dmsh_facts.load()
        if not os.path.exists(dmsh_facts.TABLE): sys.exit(f'[RED] no answer table {dmsh_facts.TABLE} (it comes with the game profile; an empty table = every query answered with defaults)')
        self.dm_dir, self.cache_root, self.want = dm_dir, cache_root, want
        self.out = os.path.join(cache_root, dmsh_chain_vk.PREFIX + self.chain); os.makedirs(self.out, exist_ok=True)
        g = dmsh_chain.read_stamp(self.out).get('game')
        if g and g != self.game.id: sys.exit(f'[RED] {self.out} holds the shader libraries of game {g}, not {self.game.id}: give each game its own output directory')
        self.xlate = [os.environ.get('DOTNET', 'dotnet'), dmsh_chain_vk.XLATE_VK]
        self.cross = dmsh_chain_vk.spirv_cross()
        self.cas = os.path.join(os.environ.get('DMSH_CAS', os.path.join(cache_root, 'cas')), 'vk')
        self.nj = int(os.environ.get('J', '8'))
        self.stats = collections.Counter(); self.lock = threading.Lock()
        self.s = shared({'re': re, 'os': os, 'sys': sys, 'struct': struct, 'json': json, 'hashlib': hashlib, 'collections': collections, 'inspect': __import__('inspect')})
        self.read_map()
        self.s.update(FACTS=self.facts, MAP_ROWS=self.map_rows, MAP_CONFLICT_ROWS=self.map_conflict_rows, TOOL_FP=tool_fingerprint(), CODE_FP=code_fingerprint(),
                      PROFILE_ROWS=dmsh_chain.profile_rows(self.game), dmsh_facts=dmsh_facts)
        self.s['PAIRS'] = self.pairs = self.s['load_pairs'](self.game, dm_dir)

    def read_map(self):   # the shader map, as dmsh_build.py reads it: key → (package, index), its PC pair, and the rows that enter its cache key
        path = str(self.game.table('shader_map.tsv'))
        self.pc_of, self.loc, self.map_rows, self.map_conflict_rows = {}, {}, {}, {}
        with open(path, 'rb') as f:
            for raw in f:
                c = raw.rstrip(b'\n').split(b'\t')
                if len(c) > 4: self.map_rows.setdefault(c[3].decode(), raw)
        conflicts = path + '.conflicts.tsv'
        if os.path.exists(conflicts):
            with open(conflicts, 'rb') as f:
                for raw in f:
                    c = raw.rstrip(b'\n').split(b'\t')
                    if len(c) > 1 and not raw.startswith((b'#', b'dm_')): self.map_conflict_rows.setdefault(c[0].decode(), raw)
        with open(path) as f:
            for ln in f:
                c = ln.rstrip('\n').split('\t')
                if len(c) > 4 and c[0].endswith('.bin'): self.pc_of.setdefault(c[3], c[4]); self.loc.setdefault(c[3], (c[0], int(c[1])))
        if os.path.exists(conflicts):
            with open(conflicts) as f:
                for ln in f:
                    c = ln.rstrip('\n').split('\t')
                    if len(c) > 1 and not ln.startswith(('#', 'dm_')) and c[0] in self.pc_of: self.pc_of[c[0]] = c[1]

    # The translator
    def translate(self, jobs, w):
        jp = os.path.join(w, f'job_{abs(hash(jobs[0]["key"]))}_{threading.get_ident()}.json'); atomic_json(jp, jobs)
        return subprocess.run([*self.xlate, jp, w], capture_output=True, text=True)

    def translate_one(self, job, w):   # a second translation of one key (integer inputs, integer outputs): its outputs replace the first's
        r = self.translate([job], w)
        if r.returncode or not os.path.exists(os.path.join(w, job['key'] + '.spv')): return (r.stderr.strip() or r.stdout.strip())[:160] or 'no spv'
        return None

    # The analysis rendering
    def render(self, spv_path, w):
        """The MSL of a module for analysis (the descriptors flattened into set 0, the Metal chain's SPIRV-Cross options); None when SPIRV-Cross does not render it
        (a geometry or tessellation stage, or what MSL cannot express)"""
        flat = spv_path[:-4] + '.flat.spv'; dmsh_spv.flatten(dmsh_spv.Module.read(spv_path)).write(flat)
        r = subprocess.run([self.cross, *dmsh_atomic.cross_args(self.s['XC'], flat), flat], capture_output=True, text=True)
        os.unlink(flat)
        return r.stdout if r.returncode == 0 else None

    @staticmethod
    def colors(msl):   # the colour indices of a pixel shader's float outputs in the analysis MSL
        st = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
        return sorted({int(c) for ty, c in re.findall(r'^\s*(\w+)\s+\w+\s*\[\[color\((\d+)\)\]\];', st.group(1) if st else '', re.M) if ty.startswith('float')})

    # The reflection
    def bindings(self, stage, m, res):
        """The reflection's bindings: dmsh_build.py reflect's decisions taken from the SPIR-V resources (their names are the MSL member names reflect reads), each with its set
        and binding; a combined image sampler gives a texture and a sampler entry on one binding"""
        s, uses = self.s, dmsh_spv.image_uses(m)
        sb = {b['slot']: b for b in res['sbufs']}; base = s['SSBO'].get(stage); fetch1d = set(res.get('fetch1d', ()))
        out, seen = [], set()
        for r in sorted(dmsh_spv.resources(m), key=lambda r: (r['set'], r['binding'])):
            if (r['set'], r['binding']) in seen or not r['used']: continue   # one variable per entry point in a merged module (main_u, main_m): one descriptor; a
            # descriptor no function uses is left out, as SPIRV-Cross leaves it out of the Metal argument buffer (the translator declares the support buffer in every shader)
            seen.add((r['set'], r['binding'])); nm, cls = r['name'], r['class']
            e = {'name': nm, 'set': r['set'], 'binding': r['binding'], 'class': cls}
            if r['array'] != 1: e['n'] = r['array']
            if 'dim' in r: e.update({k: r[k] for k in ('dim', 'sampled', 'depth', 'arrayed', 'ms')})
            if cls == 'ubo':
                c = re.fullmatch(r'[a-z]+_c(\d+)', nm)
                if nm == 'support_buffer': e['kind'] = 'support'
                elif not c: e['kind'] = '?'
                elif 3 <= int(c.group(1)) <= 16: e.update(kind='cbuf', reg=int(c.group(1)) - 3)
                else: e.update(kind='c1') if c.group(1) == '1' else e.update(kind='drvcb', bank=int(c.group(1)))
                out.append(e); continue
            if cls == 'ssbo':   # storage buffers: the translator's index → its c0 offset → the NVN slot (as reflect)
                c = re.fullmatch(r'[a-z]+_s(\d+)', nm); b = sb.get(int(c.group(1))) if c else None
                off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
                if stage == 'cs' and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0: e.update(kind='ubo_ptr', reg=(off - 0x210) // 16)
                elif b and b['cb_slot'] >= 3: e.update(kind='sbuf_ubo', reg=b['cb_slot'] - 3, off=b['cb_offset'] * 4)
                else: e.update(kind='sbuf', reg=(off - base) // 16 if off is not None and base is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0 else None)
                e['rw'] = bool(b and b['flags'] & 1)
                out.append(e); continue
            ty = 'array<' if r['array'] != 1 else ''
            if cls in ('storage_image', 'storage_texel_buffer'):
                d = s['decode_tex'](nm)
                e.update(kind='img', reg=d[0] if d else None) if d and re.match(r'[a-z]+_i', nm) else e.update(kind='?')
                u = uses.get(r['id'], set())
                e['rw'] = bool(u & {'write', 'atomic'})
                if 'atomic' in u: e['atomic'] = True
                out.append(e); continue
            if nm.endswith('_cb0_40000000') and r['array'] != 1:   # Ryujinx's array over the whole pool for a handle it cannot trace (nvn_refl.h TEXD / SAMPD)
                out.append({**e, 'kind': 'samp_dyn' if cls == 'sampler' else 'tex_dyn'}); continue
            if cls == 'sampler': out.append({**e, 'kind': '?'}); continue
            sampler = None
            u = s['decode_ubo_tex'](nm)
            if u:   # bindless: the handle is in a uniform buffer
                ut, us = u
                e.update(kind='?') if cls == 'texel_buffer' else e.update(kind='tex_ubo', reg=ut[0], off=ut[1])
                sampler = {'kind': 'samp_id', 'reg': us[1]} if us[0] == 'id' else {'kind': 'samp_ubo', 'reg': us[0], 'off': us[1]}
            elif s['c2_array'](nm, ty) is not None:
                e.update(kind='tex_c2a', off=s['c2_array'](nm, ty)); sampler = {'kind': 'samp_c2a', 'off': s['c2_array'](nm, ty)}
            else:
                d = s['decode_tex'](nm)
                if not d: e['kind'] = '?'
                else:
                    t, sl, hty = d
                    e.update(kind='tbuf' if cls == 'texel_buffer' else 'tex', reg=t)
                    sampler = {'kind': 'samp_id', 'reg': s['const_sampler'](nm)} if hty == 3 else {'kind': 'samp', 'reg': sl} if hty else {'kind': 'samp_tex', 'reg': t}
                    if e['kind'] == 'tex' and t in fetch1d:
                        e['fetch1d'] = True
                        if 'size' in uses.get(r['id'], set()): e['fetch1d_size'] = True
            out.append(e)
            if cls == 'combined' and sampler and e['kind'] != '?': out.append({'name': nm, 'set': r['set'], 'binding': r['binding'], 'class': 'sampler', **sampler})
        return out

    def reflection(self, key, j, c1, m, res, msl, extra):
        name, k = self.loc[key]; stage = j['stage']
        io = dmsh_spv.interface(m)
        dr = self.s['drv_reads'](msl) if msl else {}
        refl = {'key': key, 'target': 'vk', 'chain': self.chain, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': self.pc_of.get(key, ''), 'stage': stage,
                'entry': 'main', 'entries': [e[1] for e in m.entry_points()], 'threadgroup_size': j.get('local'), 'c1': c1.hex(),
                'notes': res['notes'] + (['not in the answer table, queries use defaults'] if j.get('no_facts') else []) + extra.pop('notes', []),
                'drv_reads': dr, 'drv_unfillable': self.s['unfillable'](dr), 'bindings': self.bindings(stage, m, res), 'sbuf_align': res.get('sbuf_align'),
                'vertex_inputs': sorted({x['location']: {'location': x['location'], 'type': x['base'], 'n': x['n']} for x in io if x['storage'] == 'in' and stage == 'vs'}.values(), key=lambda v: v['location']),
                'raster': self.s['raster_facts'](stage, msl) if msl else {}, 'analysis': msl is not None, **extra}
        if stage == 'ps':   # the interpolation of each input location (section 3.2: pipeline libraries without independent interpolation decorations need it to match)
            refl['interp'] = {q: sum(1 << l for l in {x['location'] for x in io if x['storage'] == 'in' and (x['interp'] == q if q in ('flat', 'noperspective') else x[q])})
                              for q in ('flat', 'noperspective', 'centroid', 'sample')}   # a set: a merged module has each input once per entry point
        if j.get('tf'): refl['tf_strides'] = [e['stride'] if e else 0 for e in j['tf']]
        return refl

    # One shader
    def finish(self, key, j, c1, w):
        stage, spv = j['stage'], os.path.join(w, key + '.spv')
        msl = self.render(spv, w) if stage in ('vs', 'ps', 'cs') else None
        extra, notes = {}, []
        if stage in ('vs', 'ps', 'cs') and msl is None: notes.append('[RED] no analysis rendering (SPIRV-Cross): integer inputs and outputs not checked, c1 listed as unproven')
        if stage == 'vs' and msl:   # inputs only used as integers: translated again with them reported as uint (dmsh_build.py int_inputs)
            more = {l: t for l, t in self.s['int_inputs'](msl).items() if l not in j['attr']}
            if more:
                j = dict(j, attr={**j['attr'], **more}); err = self.translate_one(j, w)
                if err: return 'translation failed', 'integer-input retranslation: ' + err, None
                msl = self.render(spv, w)
                extra['int_inputs'] = sorted(int(l) for l in more)
        res = read_json(os.path.join(w, key + '.res.json'))
        if stage == 'ps' and msl:   # an output written from integer bits: main_u (every colour output uint) and, for a mix, main_m (dmsh_build.py fs_uint), by translating again
            fu, mask = self.s['fs_uint'](msl)
            if fu:
                parts, cols = [(spv, 'main')], self.colors(msl)
                for sfx, targets in (('u', cols), ('m', [c for c in cols if mask >> c & 1] if mask else [])):
                    if not targets: continue
                    sk = f'{key}_{sfx}'; err = self.translate_one(dict(j, key=sk, fout={**j['fout'], **{str(c): 'uint' for c in targets}}), w)
                    if err: notes.append(f'[RED] no main_{sfx} ({err})'); continue
                    parts.append((os.path.join(w, sk + '.spv'), 'main_' + sfx))
                if len(parts) > 1:
                    merged = os.path.join(w, key + '.merged.spv')
                    ok, why = dmsh_spv.merge_entries(parts, merged)
                    if ok: os.replace(merged, spv); extra.update({'entry_u': 'main_u'} if any(p[1] == 'main_u' for p in parts) else {})
                    else: notes.append(f'[RED] the integer-output entry points do not link ({why}): built without them')
                    if ok and any(p[1] == 'main_m' for p in parts): extra.update(entry_m='main_m', uint_mask_m=mask)
        pad_want = {}
        if stage == 'vs' and self.pairs.get(key):   # the inputs of the paired pixel shaders already built (vs_pad; pairs whose pixel module is missing are skipped, as on Metal)
            for ps in sorted(self.pairs[key]):
                p = os.path.join(self.out, ps + '.spv')
                if os.path.exists(p):
                    for l, v in dmsh_spv.pixel_inputs(dmsh_spv.Module.read(p)).items(): pad_want.setdefault(l, v)
        m = dmsh_spv.Module.read(spv); done = dmsh_spv.apply(m, stage, pad=pad_want)
        extra['vs_pad'] = done.get('vs_pad', [])
        outputs = {key + '.spv': m}
        fp32 = os.path.join(w, key + '_fp32.spv')
        if os.path.exists(fp32):
            m32 = dmsh_spv.Module.read(fp32); dmsh_spv.apply(m32, stage, pad=pad_want); outputs[key + '_fp32.spv'] = m32; extra['fp32'] = key + '_fp32'
        if stage == 'vs':   # the shadow-input variant (dmsh_vin.py's, as SPIR-V)
            mb = dmsh_spv.Module(m.data()); sh = dmsh_spv.shadow_inputs(mb)
            if isinstance(sh, list): outputs[key + '_bv.spv'] = mb; extra['vin'] = {'entry': 'main', 'shadow': sh}; self.count('shadow-input variants')
            elif sh not in ('no integer input', 'no stage input'): notes.append(f'[RED] no shadow-input variant ({sh}): its integer inputs keep the CPU conversion')
        for name, mod in outputs.items():
            p = os.path.join(w, 'final_' + name); mod.write(p)
            ok, why = dmsh_spv.validate(p)
            if not ok: return 'spirv-val failed', f'{name}: {why}', None
        for name in outputs: copy(os.path.join(w, 'final_' + name), os.path.join(self.out, name))
        for sfx in ('_bv', '_fp32'):   # a variant this build did not make: none left from before
            if key + sfx + '.spv' not in outputs and os.path.exists(os.path.join(self.out, key + sfx + '.spv')): os.unlink(os.path.join(self.out, key + sfx + '.spv'))
        extra['notes'] = notes; extra['passes'] = {k: v for k, v in done.items() if k != 'vs_pad'}
        refl = self.reflection(key, j, c1, m, res, msl, extra)
        atomic_json(os.path.join(self.out, key + '.json'), refl)
        an = os.path.join(self.out, 'analysis')
        if msl:
            with open(os.path.join(w, key + '_b.metal'), 'w') as f: f.write(msl)
            copy(os.path.join(w, key + '_b.metal'), os.path.join(an, key + '_b.metal'))
            atomic_json(os.path.join(an, key + '_b.json'), {'key': key, 'c1': c1.hex(), 'bindings': [{'kind': b['kind'], 'reg': b.get('reg'), 'msl_name': b['name']}
                                                                                                      for b in refl['bindings'] if b['kind'] in ('cbuf', 'sbuf') and b.get('reg') is not None]})
        else:
            for n in (key + '_b.metal', key + '_b.json'):
                if os.path.exists(os.path.join(an, n)): os.unlink(os.path.join(an, n))
        return 'ok', '', refl

    def count(self, what, n=1):
        with self.lock: self.stats[what] += n

    # The cache (dmsh_build.py's model: a key's outputs under its digest)
    def outputs_of(self, key, d):
        return [n for n in (key + '.spv', key + '_bv.spv', key + '_fp32.spv', key + '.json', 'analysis/' + key + '_b.metal', 'analysis/' + key + '_b.json') if os.path.exists(os.path.join(d, n))]

    def restore(self, key, digest):
        d = os.path.join(self.cas, 'shader', digest)
        if not os.path.exists(os.path.join(d, key + '.json')): return False
        for n in self.outputs_of(key, d):
            if n == key + '.json':
                value = read_json(os.path.join(d, n)); value['chain'] = self.chain; atomic_json(os.path.join(self.out, n), value)
            else: copy(os.path.join(d, n), os.path.join(self.out, n))
        return True

    def publish(self, key, digest):
        d = os.path.join(self.cas, 'shader', digest)
        for n in self.outputs_of(key, self.out): copy(os.path.join(self.out, n), os.path.join(d, n))

    def run(self):
        s = self.s
        keys = sorted(self.want) if self.want else sorted(self.loc)
        unknown = [k for k in keys if k not in self.loc]
        if unknown: sys.exit(f'[RED] not in the shader map: {" ".join(unknown[:8])}')
        # A full build starts from no outputs, a partial one from none of its keys (as dmsh_build.py); the stamp goes unless a partial build keeps the current inputs
        stamp_before = dmsh_chain.read_stamp(self.out).get('inputs')
        for d in (self.out, os.path.join(self.out, 'analysis')):
            for n in os.listdir(d) if os.path.isdir(d) else []:
                mk = re.match(r'([0-9a-f]{16})(?:_bv|_fp32|_b)?\.(?:spv|json|metal)$', n)
                if mk and (not self.want or mk.group(1) in self.want): os.unlink(os.path.join(d, n))
        if (not self.want or stamp_before != self.inputs) and os.path.exists(os.path.join(self.out, dmsh_chain.STAMP)): os.unlink(os.path.join(self.out, dmsh_chain.STAMP))
        w = tempfile.mkdtemp(prefix='dmsh_vk_', dir=os.environ.get('WORK'))
        packs, recs, digests, jobs, c1s, hits = {}, {}, {}, {}, {}, set()
        def rec_of(key):
            name, k = self.loc[key]
            if name not in packs:
                with open(os.path.join(self.dm_dir, name), 'rb') as f: packs[name] = s['records'](f.read())
            return name, packs[name][k]
        def digest_of(key, name, rec): return hashlib.sha256(b'target\0vk\0' + s['shader_key'](key, name, rec).encode()).hexdigest()
        for key in keys: recs[key] = rec_of(key); digests[key] = digest_of(key, *recs[key])
        def plain(k): return digests[k] if k in digests else digest_of(k, *rec_of(k)) if k in self.loc else 'missing'
        for key in keys:
            if self.pairs.get(key): digests[key] = s['pair_keyed'](digests[key], [plain(p) for p in sorted(self.pairs[key])])
        for key in keys:
            if self.restore(key, digests[key]): hits.add(key); self.count('cache hit')
            else: jobs[key], c1s[key] = s['job_for'](key, recs[key][0], recs[key][1], w); self.count('cache miss')
        misses = [k for k in keys if k not in hits]
        chunks = [c for c in (misses[i::self.nj] for i in range(self.nj)) if c]
        with cf.ThreadPoolExecutor(self.nj) as ex: xr = list(ex.map(lambda ch: self.translate([jobs[k] for k in ch], w), chunks))
        xerr = {}
        for r in xr:
            for ln in r.stderr.splitlines():
                mr = re.match(r'\[RED\] (\w+): (.*)', ln)
                if mr: xerr[mr.group(1)] = mr.group(2)[:160]
        def fin(key):
            if key in hits: return key, 'ok', '', read_json(os.path.join(self.out, key + '.json'))
            if key in xerr or not os.path.exists(os.path.join(w, key + '.spv')): return key, 'translation failed', xerr.get(key, 'no spv'), None
            try: result = (key,) + self.finish(key, jobs[key], c1s[key], w)
            except Exception as e: result = (key, 'failed', f'{type(e).__name__}: {e}'[:160], None)   # a key that fails is reported, the others still build (as the translator does)
            if result[1] == 'ok': self.publish(key, digests[key])
            return result
        with cf.ThreadPoolExecutor(self.nj) as ex:   # two phases: paired vertex shaders read their pixel shaders' inputs (vs_pad), so they go last
            res = list(ex.map(fin, [k for k in keys if k not in self.pairs])) + list(ex.map(fin, [k for k in keys if k in self.pairs]))
        shutil.rmtree(w, ignore_errors=True)
        self.summary(res)
        atomic_json(os.path.join(self.out, '_vk_build.json'), [{'key': r[0], 'st': r[1], 'err': r[2]} for r in res])
        if not self.want: self.stamp_and_tables([r[3] for r in res if r[1] == 'ok'])
        elif stamp_before != self.inputs: print(f'no library stamp: the profile\'s inputs changed since the last full build ({stamp_before} → {self.inputs}), and a partial build does not rebuild the rest', flush=True)
        return 0 if all(r[1] == 'ok' for r in res) else 1

    def summary(self, res):
        st = collections.Counter(r[1] for r in res); ok = [r[3] for r in res if r[1] == 'ok']
        print(f'shaders {len(res)}: ok {st["ok"]}' + ''.join(f', {k} {v}' for k, v in st.items() if k != 'ok'))
        print(f'shader cache: hit {self.stats["cache hit"]}, built {self.stats["cache miss"]}')
        entries = collections.Counter(e for x in ok for e in x['entries'])
        print('entry points: ' + ', '.join(f'{e} {n}' for e, n in sorted(entries.items())) + f'; FP32 variants {sum(1 for x in ok if x.get("fp32"))}, shadow-input variants '
              f'{sum(1 for x in ok if x.get("vin"))}, integer-input retranslations {sum(1 for x in ok if x.get("int_inputs"))}, padded vertex shaders {sum(1 for x in ok if x.get("vs_pad"))}')
        print(f'analysis renderings: {sum(1 for x in ok if x.get("analysis"))} of {len(ok)}')
        for lvl in [k for k in st if k != 'ok']:
            for e, n in collections.Counter(r[2] for r in res if r[1] == lvl).most_common(10): print(f'  [{lvl}] {n:5d}  {e}')
        unk = collections.Counter(f"{b['kind']} {b['name'][:28]}" for x in ok for b in x['bindings'] if b['kind'] == '?' or (b['kind'] not in KINDS))
        noreg = collections.Counter(f"{b['kind']} {b['name'][:28]}" for x in ok for b in x['bindings'] if b['kind'] in ('cbuf', 'samp', 'samp_tex', 'tex', 'img', 'tbuf', 'sbuf') and b.get('reg') is None)
        print(f'bindings with an unrecognized kind {sum(unk.values())}, with a slot not decoded {sum(noreg.values())} (loading them prints [RED] lines)')
        for e, n in (unk + noreg).most_common(8): print(f'  {n:5d}  {e}')
        for e, n in collections.Counter(nn for x in ok for nn in x['notes'] if nn.startswith('[RED] ')).most_common(8): print(f'  {n:5d}  {e}')

    def stamp_and_tables(self, ok):
        dmsh_chain.write_stamp(self.out, self.game.id, self.inputs)
        print(f'library stamp: game {self.game.id}, inputs {self.inputs}', flush=True)
        an = os.path.join(self.out, 'analysis')
        with tempfile.TemporaryDirectory(prefix='dmsh_vk_tables_', dir=os.environ.get('WORK')) as t:   # the analyses read a directory named as the library, with its stamp
            lib = os.path.join(t, os.path.basename(self.out)); os.makedirs(lib)
            shutil.copyfile(os.path.join(self.out, dmsh_chain.STAMP), os.path.join(lib, dmsh_chain.STAMP))
            for n in os.listdir(an) if os.path.isdir(an) else []: shutil.copyfile(os.path.join(an, n), os.path.join(lib, n))
            for tool, table in (('c1_guard.py', 'c1_full_vk.tsv'), ('cb_extent.py', 'cb_extent_vk.tsv')):
                r = subprocess.run([sys.executable, os.path.join(HERE, tool), lib, str(self.game.table(table))], capture_output=True, text=True)
                if r.returncode: sys.exit(f'[RED] {tool} failed ({r.returncode}): {r.stderr.strip()[-500:]}')
                print(f'{table}: {(r.stdout.strip().splitlines() or [""])[-1]}', flush=True)
        unrendered = sorted(x['key'] for x in ok if not x.get('analysis') and any(b['kind'] == 'c1' for b in x['bindings']))
        if unrendered:   # a shader without an analysis rendering reads its c1 as unproven (the conservative answer)
            with open(self.game.table('c1_full_vk.tsv'), 'a') as f: f.write(''.join(k + '\n' for k in unrendered))
            print(f'c1_full_vk.tsv: {len(unrendered)} shaders without an analysis rendering listed as not proven', flush=True)


FMT_INT = re.compile(r'(?:Uint|Sint)$')   # an answer-table format (Ryujinx TextureFormat) whose texels are integers


def census(cache_root, out_tsv=None):
    """The census of the file header over <cache_root>/vk-<V>/; → the number of reads listed"""
    import dmsh_facts
    facts, chain = dmsh_facts.load(), dmsh_chain_vk.version()
    lib = os.path.join(cache_root, dmsh_chain_vk.PREFIX + chain)
    if not os.path.isdir(lib): sys.exit(f'[RED] no library {lib}: build it first (dmsh_build_vk.py)')
    out_tsv = out_tsv or os.path.join(lib, 'census.tsv'); rows, seen, total = [], 0, collections.Counter()
    for n in sorted(os.listdir(lib)):
        if not re.fullmatch(r'[0-9a-f]{16}\.json', n): continue
        refl = read_json(os.path.join(lib, n)); key = refl['key']; seen += 1
        m = dmsh_spv.Module.read(os.path.join(lib, key + '.spv')); names = m.names()
        by_name = {b['name']: b for b in refl['bindings'] if b['kind'] not in ('samp', 'samp_tex', 'samp_id', 'samp_ubo', 'samp_c2a', 'samp_dyn')}
        for r in dmsh_spv.census(m):
            b = by_name.get(names.get(r['var'], ''), {}); kind, reg = b.get('kind', '?'), b.get('reg')
            table = 'img' if kind == 'img' else 'tex'
            f = (facts.get(key, {}).get(table) or {}).get(str(reg)) if reg is not None else None
            fmt = (f.get('fmt') if isinstance(f, dict) else None) or '-'
            total[r['use']] += 1
            row = (key, refl['stage'], kind, '-' if reg is None else str(reg), str(b.get('set', '-')), str(b.get('binding', '-')), b.get('dim', '-'), r['op'], r['use'], fmt)
            if (r['use'] in ('bits', 'mixed') or FMT_INT.search(fmt)) and row not in rows:   # a merged module reads a slot once per entry point: one row
                rows.append(row)
    os.makedirs(os.path.dirname(os.path.abspath(out_tsv)), exist_ok=True)
    with open(out_tsv, 'w', newline='\n') as f:
        f.write(f'# chain {os.path.basename(lib)}: image reads through a float declaration whose bits are used as an integer, or whose slot the answer table gives an integer format '
                f'(dmsh_build_vk.py --census). {seen} shaders; float-declared reads by use: ' + ', '.join(f'{k} {v}' for k, v in sorted(total.items())) + '\n')
        f.write('key\tstage\tkind\treg\tset\tbinding\tdim\top\tuse\tfacts_format\n')
        for row in rows: f.write('\t'.join(row) + '\n')
    print(f'census: {len(rows)} reads listed in {out_tsv} ({seen} shaders; float-declared reads: ' + ', '.join(f'{k} {v}' for k, v in sorted(total.items())) + ')')
    return len(rows)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['--census'] and len(a) in (2, 3): census(a[1], a[2] if len(a) == 3 else None)
    elif len(a) >= 2 and not a[0].startswith('-'): sys.exit(Build(a[0], a[1], set(a[2:])).run())
    else: sys.exit('usage: see the file header')
