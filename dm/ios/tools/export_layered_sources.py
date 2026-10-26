#!/usr/bin/env python3
"""Export conservatively proven per-slice shaders beside chain-matched libraries.

Game-derived sources and outputs stay outside source control. The base shader
chain and libraries are unchanged. Unknown layer expressions remain unsupported.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

SLOT = 30
READ_ONLY = {"cbuf", "c1", "drvcb", "support", "tex", "tbuf", "samp", "sampt", "sampu", "texu", "samp_tex", "samp_ubo"}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def functions(source):
    result = []
    for match in re.finditer(r"\b(vertex|fragment|kernel)\s+\w+\s+(\w+)\s*\(", source):
        at = match.end() - 1
        depth = 1
        end = at + 1
        while depth and end < len(source):
            depth += (source[end] == "(") - (source[end] == ")")
            end += 1
        opening = source.find("{", end)
        if opening < 0:
            raise ValueError("Missing function body")
        depth = 1
        close = opening + 1
        while depth and close < len(source):
            depth += (source[close] == "{") - (source[close] == "}")
            close += 1
        if depth:
            raise ValueError("Unbalanced function body")
        result.append((match.group(1), match.group(2), match.start(), end - 1, opening, close))
    return result


def pure(meta):
    return all(binding.get("kind") in READ_ONLY and not binding.get("rw") and
               "access::write" not in binding.get("msl_type", "") and
               "access::read_write" not in binding.get("msl_type", "")
               for binding in meta.get("bindings", []))


def prove_layer(body):
    if re.search(r"\b(if|for|while|switch|atomic|threadgroup)\b", body):
        raise ValueError("Layer proof requires a straight-line, side-effect-free vertex entry")
    layers = re.findall(r"out\.gl_Layer\s*=\s*([^;]+);", body)
    if len(layers) != 1 or len(re.findall(r"return\s+out\s*;", body)) != 1:
        raise ValueError("Expected one layer assignment and one return")
    definitions = {}
    for name, expression in re.findall(r"\b(_\d+)\s*=\s*([^;]+);", body):
        if name in definitions:
            raise ValueError("Mutable layer temporary")
        definitions[name] = expression
    checked = set()

    def visit(expression):
        if "gl_VertexIndex" in expression or "gl_VertexID" in expression or "out." in expression:
            raise ValueError("Layer varies with vertex ID")
        for name in re.findall(r"\b[A-Za-z_]\w*\b", expression):
            if name in {"uint", "int", "float", "as_type", "int32_t", "uint32_t", "gl_InstanceIndex", "gl_BaseInstance", "x", "y", "z", "w"}:
                continue
            if re.fullmatch(r"_\d+|_m\d+|spvDescriptorSet\d+|vp_c\d+", name):
                continue
            raise ValueError("Unproven layer dependency " + name)
        for name in re.findall(r"\b_\d+\b", expression):
            if name in checked:
                continue
            if name not in definitions:
                raise ValueError("Unresolved layer dependency")
            checked.add(name)
            visit(definitions[name])
        # SPIRV-Cross arithmetic and uniform reads are the only accepted calls.
        calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*(?:<[^>]+>)?\s*\(", expression))
        if calls - {"as_type", "uint", "int", "float", "int32_t", "uint32_t"}:
            raise ValueError("Unproven function in layer expression")
    visit(layers[0])


def lower(source, meta):
    if not pure(meta) or re.search(r"\[\[\s*buffer\s*\(\s*30\s*\)\s*\]\]", source):
        raise ValueError("Writable resources or occupied compatibility buffer slot")
    stage = meta.get("stage")
    if stage == "vs":
        if meta.get("vertex_inputs") or "point_size" in source:
            raise ValueError("Vertex inputs or point-size output require a separate proof")
        entries = {meta.get("entry"), meta.get("entry_z")} - {None}
        found = set()
        for kind, name, start, signature_end, opening, end in reversed(functions(source)):
            if kind != "vertex" or name not in entries:
                source = source[:start] + source[end:]
                continue
            body = source[opening:end]
            prove_layer(body)
            found.add(name)
            body = body.replace("out.gl_Layer", "dm_slice_layer")
            body = body.replace("{", "{\n    uint dm_slice_layer = 0;", 1)
            body = re.sub(r"return\s+out\s*;", "if (dm_slice_layer != dm_selected_slice) out.gl_Position = float4(2.0, 2.0, 2.0, 1.0);\n    return out;", body)
            signature = source[start:signature_end] + ", constant uint& dm_selected_slice [[buffer(30)]])"
            source = source[:start] + signature + "\n" + body + source[end:]
        if found != entries:
            raise ValueError("Missing vertex entry")
        source, count = re.subn(r"\s*uint\s+gl_Layer\s*\[\[render_target_array_index\]\]\s*;", "", source)
        if count != 1:
            raise ValueError("Unknown vertex layer output structure")
    elif stage == "ps":
        source, count = re.subn(r"\buint\s+gl_Layer\s*\[\[render_target_array_index\]\]", "constant uint& gl_Layer [[buffer(30)]]", source)
        if not count:
            raise ValueError("Unknown fragment layer input")
    else:
        raise ValueError("Unsupported shader stage")
    if "render_target_array_index" in source:
        raise ValueError("Unlowered layer builtin")
    return source


def export(cache, libraries, output, chain):
    if cache.name != "b-" + chain or libraries.name != "b-" + chain:
        raise ValueError("Source and library directories must match the host chain")
    output.mkdir(parents=True, exist_ok=True)
    records = {}
    for path in sorted(libraries.glob("*_b.json")):
        original = (cache / (path.stem + ".metal")).read_bytes()
        if b"render_target_array_index" not in original:
            continue
        meta = json.loads(path.read_text())
        if meta.get("key") != path.stem or meta.get("chain") != chain:
            raise ValueError("Mismatched shader reflection " + path.name)
        record = {"stage": meta.get("stage"), "library_sha256": digest((libraries / (path.stem + ".metallib")).read_bytes()),
                  "reflection_sha256": digest(path.read_bytes()), "original_source_sha256": digest(original)}
        try:
            data = lower(original.decode(), meta).encode()
            (output / (path.stem + ".metal")).write_bytes(data)
            record.update(supported=True, proof="instance-layer-pure-v1", source_sha256=digest(data), slot=SLOT)
        except ValueError as error:
            record.update(supported=False, reason=str(error))
        records[path.stem] = record
    (output / "manifest.json").write_text(json.dumps({"schema": 1, "chain": chain, "kind": "layered-slices-v1", "shaders": records}, separators=(",", ":")) + "\n")
    return records


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cache", "libraries", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[2] / "gfx/nvn/dmsh_chain.h"
    chain = re.search(r'#define\s+DMSH_CHAIN\s+"([a-f0-9]+)"', header.read_text()).group(1)
    records = export(args.cache, args.libraries, args.output, chain)
    print(f"Layered sources: {sum(r['supported'] for r in records.values())} proven / {len(records)} total (chain {chain})")
