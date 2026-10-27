#!/usr/bin/env python3
"""Export hash-gated MIN-reduction companions without changing base shaders.

The two audited shaders each have one ordinary, integer-explicit-LOD sample of
slot 17. Runtime must additionally validate the recorded sampler/texture contract
before selecting a companion. Inputs and exported game-derived MSL stay outside
the repository. No compiled library, original source or reflection is replaced.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
CHAIN = "f80f842de321"
KIND = "linear-min-reduction-v1"
AUDITED = {
    "151607c7a92386cc_b": {
        "stage": "ps", "prefix": "fp",
        "original_source_sha256": "7c6e776f1cccd8ecf7310e5083664697e35de1ab85a9f21466cd307426de297f",
    },
    "ec5ebd6d9902a9a1_b": {
        "stage": "cs", "prefix": "cp",
        "original_source_sha256": "bcd3d2c291a896ee1a22a828b7a461cc6501a33e835465df24d5c3cb71610435",
    },
}
SAMPLER = {
    "reduction": 1, "minf": 3, "magf": 1,
    "ws": 7, "wt": 7, "wr": 7,
    "lod_min": 0, "lod_max": 1000, "lod_bias": 0, "aniso": 1,
    "cmp_mode": 0,
}
HELPER_BEGIN = "// DM_MIN_REDUCTION_HELPER_BEGIN\n"
HELPER_END = "// DM_MIN_REDUCTION_HELPER_END\n"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def helper_source():
    directory = ROOT / "dm/gfx/nvn"
    parts = []
    for name in ("nvn_reduction_footprint.h", "nvn_reduction_sample.metal.h"):
        lines = (directory / name).read_text().splitlines()
        lines = [line for line in lines if line != "#pragma once" and line != '#include "nvn_reduction_footprint.h"']
        parts.append("\n".join(lines).rstrip())
    return "\n\n".join(parts) + "\n"


def close_paren(source, start):
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "(":
            depth += 1
        elif source[index] == ")":
            depth -= 1
            if not depth:
                return index
    raise ValueError("Unbalanced generated sample call")


def split_args(source):
    depth, start, args = 0, 0, []
    for index, char in enumerate(source):
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and not depth:
            args.append(source[start:index].strip())
            start = index + 1
    args.append(source[start:].strip())
    return args


def transform(source, key):
    """Replace exactly one audited binding's sample; preserve the entry and ABI.

    Export enforces the original SHA before calling this structural transform.
    Keeping this function independent permits entirely synthetic unit fixtures.
    """
    prefix = AUDITED[key]["prefix"]
    texture = f"spvDescriptorSet0.{prefix}_t_tcb_2A"
    sampler = texture + "Smplr"
    call = re.compile(re.escape(texture) + r"\.sample\s*\(")
    matches = list(call.finditer(source))
    if len(matches) != 1 or HELPER_BEGIN in source:
        raise ValueError("Expected exactly one unmodified slot-17 ordinary sample")
    match = matches[0]
    end = close_paren(source, match.end() - 1)
    args = split_args(source[match.end():end])
    if len(args) != 3 or args[0] != sampler:
        raise ValueError("Unexpected generated sampler/coordinate/LOD arguments")
    lod = re.fullmatch(r"level\((.*)\)", args[2], re.S)
    if not lod or not lod.group(1).strip():
        raise ValueError("Expected one explicit level() argument without offsets")
    replacement = f"dm_reduction_sample_lmn_edge({texture}, {args[1]}, {lod.group(1)}, 0.0f, 1000.0f, false)"
    variant = source[:match.start()] + replacement + source[end + 1:]
    marker = "using namespace metal;"
    if variant.count(marker) != 1:
        raise ValueError("Expected one generated Metal namespace declaration")
    position = variant.index(marker) + len(marker)
    payload = helper_source()
    injected = "\n" + HELPER_BEGIN + payload + HELPER_END
    return variant[:position] + injected + variant[position:], payload


def validate_reflection(meta, key, chain):
    profile = AUDITED[key]
    if meta.get("chain") != chain or meta.get("key") != key or meta.get("stage") != profile["stage"] or meta.get("entry") != "main0":
        raise ValueError(f"Mismatched reflection: {key}")
    bindings = meta.get("bindings", [])
    prefix = profile["prefix"]
    for kind, name, msl_type in (("tex", f"{prefix}_t_tcb_2A", "texture2d<float>"),
                                 ("samp_tex", f"{prefix}_t_tcb_2ASmplr", "sampler")):
        found = [row for row in bindings if row.get("kind") == kind and row.get("reg") == 17]
        if len(found) != 1 or found[0].get("msl_name") != name or found[0].get("msl_type") != msl_type:
            raise ValueError(f"Mismatched slot-17 reflection: {key} {kind}")


def export(cache, libraries, output, chain):
    cache, libraries, output = Path(cache).resolve(), Path(libraries).resolve(), Path(output).resolve()
    if chain != CHAIN or cache.name != f"b-{chain}" or libraries.name != f"b-{chain}":
        raise ValueError("Source/library directories and chain must match the audited shader chain")
    for path in (cache, libraries, output):
        if path == ROOT or ROOT in path.parents:
            raise ValueError("Game-derived inputs and exported sources must stay outside the repository")
    if any(output == original or output in original.parents or original in output.parents for original in (cache, libraries)):
        raise ValueError("Companion output must not overlap an original source/library directory")
    pending = {}
    for key, profile in AUDITED.items():
        meta = json.loads((libraries / (key + ".json")).read_text())
        validate_reflection(meta, key, chain)
        original = (cache / (key + ".metal")).read_bytes()
        if digest(original) != profile["original_source_sha256"]:
            raise ValueError(f"Original source SHA256 differs from the disassembled/audited shader: {key}")
        library = (libraries / (key + ".metallib")).read_bytes()
        variant, payload = transform(original.decode("utf-8"), key)
        data = variant.encode("utf-8")
        record = {
            "schema": 1, "chain": chain, "kind": KIND, "key": key,
            "entry": "main0", "stage": profile["stage"], "reg": 17, "calls": 1,
            "texture_dimension": "2d", "integer_explicit_lod": True,
            "helper_file": "embedded", "helper_sha256": digest(payload.encode("utf-8")),
            "original_source_sha256": digest(original), "source_sha256": digest(data),
            "library_sha256": digest(library), "sampler": dict(SAMPLER),
        }
        pending[key + ".metal"] = data
        pending[key + ".json"] = (json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n").encode()
    # Validate the entire pair before writing anything. Rerunning is idempotent;
    # differing companions are refused rather than overwriting a checkpoint.
    for name, data in pending.items():
        target = output / name
        if target.exists() and target.read_bytes() != data:
            raise ValueError(f"Existing companion differs; use a fresh output directory: {target.name}")
    output.mkdir(parents=True, exist_ok=True)
    for name, data in pending.items():
        (output / name).write_bytes(data)
    return len(AUDITED)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cache", "libraries", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    header = ROOT / "dm/gfx/nvn/dmsh_chain.h"
    match = re.search(r'#define\s+DMSH_CHAIN\s+"([a-f0-9]+)"', header.read_text())
    if match is None:
        raise ValueError(f"No DMSH_CHAIN in {header}")
    chain = match.group(1)
    print(f"Exported {export(args.cache, args.libraries, args.output, chain)} audited MIN companions (chain {chain})")
