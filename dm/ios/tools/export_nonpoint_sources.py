#!/usr/bin/env python3
"""Export unmodified, chain-matched vertex sources for legacy Metal variants.

Inputs and outputs contain game-derived data and must stay outside source control.
The runtime checks both source and original library hashes before compiling a
non-point variant. This export does not change the base translation chain.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


def export(cache: Path, libraries: Path, output: Path, chain: str) -> int:
    if cache.name != f"b-{chain}" or libraries.name != f"b-{chain}":
        raise ValueError("Source and library directories must match the host chain")
    records = {}
    output.mkdir(parents=True, exist_ok=True)
    for reflection in sorted(libraries.glob("*_b.json")):
        meta = json.loads(reflection.read_text())
        if meta.get("stage") != "vs":
            continue
        if meta.get("chain") != chain or meta.get("key") != reflection.stem:
            raise ValueError(f"Mismatched reflection: {reflection.name}")
        source = cache / (reflection.stem + ".metal")
        data = source.read_bytes()
        if b"[[point_size]]" not in data:
            continue
        library = libraries / (reflection.stem + ".metallib")
        records[reflection.stem] = {
            "source_sha256": hashlib.sha256(data).hexdigest(),
            "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        }
        shutil.copyfile(source, output / source.name)
    manifest = {"schema": 1, "chain": chain, "shaders": records}
    (output / "manifest.json").write_text(json.dumps(manifest, separators=(",", ":")) + "\n")
    return len(records)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("libraries", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[2] / "gfx/nvn/dmsh_chain.h"
    match = re.search(r'#define\s+DMSH_CHAIN\s+"([a-f0-9]+)"', header.read_text())
    if match is None:
        raise ValueError(f"No DMSH_CHAIN in {header}")
    chain = match.group(1)
    count = export(args.cache, args.libraries, args.output, chain)
    print(f"Exported {count} chain-matched non-point sources (chain {chain})")
