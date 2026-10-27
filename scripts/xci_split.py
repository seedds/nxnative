#!/usr/bin/env python3
# Split a cartridge dump (.xci) whose secure partition also holds an update or DLC into one NSP per title (base game, update, each DLC), the layout build_ipa_from_nsp.py reads.
# Usage: uv run --with cryptography scripts/xci_split.py <game.xci> <prod.keys> [<out dir>]   (without <out dir>: only list the NCAs)
# Name the outputs as the NSP script expects: <name> [<title ID>][v0].nsp and <name> [<update title ID>][v<version>].nsp
import struct, os, sys, re
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
xci, keys = sys.argv[1], sys.argv[2]; out = sys.argv[3] if len(sys.argv) > 3 else None
hk = bytes.fromhex(re.search(r'^header_key\s*=\s*([0-9a-fA-F]{64})', open(keys).read(), re.M)[1])
f = open(xci, 'rb')
def hfs(off):
    f.seek(off); h = f.read(16); assert h[:4] == b'HFS0'
    n, st = struct.unpack('<II', h[4:12]); ents = f.read(n * 0x40); strs = f.read(st); data = off + 16 + n * 0x40 + st
    return [(strs[no:strs.index(b'\0', no)].decode(), data + o, s) for o, s, no in (struct.unpack('<QQI', ents[i*0x40:i*0x40+20]) for i in range(n))]
f.seek(0x130); root = struct.unpack('<Q', f.read(8))[0]
sec = next(o for n, o, s in hfs(root) if n == 'secure')
groups = {}; labels = {}; idx = 0; extra = []
for name, o, s in hfs(sec):
    if name.endswith('.nca'):
        f.seek(o + 0x200); d = Cipher(algorithms.AES(hk), modes.XTS((1).to_bytes(16, 'big'))).decryptor().update(f.read(0x200))
        assert d[:4] == b'NCA3', name
        tid = struct.unpack('<Q', d[0x10:0x18])[0]; rights = d[0x30:0x40].hex()
        g = idx   # the secure partition lists each title's NCAs up to its meta NCA: the base game, then the update and DLC (an update's control and legal NCAs carry the base title ID)
        if name.endswith('.cnmt.nca'): labels[idx] = f'{tid:016x}'; idx += 1
        print(f'{name} type={d[5]} title={tid:016x} rights={rights} -> group {g}')
        groups.setdefault(g, []).append((name, o, s))
    else: extra.append((name, o, s))   # ticket and cert: the only ones are the update's (title …800)
upd = next((g for g, t in labels.items() if t.endswith('800')), None)
if extra: groups.setdefault(upd if upd is not None else idx, []).extend(extra)
groups = {labels.get(g, 'rest'): v for g, v in groups.items()}
if out:
    for g, files in groups.items():
        dst = os.path.join(out, f'{os.path.splitext(os.path.basename(xci))[0]} {g}.nsp')
        strs = b''.join(n.encode() + b'\0' for n, _, _ in files); strs += b'\0' * (-(16 + len(files) * 0x18 + len(strs)) % 0x20)
        ents, off, si = b'', 0, 0
        for n, _, s in files: ents += struct.pack('<QQII', off, s, si, 0); off += s; si += len(n) + 1
        with open(dst, 'wb') as w:
            w.write(b'PFS0' + struct.pack('<II', len(files), len(strs)) + b'\0' * 4 + ents + strs)
            for n, o, s in files:
                f.seek(o); left = s
                while left: b = f.read(min(left, 64 << 20)); w.write(b); left -= len(b)
        print('wrote', dst, os.path.getsize(dst))
