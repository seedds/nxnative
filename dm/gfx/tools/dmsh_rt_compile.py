"""Offline compilation of one universal NVN2 ray-tracing library.

Package records and metadata define the inputs. No engine filenames, shader
variations, image-format answer table or native private shader-group flags are
used. Metal compilation and publication use the caller's normal target cache.
"""
from __future__ import annotations

from collections.abc import Callable, Mapping, Set
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import dmsh_abi as abi
import dmsh_atomic
import dmsh_reflect
import dmsh_rewrite
import dmsh_wide
from dmsh_rt import InvalidModule
from dmsh_rt_build import BuiltJob


KEY = 'rt_library'
LOCAL = (8, 4, 1)
CROSS_ARGS = ('--msl', '--msl-version', '40000', '--msl-argument-buffers',
              '--msl-texture-buffer-native', '--msl-pad-fragment-output')
_MANUAL = {'rt_scene': abi.KIND_RT_SCENE, 'rt_dispatch': abi.KIND_RT_PARAMETERS,
           'rt_errors': abi.KIND_RT_ERRORS, 'rt_instance_sbt': abi.KIND_RT_INSTANCE_SBT,
           'cp_c0': abi.KIND_RT_BANK0, 'cp_c3': abi.KIND_RT_BANK3}
_IMAGE = re.compile(r'cp_i\w*?_cb(\d+)_([0-9A-F]+)(?:_|$)')


def package_record(key: str, record: bytes) -> dict:
    """Read the explicit BTCDBTCD lengths, preserving the complete code area."""
    if (not re.fullmatch(r'[0-9a-f]{16}', key) or len(record) < 0x38 or
            record[:8] != b'BTCDBTCD' or record[8:16].hex() != key):
        raise InvalidModule(f'{key}: invalid RT package record header/key')
    control_size, code_size = struct.unpack_from('<II', record, 0x28)
    if len(record) != 0x38 + code_size + control_size:
        raise InvalidModule(f'{key}: RT package record lengths disagree with its extent')
    control = record[0x38 + code_size:]
    code = record[0x38:0x38 + code_size]
    identity = control[:0x30] + bytes(4) + control[0x34:0x48] + code
    if hashlib.sha256(identity).hexdigest()[:16] != key:
        raise InvalidModule(f'{key}: RT package record content key disagrees')
    return dict(key=key, control=control, code=code)


def cache_digest(plan: BuiltJob, tools: bytes, code: str, profile: bytes) -> str:
    """The universal product changes when any module or compilation input does."""
    payload = (b'rt-library-v1\0' + json.dumps(plan.manifest, sort_keys=True, separators=(',', ':')).encode() +
               b'\0tools\0' + tools + b'\0code\0' + code.encode() + b'\0profile\0' + profile)
    return hashlib.sha256(payload).hexdigest()


def translator_job(plan: BuiltJob, code_path: str, *, debug: bool, reduced: bool) -> dict:
    return dict(key=KEY, stage='cs', arch='sm86', code=code_path, local=list(LOCAL),
                tex={}, img={}, attr={}, fout={}, c1='', debug=debug, reduced=reduced, rt=plan.rt)


def _image_slot(name: str, descriptor_offset: int) -> int:
    m = _IMAGE.match(name)
    if not m or int(m[1]) != 3:
        raise ValueError(f'{KEY}: unsupported RT image handle resource {name}')
    handle = int(m[2], 16)
    if handle > 0xffffffff or handle >> 28 > 3:
        raise ValueError(f'{KEY}: unsupported RT image handle encoding {name}')
    # Same TextureHandle packed offsets used by the translator accessor. Native
    # bank3 image handles follow the metadata-defined descriptor arena layout.
    words = [handle & 0x3fff]
    if handle >> 28 not in (0, 3):
        words.append((handle >> 14) & 0x3fff)
    base = descriptor_offset + abi.RT_DESCRIPTOR_HANDLES + abi.C2_IMG
    slots = {(word * 4 - base) // abi.C2_ROW for word in words
             if 0 <= word * 4 - base < abi.RT_IMAGE_SLOTS * abi.C2_ROW and
             (word * 4 - base) % abi.C2_ROW == 0}
    if len(slots) != 1:
        raise ValueError(f'{KEY}: RT image handle {name} does not name one image slot')
    return slots.pop()


