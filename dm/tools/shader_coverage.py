"""Read-only completeness gate for a full Mac NVN shader library; no compilation."""
import collections
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gfx/tools'))
import dmsh_abi as abi
import dmsh_chain
import shader_regressions
import shader_lib

SLOTS = {getattr(abi, 'KIND_' + name) for name in ('CBUF', 'SAMP', 'SAMP_TEX', 'TEX', 'IMG', 'TBUF', 'SBUF',
    'UBO_PTR', 'SBUF_UBO', 'TEX_UBO', 'SAMP_UBO', 'SAMP_ID', 'IMG_UBO', 'TBUF_UBO')}


def reflection_issues(key, refl, chain):
    issues = []
    if refl.get('dm_ctl') != key or refl.get('chain') != chain:
        issues.append('reflection identity/chain mismatch')
    deleted, lost = shader_regressions.losses(refl)
    if deleted or lost:
        issues.append('deleted bindless access or lost entry point')
    if refl.get('drv_unfillable'):
        issues.append('unfillable driver-bank reads')
    if any(b.get('kind') not in abi.KINDS or b.get('kind') == abi.KIND_UNKNOWN or (b.get('kind') in SLOTS and b.get('reg') is None) for b in refl.get('bindings', [])):
        issues.append('unresolved resource binding')
    return [key + ': ' + issue for issue in issues]


def audit(g, library):
    expected = {}
    for line in g.table('shader_map.tsv').read_text().splitlines():
        c = line.split('\t')
        if len(c) > 4 and c[0].endswith('.bin'):
            expected[c[3]] = c[2]
    v = shader_lib.view(library)
    issues = []
    stamp = shader_lib.read_stamp(library)
    if not expected:
        issues.append('empty shader map')
    if stamp.get('game') != g.id or stamp.get('inputs') != dmsh_chain.inputs(g):
        issues.append('missing or incompatible full-library stamp')
    head = shader_lib.table_head(library)
    for name in ('c1_full.tsv', 'cb_extent.tsv'):
        p = g.table(name)
        first = p.read_text().splitlines()[0] if p.is_file() and p.stat().st_size else ''
        if first != head and not first.startswith(head + ' '):
            issues.append('missing or mismatched ' + name)
    try:
        rows = json.loads((v / '_b_build.json').read_text())
        if not isinstance(rows, list) or any(not isinstance(row, dict) or not isinstance(row.get('key'), str) for row in rows):
            raise ValueError('invalid report rows')
    except (OSError, ValueError):
        rows = []
        issues.append('missing or invalid full-build report')
    report = {}
    for row in rows:
        key = row['key']
        if key in report:
            issues.append(key + ': duplicate build-report row')
        report[key] = row
    if set(report) - expected.keys():
        issues.append('unexpected shader keys in full-build report')
    dual_file = g.table('dual_want.tsv')
    dual = {line.split()[0] for line in dual_file.read_text().splitlines() if line.strip() and not line.lstrip().startswith('#')} if dual_file.is_file() else set()
    for key, stage in sorted(expected.items()):
        row = report.get(key)
        if not row or row.get('st') != 'ok':
            issues.append(key + ': ' + (str(row.get('st')) + ' ' + str(row.get('err', '')) if row else 'missing build result'))
        suffixes = ['_b']
        try:
            refl = json.loads((v / (key + '_b.json')).read_text())
            if not isinstance(refl, dict) or not refl:
                raise ValueError('empty/invalid reflection')
        except (OSError, ValueError):
            issues.append(key + ': missing/invalid reflection')
            refl = {}
        if refl:
            issues += reflection_issues(key, refl, shader_lib.name_of(library).removeprefix('b-'))
            if refl.get('stage') != {'vertex': 'vs', 'pixel': 'ps', 'geometry': 'gs', 'compute': 'cs'}.get(stage):
                issues.append(key + ': reflection stage mismatch')
            if key in dual and not refl.get('entry_d'):
                issues.append(key + ': required dual-source entry missing')
            for field, suffix in (('fetch', '_bf'), ('vin', '_bv'), ('pull', '_bp')):
                if refl.get(field): suffixes.append(suffix)
            if refl.get('alpha') or refl.get('logic'): suffixes.append('_bx')
        folded = stage == 'geometry' and isinstance(refl.get('gs_slice'), dict) and bool(refl['gs_slice'])
        if stage == 'geometry' and not folded:
            suffixes += ['_bg' + str(i) for i in range(5)] + ['_bgv']
        for suffix in suffixes:
            # Ordinary side-library reflection is embedded in _b.json; geometry owns separate JSON.
            exts = ('.json',) if folded else ('.metallib', '.json') if suffix == '_b' or suffix.startswith('_bg') else ('.metallib',)
            for ext in exts:
                p = v / (key + suffix + ext)
                if not p.is_file() or not p.stat().st_size:
                    issues.append(key + ': missing/empty ' + suffix + ext)
                elif ext == '.json' and suffix != '_b':
                    try:
                        meta = json.loads(p.read_text())
                        if not isinstance(meta, dict) or not meta:
                            raise ValueError('empty reflection')
                        issues += reflection_issues(key, meta, shader_lib.name_of(library).removeprefix('b-'))
                    except (OSError, ValueError):
                        issues.append(key + ': invalid ' + suffix + ext)
    return {'game': g.id, 'expected': len(expected), 'stages': dict(collections.Counter(expected.values())),
            'reported': len(report), 'issues': issues, 'complete': not issues}
