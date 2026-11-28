"""Synthetic RTCore serializer fixtures; no game data or native runtime needed."""
from dataclasses import FrozenInstanceError
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from dmsh_rt import InvalidModule, make_link_plan, parse_module


class Writer:
    def __init__(self):
        self.data = bytearray()
        self.marks = {}

    def align(self, n):
        self.data.extend(bytes((-len(self.data)) & (n - 1)))

    def put(self, fmt, *values):
        p = len(self.data)
        self.data.extend(struct.pack('<' + fmt, *values))
        return p

    def count(self, n):
        self.align(8)
        return self.put('Q', n)

    def blob(self, data=b''):
        self.count(len(data))
        self.data.extend(data)

    def name(self, text):
        self.blob(text.encode() if isinstance(text, str) else text)

    def region(self, name):
        self.name(name)
        self.align(4)
        self.put('17I', *range(17))
        self.align(8)
        self.put('8Q', *range(8))
        self.count(1)  # named access record with unknown numeric fields
        self.name('read')
        self.align(4)
        self.put('16I', *range(16))
        self.count(1)  # u32/u64 pair
        self.put('I', 17)
        self.align(8)
        self.put('Q', 0x100000003)
        self.put('B', 0)
        self.align(4)
        self.put('I', 0)
        self.count(1)  # key/value pair
        self.put('2I', 19, 23)
        self.put('2B', 0, 0)

    def entry(self, number, names, kind, frame):
        self.name(f'entry{number}')
        self.align(4)
        self.marks[f'entry{number}'] = self.put('9I', len(names), 16, frame, 0, 0, 0, 0, 0, kind)
        self.data.extend(bytes(9))
        self.align(4)
        self.put('I', 0)
        self.count(len(names))
        for name in names:
            self.region(name)
        self.align(4)
        self.put('I', 0)
        self.count(0)
        self.align(4)
        self.put('I2B', 0, 0, 0)
        self.align(4)
        self.put('I', 0)

    def resource(self, number, name, size, alignment, offset, data=b''):
        self.name(name)
        self.align(4)
        self.put('I', 3)
        self.align(8)
        self.marks[f'resource{number}'] = self.put('4Q', size, alignment, offset, len(data))
        self.data.extend(data)
        self.align(4)
        self.put('I', 11)
        self.align(8)
        self.put('QI', 22, 33)


def fixture(*, entries=((('start', 'resume'), 0, 96),), resource_name='parameters'):
    """Hand-built small serializer input with deliberately non-title values."""
    w = Writer()
    w.data.extend(bytes(range(16)))
    w.blob()
    w.align(8)
    w.data.extend(bytes(0xc5))
    w.blob()
    w.put('3B', 0, 0, 0)
    w.align(4)
    w.put('I', 0)
    w.marks['parameters'] = w.count(0)
    w.align(4)
    w.put('I', 0)
    w.name('synthetic_module')
    w.align(8)
    w.put('Q5IB3xI', 0, 32, 0, 0, 0, 0, 0, 0)
    w.align(4)
    w.put('I', 0)
    w.count(0)  # module0x180 records
    w.count(len(entries))
    names = []
    for i, (part, kind, frame) in enumerate(entries):
        w.entry(i, part, kind, frame)
        names.extend(part)
    w.count(0)  # named opaque records
    w.put('B', 0)
    w.count(0)  # string/u64 map
    w.count(0)  # string/u64 map
    w.count(0)  # string/string-set map
    w.align(4)
    w.marks['module_fields'] = w.put('2I', 0, 3)  # external source memory kind
    w.count(2)
    w.resource(0, resource_name, 32, 16, 0, b'abc')
    w.resource(1, 'descriptors', 80, 16, 32)
    w.put('B', 0)
    w.align(4)
    w.marks['module_subtype'] = w.put('I', 7)  # external machine-code subtype
    w.blob()
    inner = bytes(w.data)
    out = Writer()
    out.data.extend(bytes(56))
    out.data.extend(inner)
    count = len(names)
    h = [0x123, 0x456, len(inner), 56, count, 0, 0, 0, 0, 1, 0, len(entries), 0, 0]
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
    return bytes(out.data), bytes(code), {k: v + 56 for k, v in w.marks.items()}


def replace(data, offset, fmt, *values):
    data = bytearray(data)
    struct.pack_into('<' + fmt, data, offset, *values)
    return bytes(data)


