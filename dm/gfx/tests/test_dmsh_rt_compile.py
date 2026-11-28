"""Universal RT build plumbing; synthetic inputs, no shader or native builds."""
import ast
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / 'tools'
sys.path.insert(0, str(HERE.parents[1] / 'tools'))
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(HERE))
import dmsh_abi as abi
import dmsh_ampere
import dmsh_rt_compile as rt
import romfs
import shader_lib
from dmsh_rt import InvalidModule
from dmsh_rt_build import make_job
from test_dmsh_rt_build import record


def keyed_record():
    item, _ = record()
    control, code = item['control'], item['code']
    key = hashlib.sha256(control[:0x30] + bytes(4) + control[0x34:0x48] + code).hexdigest()[:16]
    raw = bytearray(b'BTCDBTCD' + bytes.fromhex(key) + bytes(0x28))
    struct.pack_into('<II', raw, 0x28, len(control), len(code))
    return key, bytes(raw) + code + control


def source(*extra):
    members = [('constant Support*', 'support_buffer'), ('instance_acceleration_structure', 'rt_scene'),
               ('constant Params*', 'rt_dispatch'), ('device Errors*', 'rt_errors'),
               ('const device uint*', 'rt_instance_sbt'), ('constant Bank0*', 'cp_c0'),
               ('constant Bank3*', 'cp_c3'), *extra]
    return ('struct spvDescriptorSetBuffer0\n{\n' + ''.join(
        f'    {ty} {name} [[id({index})]];\n' for index, (ty, name) in enumerate(members)) +
        '};\nkernel void main0()\n{\n}\n')


def resources() -> dict:
    return dict(cbufs=[dict(slot=0, fixed=0), dict(slot=3, fixed=0)], sbufs=[], textures=[], images=[], notes=[])


def runtime(plan) -> dict:
    # Independent public writer extent and reserved ABI constants. Entry records
    # are passed through by the compiler and must match the validated plan.
    return dict(version=1, scene_binding=1, parameters_binding=2, errors_binding=3, instance_sbt_binding=4,
                parameter_words=28, local_bytes=96, local_bias=65536, image_class_base=16,
                membership_base=24, core_offset=0, descriptor_offset=320, entries=plan.rt['entries'],
                as_slots_by_module={str(module.address): [3] for module in plan.modules})


def lift(*names, **environment):
    """Actual build functions without importing its command-line side effects."""
    tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
    tree.body = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    exec(compile(tree, 'dmsh_build.py', 'exec'), environment)
    return environment


