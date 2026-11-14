#!/usr/bin/env python3
# Mario Kart World: the sound category table the host's Sound menu works from (dm/host/hle_audren.c, "sound categories").
# Reads every sound archive of the game (Sound/Resource/*.bars.zs in the RomFS image or folder), takes each sound's name from its AMTA entry and the CRC-32 and ADPCM
# coefficients from its BWAV header, and gives each a category by its name. Writes sound_cats.bin (what the host loads) and sound_names.tsv (CRC, category, name: for reading
# a DM_SOUND_LOG log) into the folder given, by default the game's Application Support folder.
# Usage: sound_cats.py <RomFS.img or RomFS folder> [out folder]        needs the zstandard module (the repository's .venv has it)
import collections, os, re, struct, sys, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import romfs_image, zstandard

CATS = ['other', 'music', 'engine', 'kart', 'item', 'voice', 'world', 'ui']   # the host's DM_SND_* order (dm.h)
# First match wins; a name no rule takes is the world's (ambience, objects, animals, traffic, audience)
RULES = [
    ('music',  r'^(BGM_|Dm_)'),                                              # Dm_: the cup and unlock movies' sound tracks
    ('engine', r'^(SE_Kart_.*_(Engine|Idling|Propeller)|SE_ENG_)'),
    ('ui',     r'^(SE_UI_|UI_|SE_SYS_|SE_BootSound|SE_FirstBoot|SE_Camerachange|SE_RC_|SE_Mission)'),
    ('voice',  r'^(Voice_|SE_Vo_|MobPilot_)'),
    ('item',   r'^(SE_Item|SE_Hit_Item|SE_Driver_ItemThrow|Coin_|SE_Coin|SE_KartAction_(Coin|HitStar|DamageFireBall|DamageIceBall))'),
    ('kart',   r'^(SE_KartAction|SE_KardtAction|SE_Kart_|SE_RoadNoise|SE_Collide|Ap_|SE_Damage|SE_Burn|SE_Driver_|SE_CmnWater|Cmn_Water|SE_Disappear|SE_SkinChange|SE_Metamorphosis)'),
]
PRIORITY = ['music', 'engine', 'ui', 'voice', 'item', 'kart', 'world']        # one CRC under names of two categories (none in 1.8.0)


def category(name):
    return next((c for c, r in RULES if re.search(r, name)), 'world')


def archives(src):
    """(name, bytes) of every decompressed sound archive"""
    z = zstandard.ZstdDecompressor()
    if os.path.isdir(src):
        d = os.path.join(src, 'Sound', 'Resource')
        for n in sorted(os.listdir(d)):
            if n.endswith('.bars.zs'): yield n, z.decompress(open(os.path.join(d, n), 'rb').read(), max_output_size=1 << 31)
        return
    idx = romfs_image.read_index(src)
    with open(src, 'rb') as f:
        for p in sorted(k for k, (isdir, _, _) in idx.items() if not isdir and k.startswith('Sound/Resource/') and k.endswith('.bars.zs')):
            _, off, size = idx[p]; f.seek(off)
            yield p.split('/')[-1], z.decompress(f.read(size), max_output_size=1 << 31)


def sounds(d):
    """(name, BWAV offset) of a BARS archive: count at +0xc, the names' CRC-32s, then (AMTA offset, BWAV offset) pairs; an AMTA's name is at +0x24 + the offset stored there"""
    assert d[:4] == b'BARS', d[:8]
    n = struct.unpack_from('<I', d, 0xc)[0]
    hashes = struct.unpack_from(f'<{n}I', d, 0x10); offs = struct.unpack_from(f'<{2 * n}I', d, 0x10 + 4 * n)
    for i in range(n):
        am, bw = offs[2 * i], offs[2 * i + 1]
        s = am + 0x24 + struct.unpack_from('<I', d, am + 0x24)[0]; name = d[s:d.index(b'\0', s)].decode()
        assert zlib.crc32(name.encode()) == hashes[i], name
        yield name, bw


def main():
    if len(sys.argv) < 2: sys.exit(__doc__ or 'usage: sound_cats.py <RomFS.img or RomFS folder> [out folder]')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/Library/Application Support/nxnative/0400C3F00006E000')
    by_crc, by_coef, names, count = collections.defaultdict(set), collections.defaultdict(list), {}, collections.Counter()
    for _, d in archives(sys.argv[1]):
        for name, bw in sounds(d):
            if d[bw:bw + 4] != b'BWAV': continue
            crc, _, nch = struct.unpack_from('<IHH', d, bw + 8); c = category(name)
            by_crc[crc].add(c); names.setdefault(crc, name); count[c] += 1
            for ch in range(nch):   # a channel: 0x4c bytes from +0x10, encoding at +0 (1 ADPCM), the 16 coefficients at +0x10
                o = bw + 0x10 + 0x4c * ch
                if struct.unpack_from('<H', d, o)[0] == 1: by_coef[d[o + 0x10:o + 0x30]].append(c)
    crcs = sorted((k, CATS.index(next(c for c in PRIORITY if c in v))) for k, v in by_crc.items())
    coefs = sorted((zlib.crc32(k), CATS.index(collections.Counter(v).most_common(1)[0][0])) for k, v in by_coef.items())   # shared coefficients: the category most sounds with them have
    seen = collections.Counter(h for h, _ in coefs); coefs = [(h, c) for h, c in coefs if seen[h] == 1]                    # two coefficient sets with one hash: neither
    def block(rows): return b''.join(struct.pack('<I', k) for k, _ in rows) + bytes(c for _, c in rows) + b'\0' * (-len(rows) % 4)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'sound_cats.bin'), 'wb') as f: f.write(b'DMSNDCAT' + struct.pack('<III', 1, len(crcs), len(coefs)) + block(crcs) + block(coefs))
    with open(os.path.join(out, 'sound_names.tsv'), 'w') as f:
        for k, c in crcs: f.write(f'{k:08x}\t{CATS[c]}\t{names[k]}\n')
    print(f'{sum(count.values())} sounds: ' + ', '.join(f'{c} {count[c]}' for c in CATS[1:]) + f'; {len(crcs)} CRCs, {len(coefs)} coefficient sets -> {out}/sound_cats.bin')


if __name__ == '__main__': main()
