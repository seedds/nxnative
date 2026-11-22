"""Synthetic universal RT planning; no game files or native tools required."""
from dataclasses import FrozenInstanceError
import json
from pathlib import Path
import struct
import sys
from typing import Any, cast
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dmsh_rt import InvalidModule
from dmsh_rt_build import make_job
from test_dmsh_rt import Writer, replace


def record(key=1, *, entries=((('begin', 'continue'), 0, 96),), fingerprint=bytes(range(16)),
           resources=(('coreParams', 0x140, 0x40, 0, b''),
                      ('descriptorBaseArray', 0x1430, 0x10, 0x140, b''))) -> tuple[dict, dict[str, int]]:
    """Independent small serialized inputs with explicit expected ABI values."""
    w = Writer()
    w.data.extend(fingerprint)
    w.blob()
    w.align(8)
    w.data.extend(bytes(0xc5))
    w.blob()
    w.put('3B', 0, 0, 0)
    w.align(4)
    w.put('I', 0)
    w.count(0)
    w.align(4)
    w.put('I', 0)
    w.name('unclassified_name')
    w.align(8)
    fixed = w.put('Q5IB3xI', 0, 0, 0, 0, 0, 0, 0, 0)
    w.align(4)
    w.put('I', 0)
    w.count(0)
    w.count(len(entries))
    names = []
    for i, (part, kind, frame) in enumerate(entries):
        w.entry(i, part, kind, frame)
        struct.pack_into('<I', w.data, w.marks[f'entry{i}'] + 4, 0)
        names.extend(part)
    w.count(0)
    w.put('B', 0)
    for _ in range(3):
        w.count(0)
    w.align(4)
    w.put('2I', 0, 3)
    w.count(len(resources))
    for i, resource in enumerate(resources):
        w.resource(i, *resource)
    w.put('B', 0)
    w.align(4)
    w.put('I', 7)
    w.blob()
    inner = bytes(w.data)
    out = Writer()
    out.data.extend(bytes(56) + inner)
    count = len(names)
    h = [0xabc, 0xdef, len(inner), 56, count, 0, 0, 0, 0, 1, 0, len(entries), 0, 0]
    for index, values in [(5, [32 * i for i in range(count)]), (6, [32] * count),
                          (7, [0] * count), (8, [0xffffffff] * count)]:
        out.align(4)
        h[index] = out.put('I' * count, *values)
    positions = []
    for name in names:
        positions.append(len(out.data))
        out.data.extend(name.encode() + b'\0')
    struct.pack_into('<' + 'I' * count, out.data, h[7], *positions)
    out.align(8)
    h[10] = out.put('QQQiIII', 16, 0, 0x27802, -56, 0, count - 1, 0)
    out.align(4)
    h[12] = out.put('I' * len(entries), *(len(e[0]) for e in entries))
    struct.pack_into('<14I', out.data, 0, *h)
    code = bytearray(count * 32)
    struct.pack_into('<QQ', code, 16, 0x27802, 0x000fe40000000f00)
    control = bytearray(0xe0) + out.data
    struct.pack_into('<II', control, 0, 0x98761234, 0xc9)
    struct.pack_into('<I', control, 8, 0xdead)  # No minor-version whitelist.
    struct.pack_into('<I', control, 0x44, len(code))
    control[0x56] = 8
    struct.pack_into('<III', control, 0x5c, len(out.data), 0xe0, len(code))
    marks = {name: at + 56 + 0xe0 for name, at in w.marks.items()}
    marks.update(fixed=fixed + 56 + 0xe0, reloc=h[10] + 0xe0, selector=h[8] + 0xe0)
    return dict(key=key, control=bytes(control), code=bytes(code)), marks