class RtCompile(unittest.TestCase):
    def setUp(self):
        self.plan = make_job([record()[0]])

    def test_ordinary_nvn2_graphics_job_accepts_short_control_sections(self):
        code = bytes(0x100) + b'constant data'
        env = lift('job_for', dmsh_ampere=dmsh_ampere, abi=abi, struct=struct, os=os,
                   FACTS={})
        with tempfile.TemporaryDirectory() as work:
            for size in (0x48, 0x50, 0x56, 0x57, 0xe0):
                with self.subTest(control_size=size):
                    control = bytearray(size)
                    struct.pack_into('<II', control, 0, 0x98761234, 0xc9)
                    struct.pack_into('<I', control, 0x38, 0x80)
                    struct.pack_into('<II', control, 0x40, 0x100, len(code))
                    raw = bytes(0x38) + code + control
                    job, c1 = env['job_for']('short-control', 'vs_other.bin', raw, work)
                    self.assertEqual((job['stage'], job['arch']), ('vs', 'sm86'))
                    self.assertEqual(Path(job['code']).read_bytes(), code[:0x80])
                    self.assertEqual(c1, b'constant data')

    def test_ordinary_job_still_rejects_nvn2_rt_stage_when_present(self):
        env = lift('job_for', dmsh_ampere=dmsh_ampere)
        for size in (0x57, 0xe0):
            with self.subTest(control_size=size):
                control = bytearray(size)
                struct.pack_into('<II', control, 0, 0x98761234, 0xc9)
                control[0x56] = 8
                raw = bytes(0x38) + bytes(0x80) + control
                with self.assertRaisesRegex(ValueError, 'stage8 RT record reached the ordinary shader path'):
                    env['job_for']('rt-control', 'vs_other.bin', raw, 'unused')

    def test_rewrites_keep_native_msl4_fences_and_execution_barriers(self):
        fence = ('atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, '
                 'thread_scope_device);')
        barrier = 'threadgroup_barrier(mem_flags::mem_threadgroup);'
        translated = source().replace('kernel void main0()\n{\n}',
                                      'kernel void main0()\n{\n    ' + fence + '\n    ' + barrier + '\n}')
        rewritten = rt.rewrite(translated)
        self.assertEqual(rewritten.count(fence), 1)
        self.assertEqual(rewritten.count(barrier), 1)
        self.assertLess(rewritten.index(fence), rewritten.index(barrier))

    def test_explicit_record_lengths_full_code_and_content_key(self):
        key, raw = keyed_record()
        parsed = rt.package_record(key, raw)
        self.assertEqual(parsed['code'], record()[0]['code'])
        self.assertEqual(parsed['control'], record()[0]['control'])
        self.assertEqual(parsed['key'], key)
        for bad in (raw[:-1], raw + b'\0', b'X' + raw[1:], raw[:0x38],
                    raw[:0x38] + bytes([raw[0x38] ^ 1]) + raw[0x39:]):
            with self.subTest(size=len(bad)), self.assertRaises(InvalidModule):
                rt.package_record(key, bad)
        with self.assertRaises(InvalidModule):
            rt.package_record('0' * 16, raw)

    def test_job_uses_complete_plan_and_cache_covers_all_modules(self):
        job = rt.translator_job(self.plan, '/external/linked.code', debug=False, reduced=True)
        self.assertEqual(job['stage'], 'cs')
        self.assertEqual(job['arch'], 'sm86')
        self.assertEqual(job['local'], [8, 4, 1])
        self.assertEqual(job['rt'], self.plan.rt)
        self.assertEqual(job['img'], {})
        self.assertNotIn('nvn2_cs', job)
        before = self.plan.code
        digest = rt.cache_digest(self.plan, b'tools', 'source', b'profile')
        for args in ((self.plan, b'tools2', 'source', b'profile'),
                     (self.plan, b'tools', 'source2', b'profile'),
                     (self.plan, b'tools', 'source', b'profile2'),
                     (make_job([record()[0], record(2)[0]]), b'tools', 'source', b'profile')):
            self.assertNotEqual(digest, rt.cache_digest(*args))
        self.assertEqual(self.plan.code, before)

    def test_rt_banks_manual_bindings_and_all_image_component_classes(self):
        # Offset0x140 + descriptor handle block0x430 + image table0x120 + slot7*8.
        image_word = (0x140 + 0x430 + 0x120 + 7 * 8) // 4
        names = [f'cp_i2d_cb3_{image_word:X}_{suffix}' for suffix in ('R32Float', 'R32Sint', 'R32Uint')]
        text = source(*[(f'texture2d<{ty}, access::write>', name)
                        for ty, name in zip(('float', 'int', 'uint'), names)])
        binds = rt.bindings(text, resources(), 0x140)
        self.assertEqual([b['kind'] for b in binds[:7]], ['support', 'rt_scene', 'rt_parameters',
                         'rt_errors', 'rt_instance_sbt', 'rt_bank0', 'rt_bank3'])
        for b in binds[5:7]:
            self.assertNotIn('reg', b)
            self.assertNotIn('bank', b)
            self.assertNotIn('fixed', b)
        for index, (b, name, ty) in enumerate(zip(binds[7:], names, ('float', 'int', 'uint')), 7):
            self.assertEqual((b['arg_id'], b['reg'], b['kind'], b['msl_name']), (index, 7, 'img', name))
            self.assertEqual(b['msl_type'], f'texture2d<{ty}, access::write>')
            self.assertNotIn('off', b)
        for name in ('cp_i2d_cb3_1A0_R32Uint', 'cp_i2d_cb3_19D_R32Uint',
                     'cp_i2d_cb4_19C_R32Uint', 'cp_i2d_tcb_19C_R32Uint',
                     'cp_i2d_cb3_400001A4_R32Uint', 'unrecognized'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                rt.bindings(source(('texture2d<uint, access::write>', name)), resources(), 0x140)

    def test_normal_raw_memory_and_pool_classification_is_retained(self):
        res = resources()
        res['sbufs'] = [dict(binding=9, slot=0, cb_slot=abi.GMEM_MAP_CB_SLOT, cb_offset=0, flags=0),
                        dict(binding=10, slot=1, cb_slot=abi.GMEM_MAP_CB_SLOT,
                             cb_offset=abi.GMEM_LOG_CB_OFFSET, flags=1)]
        res['notes'] = ['operation "Store" traced to no storage buffer']
        text = ('struct spvPool_cp_t_cb0_40000000 { uint n; ulong seen; array<texture2d<float>, 1> e; };\n' +
                source(('device RawMap*', 'cp_s0'), ('device RawLog*', 'cp_s1'),
                       ('constant spvPool_cp_t_cb0_40000000*', 'cp_t_cb0_40000000')))
        binds = rt.bindings(text, res, 0x140)
        self.assertEqual([b['kind'] for b in binds[-3:]], ['gmem_map', 'gmem_log', 'tex_pool'])
        self.assertTrue(all(b['rw'] for b in binds[-3:-1]))
        for name in ('cp_c4', 'cp_t2d_cb3_15C', 'cp_s2'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                rt.bindings(source(('device Unknown*', name)), res, 0x140)

    def test_manifest_records_all_modules_and_rejects_compiler_layout_mismatch(self):
        res = dict(resources(), rt=runtime(self.plan))
        result = rt.reflection(self.plan, source(), res, 'test-chain')
        self.assertEqual(result['stage'], 'rt')
        self.assertEqual(result['threadgroup_size'], [8, 4, 1])
        self.assertEqual(result['rt']['catalog'], self.plan.manifest)
        self.assertEqual(result['rt']['module_as_slots'], {'0000000000000001': [3]})
        self.assertEqual(result['rt']['catalog']['resources'][0]['data'], '')
        self.assertEqual(result['rt']['module_entries']['0000000000000001'], [
            dict(address=0, id=1, owner=1, kind=0, frame=96, entry_index=0, region_index=0),
            dict(address=32, id=2, owner=1, kind=0, frame=96, entry_index=0, region_index=1)])
        self.assertEqual(json.loads(json.dumps(result)), result)
        for field in ('version', 'core_offset', 'descriptor_offset', 'local_bytes', 'parameter_words', 'scene_binding'):
            broken = dict(res, rt=dict(res['rt'], **{field: -1}))
            with self.subTest(field=field), self.assertRaises(ValueError):
                rt.reflection(self.plan, source(), broken, 'test-chain')
        with self.assertRaises(ValueError):
            rt.reflection(self.plan, source(), resources(), 'test-chain')
        for dependencies in (None, {}, {'0': [32]}, {'0': [3, 3]}, {'0': [True]}, {'0': [], '256': []}):
            with self.subTest(dependencies=dependencies), self.assertRaises(ValueError):
                rt.reflection(self.plan, source(), dict(res, rt=dict(res['rt'], as_slots_by_module=dependencies)), 'test-chain')

    def test_failed_target_never_publishes_partial_library_or_reflection(self):
        # These are orchestration/failure spies, not fake successful shader tests.
        with tempfile.TemporaryDirectory(prefix='dm-rt-compile-test-') as tmp:
            work = Path(tmp)
            outputs = {t: str(work / t) for t in ('mac', 'ios')}
            root = work / rt.KEY
            def tool_failure(command):
                raise ValueError('translator deliberately rejected input')
            with patch.object(rt, '_run', tool_failure), self.assertRaisesRegex(ValueError, 'rejected'):
                rt.build_library(self.plan, work=tmp, outputs=outputs, targets={'mac', 'ios'},
                                 xlate='xlate', cross='cross', chain='chain', debug=True, reduced=True,
                                 compile_metal=Mock(), with_compiler=Mock())
            calls = []
            def metal_failure(target, src, lib):
                calls.append(target)
                Path(lib).write_bytes(b'partial target output for cleanup test')
                if target == 'ios':
                    raise subprocess.CalledProcessError(1, ['metal'])
            def tool_artifacts(command):
                if command[0] == 'xlate':
                    (root / 'rt_library.spv').write_bytes(b'not used by this failure spy')
                    (root / 'rt_library.res.json').write_text(json.dumps(dict(resources(), rt=runtime(self.plan))))
                else:
                    self.assertEqual(command[command.index('--msl-version') + 1], '40000')
                    (root / 'rt_library.metal').write_text(source())
            with (patch.object(rt, '_run', tool_artifacts),
                  patch.object(rt, 'rewrite', side_effect=lambda x: x)):
                with self.assertRaises(subprocess.CalledProcessError):
                    rt.build_library(self.plan, work=tmp, outputs=outputs, targets={'mac', 'ios'},
                                     xlate='xlate', cross='cross', chain='chain', debug=True, reduced=True,
                                     compile_metal=metal_failure, with_compiler=Mock())
            self.assertEqual(calls, ['mac', 'ios'])
            self.assertEqual(list((work / 'mac').glob('rt_library_b.*')), [])
            self.assertEqual(list((work / 'ios').glob('rt_library_b.*')), [])
            with (patch.object(rt, '_run', tool_artifacts),
                  patch.object(rt, 'rewrite', side_effect=lambda x: x)):
                with self.assertRaisesRegex(ValueError, 'produced no library'):
                    rt.build_library(self.plan, work=tmp, outputs=outputs, targets={'mac', 'ios'},
                                     xlate='xlate', cross='cross', chain='chain', debug=True, reduced=True,
                                     compile_metal=Mock(), with_compiler=Mock())
            self.assertEqual(list((work / 'mac').glob('rt_library_b.*')), [])
            self.assertEqual(list((work / 'ios').glob('rt_library_b.*')), [])

    def test_build_hook_replaces_universal_product_and_keeps_ordinary_cache(self):
        env = lift('owned_output', want={'1' * 16}, dmsh_rt_compile=rt)
        self.assertTrue(env['owned_output']('1' * 16))
        self.assertTrue(env['owned_output']('rt_library'))
        self.assertFalse(env['owned_output']('2' * 16))
        helper = SimpleNamespace(KEY=rt.KEY, cache_digest=Mock(return_value='digest'),
                                 build_library=Mock(side_effect=ValueError('bad RT module')))
        env = lift('rt_finish', dmsh_rt_compile=helper, TOOL_FP=b'', CODE_FP='', PROFILE_ROWS=b'',
                   digests={}, rule_fps={}, cache_lookup=Mock(return_value=(False, 'miss')),
                   restore_cache=Mock(), output_of=Mock(), publish_cache=Mock(), commit=Mock(),
                   w='/tmp', out='/tmp/out', IOSOUT=None, TARGETS={'mac'}, XLATE='xlate', SPIRV_CROSS='cross',
                   CHAIN='chain', MTL=SimpleNamespace(compile=Mock(), with_compiler=Mock()),
                   PRE_STATS=Counter(), os=__import__('os'), json=json, subprocess=subprocess)
        self.assertEqual(env['rt_finish'](self.plan), ('rt_library', 'RT failed', 'bad RT module', None))
        env['publish_cache'].assert_not_called()
        env['commit'].assert_not_called()
        env['cache_lookup'].return_value = True, 'hit'
        env['restore_cache'].return_value = True
        env['output_of'].return_value = b'{"stage":"rt"}'
        self.assertEqual(env['rt_finish'](self.plan)[1], 'ok')
        env['commit'].assert_called_once_with('rt_library')

    def test_rt_only_selection_remains_partial_and_universal_input_is_not_filtered(self):
        tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
        start = next(i for i, n in enumerate(tree.body) if isinstance(n, ast.Assign) and
                     any(isinstance(t, ast.Name) and t.id == 'keys' for t in n.targets) and
                     isinstance(n.value, ast.Call) and getattr(n.value.func, 'id', '') == 'build_keys')
        selection = ast.Module(body=tree.body[start:start + 3], type_ignores=[])
        env: dict = dict(build_keys=lambda want, loc, pairs: sorted(want), want={'b'}, loc={}, PAIRS={}, RT_KEYS={'a', 'b'})
        exec(compile(selection, 'dmsh_build.py', 'exec'), env)
        self.assertEqual(env['keys'], [])
        self.assertEqual(env['want'], {'b'})  # nonempty: output cleanup must preserve ordinary libraries
        plan_statement = next(n for n in tree.body if isinstance(n, ast.Try) and
                              any(isinstance(t, ast.Name) and t.id == 'RT_PLAN' for node in n.body
                                  if isinstance(node, ast.Assign) for t in node.targets))
        seen = []
        env.update(dmsh_rt_build=SimpleNamespace(make_job=list),
                   dmsh_rt_compile=SimpleNamespace(package_record=lambda key, rec: seen.append((key, rec))),
                   rec_of=lambda key: ('unclassified.bin', key.encode()), sys=sys)
        exec(compile(ast.Module(body=[plan_statement], type_ignores=[]), 'dmsh_build.py', 'exec'), env)
        self.assertEqual(seen, [('a', b'a'), ('b', b'b')])

    def test_cache_requires_ios_rt_source_without_changing_ordinary_outputs(self):
        with tempfile.TemporaryDirectory(prefix='dm-rt-cache-test-') as tmp:
            out, ios = (Path(tmp) / label for label in ('mac', 'ios'))
            out.mkdir(); ios.mkdir()
            for directory in (out, ios):
                for key in ('rt_library', 'a' * 16):
                    for ext in ('.json', '.metallib'):
                        (directory / (key + '_b' + ext)).write_text('test cache artifact')
                    if directory == out:
                        (directory / (key + '_b.metal')).write_text('test source artifact')
            env = lift('cache_outputs', out=str(out), IOSOUT=str(ios), TARGETS={'mac', 'ios'},
                       cache_path=lambda digest: digest, dmsh_rt_compile=rt, os=os)
            self.assertIsNotNone(env['cache_outputs']('a' * 16, 'digest'))
            self.assertIsNone(env['cache_outputs']('rt_library', 'digest'))
            (ios / 'rt_library_b.metal').write_text('test source artifact')
            self.assertIsNotNone(env['cache_outputs']('rt_library', 'digest'))

    def test_partial_image_drops_old_rt_and_preserves_unselected_ordinary_library(self):
        with tempfile.TemporaryDirectory(prefix='dm-rt-publish-test-') as tmp:
            out = Path(tmp) / 'library'
            out.mkdir()
            image = shader_lib.image_of(out)
            keep = 'b' * 16 + '_b.metallib'
            shader_lib.write({'rt_library_b.metallib': b'old RT library', 'a' * 16 + '_b.metallib': b'old selected library',
                              keep: b'old unselected library', 'inputs.tsv': b'stamp'}, image)
            (out / 'rt_library_b.json').write_bytes(b'new catalog')
            store = SimpleNamespace(blob_ids=lambda prefix: {}, delete_prefix=Mock())
            env = lift('owned_output', 'publish_library', want={'a' * 16}, dmsh_rt_compile=rt,
                       label_dir=lambda label: str(out), STAGE='stage/test',
                       shader_lib=shader_lib, romfs=romfs, OLD_IMAGE={'mac': romfs.Path(image)},
                       OUT_KEY=re.compile(r'([0-9a-f]{16}|rt_library)_b'),
                       dmsh_chain=SimpleNamespace(STAMP='inputs.tsv'), STAMP_BEFORE='same', INPUTS='same',
                       STORE=store, os=os, shutil=shutil)
            env['publish_library']('mac')
            published = {p.name: p.read_bytes() for p in romfs.Path(image).iterdir()}
            self.assertEqual(published, {keep: b'old unselected library', 'inputs.tsv': b'stamp', 'rt_library_b.json': b'new catalog'})


if __name__ == '__main__':
    unittest.main()
