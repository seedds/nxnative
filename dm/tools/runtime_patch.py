"""Generate strict DMRT1 manifests from a verified original module and its rewritten image.

No community patches are read. Offsets are original virtual offsets; a rewritten site is rejected.
Mac only: iOS's executable pages are signed. The host validates all rows again before any write.
"""
from pathlib import Path
import re


def layout(path):
    return dict(re.findall(r'^# (\w+)\t([^\n]+)$', Path(path).read_text(), re.M))


def manifest(module, patched, fixups, build, variable, writes, variant=None):
    """writes = [(choice, offset, original bytes, replacement bytes), ...]; default has no writes.
    variant: a second DM_ variable; loader.c takes <value>_<its value> when declared (runtime_patch.h)."""
    if module.build_id != bytes.fromhex(build):
        raise ValueError('unsupported NSO build ID')
    if fixups.get('BUILD_ID') != build[:32] or int(fixups['TEXT_END'], 0) != module.text_end:
        raise ValueError('rewritten module build/layout mismatch')
    if not re.fullmatch(r'DM_[A-Z0-9_]+', variable) or (variant is not None and not re.fullmatch(r'DM_[A-Z0-9_]+', variant)):
        raise ValueError('invalid runtime variable')
    choices = list(dict.fromkeys(w[0] for w in writes))
    if not choices or len(choices) >= 16 or 'default' in choices:
        raise ValueError('invalid runtime choices')
    rows = [f'DMRT1\t{build[:32]}\t{module.text_end:x}\t{variable}' + (f'\t{variant}' if variant else ''), 'choice\tdefault']
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


def choices(folder, variable):
    """Read choices only from a complete manifest matching the rewritten image.

    Used for launcher availability, not as gameplay validation. The host performs
    its own full validation again before writing any executable page.
    """
    folder = Path(folder)
    paths = [p for p in [folder / 'main.runtime.tsv', *sorted(folder.glob('main.*.runtime.tsv'))] if p.is_file()]   # one DM_ variable each (loader.c)
    if not paths: return ()
    fix = layout(folder / 'main.fixups.tsv'); image = (folder / 'main.patched.bin').read_bytes()
    for path in paths:
        found = _choices(path, fix, image, variable)
        if found: return found
    return ()


def _choices(path, fix, image, variable):
    lines = [l.split('\t') for l in path.read_text().splitlines() if l and not l.startswith('#')]
    try:
        header = lines.pop(0)
        if len(header) not in (4, 5) or header[:2] != ['DMRT1', fix['BUILD_ID']] or int(header[2], 16) != int(fix['TEXT_END'], 0):
            raise ValueError('runtime header/build/layout mismatch')
        end = int(header[2], 16)
        if end > len(image) or not all(re.fullmatch(r'DM_[A-Z0-9_]+', h) for h in header[3:]): raise ValueError('runtime image/header mismatch')
        names, spans, written = [], [], set()
        for row in lines:
            if row[0] == 'choice' and len(row) == 2 and not spans and len(names) < 16:
                if not re.fullmatch(r'[A-Za-z0-9_]{1,31}', row[1]) or row[1] in names: raise ValueError('runtime choice invalid')
                names.append(row[1]); continue
            if len(row) != 5 or row[0] != 'write' or row[1] not in names or row[1] == 'default' or len(spans) >= 1024:
                raise ValueError('runtime write invalid')
            off = int(row[2], 16); before = bytes.fromhex(row[3]); after = bytes.fromhex(row[4])
            if off < 0 or off % 4 or not before or len(before) % 4 or len(before) > 64 or len(before) != len(after) or off + len(before) > end:
                raise ValueError('runtime offset/length invalid')
            if image[off:off + len(before)] != before: raise ValueError('runtime instructions differ')
            if any(c == row[1] and a < off + len(before) and off < b for c, a, b in spans): raise ValueError('runtime writes overlap')
            spans.append((row[1], off, off + len(before))); written.add(row[1])
        if 'default' not in names or not spans or set(names) - {'default'} != written: raise ValueError('runtime manifest incomplete')
        if len(header) == 5 and header[4] == variable:   # the variant variable's values: the suffixes of <value>_<variant> choices
            bases = set(names)   # a variant value is digits (1610): 1440p_full is a choice of its own, not 1440p with a variant
            return ('default', *dict.fromkeys(n.rsplit('_', 1)[1] for n in names if '_' in n and n.rsplit('_', 1)[0] in bases and n.rsplit('_', 1)[1].isdigit()))
        return tuple(names) if header[3] == variable else ()
    except (KeyError, IndexError) as e:
        raise ValueError('runtime manifest/fixups incomplete') from e
