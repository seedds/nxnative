#!/usr/bin/env python3
"""Export tightly proven zero-image writes as atomic shadow-buffer writes.

This companion preserves the original fragment coverage and coordinates. It does
not prove that a draw covers a rectangle or authorize clearing an entire image.
The host owns complete shadow initialization/copyback and validates its layout.
Inputs and generated shader data stay outside source control; the base shader
translation chain and libraries are unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


SLOT = 29
PROOF = "fragment-zero-writes-v1"
IDENT = r"[A-Za-z_]\w*"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def uncomment(source):
    return re.sub(r"//[^\n]*|/\*.*?\*/", " ", source, flags=re.S)


def prove(source, meta):
    """Accept a small straight-line grammar, never arbitrary Metal expressions."""
    source = uncomment(source)
    if re.search(r"\bdm_zero_(?:store|layout)\b", source):
        raise ValueError("Reserved compatibility symbol is occupied")
    if re.search(r"\[\[\s*buffer\s*\(\s*29\s*\)\s*\]\]", source):
        raise ValueError("Compatibility buffer slot 29 is occupied")
    if meta.get("stage") != "ps" or meta.get("msl_entry_kind") != "fragment":
        raise ValueError("Only fragment entries are supported")
    raster = meta.get("raster")
    if not isinstance(raster, dict) or raster.get("discard") != 0 or raster.get("sample_mask") != 0 or raster.get("colors") != []:
        raise ValueError("Fragment discard, sample mask or color outputs are unproven")
    if meta.get("drv_unfillable") or meta.get("drv_reads") or meta.get("vertex_inputs"):
        raise ValueError("Additional shader inputs are unproven")
    bindings = meta.get("bindings", [])
    if len(bindings) != 2:
        raise ValueError("Exactly one support and one write-only image binding are required")
    support = next((b for b in bindings if b.get("kind") == "support"), None)
    image = next((b for b in bindings if b.get("kind") == "img"), None)
    if not support or not image or support.get("rw") or image.get("rw"):
        raise ValueError("Unexpected or writable resource binding")
    if image.get("msl_type") != "texture2d_array<float, access::write>":
        raise ValueError("Image must be a write-only float 2D array")
    if support.get("arg_id") != 0 or image.get("arg_id") != 1 or not isinstance(image.get("reg"), int) or isinstance(image.get("reg"), bool) or image["reg"] < 0:
        raise ValueError("Unsupported resource argument layout")
    for binding in bindings:
        if not re.fullmatch(IDENT, binding.get("msl_name", "")):
            raise ValueError("Invalid resource name")
    entry = meta.get("entry", "")
    if not re.fullmatch(IDENT, entry):
        raise ValueError("Invalid entry name")

    # The fixed support structure is part of the current host ABI. Matching its
    # entire declaration also rejects hidden device pointers and helper methods.
    grammar = rf"""\A\s*\#include\s*<metal_stdlib>\s*\#include\s*<simd/simd.h>\s*
        using\s+namespace\s+metal\s*;\s*
        struct\s+(?P<support_type>_\d+)\s*\{{\s*
        uint\s+_m0\s*;\s*uint4\s+_m1\s*\[\s*8\s*\]\s*;\s*
        float4\s+_m2\s*;\s*float4\s+_m3\s*;\s*int\s+_m4\s*;\s*
        float4\s+_m5\s*\[\s*73\s*\]\s*;\s*int4\s+_m6\s*;\s*int\s+_m7\s*;\s*\}}\s*;\s*
        struct\s+(?P<argument_type>{IDENT})\s*\{{\s*
        constant\s+(?P=support_type)\s*\*\s*{re.escape(support['msl_name'])}\s*\[\[\s*id\s*\(\s*0\s*\)\s*\]\]\s*;\s*
        (?P<image_declaration>texture2d_array\s*<\s*float\s*,\s*access::write\s*>\s*{re.escape(image['msl_name'])})\s*\[\[\s*id\s*\(\s*1\s*\)\s*\]\]\s*;\s*\}}\s*;\s*
        fragment\s+void\s+{re.escape(entry)}\s*\(\s*
        constant\s+(?P=argument_type)\s*&\s*(?P<arguments>{IDENT})\s*\[\[\s*buffer\s*\(\s*0\s*\)\s*\]\]\s*,\s*
        float4\s+(?P<position>{IDENT})\s*\[\[\s*position\s*\]\]\s*
        (?P<signature_end>\))\s*\{{(?P<body>[^{{}}]*)\}}\s*\Z"""
    match = re.fullmatch(grammar, source, flags=re.X | re.S)
    if not match or support.get("msl_type") != "constant " + match["support_type"] + "*":
        raise ValueError("Source declarations, entry signature or support ABI are unproven")
    values = {}
    writes = []
    body = match["body"]
    statements = body.split(";")
    if statements[-1].strip():
        raise ValueError("Unterminated fragment statement")
    scale_read = f"(*{match['arguments']}.{support['msl_name']})._m5[0].x"
    image_expr = f"{match['arguments']}.{image['msl_name']}"
    replacements = []
    cursor = 0
    for statement in statements[:-1]:
        compact = re.sub(r"\s+", "", statement)
        write = re.fullmatch(re.escape(image_expr) + rf"\.write\(float4\(0(?:\.0+)?\),uint2\(({IDENT})\.xy\),uint\(\1\.z\)\)", compact)
        if write:
            coordinate = values.get(write[1])
            if not coordinate or coordinate[0] != "pixel":
                raise ValueError("Image write coordinates are unproven")
            writes.append(coordinate[1])
            replacements.append((cursor, cursor + len(statement), f"\n    dm_zero_store({image_expr}, uint2({write[1]}.xy), uint({write[1]}.z), dm_zero_layout)"))
        else:
            declaration = re.fullmatch(rf"\s*(float|int|int3)\s+({IDENT})\s*=\s*(.*?)\s*", statement, flags=re.S)
            if not declaration or declaration[2] in values:
                raise ValueError("Only immutable typed locals and proven zero writes are permitted")
            kind, name, expression = declaration.groups()
            expression = re.sub(r"\s+", "", expression)
            value = None
            if kind == "float":
                for axis in "xy":
                    if expression == match["position"] + "." + axis:
                        value = ("fragment", axis)
                if expression == scale_read:
                    value = ("scale",)
                division = re.fullmatch(rf"({IDENT})/({IDENT})", expression)
                if division and values.get(division[1], (None,))[0] == "fragment" and values.get(division[2]) == ("scale",):
                    value = ("scaled", values[division[1]][1])
                truncation = re.fullmatch(rf"trunc\(({IDENT})\)", expression)
                if truncation and values.get(truncation[1], (None,))[0] == "scaled":
                    value = ("truncated", values[truncation[1]][1])
            elif kind == "int":
                conversion = re.fullmatch(rf"int\(({IDENT})\)", expression)
                if conversion and values.get(conversion[1], (None,))[0] == "truncated":
                    value = ("coordinate", values[conversion[1]][1])
            else:
                vector = re.fullmatch(rf"int3\(({IDENT}),({IDENT}),(0|[1-9]\d*)\)", expression)
                if vector and values.get(vector[1]) == ("coordinate", "x") and values.get(vector[2]) == ("coordinate", "y") and int(vector[3]) < 2**31:
                    value = ("pixel", int(vector[3]))
            if value is None:
                raise ValueError("Unproven local expression: " + name)
            values[name] = value
        cursor += len(statement) + 1
    if not writes:
        raise ValueError("No proven zero image writes")
    return source, match, replacements, sorted(set(writes)), image


HELPER = """// The host validates layout and supplies a fully initialized shadow buffer.
static inline void dm_zero_store(device atomic_uint* image, uint2 xy, uint layer,
                                 constant uint* layout) {
    uint width = layout[0], height = layout[1], layers = layout[2];
    uint words = layout[3], row_words = layout[4], image_words = layout[5];
    if (xy.x >= width || xy.y >= height || layer >= layers) return;
    if (words != 1u && words != 2u && words != 4u) return;
    ulong at = ulong(layer) * image_words + ulong(xy.y) * row_words + ulong(xy.x) * words;
    for (uint word = 0; word < words; word++)
        atomic_store_explicit(image + at + word, 0u, memory_order_relaxed);
}

