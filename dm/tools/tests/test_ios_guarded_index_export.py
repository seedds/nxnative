"""Dead seed proofs are structural, closed, and bound to verified base bytes.

Fixtures below are synthetic toy functions, not extracted shader sources.
"""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[2] / "ios/tools/export_guarded_index_sources.py"
SPEC = importlib.util.spec_from_file_location("guarded_export", TOOL)
assert SPEC is not None and SPEC.loader is not None
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)
META = {"stage": "vs", "entry": "toy", "entry_z": "toy_z"}

# A small uniform table stores coordinate pairs. A flag switches between those
# pairs and the unmodified input coordinates. The large bitcast seeds are dead
# on both paths, but their integer copies survive outside the guarded blocks.
REGION = """
    int mask = as_type<int>((*args.constants).values[0].w) & 2;
    bool skip = mask != 0;
    copied_guard = skip;
    if (!skip) {
        float trunc_x = trunc(coord_x);
        int int_x = int(trunc_x);
        seed_x = int_x;
    }
    int x = seed_x;
    int sum = x;
    if (!skip) {
        float trunc_y = trunc(coord_y);
        int int_y = int(trunc_y);
        seed_y = int_y;
    }
    int y = seed_y;
    if (!skip) {
        int twice_y = y << 1;
        int joined = twice_y + x;
        sum = joined;
    }
    int index = sum;
    int scaled = index;
    if (!skip) {
        int shifted = index << 4;
        scaled = shifted;
    }
    int offset = scaled;
    if (!skip) {
        int address = offset + 16;
        uint word = uint(int(uint(address) >> uint(2)));
        uint vector = uint(int(uint(int(word)) >> uint(2)));
        int channel = int(word) & 3;
        float value_x = (*args.constants).values[int(vector)][channel];
        int next_word = int(word) + 1;
        uint next_vector = uint(int(uint(next_word) >> uint(2)));
        int next_channel = next_word & 3;
        float value_y = (*args.constants).values[int(next_vector)][next_channel];
        result_x = value_x;
        result_y = value_y;
    }
"""
NAMES = set(re.findall(r"\b(?:int|uint|float|bool) (\w+)\b", REGION)) | {"seed_x", "seed_y", "copied_guard"}


def region(prefix):
    return re.sub(r"\b\w+\b", lambda m: prefix + "_" + m[0] if m[0] in NAMES else m[0], REGION)


def function(name):
    return """
vertex Output NAME(Input in, Args& args) {
    Output out = {};
    float coord_x = in.x;
    float coord_y = in.y;
    float initial = in.z;
    int a_seed_x = as_type<int>(initial);
    int a_seed_y = as_type<int>(initial);
    int b_seed_x = as_type<int>(initial);
    int b_seed_y = as_type<int>(initial);
    bool a_copied_guard;
    bool b_copied_guard;
    float result_x = coord_x;
    float result_y = coord_y;
    if (in.branch) {
REGION_A
    } else {
REGION_B
    }
    out.x = result_x;
    out.y = result_y;
    return out;
}
""".replace("NAME", name).replace("REGION_A", region("a")).replace("REGION_B", region("b"))


SOURCE = "struct Output { float x, y; float point [[point_size]]; };\n" + function("toy") + function("toy_z") + function("toy_capture")


