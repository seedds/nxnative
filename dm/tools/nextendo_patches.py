"""From RobiiAragon/NX-Port-Studio dm/tools/nextendo_patches.py @ bd13a49b (MIT).

Signed IPS32 adapter for private, mode-specific native builds.

Provider patches are downloaded as data, never imported as executable scripts.
Exact 32-byte NSO build identity, complete guards and bounds are mandatory.
The original input and the offline/LAN rewrite are never modified.
"""
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import urllib.request

ENDPOINT = 'https://nextendo.network/service-catalog/v1/npln-patches'
MAX_DOCUMENT = 2 * 2**20
MAX_IMAGE = 512 * 2**20


def strict_json(data):
    def unique(pairs):
        output = {}
        for key, value in pairs:
            if key in output: raise ValueError('Duplicate signed document field')
            output[key] = value
        return output
    return json.loads(data, object_pairs_hook=unique, parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Invalid JSON number')))


def verified_catalog(repository, cache, games=False):
    cache = Path(cache)
    if cache.is_symlink(): raise ValueError('Unsafe patch cache')
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    source = Path(repository) / 'dm/tools/nextendo_verify.swift'
    compiler = subprocess.check_output(['xcrun', 'swiftc', '--version'])
    key = hashlib.sha256(source.read_bytes() + compiler).hexdigest()[:24]
    verifier = cache / ('verify-' + key)
    if not verifier.is_file() or verifier.is_symlink():
        with tempfile.TemporaryDirectory(dir=cache) as work:
            binary = Path(work) / 'verifier'
            subprocess.run(['xcrun', 'swiftc', '-parse-as-library', str(source), '-o', str(binary)], check=True, capture_output=True)
            binary.replace(verifier)
    envelope = cache / ('game-catalog.json' if games else 'npln-patches.json')
    minimum = cache / ('game-highest-revision.json' if games else 'highest-revision.json')
    arguments = ['games'] if games else []
    validate = validate_games if games else validate_catalog
    with tempfile.TemporaryDirectory(dir=cache) as work:
        work = Path(work); payload = work / 'payload.json'
        # Verify the cached signature and expiry again, even when offline.
        if envelope.is_file() and not envelope.is_symlink() and envelope.stat().st_size <= MAX_DOCUMENT:
            result = subprocess.run([str(verifier), str(envelope), str(payload), *arguments], capture_output=True)
            if result.returncode == 0:
                value = validate(strict_json(payload.read_bytes()))
                if minimum.is_file() and value['revision'] < strict_json(minimum.read_bytes())['revision']:
                    raise ValueError('Signed patch catalog rollback')
                return value
        endpoint = 'https://nextendo.network/service-catalog/v1/game-catalog' if games else ENDPOINT
        request = urllib.request.Request(endpoint, headers={'User-Agent': 'nxnative'})
        with urllib.request.urlopen(request, timeout=20) as response: data = response.read(MAX_DOCUMENT + 1)
        if len(data) > MAX_DOCUMENT: raise ValueError('Signed patch document is too large')
        strict_json(data)
        incoming = work / 'incoming.json'; incoming.write_bytes(data)
        subprocess.run([str(verifier), str(incoming), str(payload), *arguments], check=True, capture_output=True)
        value = strict_json(payload.read_bytes())
        if minimum.is_file() and value['revision'] < strict_json(minimum.read_bytes())['revision']:
            raise ValueError('Signed patch catalog rollback')
        value = validate(value)
        incoming.chmod(0o600); incoming.replace(envelope)
        revision = work / 'revision.json'; revision.write_text(json.dumps({'revision': value['revision']})); revision.chmod(0o600); revision.replace(minimum)
        return value


def validate_games(document):
    import re
    rows = document.get('games')
    if type(document.get('revision')) is not int or not isinstance(rows,list) or len(rows)>10000:
        raise ValueError('Invalid signed game catalog')
    seen=set()
    for row in rows:
        ids=row.get('title_ids')
        if not isinstance(ids,list) or not 1<=len(ids)<=32 or type(row.get('enabled')) is not bool:
            raise ValueError('Invalid signed game policy')
        protocol=row.get('protocol')
        if protocol is not None and (not isinstance(protocol,str) or not re.fullmatch('[a-z0-9._-]{1,64}',protocol)):
            raise ValueError('Invalid signed service family')
        for title in ids:
            if not isinstance(title,str) or not re.fullmatch('[a-fA-F0-9]{16}',title) or not int(title,16) or title.lower() in seen:
                raise ValueError('Invalid signed regional identity')
            seen.add(title.lower())
    return document


def validate_catalog(document):
    patches = document.get('patches')
    if type(document.get('revision')) is not int or not isinstance(patches, list) or len(patches) > 4096:
        raise ValueError('Unsupported signed patch catalog')
    identities = set()
    for patch in patches:
        title, build = patch.get('title_id', ''), patch.get('build_id', '')
        if len(title) != 16 or len(build) != 64:
            raise ValueError('Invalid patch identity')
        try: bytes.fromhex(title); bytes.fromhex(build)
        except ValueError: raise ValueError('Invalid patch identity') from None
        identity = (title.lower(), build.lower(), patch.get('id'))
        if identity in identities: raise ValueError('Duplicate signed patch identity')
        identities.add(identity)
        if patch.get('module') != 'main' or patch.get('patch_version') != 1 or patch.get('offset_basis') != 'nso_header_included' or patch.get('required_capabilities') != ['npln.ips32.v1'] or type(patch.get('enabled')) is not bool:
            raise ValueError('Unsupported signed patch policy')
        records(patch)
    return document


def records(patch):
    encoded = patch.get('ips32_base64', '')
    data = base64.b64decode(encoded, validate=True)
    if len(data) > MAX_DOCUMENT or hashlib.sha256(data).hexdigest() != patch.get('sha256') or not data.startswith(b'IPS32'):
        raise ValueError('Invalid signed IPS32 integrity')
    offset, output, covered = 5, [], set()
    while offset + 4 <= len(data) and data[offset:offset + 4] != b'EEOF':
        if offset + 6 > len(data): raise ValueError('Truncated IPS32 record')
        start, length = struct.unpack_from('>IH', data, offset); offset += 6
        if not length:
            if offset + 3 > len(data): raise ValueError('Truncated IPS32 run')
            length = struct.unpack_from('>H', data, offset)[0]; value = data[offset + 2:offset + 3] * length; offset += 3
        else:
            if offset + length > len(data): raise ValueError('Truncated IPS32 bytes')
            value = data[offset:offset + length]; offset += length
        if not length or start < 0x100 or start + length > MAX_IMAGE + 0x100 or len(output) >= 4096:
            raise ValueError('IPS32 bounds exceeded')
        positions = set(range(start, start + length))
        if covered & positions: raise ValueError('Overlapping IPS32 records')
        covered |= positions; output.append((start - 0x100, value))
    if data[offset:] != b'EEOF' or not output: raise ValueError('Invalid IPS32 termination')
    guards = patch.get('guards')
    if not isinstance(guards, list) or not 0 < len(guards) <= 4096: raise ValueError('Missing IPS32 preimage guards')
    originals, replacements = {}, {}
    for guard in guards:
        start = guard.get('offset'); original = bytes.fromhex(guard.get('original_hex', '')); changed = bytes.fromhex(guard.get('patched_hex', ''))
        if type(start) is not int or start < 0x100 or not original or len(original) != len(changed) or len(original) > 65536:
            raise ValueError('Invalid IPS32 guard')
        for i, (a, b) in enumerate(zip(original, changed)):
            if start + i in originals: raise ValueError('Overlapping IPS32 guards')
            originals[start + i] = a; replacements[start + i] = b
    expected = {start + 0x100 + i: byte for start, value in output for i, byte in enumerate(value)}
    if replacements != expected: raise ValueError('IPS32 bytes are not completely guarded')
    return output, originals


def apply_image(image, title_id, build_id, document):
    validate_catalog(document)
    matches = [p for p in document['patches'] if p['enabled'] and p['title_id'].lower() == title_id.lower() and p['build_id'].lower() == build_id.lower()]
    if not matches: return None, []
    if len(image) > MAX_IMAGE: raise ValueError('NSO image exceeds patch bounds')
    changed = bytearray(image); all_positions = set()
    for patch in matches:
        edits, guards = records(patch)
        for position, original in guards.items():
            position -= 0x100
            if position >= len(image) or image[position] != original: raise ValueError('Nextendo patch preimage differs from the exact game build')
            if position in all_positions: raise ValueError('Conflicting signed game patches')
            all_positions.add(position)
        for start, value in edits:
            if start + len(value) > len(image): raise ValueError('Signed patch exceeds this NSO image')
            changed[start:start + len(value)] = value
    return changed, [p['id'] for p in matches]


def prepare_nso(source, destination, title_id, document):
    data = Path(source).read_bytes()
    if len(data) < 0x100 or len(data) > MAX_IMAGE or data[:4] != b'NSO0': raise ValueError('Expected a bounded NSO module')
    flags = struct.unpack_from('<I', data, 0xc)[0]
    segments = [struct.unpack_from('<III', data, 0x10 + i * 0x10) for i in range(3)]
    sizes = struct.unpack_from('<III', data, 0x60)
    length = max(mo + size for _, mo, size in segments)
    if length > MAX_IMAGE: raise ValueError('NSO virtual layout exceeds patch bounds')
    image = bytearray(length)
    for i, (fo, mo, size) in enumerate(segments):
        stored = sizes[i] if flags & (1 << i) else size
        if fo < 0x100 or fo + stored > len(data): raise ValueError('NSO segment exceeds the file')
        raw = data[fo:fo + stored]
        if flags & (1 << i):
            import lz4.block
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        if len(raw) != size: raise ValueError('Invalid decompressed NSO segment')
        image[mo:mo + size] = raw
    changed, applied = apply_image(image, title_id, data[0x40:0x60].hex(), document)
    if changed is None: return []
    for patch in document['patches']:
        if patch.get('id') in applied:
            for offset in records(patch)[1]:
                position = offset - 0x100
                if not any(mo <= position < mo + size for _, mo, size in segments):
                    raise ValueError('A signed patch targets an unmapped NSO gap')
    header = bytearray(data[:0x100]); struct.pack_into('<I', header, 0xc, flags & ~7)
    body = bytearray()
    for i, (_, mo, size) in enumerate(segments):
        raw = changed[mo:mo + size]
        struct.pack_into('<I', header, 0x10 + i * 0x10, len(body) + 0x100)
        struct.pack_into('<I', header, 0x60 + i * 4, size)
        header[0xa0 + i * 32:0xc0 + i * 32] = hashlib.sha256(raw).digest(); body.extend(raw)
    destination = Path(destination); destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(header + body)
    return applied
