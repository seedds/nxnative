"""Production RT catalog/program validation with generated inputs and Metal."""
import hashlib
import json
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
NVN = HERE.parent / 'nvn'
sys.path.insert(0, str(HERE.parent / 'tools'))
sys.path.insert(0, str(HERE))
from dmsh_rt_build import make_job
from dmsh_rt_compile import reflection
from test_dmsh_rt_build import record


class RayTracingProgram(unittest.TestCase):
    def test_catalog_membership_serial_handles_and_borrowed_input_lifetime(self):
        clang = shutil.which('clang')
        if not clang or platform.system() != 'Darwin':
            self.skipTest('requires Apple clang and Metal')
        records = []
        for i, (kind, regions, frame) in enumerate(((0, ('begin', 'resume'), 96), (7, ('closest',), 32), (8, ('miss',), 16))):
            item, _ = record(entries=((regions, kind, frame),))
            code = bytearray(item['code'])
            struct.pack_into('<I', code, 4, i + 1)  # Distinct content keys; code is not executed by this catalog fixture.
            control = item['control']
            key = hashlib.sha256(control[:0x30] + bytes(4) + control[0x34:0x48] + code).hexdigest()[:16]
            records.append(dict(key=key, control=control, code=bytes(code)))
        plan = make_job(records)
        runtime = dict(version=1, scene_binding=1, parameters_binding=2, errors_binding=3, instance_sbt_binding=4,
                       parameter_words=28, local_bytes=128, local_bias=65536, image_class_base=16,
                       membership_base=24, core_offset=0, descriptor_offset=320, entries=plan.rt['entries'],
                       as_slots_by_module={str(module.address): [] if module.entries[0].kind == 8 else [3]
                                           for module in plan.modules})
        res = dict(rt=runtime, notes=[], cbufs=[], sbufs=[], textures=[], images=[])
        rt = reflection(plan, 'kernel void main0() {}', res, 'synthetic-catalog')['rt']
        fixture = dict(rt=rt, module_keys=[item['key'] for item in records],
                       primary=[dict(id=plan.module_entries[int(item['key'], 16)][0].id, kind=kind)
                                for item, kind in zip(records, (0, 7, 8))])
        source = (NVN / 'hle_nvn.c').read_text()
        key_code = source[source.index('#define NVN2_KEY_CTL'):source.index('int prog_key(')]
        template = (HERE / 'rt_api/program.m').read_text().replace('/* PRODUCTION_SHADER_KEY */', key_code)
        with tempfile.TemporaryDirectory(prefix='dm-rt-program-') as tmp:
            root = Path(tmp)
            (root / 'fixture.json').write_text(json.dumps(fixture))
            for i, item in enumerate(records):
                for suffix, field in (('.ctl', 'control'), ('.code', 'code')):
                    (root / f'module{i}{suffix}').write_bytes(item[field])
            test, binary = root / 'program.m', root / 'program'
            test.write_text(template)
            result = subprocess.run([clang, '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                                     '-fobjc-arc', '-I', str(NVN), '-I', str(NVN / 'include'), str(test),
                                     '-framework', 'Metal', '-framework', 'Foundation', '-o', str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary), str(root)], capture_output=True, text=True)
            if result.returncode == 77:
                self.skipTest('no Metal device')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('0 failures', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