def bindings(msl: str, resources: dict, descriptor_offset: int) -> list[dict]:
    """Reuse normal pools/raw-map reflection, with explicit RT bank/image kinds."""
    result = dmsh_reflect.reflect(KEY, 'cs', msl, resources)
    for binding in result:
        name = binding['msl_name']
        if name in _MANUAL:
            ty, argument = binding['msl_type'], binding['arg_id']
            binding.clear()
            binding.update(msl_name=name, msl_type=ty, arg_id=argument, kind=_MANUAL[name])
        elif name.startswith('cp_i'):
            binding.update(kind=abi.KIND_IMG, reg=_image_slot(name, descriptor_offset))
            binding.pop('off', None)
            binding.pop('tbx', None)
        if binding['kind'] == abi.KIND_UNKNOWN:
            raise ValueError(f'{KEY}: unrecognized RT resource {name}')
        if binding['kind'] in (abi.KIND_CBUF, abi.KIND_IMG_UBO, abi.KIND_DRVCB,
                                abi.KIND_TEX_UBO, abi.KIND_SAMP_UBO, abi.KIND_TBUF_UBO,
                                abi.KIND_SBUF_UBO, abi.KIND_SBUF_PUSH, abi.KIND_UBO_PTR):
            raise ValueError(f'{KEY}: unsupported ordinary bank resource in RT library: {name}')
        if 'reg' in binding and binding['reg'] is None:
            raise ValueError(f'{KEY}: RT resource has no decoded slot: {name}')
    return result