class GuardedIndexProofTests(unittest.TestCase):
    def test_only_selected_entry_changes_and_names_are_not_shader_ids(self):
        transformed, count = EXPORT.transform(SOURCE, META)
        self.assertEqual(count, 4)
        self.assertEqual(transformed[:transformed.index("vertex Output toy_z")], SOURCE[:SOURCE.index("vertex Output toy_z")])
        self.assertEqual(transformed[transformed.index("vertex Output toy_capture"):], SOURCE[SOURCE.index("vertex Output toy_capture"):])
        self.assertEqual(len(re.findall(r"int [ab]_seed_[xy] = 0;", transformed)), 4)
        both, count = EXPORT.transform(SOURCE, META, {"toy", "toy_z"})
        self.assertEqual(count, 8)
        self.assertIn("int a_seed_x = 0;", both)

    def test_each_entry_requires_its_own_complete_proof(self):
        with self.assertRaises(ValueError):
            EXPORT.transform(SOURCE.replace("toy_z(Input", "missing(Input"), META)
        with self.assertRaises(ValueError):
            EXPORT.transform(SOURCE, META, {"toy_capture"})
        bad = SOURCE.replace(
            "int a_seed_x = as_type<int>(initial);", "int a_seed_x = int(initial);", 2)
        with self.assertRaises(ValueError):
            EXPORT.transform(bad, META)

    def test_rejects_near_misses_not_just_wrong_shader_keys(self):
        entry = function("toy_z")
        changes = [
            ("a_seed_x = a_int_x;", "a_seed_x = a_seed_x;"),
            ("if (!a_skip)", "if (a_skip)"),
            ("a_seed_x = a_int_x;", "a_seed_y = a_int_x;"),
            ("out.x = result_x;", "out.x = float(a_seed_x);"),
            ("out.x = result_x;", "out.x = float(a_offset);"),
            ("a_result_x = a_value_x;", "a_result_x = a_value_x; consume(a_word);"),
            ("int a_x = a_seed_x;", "int a_x = a_seed_x; int a_offset = 7;"),
            ("int a_seed_x = as_type<int>(initial);", "volatile int a_seed_x = as_type<int>(initial);"),
            ("float initial = in.z;", "volatile float initial = in.z;"),
        ]
        # The value assignment is deliberately outside NAMES; use its actual
        # spelling for the extra-call near miss.
        changes[5] = ("result_x = a_value_x;", "result_x = a_value_x; consume(a_word);")
        for before, after in changes:
            with self.subTest(before=before):
                self.assertIn(before, entry)
                with self.assertRaises(ValueError):
                    EXPORT.transform(entry.replace(before, after, 1), META)

    def test_comments_do_not_count_as_uses_and_macros_fail_closed(self):
        source = SOURCE.replace("int a_x = a_seed_x;", "// a_seed_x is not a live use\nint a_x = a_seed_x;")
        self.assertEqual(EXPORT.transform(source, META)[1], 4)
        for directive in ("#define a_seed_x another\n", '#include "unknown.h"\n', "#if 1\n"):
            with self.assertRaises(ValueError):
                EXPORT.transform(directive + SOURCE, META)

    def test_adjacent_float_seed_operands_each_receive_a_proof(self):
        source = SOURCE.replace("float initial = in.z;", "float first_initial = in.z; float second_initial = in.y;")
        source = source.replace("as_type<int>(initial);", "as_type<int>(first_initial);")
        source = re.sub(r"(int [ab]_seed_y = as_type<int>\()first_initial(\);)", r"\1second_initial\2", source)
        self.assertEqual(EXPORT.transform(source, META)[1], 4)

    @unittest.skipUnless(shutil.which("c++"), "C++ compiler is needed for executed semantics comparison")
    def test_executed_active_and_inactive_paths_preserve_outputs_and_memory_reads(self):
        # Execute the actual before/after toy functions, including table bounds
        # checks and an ordered read trace. Large/NaN seeds must never become
        # active addresses. This does not claim anything about GPU speculation.
        prefix = r"""
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define vertex
using uint = unsigned;
using std::trunc;
template<class T> T as_type(float x) { T r; std::memcpy(&r, &x, 4); return r; }
unsigned long long trace = 0;
struct Row {
    float x, y, z, w;
    float operator[](int i) const {
        if (i < 0 || i >= 4) std::abort();
        trace = trace * 131 + unsigned(i + 1);
        return i == 0 ? x : i == 1 ? y : i == 2 ? z : w;
    }
};
struct Rows {
    Row rows[32];
    Row& operator[](int i) {
        if (i < 0 || i >= 32) std::abort();
        trace = trace * 131 + unsigned(i + 7);
        return rows[i];
    }
};
struct Table { Rows values; };
struct Args { Table* constants; };
struct Input { float x, y, z; bool branch; };
"""
        harness = r"""
int main() {
    Table table = {};
    for (int i = 0; i < 32; ++i) table.values.rows[i] = {float(i), float(i + 100), float(i + 200), float(i + 300)};
    Args args = {&table};
    float seeds[] = {-1.0f, 0.0f, 1.5f, std::nanf("")};
    for (int flags : {0, 2, 3, 65538}) {
        std::memcpy(&table.values.rows[0].w, &flags, 4);
        for (bool branch : {false, true}) for (float seed : seeds)
        for (int x = 0; x < 3; ++x) for (int y = 0; y < 3; ++y) {
            trace = 0;
            Output out = toy_z({x + 0.75f, y + 0.25f, seed, branch}, args);
            std::printf("%a %a %llu\n", double(out.x), double(out.y), trace);
        }
    }
}
"""
        transformed, _ = EXPORT.transform(SOURCE, META)
        with tempfile.TemporaryDirectory() as tmp:
            outputs = []
            for i, source in enumerate((SOURCE, transformed)):
                path = Path(tmp) / f"variant{i}.cpp"
                path.write_text(prefix + source + harness)
                binary = path.with_suffix("")
                subprocess.run(["c++", "-std=c++17", "-Wno-unknown-attributes", "-include", "initializer_list", str(path), "-o", str(binary)], check=True, capture_output=True)
                outputs.append(subprocess.run([str(binary)], check=True, capture_output=True).stdout)
            self.assertEqual(outputs[0], outputs[1])
            self.assertEqual(len(outputs[0].splitlines()), 288)


class GuardedIndexExportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.libs = Path(self.tmp.name) / "b-abc"
        self.base = self.libs / "nonpoint-v1"
        self.out = self.libs / "guarded-index-v1"
        self.base.mkdir(parents=True)
        self.source = SOURCE.encode()
        self.library = b"MTLB-original"
        self.meta = dict(META, chain="abc", key="0123_b")
        self.reflection = (json.dumps(self.meta) + "\n").encode()
        (self.base / "0123_b.metal").write_bytes(self.source)
        (self.libs / "0123_b.metallib").write_bytes(self.library)
        (self.libs / "0123_b.json").write_bytes(self.reflection)
        self.manifest = {"schema": 1, "chain": "abc", "shaders": {"0123_b": {
            "source_sha256": EXPORT.digest(self.source), "library_sha256": EXPORT.digest(self.library)}}}
        self.write_manifest()

    def write_manifest(self):
        (self.base / "manifest.json").write_text(json.dumps(self.manifest))

    def test_manifest_binds_exact_input_and_output_bytes_and_preserves_originals(self):
        originals = {p: p.read_bytes() for p in self.libs.rglob("*") if p.is_file()}
        self.assertEqual(EXPORT.export(self.libs, self.out, "abc"), 1)
        manifest = json.loads((self.out / "manifest.json").read_bytes())
        data = (self.out / "0123_b.metal").read_bytes()
        self.assertEqual(manifest, {"schema": 1, "chain": "abc", "kind": "guarded-index-v1", "shaders": {"0123_b": {
            "source": "0123_b.metal", "source_sha256": EXPORT.digest(data),
            "original_source_sha256": EXPORT.digest(self.source), "base_library_sha256": EXPORT.digest(self.library),
            "reflection_sha256": EXPORT.digest(self.reflection), "proof": "dead-guarded-index-seeds-v1", "replacement_count": 4}}})
        for path, original in originals.items():
            self.assertEqual(path.read_bytes(), original)

    def test_rejects_mixed_chain_source_library_and_reflection_before_writing(self):
        for path in (self.base / "0123_b.metal", self.libs / "0123_b.metallib", self.libs / "0123_b.json"):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"tampered")
                with self.assertRaises((ValueError, json.JSONDecodeError)):
                    EXPORT.export(self.libs, self.out, "abc")
                self.assertFalse(self.out.exists())
                path.write_bytes(original)
        self.meta["key"] = "other_b"
        (self.libs / "0123_b.json").write_text(json.dumps(self.meta))
        with self.assertRaises(ValueError):
            EXPORT.export(self.libs, self.out, "abc")
        with self.assertRaises(ValueError):
            EXPORT.export(self.libs, self.out, "wrong")

    def test_existing_linked_outputs_cannot_modify_originals(self):
        self.out.mkdir()
        (self.out / "0123_b.metal").hardlink_to(self.base / "0123_b.metal")
        (self.out / "manifest.json").symlink_to(self.base / "manifest.json")
        before = (self.base / "manifest.json").read_bytes()
        self.assertEqual(EXPORT.export(self.libs, self.out, "abc"), 1)
        self.assertEqual((self.base / "0123_b.metal").read_bytes(), self.source)
        self.assertEqual((self.base / "manifest.json").read_bytes(), before)

    def test_rejects_bad_base_manifest_and_output_collision(self):
        for field, value in (("schema", 2), ("chain", "wrong"), ("shaders", [])):
            old = self.manifest[field]
            self.manifest[field] = value
            self.write_manifest()
            with self.assertRaises(ValueError):
                EXPORT.export(self.libs, self.out, "abc")
            self.manifest[field] = old
        self.write_manifest()
        for output in (self.libs, self.base, self.base / "nested"):
            with self.assertRaises(ValueError):
                EXPORT.export(self.libs, output, "abc")

    def test_unsupported_source_has_no_record_or_companion(self):
        unsupported = self.source.replace(b"a_seed_x = a_int_x;", b"a_seed_x = a_seed_x;")
        (self.base / "0123_b.metal").write_bytes(unsupported)
        self.manifest["shaders"]["0123_b"]["source_sha256"] = EXPORT.digest(unsupported)
        self.write_manifest()
        self.assertEqual(EXPORT.export(self.libs, self.out, "abc"), 0)
        self.assertEqual(json.loads((self.out / "manifest.json").read_bytes())["shaders"], {})
        self.assertFalse((self.out / "0123_b.metal").exists())


if __name__ == "__main__":
    unittest.main()
