#!/usr/bin/env python3
"""Fail closed when a profile's packaged NVN companions are incomplete."""
import argparse
import hashlib
import json
from pathlib import Path
import re


KEY = re.compile(r"[0-9a-f]{16}_b")
HASH = re.compile(r"[0-9a-f]{64}")


class CompanionError(ValueError):
    pass


def read_json(path, what):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise CompanionError(f"missing {what}: {path}") from None
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CompanionError(f"invalid {what} {path}: {error}") from None
    if not isinstance(value, dict):
        raise CompanionError(f"{what} must be a JSON object: {path}")
    return value


def component(value, what):
    if not isinstance(value, str) or not value or value != Path(value).name or value in (".", ".."):
        raise CompanionError(f"invalid {what}: {value!r}")
    return value


def shader_key(value, what):
    if not isinstance(value, str) or not KEY.fullmatch(value):
        raise CompanionError(f"invalid {what}: {value!r}")
    return value


def sha256(path, cache):
    path = path.resolve()
    if path in cache:
        return cache[path]
    try:
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
    except OSError as error:
        raise CompanionError(f"missing or unreadable companion file {path}: {error}") from None
    cache[path] = digest
    return digest


def verify_hash(record, field, path, cache, what):
    expected = record.get(field)
    if not isinstance(expected, str) or not HASH.fullmatch(expected):
        raise CompanionError(f"{what} has no valid {field}")
    actual = sha256(path, cache)
    if actual != expected:
        raise CompanionError(f"{what} {field} mismatch: {path}")


def fields(value, allowed, what):
    if not isinstance(value, dict):
        raise CompanionError(f"{what} must be an object")
    extra = sorted(set(value) - set(allowed))
    if extra:
        raise CompanionError(f"unknown {what} field(s): {', '.join(extra)}")


def required_keys(spec, what):
    values = spec.get("required_records", [])
    if not isinstance(values, list) or not values:
        raise CompanionError(f"{what} must name at least one required record")
    keys = [shader_key(value, f"{what} required record") for value in values]
    if len(keys) != len(set(keys)):
        raise CompanionError(f"{what} repeats a required record")
    return keys


def validate_manifest(root, chain, spec, cache):
    fields(spec, {"directory", "kind", "limit", "required_records", "original_directory"}, "manifest requirement")
    directory = component(spec.get("directory"), "companion directory")
    where = root / directory
    manifest = read_json(where / "manifest.json", f"{directory} manifest")
    if manifest.get("schema") != 1 or manifest.get("chain") != chain:
        raise CompanionError(f"{directory} manifest schema/chain does not match b-{chain}")
    if "kind" in spec and manifest.get("kind") != spec["kind"]:
        raise CompanionError(f"{directory} manifest kind is not {spec['kind']!r}")
    if "limit" in spec and manifest.get("limit") != spec["limit"]:
        raise CompanionError(f"{directory} manifest limit is not {spec['limit']!r}")
    records = manifest.get("shaders")
    if not isinstance(records, dict) or not records:
        raise CompanionError(f"{directory} manifest has no shader records")

    original_directory = spec.get("original_directory")
    if original_directory is not None:
        original_directory = component(original_directory, f"{directory} original directory")
    active = set()
    for key, record in records.items():
        shader_key(key, f"{directory} shader key")
        if not isinstance(record, dict):
            raise CompanionError(f"{directory} record {key} is not an object")
        supported = record.get("supported", True)
        if not isinstance(supported, bool):
            raise CompanionError(f"{directory} record {key} has a non-boolean supported value")
        if not supported:
            continue
        source = record.get("source", key + ".metal")
        source = component(source, f"{directory} source for {key}")
        if source != key + ".metal":
            raise CompanionError(f"{directory} source name does not match record {key}: {source}")
        verify_hash(record, "source_sha256", where / source, cache, f"{directory} record {key}")
        library_hash = "base_library_sha256" if "base_library_sha256" in record else "library_sha256"
        verify_hash(record, library_hash, root / (key + ".metallib"), cache, f"{directory} record {key}")
        if "reflection_sha256" in record:
            verify_hash(record, "reflection_sha256", root / (key + ".json"), cache, f"{directory} record {key}")
        if original_directory is not None:
            verify_hash(record, "original_source_sha256", root / original_directory / (key + ".metal"), cache,
                        f"{directory} record {key}")
        active.add(key)
    if not active:
        raise CompanionError(f"{directory} manifest has no usable shader records")
    for key in required_keys(spec, directory):
        if key not in records:
            raise CompanionError(f"{directory} is missing required record {key}")
        if key not in active:
            raise CompanionError(f"{directory} required record {key} is unsupported")
    return len(active)


def validate_record_directory(root, chain, spec, cache):
    fields(spec, {"directory", "kind", "required_records"}, "record requirement")
    directory = component(spec.get("directory"), "record companion directory")
    kind = spec.get("kind")
    if not isinstance(kind, str) or not kind:
        raise CompanionError(f"{directory} record requirement has no kind")
    where = root / directory
    count = 0
    for key in required_keys(spec, directory):
        record = read_json(where / (key + ".json"), f"{directory} record {key}")
        if record.get("schema") != 1 or record.get("chain") != chain or record.get("kind") != kind:
            raise CompanionError(f"{directory} record {key} schema/chain/kind mismatch")
        verify_hash(record, "source_sha256", where / (key + ".metal"), cache, f"{directory} record {key}")
        verify_hash(record, "library_sha256", root / (key + ".metallib"), cache, f"{directory} record {key}")
        count += 1
    return count


def validate(profile, libraries, chain):
    profile, libraries = Path(profile), Path(libraries)
    requirements = read_json(profile, "iOS companion requirements")
    fields(requirements, {"schema", "manifests", "records"}, "requirements")
    if requirements.get("schema") != 1:
        raise CompanionError(f"unsupported requirements schema in {profile}")
    if libraries.name != "b-" + chain or not libraries.is_dir():
        raise CompanionError(f"shader library directory must be an existing b-{chain}: {libraries}")
    manifests, records = requirements.get("manifests"), requirements.get("records", [])
    if not isinstance(manifests, list) or not manifests:
        raise CompanionError("requirements must contain at least one manifest companion")
    if not isinstance(records, list):
        raise CompanionError("record companions must be a list")
    cache = {}
    shaders = sum(validate_manifest(libraries, chain, item, cache) for item in manifests)
    standalone = sum(validate_record_directory(libraries, chain, item, cache) for item in records)
    return len(manifests), shaders, standalone


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path, help="profile ios_companions.json")
    parser.add_argument("libraries", type=Path, help="packaged b-CHAIN directory")
    parser.add_argument("chain", help="host DMSH_CHAIN")
    args = parser.parse_args()
    try:
        manifests, shaders, standalone = validate(args.profile, args.libraries, args.chain)
    except CompanionError as error:
        parser.exit(1, f"[RED] shader companions: {error}\n")
    print(f"shader companions: {manifests} manifests, {shaders} usable shader records, "
          f"{standalone} standalone records (chain {args.chain})")


if __name__ == "__main__":
    main()