def reflection(plan: BuiltJob, source: str, resources: dict, chain: str) -> dict:
    """Join verified compiler runtime layout with the immutable input catalog."""
    runtime = resources.get('rt')
    if not isinstance(runtime, dict):
        raise ValueError(f'{KEY}: translator did not emit RT runtime metadata')
    expected = dict(version=1, scene_binding=1, parameters_binding=2, errors_binding=3,
                    instance_sbt_binding=4, core_offset=plan.rt['core_offset'],
                    descriptor_offset=plan.rt['descriptor_offset'], image_class_base=abi.RT_IMAGE_CLASSES,
                    membership_base=abi.RT_MEMBERSHIP, local_bytes=plan.local_memory_bytes,
                    local_bias=abi.RT_LOCAL_BIAS, entries=plan.rt['entries'])
    for name, value in expected.items():
        if runtime.get(name) != value:
            raise ValueError(f'{KEY}: translator RT {name} disagrees with the link plan/ABI')
    max_id = max(entry.id for module in plan.modules for entry in module.entries)
    words = (abi.RT_MEMBERSHIP + max_id // 32 + 1 + 3) & ~3
    if runtime.get('parameter_words') != words:
        raise ValueError(f'{KEY}: translator RT parameter extent disagrees with entry IDs')
    dependencies = runtime.get('as_slots_by_module')
    if not isinstance(dependencies, dict) or set(dependencies) != {str(module.address) for module in plan.modules}:
        raise ValueError(f'{KEY}: translator RT module AS dependencies are missing/inconsistent')
    module_as_slots = {}
    for module in plan.modules:
        slots = dependencies[str(module.address)]
        if (not isinstance(slots, list) or any(type(slot) is not int or not 0 <= slot < 32 for slot in slots) or
                slots != sorted(set(slots))):
            raise ValueError(f'{KEY}: translator RT module AS slot list is invalid')
        module_as_slots[f'{module.key:016x}'] = list(slots)
    entry = re.search(r'\bkernel\s+\S+\s+(\w+)\s*\(', source)
    if not entry:
        raise ValueError(f'{KEY}: translated MSL has no kernel entry')
    notes = list(resources.get('notes', ()))
    if any(n.startswith('translator: Failed to find handle source') for n in notes):
        raise ValueError(f'{KEY}: translator removed a native resource access')
    rt = dict(runtime, catalog=plan.manifest, module_as_slots=module_as_slots,
              module_entries={f'{module.key:016x}':
                              [dict(entry.job_record(), entry_index=entry.entry_index,
                                    region_index=entry.region_index) for entry in module.entries]
                              for module in plan.modules})
    return dict(key=KEY + '_b', route='B', chain=chain, dm_ctl=KEY, source='all stage8 modules',
                stage='rt', entry=entry[1], msl_entry_kind='kernel', threadgroup_size=list(LOCAL),
                bindings=bindings(source, resources, plan.rt['descriptor_offset']), rt=rt,
                notes=notes, drv_reads={}, drv_unfillable=[], c1='', vertex_inputs=[], raster={})


def rewrite(source: str) -> str:
    """The shared memory/resource rewrites applicable to native RT functions."""
    source = dmsh_atomic.check_msl(source)
    for transform in (dmsh_rewrite.precise, dmsh_rewrite.bit0_mask,
                      dmsh_rewrite.select_guards, dmsh_rewrite.mul_extended, dmsh_rewrite.float_add_msl,
                      dmsh_rewrite.gather_offsets, dmsh_rewrite.hmma_msl, dmsh_rewrite.shared_store_msl,
                      dmsh_reflect.pool_arrays, dmsh_wide.wide_loads, dmsh_rewrite.tg_fence,
                      dmsh_rewrite.border_emu, dmsh_rewrite.uniform_tests):
        source = transform(source)
    if 'dmsh_raw_find' in source:
        source = dmsh_rewrite.raw_noinline(source)
    return dmsh_rewrite.max_threads(source, list(LOCAL))


def _run(command: list[str]) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()[-2000:]
        raise ValueError(f'{KEY}: {Path(command[0]).name} failed ({result.returncode}): {detail}')


def build_library(plan: BuiltJob, *, work: str, outputs: Mapping[str, str], targets: Set[str],
                  xlate: str, cross: str, chain: str, debug: bool, reduced: bool,
                  compile_metal: Callable[[str, str, str], object],
                  with_compiler: Callable[[str, dict], dict]) -> dict:
    """Build both targets transactionally; failed work publishes no RT outputs.

    The caller supplies its existing MSL-keyed Metal cache. All required target
    libraries must compile before reflection is written. A failed target removes
    the complete RT product, including a library another target already made.
    """
    root = Path(work) / KEY
    root.mkdir(parents=True, exist_ok=True)
    paths = [Path(directory) / (KEY + '_b' + ext) for directory in outputs.values()
             for ext in ('.metal', '.metallib', '.json')]
    try:
        code = root / 'linked.code'
        code.write_bytes(plan.code)
        job = root / 'jobs.json'
        job.write_text(json.dumps([translator_job(plan, str(code), debug=debug, reduced=reduced)]))
        _run([xlate, str(job), str(root)])
        spirv, metal = root / (KEY + '.spv'), root / (KEY + '.metal')
        # MSL 4 already includes the image-atomic versions; the ordinary atomic
        # helper replaces the version with 3.1 and would lower this requirement.
        _run([cross, *CROSS_ARGS, str(spirv), '--output', str(metal)])
        source = rewrite(metal.read_text())
        metal.write_text(source)
        refl = reflection(plan, source, json.loads((root / (KEY + '.res.json')).read_text()), chain)
        for target, directory in outputs.items():
            Path(directory).mkdir(parents=True, exist_ok=True)
            if target in targets:
                library = Path(directory) / (KEY + '_b.metallib')
                compile_metal(target, str(metal), str(library))
                if not library.is_file() or not library.stat().st_size:
                    raise ValueError(f'{KEY}: {target} Metal compiler produced no library')
        for target, directory in outputs.items():
            (Path(directory) / (KEY + '_b.metal')).write_text(source)
            path = Path(directory) / (KEY + '_b.json')
            temporary = path.with_suffix('.json.tmp')
            temporary.write_text(json.dumps(with_compiler(target, refl), ensure_ascii=False, indent=1))
            temporary.replace(path)
        return refl
    except BaseException:
        for path in paths:
            path.unlink(missing_ok=True)
            path.with_suffix(path.suffix + '.tmp').unlink(missing_ok=True)
        raise
