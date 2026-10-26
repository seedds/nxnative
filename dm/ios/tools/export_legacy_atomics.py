#!/usr/bin/env python3
"""Export bounds-checked Metal 3 image-atomic companions from original MSL.

Base chain libraries remain untouched. Output contains game-derived source and
must live with the user's game inputs, outside source control.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


HELPERS = r"""
// Legacy linear companions must preserve image bounds and array layers.
template<typename T, access A>
inline bool dm_image_atomic_valid(int2 c, texture2d<T, A> t) {
    return all(c >= int2(0)) && uint(c.x) < t.get_width() && uint(c.y) < t.get_height();
}
template<typename T, access A>
inline bool dm_image_atomic_valid(int3 c, texture2d_array<T, A> t) {
    return all(c >= int3(0)) && uint(c.x) < t.get_width() && uint(c.y) < t.get_height() && uint(c.z) < t.get_array_size();
}
template<typename T, access A>
inline ulong dm_image_atomic_index(int2 c, texture2d<T, A> t) {
    ulong a = spvLinearTextureAlignment / 4;
    ulong pitch = (ulong(t.get_width()) + a - 1) & ~(a - 1);
    return ulong(uint(c.y)) * pitch + uint(c.x);
}
template<typename T, access A>
inline ulong dm_image_atomic_index(int3 c, texture2d_array<T, A> t) {
    ulong a = spvLinearTextureAlignment / 4;
    ulong pitch = (ulong(t.get_width()) + a - 1) & ~(a - 1);
    return (ulong(uint(c.z)) * t.get_height() + uint(c.y)) * pitch + uint(c.x);
}
#define spvImage2DAtomicCoord(tc, tex) dm_image_atomic_index(tc, tex)
"""


def close_paren(source, start):
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "(":
            depth += 1
        elif source[i] == ")":
            depth -= 1
            if not depth:
                return i
    raise ValueError("Unbalanced generated atomic call")


def split_args(source):
    depth, start, args = 0, 0, []
    for i, c in enumerate(source):
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "," and not depth:
            args.append(source[start:i].strip())
            start = i + 1
    args.append(source[start:].strip())
    return args


def transform(source):
    macro = re.compile(r"^#define spvImage2DAtomicCoord[^\n]*$", re.M)
    if len(macro.findall(source)) != 1:
        raise ValueError("Expected one original image-atomic coordinate macro")
    edits = []
    for call in re.finditer(r"\batomic_(?:fetch_\w+|exchange)_explicit\s*\(", source):
        end = close_paren(source, call.end() - 1)
        body = source[call.start():end + 1]
        coord = re.search(r"\bspvImage2DAtomicCoord\s*\(", body)
        if not coord:
            continue
        coord_end = close_paren(body, coord.end() - 1)
        args = split_args(body[coord.end():coord_end])
        if len(args) != 2:
            raise ValueError("Unexpected image-atomic coordinate arguments")
        guard = f"dm_image_atomic_valid({args[0]}, {args[1]})"
        edits.append((call.start(), end + 1, f"({guard} ? ({body}) : 0)"))
    expected = len(re.findall(r"\bspvImage2DAtomicCoord\s*\(", source)) - 1
    if not edits or len(edits) != expected:
        raise ValueError("An image-atomic coordinate use could not be guarded")
    for start, end, replacement in reversed(edits):
        source = source[:start] + replacement + source[end:]
    return macro.sub(lambda _: HELPERS.strip(), source), len(edits)


def export(cache, libraries, output, chain):
    if cache.name != f"b-{chain}" or libraries.name != f"b-{chain}":
        raise ValueError("Source and library directories must match the host chain")
    output.mkdir(parents=True, exist_ok=True)
    count = 0
    for reflection in sorted(libraries.glob("*_b.json")):
        meta = json.loads(reflection.read_text())
        if not any("_atomic" in b.get("msl_name", "") for b in meta.get("bindings", [])):
            continue
        if meta.get("chain") != chain or meta.get("key") != reflection.stem:
            raise ValueError(f"Mismatched reflection: {reflection.name}")
        original = (cache / (reflection.stem + ".metal")).read_bytes()
        if b"spvImage2DAtomicCoord" not in original:
            continue
        variant, calls = transform(original.decode())
        data = variant.encode()
        (output / (reflection.stem + ".metal")).write_bytes(data)
        record = {"schema": 1, "chain": chain, "kind": "bounded-image-atomics-v1", "calls": calls,
                  "original_source_sha256": hashlib.sha256(original).hexdigest(),
                  "source_sha256": hashlib.sha256(data).hexdigest(),
                  "library_sha256": hashlib.sha256((libraries / (reflection.stem + ".metallib")).read_bytes()).hexdigest()}
        (output / reflection.name).write_text(json.dumps(record) + "\n")
        count += 1
    return count


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cache", "libraries", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[2] / "gfx/nvn/dmsh_chain.h"
    chain = re.search(r'#define\s+DMSH_CHAIN\s+"([a-f0-9]+)"', header.read_text()).group(1)
    print(f"Exported {export(args.cache, args.libraries, args.output, chain)} bounded atomic variants (chain {chain})")
