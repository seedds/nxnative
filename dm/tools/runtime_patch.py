"""Generate strict DMRT1 manifests from a verified original module and its rewritten image.

No community patches are read. Offsets are original virtual offsets; a rewritten site is rejected.
Mac only: iOS's executable pages are signed. The host validates all rows again before any write.
"""
from pathlib import Path
import re


def layout(path):
    return dict(re.findall(r'^# (\w+)\t([^\n]+)$', Path(path).read_text(), re.M))


def manifest(module, patched, fixups, build, variable, writes):
    """writes = [(choice, offset, original bytes, replacement bytes), ...]; default has no writes."""
    if module.build_id != bytes.fromhex(build):
        raise ValueError('unsupported NSO build ID')
    if fixups.get('BUILD_ID') != build[:32] or int(fixups['TEXT_END'], 0) != module.text_end:
        raise ValueError('rewritten module build/layout mismatch')
    if not re.fullmatch(r'DM_[A-Z0-9_]+', variable):
        raise ValueError('invalid runtime variable')
    choices = list(dict.fromkeys(w[0] for w in writes))
    if not choices or len(choices) >= 16 or 'default' in choices:
        raise ValueError('invalid runtime choices')
    rows = [f'DMRT1\t{build[:32]}\t{module.text_end:x}\t{variable}', 'choice\tdefault']
    for value in choices:
        if not re.fullmatch(r'[A-Za-z0-9_]{1,31}', value):
            raise ValueError('invalid runtime choice')
        rows.append(f'choice\t{value}')
    seen = []
    for value, off, before, after in writes:
        if off < 0 or off % 4 or not before or len(before) % 4 or len(before) != len(after) or len(before) > 64 or off + len(before) > module.text_end:
            raise ValueError(f'invalid runtime patch offset/length: {off:#x}')
        if bytes(module.image[off:off + len(before)]) != before:
            raise ValueError(f'original instruction mismatch at {off:#x}')
        if patched[off:off + len(before)] != before:
            raise ValueError(f'runtime site was rewritten at {off:#x}')
        if any(v == value and a < off + len(before) and off < b for v, a, b in seen):
            raise ValueError('overlapping runtime writes')
        seen.append((value, off, off + len(before)))
        rows.append(f'write\t{value}\t{off:x}\t{before.hex()}\t{after.hex()}')
    return '\n'.join(rows) + '\n'
