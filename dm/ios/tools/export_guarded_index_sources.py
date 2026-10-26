#!/usr/bin/env python3
"""Export proven dead-index seed companions from verified non-point sources.

The deliberately narrow proof recognizes a complete SPIRV-Cross coordinate-to-
constant-buffer index chain, independent of shader keys and temporary names.
When !guard is true both seeds are overwritten before use; otherwise every
arithmetic operation and memory read depending on them is disabled. All uses of
the seeds and intermediate indices must be inside that closed chain. Unknown
patterns produce no companion. This is not a general MSL optimizer.

Game-derived inputs/outputs stay outside source control. Original sources,
libraries, reflection and the translation chain are never changed. Companions
are consumed only by the separately verified legacy non-point runtime path.
"""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import re
import tempfile


PROOF = "dead-guarded-index-seeds-v1"
KIND = "guarded-index-v1"
IDENT = r"[A-Za-z_]\w*"
TOKEN = re.compile(r"[A-Za-z_]\w*|0[xX][0-9a-fA-F]+|\d+|<<|>>|!=|==|&&|\|\||[^\s]")

# Every branch uses the SAME boolean. The two loads are the only sinks of the
# index chain. Capture names, not numeric SPIR-V IDs or per-game constants.
CHAIN = r"""
int $mask = as_type<int>((*$args.$binding).$array[#flag_index].$component) & #bit;
bool $guard = $mask != 0;
COPY_GUARD
if (!$guard) {
    float $trunc_x = trunc($coord_x);
    int $int_x = int($trunc_x);
    $seed_x = $int_x;
}
int $x = $seed_x;
int $sum = $x;
if (!$guard) {
    float $trunc_y = trunc($coord_y);
    int $int_y = int($trunc_y);
    $seed_y = $int_y;
}
int $y = $seed_y;
if (!$guard) {
    int $twice_y = $y << #row_shift;
    int $joined = $twice_y + $x;
    $sum = $joined;
}
int $index = $sum;
int $scaled = $index;
if (!$guard) {
    int $shifted = $index << #scale_shift;
    $scaled = $shifted;
}
int $offset = $scaled;
if (!$guard) {
    int $address = $offset + #base;
    uint $word = uint(int(uint($address) >> uint(#word_shift)));
    uint $vector = uint(int(uint(int($word)) >> uint(#vector_shift)));
    int $channel = int($word) & #channel_mask;
    float $value_x = (*$args.$binding).$array[int($vector)][$channel];
    int $next_word = int($word) + #next;
    uint $next_vector = uint(int(uint($next_word) >> uint(#next_vector_shift)));
    int $next_channel = $next_word & #next_channel_mask;
    float $value_y = (*$args.$binding).$array[int($next_vector)][$next_channel];
    $output_x = $value_x;
    $output_y = $value_y;
}
"""
DEPENDENT = {
    "seed_x", "seed_y", "x", "sum", "y", "twice_y", "joined", "index",
    "scaled", "shifted", "offset", "address", "word", "vector", "channel",
    "next_word", "next_vector", "next_channel",
}
LOCALS = DEPENDENT | {
    "mask", "guard", "trunc_x", "int_x", "trunc_y", "int_y", "value_x", "value_y",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_output(path, data):
    # Replacing a directory entry preserves original inputs even if a previous
    # output file was accidentally hard-linked or symlinked to an input.
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as tmp:
        temporary = Path(tmp.name)
        try:
            tmp.write(data)
            tmp.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def masked(source):
    # Preserve offsets while removing comments. Macro expansion would invalidate
    # a textual use-def proof; only ordinary includes are accepted.
    clean = re.sub(r"//[^\n]*|/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m[0]), source, flags=re.S)
    directives = re.findall(r"^\s*#[^\n]*", clean, re.M)
    if any(not re.fullmatch(r"\s*#\s*include\s*<(?:metal_stdlib|simd/simd\.h)>\s*", d) for d in directives):
        raise ValueError("Only standard Metal includes are supported")
    return clean


def functions(source):
    clean = masked(source)
    for match in re.finditer(r"\bvertex\s+" + IDENT + r"\s+(" + IDENT + r")\s*\(", clean):
        opening, depth = match.end() - 1, 1
        at = opening + 1
        while at < len(clean) and depth:
            depth += (clean[at] == "(") - (clean[at] == ")")
            at += 1
        while at < len(clean) and clean[at].isspace():
            at += 1
        if depth or at == len(clean) or clean[at] != "{":
            raise ValueError("Unsupported vertex function signature")
        opening, depth = at, 1
        at += 1
        while at < len(clean) and depth:
            depth += (clean[at] == "{") - (clean[at] == "}")
            at += 1
        if depth:
            raise ValueError("Unbalanced vertex function")
        yield match[1], opening + 1, at - 1, clean[opening + 1:at - 1]


def pattern(copy_guard):
    template = CHAIN.replace("COPY_GUARD", "$guard_copy = $guard;" if copy_guard else "")
    seen, parts = set(), []
    for token in re.findall(r"\$\w+|#\w+|" + TOKEN.pattern, template):
        if token[0] in "$#":
            name = token[1:]
            if name in seen:
                parts.append("(?P=" + name + ")")
            else:
                seen.add(name)
                parts.append("(?P<" + name + ">" + (IDENT if token[0] == "$" else r"(?:0|[1-9]\d*)") + ")")
        else:
            parts.append(re.escape(token))
    return re.compile(r"(?<!\w)" + " ".join(parts))


PATTERNS = (pattern(False), pattern(True))
SEED = re.compile(r"\bint (" + IDENT + r") = (as_type < int > \( (" + IDENT + r") \)) ;")


def prove_body(body):
    """Return byte-position edits only for completely closed seed-use chains."""
    tokens = list(TOKEN.finditer(body))
    canonical = " ".join(t[0] for t in tokens)
    starts, at = [], 0
    scopes, stack = [], []
    for i, token in enumerate(tokens):
        starts.append(at)
        at += len(token[0]) + 1
        if token[0] == "}":
            if not stack:
                raise ValueError("Unbalanced body scope")
            stack.pop()
        scopes.append(tuple(stack))
        if token[0] == "{":
            stack.append(i)
    if stack or re.search(r"[\"']", body):
        raise ValueError("Unsupported body syntax")

    def token_range(start, end):
        return bisect.bisect_left(starts, start), bisect.bisect_right(starts, end - 1)

    seeds = {}
    for seed in SEED.finditer(canonical):
        seeds.setdefault(seed[1], []).append(seed)
    edits, proven_seeds = {}, set()
    for regex in PATTERNS:
        for match in regex.finditer(canonical):
            names = match.groupdict()
            # Reject aliasing/shadowed roles. A different spelling or extra use
            # needs its own proof, even if it happens to work for one draw.
            locals_ = {names[k] for k in LOCALS}
            if len(locals_) != len(LOCALS):
                continue
            if locals_ & {names[k] for k in ("args", "coord_x", "coord_y", "output_x", "output_y")}:
                continue
            if names.get("guard_copy") in locals_:
                continue
            seed_matches = [seeds.get(names[k], []) for k in ("seed_x", "seed_y")]
            if any(len(s) != 1 or s[0].end() > match.start() for s in seed_matches):
                continue
            seed_matches = [s[0] for s in seed_matches]
            region = token_range(*match.span())
            allowed = [region] + [token_range(*s.span()) for s in seed_matches]
            # Do not drop observable volatile reads/writes or match the suffix
            # of a qualified declaration. Seed operands must be plain local
            # float values, as emitted by this translation pattern.
            if any(a and tokens[a - 1][0] not in {";", "{", "}"} for a, _ in allowed):
                continue
            operands = [s[3] for s in seed_matches]
            plain_float = re.compile(r"(?:^|(?<=[;{}]))\s*float (" + IDENT + r") = [^;{}]+ ;")
            float_names = plain_float.findall(canonical)
            if any(float_names.count(name) != 1 for name in operands):
                continue
            # Both definitions must dominate the chain's lexical scope. All
            # reads/writes of every derived index must be inside these spans.
            if any(scopes[region[0]][:len(scopes[a])] != scopes[a] for a, _ in allowed[1:]):
                continue
            dependent = {names[k] for k in DEPENDENT}
            if any(s[3] in dependent for s in seed_matches):
                continue
            if any(t[0] in dependent and not any(a <= i < b for a, b in allowed)
                   for i, t in enumerate(tokens)):
                continue
            # Local variables participating in the proof may not be redeclared
            # in a nested scope: identifier-only closure is then ambiguous.
            declarations = re.findall(r"\b(?:int|uint|float|bool) (" + IDENT + r")\b", canonical)
            if any(declarations.count(name) != 1 for name in locals_):
                continue
            for seed in seed_matches:
                first, last = token_range(*seed.span(2))
                edits[(tokens[first].start(), tokens[last - 1].end())] = "0"
                proven_seeds.add(seed[1])
    if not edits:
        raise ValueError("No closed dead guarded-index seed chain")
    if proven_seeds != set(seeds):
        raise ValueError("A scalar integer bitcast seed lacks a closed proof")
    return [(start, end, value) for (start, end), value in sorted(edits.items())]


def transform(source, meta, entries=None):
    """Default to entry_z; preserve point/original and capture/TF functions.

    Explicit entry selection is available for offline proof tests. Each chosen
    function needs its own complete proof; proofs never cross function scopes.
    """
    reflected = {meta.get("entry"), meta.get("entry_z")} - {None}
    entries = set(entries) if entries is not None else {meta.get("entry_z") or meta.get("entry")}
    if meta.get("stage") != "vs" or not entries or not entries <= reflected:
        raise ValueError("Expected reflected vertex entries")
    if not re.search(r"\btrunc\b", source):
        raise ValueError("No guarded coordinate conversion")
    found, edits = set(), []
    for name, start, end, body in functions(source):
        if name not in entries:
            continue
        if name in found:
            raise ValueError("Duplicate vertex entry")
        found.add(name)
        # A partial proof in one entry is not borrowed by another entry.
        edits.extend((start + a, start + b, value) for a, b, value in prove_body(body))
    if found != entries:
        raise ValueError("Missing reflected vertex entry")
    for start, end, value in reversed(sorted(edits)):
        source = source[:start] + value + source[end:]
    return source, len(edits)


def export(libraries: Path, output: Path, chain: str) -> int:
    if libraries.name != "b-" + chain:
        raise ValueError("Library directory must match the host chain")
    base = libraries / "nonpoint-v1"
    if output.resolve() in {libraries.resolve(), base.resolve()} or base.resolve() in output.resolve().parents:
        raise ValueError("Output must not overwrite the original non-point sources")
    manifest = json.loads((base / "manifest.json").read_bytes())
    if manifest.get("schema") != 1 or manifest.get("chain") != chain or not isinstance(manifest.get("shaders"), dict):
        raise ValueError("Invalid non-point source manifest")
    records, outputs = {}, {}
    # Validate every base identity before writing anything, including records
    # whose source pattern will ultimately be unsupported.
    for key, record in sorted(manifest["shaders"].items()):
        if not re.fullmatch(r"[A-Za-z0-9_]+", key):
            raise ValueError("Invalid shader key")
        original = (base / (key + ".metal")).read_bytes()
        library = (libraries / (key + ".metallib")).read_bytes()
        reflection = (libraries / (key + ".json")).read_bytes()
        meta = json.loads(reflection)
        if (record.get("source_sha256") != digest(original) or record.get("library_sha256") != digest(library)
                or meta.get("chain") != chain or meta.get("key") != key or meta.get("stage") != "vs"):
            raise ValueError("Non-point source, library or reflection identity mismatch: " + key)
        try:
            transformed, count = transform(original.decode("utf-8"), meta)
        except ValueError:
            continue
        data = transformed.encode("utf-8")
        name = key + ".metal"
        outputs[name] = data
        records[key] = {"source": name, "source_sha256": digest(data), "original_source_sha256": digest(original),
                        "base_library_sha256": digest(library), "reflection_sha256": digest(reflection),
                        "proof": PROOF, "replacement_count": count}
    output.mkdir(parents=True, exist_ok=True)
    for name, data in outputs.items():
        write_output(output / name, data)
    write_output(output / "manifest.json", (json.dumps({"schema": 1, "chain": chain, "kind": KIND,
                                                        "shaders": records}, separators=(",", ":")) + "\n").encode())
    return len(records)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("libraries", type=Path, help="Chain-matched b-CHAIN containing verified nonpoint-v1")
    parser.add_argument("output", type=Path, help="Independent guarded-index-v1 companion directory")
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[2] / "gfx/nvn/dmsh_chain.h"
    chain = re.search(r'#define\s+DMSH_CHAIN\s+"([a-f0-9]+)"', header.read_text()).group(1)
    print(f"Exported {export(args.libraries, args.output, chain)} proven guarded-index companions (chain {chain})")
