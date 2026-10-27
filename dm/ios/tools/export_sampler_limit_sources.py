#!/usr/bin/env python3
"""Export exact, hash-bound Metal sources with unused scalar samplers removed.

Metal Tier 1 argument buffers expose at most 16 indirect sampler resources to a
single function.  SPIRV-Cross can still declare a sampler for a texture that is
only accessed with ``read()``.  This exporter removes only sampler declarations
whose identifiers are absent everywhere else in the complete source.  It does
not renumber argument IDs or rewrite texture operations.

Game-derived sources and generated companions remain outside source control;
the base shader translation chain and metallibs are unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


LIMIT = 16
PROOF = "unused-sampler-declaration-v1"
IDENT = r"[A-Za-z_]\w*"
SCALAR_DECL = re.compile(
    rf"\bsampler\s+(?P<name>{IDENT})\s*\[\[\s*id\s*\(\s*(?P<aid>\d+)\s*\)\s*\]\]\s*;"
)
ARRAY_SAMPLER = re.compile(r"array<\s*sampler\s*,\s*([1-9]\d*)\s*>")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def _masked_source(source):
    """Blank comments and literals while preserving offsets and line breaks."""
    out = list(source)
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        nxt = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and nxt == "/":
                out[index] = out[index + 1] = " "
                state = "line"
                index += 2
                continue
            if char == "/" and nxt == "*":
                out[index] = out[index + 1] = " "
                state = "block"
                index += 2
                continue
            if char == '"':
                out[index] = " "
                state = "string"
            elif char == "'":
                out[index] = " "
                state = "char"
        elif state == "line":
            if char == "\n":
                state = "code"
            else:
                out[index] = " "
        elif state == "block":
            if char == "*" and nxt == "/":
                out[index] = out[index + 1] = " "
                state = "code"
                index += 2
                continue
            if char != "\n":
                out[index] = " "
        elif state in {"string", "char"}:
            if char == "\\" and index + 1 < len(source):
                if char != "\n":
                    out[index] = " "
                if source[index + 1] != "\n":
                    out[index + 1] = " "
                index += 2
                continue
            delimiter = '"' if state == "string" else "'"
            if char == delimiter:
                out[index] = " "
                state = "code"
            elif char != "\n":
                out[index] = " "
        index += 1
    if state in {"block", "string", "char"}:
        raise ValueError("Unterminated comment or literal")
    return "".join(out)


def _check_preprocessor(masked):
    if "\\\n" in masked or "\\\r\n" in masked:
        raise ValueError("Preprocessor line continuations are unsupported")
    for line in masked.splitlines():
        match = re.match(r"\s*#\s*([A-Za-z_]\w*)", line)
        if match and match.group(1) not in {"include", "pragma"}:
            raise ValueError("Unsupported preprocessor directive #" + match.group(1))


def _struct_body(masked):
    match = re.search(r"\bstruct\s+spvDescriptorSetBuffer0\s*\{", masked)
    if not match:
        raise ValueError("Missing spvDescriptorSetBuffer0 argument structure")
    opening = masked.find("{", match.start())
    depth = 1
    end = opening + 1
    while depth and end < len(masked):
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    if depth:
        raise ValueError("Unbalanced argument structure")
    if re.search(r"\bstruct\s+spvDescriptorSetBuffer0\s*\{", masked[end:]):
        raise ValueError("Duplicate argument structure")
    return opening + 1, end - 1


def sampler_weight(binding):
    msl_type = binding.get("msl_type")
    if msl_type == "sampler":
        return 1
    if isinstance(msl_type, str):
        match = ARRAY_SAMPLER.fullmatch(msl_type)
        if match:
            return int(match.group(1))
    return 0


def sampler_count(meta):
    return sum(sampler_weight(binding) for binding in meta.get("bindings", []))


def lower(source, meta, limit=LIMIT):
    if not isinstance(limit, int) or isinstance(limit, bool) or limit < 1:
        raise ValueError("Invalid sampler limit")
    bindings = meta.get("bindings")
    if not isinstance(bindings, list):
        raise ValueError("Missing reflection bindings")
    original_count = sampler_count(meta)
    if original_count <= limit:
        raise ValueError("Shader does not exceed the sampler limit")
    if any(sampler_weight(binding) > 1 for binding in bindings):
        raise ValueError("Sampler arrays require a separate proof")
    if any(binding.get("kind") in {"img", "imga"} for binding in bindings):
        raise ValueError("Image companions cannot be composed with sampler pruning")

    names = set()
    argument_ids = set()
    scalar = []
    for binding in bindings:
        name = binding.get("msl_name")
        aid = binding.get("arg_id")
        if not isinstance(name, str) or not re.fullmatch(IDENT, name):
            raise ValueError("Invalid resource identifier")
        if not isinstance(aid, int) or isinstance(aid, bool) or aid < 0:
            raise ValueError("Invalid resource argument ID")
        if name in names or aid in argument_ids:
            raise ValueError("Duplicate resource name or argument ID")
        names.add(name)
        argument_ids.add(aid)
        if sampler_weight(binding):
            if not isinstance(binding.get("kind"), str) or not binding["kind"].startswith("samp"):
                raise ValueError("Sampler reflection kind mismatch")
            scalar.append(binding)

    masked = _masked_source(source)
    _check_preprocessor(masked)
    body_begin, body_end = _struct_body(masked)
    declarations = {}
    for match in SCALAR_DECL.finditer(masked, body_begin, body_end):
        name = match.group("name")
        if name in declarations:
            raise ValueError("Duplicate scalar sampler declaration")
        declarations[name] = match
    if set(declarations) != {binding["msl_name"] for binding in scalar}:
        raise ValueError("Sampler declarations and reflection disagree")

    candidates = []
    for binding in scalar:
        match = declarations[binding["msl_name"]]
        if int(match.group("aid")) != binding["arg_id"]:
            raise ValueError("Sampler declaration argument ID mismatch")
        without_declaration = masked[:match.start()] + " " * (match.end() - match.start()) + masked[match.end():]
        if not re.search(rf"\b{re.escape(binding['msl_name'])}\b", without_declaration):
            candidates.append((binding["arg_id"], match, binding))

    needed = original_count - limit
    if len(candidates) < needed:
        raise ValueError("Not enough statically unused scalar samplers")
    selected = sorted(candidates, key=lambda item: item[0])[:needed]
    result = source
    for _, match, _ in sorted(selected, key=lambda item: item[1].start(), reverse=True):
        result = result[:match.start()] + result[match.end():]
    removed = []
    for _, _, binding in selected:
        record = {field: binding[field] for field in ("msl_name", "msl_type", "arg_id", "kind")}
        if "reg" in binding:
            record["reg"] = binding["reg"]
        removed.append(record)
    remaining = original_count - len(removed)
    if remaining > limit or sampler_count({"bindings": [b for b in bindings if b not in [s[2] for s in selected]]}) != remaining:
        raise ValueError("Internal sampler count mismatch")
    return result, {
        "proof": PROOF,
        "limit": limit,
        "original_sampler_count": original_count,
        "remaining_sampler_count": remaining,
        "removed": removed,
    }


def export(cache, libraries, output, chain, limit=LIMIT):
    if cache.name != "b-" + chain or libraries.name != "b-" + chain:
        raise ValueError("Source and library directories must match the host chain")
    output.mkdir(parents=True, exist_ok=True)
    records = {}
    for reflection in sorted(libraries.glob("*_b.json")):
        meta = json.loads(reflection.read_text())
        if sampler_count(meta) <= limit:
            continue
        if meta.get("chain") != chain or meta.get("key") != reflection.stem or meta.get("route") != "B":
            raise ValueError("Mismatched shader reflection " + reflection.name)
        source_path = cache / (reflection.stem + ".metal")
        library_path = libraries / (reflection.stem + ".metallib")
        original = source_path.read_bytes()
        record = {
            "supported": False,
            "original_sampler_count": sampler_count(meta),
            "stage": meta.get("stage"),
            "original_source_sha256": digest(original),
            "base_library_sha256": digest(library_path.read_bytes()),
            "reflection_sha256": digest(reflection.read_bytes()),
        }
        try:
            companion, proof = lower(original.decode(), meta, limit)
            data = companion.encode()
            filename = reflection.stem + ".metal"
            (output / filename).write_bytes(data)
            record.update(proof, supported=True, source=filename, source_sha256=digest(data))
        except (UnicodeDecodeError, ValueError) as error:
            record["reason"] = str(error)
        records[reflection.stem] = record
    manifest = {"schema": 1, "chain": chain, "kind": "sampler-limit-v1", "limit": limit, "shaders": records}
    (output / "manifest.json").write_text(json.dumps(manifest, separators=(",", ":")) + "\n")
    return records


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cache", "libraries", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[2] / "gfx/nvn/dmsh_chain.h"
    match = re.search(r'#define\s+DMSH_CHAIN\s+"([a-f0-9]+)"', header.read_text())
    if match is None:
        raise ValueError(f"No DMSH_CHAIN in {header}")
    active_chain = match.group(1)
    exported = export(args.cache, args.libraries, args.output, active_chain)
    supported = sum(record.get("supported") is True for record in exported.values())
    print(f"Sampler-limit sources: {supported} proven / {len(exported)} over limit (chain {active_chain})")