class RtMetadata(unittest.TestCase):
    def test_numeric_requirements_resources_and_opaque_fields(self):
        meta, code, marks = fixture()
        module = parse_module(meta, code, expected_fingerprint=bytes(range(16)))
        self.assertEqual((module.entries[0].native_stack_bytes,
                          module.entries[0].continuation_frame_bytes), (16, 96))
        self.assertEqual(module.native_module160_bytes, 32)
        self.assertEqual(module.entries[0].fields_offset, marks['entry0'])
        self.assertEqual(module.entries[0].regions[1].fields, tuple(range(17)))
        self.assertEqual([(r.name, r.size, r.alignment, r.offset) for r in module.resources],
                         [('parameters', 32, 16, 0), ('descriptors', 80, 16, 32)])
        self.assertEqual(module.resources[0].data, b'abc')
        self.assertEqual(module.resources[0].fields, (11, 22, 33))

    def test_caller_namespace_and_immutable_plan(self):
        meta, code, _ = fixture()
        module = parse_module(meta, code)
        plan = make_link_plan(module, (71, 72))
        self.assertEqual([r.logical_id for r in plan.regions], [71, 72])
        self.assertEqual(plan.relocations[0].target_logical_id, 72)
        self.assertEqual(module.code, code)
        with self.assertRaises(FrozenInstanceError):
            plan.fingerprint = bytes(16)

    def test_multiple_entries_have_separate_contiguous_ranges(self):
        meta, code, _ = fixture(entries=((('begin', 'after'), 0, 96), (('miss',), 8, 0)))
        module = parse_module(meta, code)
        plan = make_link_plan(module, (101, 102, 301))
        self.assertEqual([r.entry_index for r in plan.regions], [0, 0, 1])
        self.assertEqual(module.entry_region_counts, (2, 1))

    def test_fingerprint_is_explicit_not_a_whitelist(self):
        meta, code, _ = fixture()
        self.assertEqual(parse_module(meta, code).fingerprint, bytes(range(16)))
        for expected in (b'short', bytes(16)):
            with self.subTest(expected=expected), self.assertRaises(InvalidModule):
                parse_module(meta, code, expected_fingerprint=expected)
        with self.assertRaises(InvalidModule):
            make_link_plan(parse_module(meta, code), (1, 2), expected_fingerprint=bytes(16))

    def test_names_are_not_classifier_rules(self):
        meta, code, _ = fixture(entries=((('banana', 'arbitrary'), 7, 128),))
        module = parse_module(meta, code)
        self.assertEqual(module.entries[0].kind, 7)
        self.assertEqual(module.entries[0].continuation_frame_bytes, 128)

    def test_mutable_inputs_are_copied(self):
        meta, code, _ = fixture()
        mutable = bytearray(meta)
        module = parse_module(mutable, bytearray(code))
        mutable[:] = bytes(len(meta))
        self.assertEqual(module.metadata, meta)

    def test_truncation_and_outer_alignment(self):
        meta, code, _ = fixture()
        for bad in (meta[:55], replace(meta, 12, 'I', 57), replace(meta, 8, 'I', len(meta))):
            with self.subTest(size=len(bad)), self.assertRaises(InvalidModule):
                parse_module(bad, code)
        with self.assertRaises(InvalidModule):
            parse_module(meta, code[:-1])

    def test_outer_counts_and_code_extent(self):
        meta, code, _ = fixture()
        h = struct.unpack_from('<14I', meta)
        cases = [replace(meta, 16, 'I', 0xffffffff),
                 replace(meta, h[6], 'I', len(code) + 16),
                 replace(meta, h[12], 'I', 1),
                 replace(meta, 24, 'I', len(meta))]
        for bad in cases:
            with self.subTest(bad=bad[:28]), self.assertRaises(InvalidModule):
                parse_module(bad, code)

    def test_inner_count_and_unsupported_parameter_variant(self):
        meta, code, marks = fixture()
        for value in (1, (1 << 64) - 1):
            with self.assertRaises(InvalidModule):
                parse_module(replace(meta, marks['parameters'], 'Q', value), code)
        with self.assertRaises(InvalidModule):
            parse_module(replace(meta, marks['entry0'], 'I', 3), code)

    def test_symbol_string_validation(self):
        for name in (b'', b'a\0b', b'\xff'):
            with self.subTest(name=name), self.assertRaises(InvalidModule):
                parse_module(*fixture(resource_name=name)[:2])

    def test_resource_alignment_and_extent_overflow(self):
        meta, code, marks = fixture()
        p = marks['resource0']
        for bad in (replace(meta, p + 8, 'Q', 3),
                    replace(meta, p + 16, 'Q', 1),
                    replace(meta, p, 'QQQ', (1 << 64) - 1, 16, 16)):
            with self.assertRaises(InvalidModule):
                parse_module(bad, code)

    def test_unknown_selector_is_inspectable_but_not_linkable(self):
        meta, code, _ = fixture()
        h = struct.unpack_from('<14I', meta)
        module = parse_module(replace(meta, h[8], 'I', 7), code)
        self.assertEqual(module.regions[0].selector, 7)
        with self.assertRaisesRegex(InvalidModule, 'selector'):
            make_link_plan(module, (1, 2))

    def test_overlap_is_preserved_but_execution_variant_is_explicit(self):
        meta, code, _ = fixture()
        h = struct.unpack_from('<14I', meta)
        module = parse_module(replace(meta, h[5] + 4, 'I', 16), code)
        self.assertEqual(module.regions[1].offset, 16)
        with self.assertRaisesRegex(InvalidModule, 'overlapping'):
            make_link_plan(module, (1, 2))

    def test_unknown_entry_kind_and_module_fields_are_explicit(self):
        meta, code, _ = fixture(entries=((('start', 'resume'), 99, 96),))
        with self.assertRaisesRegex(InvalidModule, 'entry kind'):
            make_link_plan(parse_module(meta, code), (1, 2))
        meta, code, marks = fixture()
        with self.assertRaisesRegex(InvalidModule, 'outer metadata flag'):
            make_link_plan(parse_module(replace(meta, 52, 'I', 9), code), (1, 2))
        for offset, value in ((marks['module_fields'] + 4, 99), (marks['module_subtype'], 6)):
            with self.assertRaisesRegex(InvalidModule, 'module fields'):
                make_link_plan(parse_module(replace(meta, offset, 'I', value), code), (1, 2))

    def test_duplicate_symbols_are_inspectable_but_not_linkable(self):
        for args in ({'resource_name': 'descriptors'},
                     {'entries': ((('same', 'same'), 0, 96),)}):
            meta, code, _ = fixture(**args)
            with self.assertRaisesRegex(InvalidModule, 'duplicate'):
                make_link_plan(parse_module(meta, code), (1, 2))

    def test_repeated_patch_sites_are_rejected(self):
        meta, code, _ = fixture()
        h = struct.unpack_from('<14I', meta)
        record = meta[h[10]:h[10] + 40]
        table = len(meta) + (-len(meta) & 7)
        meta = meta + bytes(table - len(meta)) + record * 2
        meta = replace(replace(meta, 36, 'I', 2), 40, 'I', table)
        with self.assertRaisesRegex(InvalidModule, 'duplicate relocation'):
            make_link_plan(parse_module(meta, code), (1, 2))

    def test_relocation_bounds_and_target(self):
        meta, code, _ = fixture()
        h = struct.unpack_from('<14I', meta)
        for bad in (replace(meta, h[10], 'Q', 32),
                    replace(meta, h[10] + 28, 'I', 16),
                    replace(meta, h[10] + 32, 'I', 2)):
            with self.assertRaises(InvalidModule):
                parse_module(bad, code)

    def test_unknown_relocation_and_addend_are_not_applied(self):
        meta, code, _ = fixture()
        h = struct.unpack_from('<14I', meta)
        for bad in (replace(meta, h[10] + 24, 'i', -77),
                    replace(meta, h[10] + 8, 'Q', 1),
                    replace(meta, h[10] + 16, 'Q', 0x1234),
                    replace(meta, h[10] + 36, 'I', 1)):
            module = parse_module(bad, code)
            with self.assertRaises(InvalidModule):
                make_link_plan(module, (1, 2))
            self.assertEqual(module.code, code)

    def test_original_mov_template_is_checked(self):
        meta, code, _ = fixture()
        with self.assertRaisesRegex(InvalidModule, 'template'):
            make_link_plan(parse_module(meta, replace(code, 20, 'I', 99)), (1, 2))

    def test_namespace_rejects_aliases_gaps_overflow_and_reserved_ids(self):
        meta, code, _ = fixture()
        module = parse_module(meta, code)
        for ids in ((1,), (1, 1), (1, 3), (0, 1), (-1, 1), (32767, 32768), (True, 2)):
            with self.subTest(ids=ids), self.assertRaises(InvalidModule):
                make_link_plan(module, ids)


if __name__ == '__main__':
    unittest.main()