"""


def lower(source, meta):
    source, match, replacements, layers, image = prove(source, meta)
    body = match["body"]
    for begin, end, replacement in reversed(replacements):
        body = body[:begin] + replacement + body[end:]
    source = source[:match.start("body")] + body + source[match.end("body"):]
    position = match.start("signature_end")
    source = source[:position] + f", constant uint* dm_zero_layout [[buffer({SLOT})]]" + source[position:]
    begin, end = match.span("image_declaration")
    source = source[:begin] + "device atomic_uint* " + image["msl_name"] + source[end:]
    position = source.index("fragment void") if "fragment void" in source else re.search(r"fragment\s+void", source).start()
    source = source[:position] + HELPER + source[position:]
    return source, {"proof": PROOF, "parameter_slot": SLOT, "image_aid": image["arg_id"], "image_reg": image["reg"], "layers": layers,
                    "coordinate_proof": "trunc-fragcoord-over-support-scale-x-v1", "support_scale_byte_offset": 192,
                    "layout_words": ["width", "height", "layers", "words_per_pixel", "row_words", "image_words"],
                    "supported_pixel_bytes": [4, 8, 16], "preserves_fragment_coverage": True}


def export(cache, libraries, output, chain):
    if cache.name != "b-" + chain or libraries.name != "b-" + chain:
        raise ValueError("Source and library directories must match the host chain")
    output.mkdir(parents=True, exist_ok=True)
    records = {}
    for reflection in sorted(libraries.glob("*_b.json")):
        meta = json.loads(reflection.read_text())
        if meta.get("stage") != "ps" or not any(b.get("kind") == "img" for b in meta.get("bindings", [])):
            continue
        if meta.get("chain") != chain or meta.get("key") != reflection.stem:
            raise ValueError("Mismatched shader reflection " + reflection.name)
        original = (cache / (reflection.stem + ".metal")).read_bytes()
        try:
            companion, record = lower(original.decode(), meta)
        except ValueError:
            continue  # Unsupported shaders have no authorization record.
        data = companion.encode()
        filename = reflection.stem + ".metal"
        (output / filename).write_bytes(data)
        record.update(source=filename, source_sha256=digest(data), base_library_sha256=digest((libraries / (reflection.stem + ".metallib")).read_bytes()),
                      reflection_sha256=digest(reflection.read_bytes()), original_source_sha256=digest(original))
        records[reflection.stem] = record
    (output / "manifest.json").write_text(json.dumps({"schema": 1, "chain": chain, "kind": "zero-image-clear-v1", "shaders": records}, separators=(",", ":")) + "\n")
    return records


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cache", "libraries", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[2] / "gfx/nvn/dmsh_chain.h"
    chain = re.search(r'#define\s+DMSH_CHAIN\s+"([a-f0-9]+)"', header.read_text()).group(1)
    records = export(args.cache, args.libraries, args.output, chain)
    print(f"Exported {len(records)} proven zero-image companions (chain {chain})")