class RtBuild(unittest.TestCase):
    def test_universal_sorted_code_ids_owners_and_emitter_moves(self):
        ray, _ = record(1)
        miss, _ = record(3, entries=((('unrelated_miss_name',), 8, 16),))
        hit, _ = record(9, entries=((('not_an_engine_group',), 7, 64),))
        job = make_job([hit, ray, miss])
        self.assertEqual([(m.key, m.address, m.length) for m in job.modules],
                         [(1, 0, 64), (3, 256, 32), (9, 512, 32)])
        self.assertEqual(job.code, ray['code'] + bytes(192) + miss['code'] + bytes(224) + hit['code'])
        self.assertEqual(job.rt['entries'], [
            dict(address=0, id=1, owner=1, kind=0, frame=96),
            dict(address=32, id=2, owner=1, kind=0, frame=96),
            dict(address=256, id=3, owner=3, kind=8, frame=16),
            dict(address=512, id=4, owner=4, kind=7, frame=64)])
        self.assertEqual(job.rt['moves'], [dict(address=16, template=0x27802, id=2),
                                         dict(address=272, template=0x27802, id=3),
                                         dict(address=528, template=0x27802, id=4)])
        self.assertEqual(job.module_entries[1][1].id, 2)
        self.assertEqual(job.local_memory_bytes, 160)
        self.assertEqual(job.manifest, make_job([miss, hit, ray]).manifest)
        self.assertEqual(json.loads(json.dumps(job.manifest)), job.manifest)
        self.assertEqual(job.manifest['version'], 1)

    def test_named_layout_and_absent_initializers_are_preserved(self):
        item, _ = record(resources=(('descriptorBaseArray', 0x1430, 0x10, 0x240, b''),
                                    ('coreParams', 0x140, 0x40, 0x80, b'')))
        job = make_job([item])
        self.assertEqual([(r.name, r.offset, r.size, r.data, r.fields) for r in job.layout],
                         [('coreParams', 0x80, 0x140, b'', (11, 22, 33)),
                          ('descriptorBaseArray', 0x240, 0x1430, b'', (11, 22, 33))])
        self.assertEqual(job.manifest['resources'][0]['data'], '')
        self.assertEqual((job.rt['core_offset'], job.rt['descriptor_offset']), (0x80, 0x240))
        self.assertEqual(job.manifest['bank_bytes'], 0x1670)

    def test_snapshot_and_read_only_views(self):
        item, _ = record()
        item['control'], item['code'] = bytearray(item['control']), bytearray(item['code'])
        job = make_job([item])
        before = job.manifest
        item['control'][:] = bytes(len(item['control']))
        item['code'][:] = bytes(len(item['code']))
        job.rt['entries'].clear()
        job.manifest['resources'].clear()
        self.assertEqual(before, job.manifest)
        with self.assertRaises(TypeError):
            cast(dict, job.module_entries)[1] = ()
        with self.assertRaises(FrozenInstanceError):
            cast(Any, job.modules[0].entries[0]).id = 9

    def test_normalized_duplicate_keys(self):
        item, _ = record(0xffffffffffffffff)
        duplicate = dict(item, key='FFFFFFFFFFFFFFFF')
        self.assertEqual(len(make_job([item, duplicate]).modules), 1)
        with self.assertRaisesRegex(InvalidModule, 'conflicting'):
            make_job([item, dict(duplicate, code=bytes(len(item['code'])))])
        for key in (-1, 1 << 64, True, '01', 'z' * 16):
            with self.subTest(key=key), self.assertRaises(InvalidModule):
                make_job([dict(item, key=key)])

    def test_invalid_record_and_control(self):
        item, _ = record()
        for records in ([], [{}], [dict(item, control=17)], [dict(item, code='bytes')]):
            with self.subTest(records=records), self.assertRaises(InvalidModule):
                make_job(records)
        for control in (item['control'][:0x67], replace(item['control'], 0, 'I', 0),
                        replace(item['control'], 4, 'I', 1), replace(item['control'], 0x56, 'B', 5),
                        replace(item['control'], 0x5c, 'I', 0), replace(item['control'], 0x60, 'I', 0x60),
                        replace(item['control'], 0x60, 'I', 0xe1),
                        replace(item['control'], 0x5c, 'I', len(item['control'])),
                        replace(item['control'], 0x64, 'I', 16), replace(item['control'], 0x44, 'I', 16)):
            with self.subTest(control=control[:8]), self.assertRaises(InvalidModule):
                make_job([dict(item, control=control)])

    def test_layout_and_fingerprint_disagreement(self):
        first, _ = record(1)
        for second in (record(2, fingerprint=bytes(16))[0],
                       record(2, resources=(('coreParams', 0x140, 0x40, 0, b'a'),
                                             ('descriptorBaseArray', 0x1430, 0x10, 0x140, b'')))[0]):
            with self.assertRaises(InvalidModule):
                make_job([first, second])
        for resources in ((('coreParams', 0x140, 0x40, 0, b''),),
                          (('unknown', 0x140, 0x40, 0, b''), ('descriptorBaseArray', 0x1430, 0x10, 0x140, b'')),
                          (('coreParams', 0x180, 0x40, 0, b''), ('descriptorBaseArray', 0x1430, 0x10, 0x180, b'')),
                          (('coreParams', 0x140, 0x40, 0, b''), ('descriptorBaseArray', 0x1430, 0x10, 0x100, b''))):
            with self.subTest(resources=resources), self.assertRaises(InvalidModule):
                make_job([record(resources=resources)[0]])

    def test_native_memory_and_unsupported_link_variants(self):
        item, marks = record()
        for offset, fmt, value in ((marks['entry0'] + 4, 'I', 16), (marks['fixed'] + 8, 'I', 32),
                                   (marks['selector'], 'i', 0), (marks['reloc'] + 8, 'Q', 1)):
            with self.subTest(offset=offset), self.assertRaises(InvalidModule):
                make_job([dict(item, control=replace(item['control'], offset, fmt, value))])
        for entries in (((('hit', 'unsupported_resume'), 7, 16),), ((('not_raygen',), 8, 0),),
                        ((('ray',), 0, 0x10000), (('hit',), 7, 32)), ((('ray',), 0, 0x10001),)):
            with self.subTest(entries=entries), self.assertRaises(InvalidModule):
                make_job([record(entries=entries)[0]])

    def test_multiple_entries_and_logical_id_exhaustion(self):
        item, _ = record(entries=((('a', 'b'), 0, 32), (('c',), 0, 64)))
        self.assertEqual([e.owner for e in make_job([item]).module_entries[1]], [1, 1, 3])
        # Reuse small valid blobs under distinct keys; fail before allocating an
        # invalid ID or accidentally aliasing a continuation namespace.
        with self.assertRaisesRegex(InvalidModule, 'signed16'):
            make_job(dict(item, key=key) for key in range(10923))


if __name__ == '__main__':
    unittest.main()
