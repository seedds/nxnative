"""Unit tests for the Z-A offline ASTC pack tool.

These tests use only synthetic bytes and temporary files.  They deliberately do
not require a RomFS, the guest Oodle module, or astcenc.
"""

import hashlib
import importlib.util
import io
import json
import math
from pathlib import Path
import shutil
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


TOOL = Path(__file__).resolve().parents[1] / "za_astc_pack.py"
SPEC = importlib.util.spec_from_file_location("za_astc_pack", TOOL)
assert SPEC is not None and SPEC.loader is not None
ZA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ZA)


def tga_header(kind, width, height, bits=32, descriptor=0x28):
    return struct.pack(
        "<BBBHHBHHHHBB",
        0,
        0,
        kind,
        0,
        0,
        0,
        0,
        0,
        width,
        height,
        bits,
        descriptor,
    )


class TgaTests(unittest.TestCase):
    def test_raw_tga_round_trip_preserves_rgba_and_orientation(self):
        rgba = bytes(
            (
                255, 0, 0, 255,
                0, 255, 0, 128,
                0, 0, 255, 64,
                20, 30, 40, 0,
            )
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "image.tga"
            ZA.write_tga(path, 2, 2, rgba)
            width, height, decoded = ZA.read_tga_rgba(path)
        self.assertEqual((width, height), (2, 2))
        self.assertEqual(decoded, rgba)

    def test_rle_tga_decodes_packets_and_bottom_origin(self):
        # File order is the bottom row followed by the top row.  Each row is
        # encoded as a two-pixel RLE packet in BGRA byte order.
        blue = bytes((255, 0, 0, 255))
        red = bytes((0, 0, 255, 255))
        encoded = tga_header(10, 2, 2, descriptor=8) + bytes((0x81,)) + blue + bytes((0x81,)) + red
        expected_top_first = bytes((255, 0, 0, 255)) * 2 + bytes((0, 0, 255, 255)) * 2
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "rle.tga"
            path.write_bytes(encoded)
            width, height, decoded = ZA.read_tga_rgba(path)
        self.assertEqual((width, height), (2, 2))
        self.assertEqual(decoded, expected_top_first)

    def test_truncated_rle_packet_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.tga"
            path.write_bytes(tga_header(10, 1, 1) + bytes((0x80, 1, 2)))
            with self.assertRaises(ZA.FormatError):
                ZA.read_tga_rgba(path)


class MetricsAndFlatBufferTests(unittest.TestCase):
    def test_rgba_metrics_are_deterministic(self):
        metrics = ZA.rgba_metrics(bytes((0, 0, 0, 0)), bytes((1, 1, 1, 1)))
        self.assertEqual(metrics["rmse_8bit"], 1.0)
        self.assertEqual(metrics["maximum_error_8bit"], 1)
        self.assertAlmostEqual(metrics["psnr_db"], 20 * math.log10(255), places=12)
        self.assertEqual(ZA.rgba_metrics(b"\0\0\0\0", b"\0\0\0\0")["psnr_db"], 999.0)

    def test_flatbuffer_field_and_vector_bounds(self):
        # vtable at 0, table at 8, field 0 at table+4, vector at 20.
        data = bytearray(26)
        struct.pack_into("<4H", data, 0, 8, 8, 4, 0)
        struct.pack_into("<iI", data, 8, 8, 8)
        struct.pack_into("<I", data, 20, 2)
        data[24:26] = b"ok"
        self.assertEqual(ZA.fb_field(data, 8, 0), 12)
        self.assertIsNone(ZA.fb_field(data, 8, 1))
        self.assertEqual(ZA.fb_vector(data, 8, 0), (24, 2))
        with self.assertRaises(ZA.FormatError):
            ZA.fb_field(data, 2, 0)
        with self.assertRaises(ZA.FormatError):
            ZA.fb_root(b"\xff\xff\xff\x7f")


class VirtualPackLayoutTests(unittest.TestCase):
    def test_rebuilds_aligned_prefix_sum_and_round_trips_redirect_table(self):
        packs = [
            {"pack_index": 0, "pack_offset": 16, "pack_size": 17},
            {"pack_index": 1, "pack_offset": 48, "pack_size": 32},
            {"pack_index": 2, "pack_offset": 80, "pack_size": 9},
        ]
        relocated = [{"pack_index": 1, "appended_offset": 128, "appended_bytes": 45}]
        offsets, rows, virtual_root = ZA.virtual_pack_layout(
            packs, relocated, source_root_offset=96, physical_root_offset=192,
            root_bytes=32, physical_file_bytes=224)
        self.assertEqual(offsets, [16, 48, 96])
        self.assertEqual(virtual_root, 112)
        self.assertEqual(rows[0]["kind"], "header")
        self.assertEqual(rows[2], {
            "pack_index": 1, "virtual_offset": 48, "virtual_bytes": 48,
            "physical_offset": 128, "physical_bytes": 48,
        })
        self.assertEqual(rows[-1]["kind"], "root")
        encoded = ZA.redirect_table_bytes(rows, 144, 224)
        parsed = ZA.parse_redirect_table(encoded)
        self.assertEqual(parsed["target"], "arc/data.trpfs")
        self.assertEqual(parsed["virtual_file_bytes"], 144)
        self.assertEqual(len(parsed["records"]), 5)

    def test_rejects_source_without_contiguous_aligned_pack_spans(self):
        packs = [
            {"pack_index": 0, "pack_offset": 16, "pack_size": 17},
            {"pack_index": 1, "pack_offset": 64, "pack_size": 16},
        ]
        with self.assertRaisesRegex(ZA.FormatError, "contiguous aligned span"):
            ZA.virtual_pack_layout(packs, [], 80, 96, 16, 112)

    def test_redirect_table_rejects_overlapping_virtual_ranges(self):
        rows = [
            {"virtual_offset": 16, "virtual_bytes": 32,
             "physical_offset": 16, "physical_bytes": 32},
            {"virtual_offset": 32, "virtual_bytes": 16,
             "physical_offset": 64, "physical_bytes": 16},
        ]
        with self.assertRaisesRegex(ZA.FormatError, "invalid or overlaps"):
            ZA.redirect_table_bytes(rows, 96, 96)


class FslogSelectionTests(unittest.TestCase):
    def test_relocated_manifest_is_a_stable_post_relocation_selection(self):
        manifest = {
            "mode": "insert-relocated-packs-before-root",
            "converted": [{"record": {"pack_index": 4, "file_index": 2},
                           "texture": {"format": 0x2001}}],
            "quality_retained": [
                {"record": {"pack_index": 9, "file_index": 1},
                 "texture": {"format": 0x2006}},
                {"record": {"pack_index": 4, "file_index": 2},
                 "texture": {"format": 0x2001}},
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text(json.dumps(manifest))
            self.assertEqual(ZA.bc7_file_selection(path), {(4, 2), (9, 1)})

    def test_cross_pack_read_and_time_cutoff(self):
        records = [
            {"pack_index": 0, "pack_offset": 100, "pack_size": 20},
            {"pack_index": 1, "pack_offset": 120, "pack_size": 30},
            {"pack_index": 2, "pack_offset": 200, "pack_size": 10},
            # Repeated file records in the same pack must not duplicate an interval.
            {"pack_index": 1, "pack_offset": 120, "pack_size": 30},
        ]
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "fs.tsv"
            log.write_text(
                "1.0\t/owned/romfs/arc/data.trpfs\t115\t10\n"
                "3.0\t/owned/romfs/arc/data.trpfs\t200\t1\n"
                "0.5\t/owned/romfs/arc/other.bin\t100\t100\n"
            )
            self.assertEqual(ZA.fslog_pack_indices(records, log, before=2.0), {0, 1})
            self.assertEqual(ZA.fslog_pack_indices(records, log), {0, 1, 2})

    def test_no_intersection_is_rejected(self):
        records = [{"pack_index": 0, "pack_offset": 100, "pack_size": 20}]
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "fs.tsv"
            log.write_text("1.0\t/owned/romfs/arc/data.trpfs\t0\t10\n")
            with self.assertRaisesRegex(ValueError, "no reads intersecting"):
                ZA.fslog_pack_indices(records, log)


class ApplyBundleTests(unittest.TestCase):
    def test_apply_verifies_source_and_writes_only_manifest_range(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "data.trpfs"
            output = root / "output.trpfs"
            bundle = root / "bundle"
            patches = bundle / "patches"
            patches.mkdir(parents=True)
            original = b"prefix-ORIGINAL-suffix"
            replacement = b"REPLACED"
            offset = original.index(b"ORIGINAL")
            source.write_bytes(original)
            (patches / "000000.bin").write_bytes(replacement)
            manifest = {
                "archive_bytes": len(original),
                "archive_sha256": hashlib.sha256(original).hexdigest(),
                "patches": [{
                    "offset": offset,
                    "bytes": len(replacement),
                    "patch": "patches/000000.bin",
                    "source_sha256": hashlib.sha256(b"ORIGINAL").hexdigest(),
                    "replacement_sha256": hashlib.sha256(replacement).hexdigest(),
                }],
            }
            (bundle / "manifest.json").write_text(json.dumps(manifest))

            def copy_for_test(left, right, allow_copy):
                self.assertFalse(allow_copy)
                shutil.copyfile(left, right)
                return "test-copy"

            args = SimpleNamespace(bundle=bundle, archive=source, output=output, allow_full_copy=False)
            with mock.patch.object(ZA, "clone_or_copy", side_effect=copy_for_test), \
                    mock.patch("sys.stdout", new=io.StringIO()):
                ZA.apply_bundle(args)

            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(output.read_bytes(), b"prefix-REPLACED-suffix")

    def test_apply_rejects_a_source_with_the_wrong_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "data.trpfs"
            output = root / "output.trpfs"
            bundle = root / "bundle"
            bundle.mkdir()
            source.write_bytes(b"source")
            (bundle / "manifest.json").write_text(
                '{"archive_bytes": 6, "archive_sha256": "wrong", "patches": []}'
            )
            args = SimpleNamespace(bundle=bundle, archive=source, output=output, allow_full_copy=False)
            with self.assertRaisesRegex(ValueError, "archive identity"):
                ZA.apply_bundle(args)
            self.assertFalse(output.exists())

    @staticmethod
    def relocated_fixture(root, final_archive_sha=None):
        source_archive = root / "data.trpfs"
        source_descriptor = root / "data.trpfd"
        output = root / "published"
        bundle = root / "bundle"
        bundle.mkdir()
        root_offset = 24
        archive = b"ONEPACK\0" + struct.pack("<Q", root_offset) + b"PREFIX!!" + b"root-slot-------"
        descriptor = b"size----descriptor"
        append = b"relocated-pack-payload"
        final_root_offset = root_offset + len(append)
        virtual_root_offset = 32
        virtual_archive_bytes = virtual_root_offset + len(archive) - root_offset
        root_pointer_replacement = struct.pack("<Q", virtual_root_offset)
        archive_replacement = b"ROOTSLOT"
        descriptor_replacement = b"SIZE++++"
        source_archive.write_bytes(archive)
        source_descriptor.write_bytes(descriptor)
        (bundle / "append.bin").write_bytes(append)
        final_root = archive_replacement + archive[root_offset + 8:]
        final_archive = (archive[:8] + root_pointer_replacement + archive[16:root_offset] +
                         append + final_root)
        final_descriptor = descriptor_replacement + descriptor[8:]
        redirect_rows = [
            {"virtual_offset": 0, "virtual_bytes": 24,
             "physical_offset": 0, "physical_bytes": 24},
            {"virtual_offset": 24, "virtual_bytes": 8,
             "physical_offset": 24, "physical_bytes": 8},
            {"virtual_offset": virtual_root_offset, "virtual_bytes": len(final_root),
             "physical_offset": final_root_offset, "physical_bytes": len(final_root)},
        ]
        redirect_data = ZA.redirect_table_bytes(
            redirect_rows, virtual_archive_bytes, len(final_archive))
        (bundle / "redirects.tsv").write_bytes(redirect_data)
        manifest = {
            "schema": 4,
            "mode": "virtual-pack-layout-before-root",
            "ready_to_apply_to_new_output_directory": True,
            "archive_name": "data.trpfs",
            "descriptor_name": "data.trpfd",
            "archive_bytes": len(archive),
            "archive_sha256": hashlib.sha256(archive).hexdigest(),
            "descriptor_bytes": len(descriptor),
            "descriptor_sha256": hashlib.sha256(descriptor).hexdigest(),
            "archive_root_offset": root_offset,
            "archive_root_bytes": len(archive) - root_offset,
            "archive_root_sha256": hashlib.sha256(archive[root_offset:]).hexdigest(),
            "final_archive_root_offset": final_root_offset,
            "virtual_archive_root_offset": virtual_root_offset,
            "virtual_archive_bytes": virtual_archive_bytes,
            "final_archive_root_sha256": hashlib.sha256(final_root).hexdigest(),
            "archive_header_edits": [{
                "source_offset": 8, "output_offset": 8, "bytes": 8,
                "source_hex": archive[8:16].hex(),
                "replacement_hex": root_pointer_replacement.hex(),
                "source_sha256": hashlib.sha256(archive[8:16]).hexdigest(),
                "replacement_sha256": hashlib.sha256(root_pointer_replacement).hexdigest(),
            }],
            "archive_index_edits": [{
                "source_offset": root_offset,
                "output_offset": final_root_offset,
                "bytes": 8, "source_hex": archive[root_offset:root_offset + 8].hex(),
                "replacement_hex": archive_replacement.hex(),
                "source_sha256": hashlib.sha256(archive[root_offset:root_offset + 8]).hexdigest(),
                "replacement_sha256": hashlib.sha256(archive_replacement).hexdigest(),
            }],
            "descriptor_size_edits": [{
                "offset": 0, "bytes": 8, "source_hex": descriptor[:8].hex(),
                "replacement_hex": descriptor_replacement.hex(),
                "source_sha256": hashlib.sha256(descriptor[:8]).hexdigest(),
                "replacement_sha256": hashlib.sha256(descriptor_replacement).hexdigest(),
            }],
            "append": {"file": "append.bin", "bytes": len(append),
                       "output_offset": root_offset,
                       "sha256": hashlib.sha256(append).hexdigest()},
            "redirects": {"file": "redirects.tsv", "bytes": len(redirect_data),
                          "sha256": hashlib.sha256(redirect_data).hexdigest(),
                          "records": len(redirect_rows), "target": "arc/data.trpfs",
                          "virtual_file_bytes": virtual_archive_bytes,
                          "physical_file_bytes": len(final_archive)},
            "relocated_packs": [{"pack_index": 7}],
            "final_archive_bytes": len(final_archive),
            "final_archive_sha256": final_archive_sha or hashlib.sha256(final_archive).hexdigest(),
            "final_descriptor_bytes": len(final_descriptor),
            "final_descriptor_sha256": hashlib.sha256(final_descriptor).hexdigest(),
        }
        (bundle / "manifest.json").write_text(json.dumps(manifest))
        return source_archive, source_descriptor, output, bundle, archive, descriptor, final_archive, final_descriptor

    def test_apply_relocated_streams_append_and_atomically_publishes_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self.relocated_fixture(Path(tmp))
            source_archive, source_descriptor, output, bundle = fixture[:4]
            original_archive, original_descriptor, final_archive, final_descriptor = fixture[4:]

            def copy_for_test(left, right, allow_copy):
                self.assertFalse(allow_copy)
                shutil.copyfile(left, right)
                return "test-copy"

            args = SimpleNamespace(bundle=bundle, archive=source_archive,
                                   descriptor=source_descriptor, output=output,
                                   allow_full_copy=False)
            with mock.patch.object(ZA, "clone_or_copy", side_effect=copy_for_test), \
                    mock.patch("sys.stdout", new=io.StringIO()):
                ZA.apply_relocated_bundle(args)

            self.assertEqual(source_archive.read_bytes(), original_archive)
            self.assertEqual(source_descriptor.read_bytes(), original_descriptor)
            self.assertEqual((output / "data.trpfs").read_bytes(), final_archive)
            self.assertEqual((output / "data.trpfd").read_bytes(), final_descriptor)
            self.assertEqual((output / "redirects.tsv").read_bytes(),
                             (bundle / "redirects.tsv").read_bytes())
            self.assertTrue((output / "READY.json").is_file())
            self.assertFalse(output.with_name(output.name + ".partial").exists())

    def test_apply_relocated_rejects_legacy_append_after_eof_schema(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self.relocated_fixture(Path(tmp))
            source_archive, source_descriptor, output, bundle = fixture[:4]
            manifest_path = bundle / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["schema"] = 3
            manifest["mode"] = "insert-relocated-packs-before-root"
            manifest_path.write_text(json.dumps(manifest))
            args = SimpleNamespace(bundle=bundle, archive=source_archive,
                                   descriptor=source_descriptor, output=output,
                                   allow_full_copy=False)
            with self.assertRaisesRegex(ValueError, "virtual-layout"):
                ZA.apply_relocated_bundle(args)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + ".partial").exists())

    def test_apply_relocated_rejects_source_root_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self.relocated_fixture(Path(tmp))
            source_archive, source_descriptor, output, bundle = fixture[:4]
            manifest_path = bundle / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["archive_root_sha256"] = "0" * 64
            manifest_path.write_text(json.dumps(manifest))
            args = SimpleNamespace(bundle=bundle, archive=source_archive,
                                   descriptor=source_descriptor, output=output,
                                   allow_full_copy=False)
            with self.assertRaisesRegex(ValueError, "root identity"):
                ZA.apply_relocated_bundle(args)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + ".partial").exists())

    def test_apply_relocated_removes_unpublished_output_after_verification_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self.relocated_fixture(Path(tmp), final_archive_sha="0" * 64)
            source_archive, source_descriptor, output, bundle = fixture[:4]

            def copy_for_test(left, right, allow_copy):
                shutil.copyfile(left, right)
                return "test-copy"

            args = SimpleNamespace(bundle=bundle, archive=source_archive,
                                   descriptor=source_descriptor, output=output,
                                   allow_full_copy=False)
            with mock.patch.object(ZA, "clone_or_copy", side_effect=copy_for_test), \
                    mock.patch("sys.stdout", new=io.StringIO()), \
                    self.assertRaisesRegex(RuntimeError, "full-file verification"):
                ZA.apply_relocated_bundle(args)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + ".partial").exists())


if __name__ == "__main__":
    unittest.main()
