#!/usr/bin/env python3
# Supergiant's texture packages (Hades: Content/Win/Packages/<resolution>/*.pkg) with their BC7 textures re-encoded as ASTC 4x4, for GPUs that cannot sample BC
# (supportsBCTextureCompression NO: the A16 and older iPhones). BC7 and ASTC 4x4 both take 16 bytes per 4x4 block, so a texture keeps its size and every size and offset the game
# reads stays right; the Vulkan layer then backs VK_FORMAT_BC7_* images with ASTC 4x4 (DM_VK_BC7_ASTC=1, hle_vk.m). Without this, every BC7 image fails to create on such a GPU.
# A package (big-endian): u32 version, 0x20000000 set when the chunks are LZ4-compressed, then chunks to the end of the file: u8 1, u32 compressed size, an LZ4 block of at most
# 32 MB. A package without the LZ4 bit is left as it is (Hades has none).
# A texture in a chunk: "XNB" + a platform letter + 06 00 + u32 size (the 30 header bytes + the data), then u32 format, width, height, mip levels and data size (little-endian),
# then the levels' blocks, largest first. Format 28 is BC7 (610 of Hades 1's 671 textures), 0 RGBA8. A format-28 texture is converted only when its data size is exactly its
# levels' 4x4 blocks × 16 bytes; anything else is left alone.
# Encoder: Xcode's TextureConverter. Each level is decoded from BC7 (NVTT) and encoded as ASTC 4x4 (ARM's astcenc, quality Normal, alpha kept as it is); results are cached by the
# BC7 data's hash and the encoder settings, so a rebuild converts only what changed.
# Usage: pkg_astc.py [--jobs N] [--cache DIR] <RomFS image> <out image>   (the converted packages replace the originals in a copy of the image: romfs_image.repack)
import argparse, concurrent.futures, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile, threading, time
from pathlib import Path

import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parent))
import romfs_image

LZ4 = 0x20000000
CHUNK = 0x2000000
BC7 = 28
XNB = re.compile(rb'XNB[A-Za-z]\x06\x00')
GL_BPTC, GL_ASTC_4x4, GL_RGBA = 0x8E8C, 0x93B0, 0x1908
ENCODE = ['--compression_format=ASTC4x4', '--compressor=ARM', '--compression_quality=Normal', '--max_mipmaps=1', '--alpha_mode=Preserve']


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode: raise RuntimeError(f'{Path(str(cmd[0])).name} failed ({r.returncode}): {(r.stdout + r.stderr).strip()[-400:]}')


def texture_converter():   # Xcode's TextureConverter, or None
    for c in (['xcrun', '--find', 'TextureConverter'], ['xcode-select', '-p']):
        try: r = subprocess.run(c, capture_output=True, text=True)
        except OSError: continue
        p = Path(r.stdout.strip()) if r.returncode == 0 else None
        if p and p.name != 'TextureConverter': p = p / 'usr/bin/TextureConverter'
        if p and os.access(p, os.X_OK): return p
    return None


def levels(w, h, mips):   # (width, height, bytes) of each level, 16 bytes per 4x4 block
    return [(max(1, w >> i), max(1, h >> i), ((max(1, w >> i) + 3) // 4) * ((max(1, h >> i) + 3) // 4) * 16) for i in range(mips)]


def textures(d):   # (data offset, width, height, mip levels) of each convertible BC7 texture in a chunk
    pos = 0
    while (m := XNB.search(d, pos)):
        start = m.start() + 30
        if start > len(d): break
        xnb, = struct.unpack_from('<I', d, m.start() + 6)
        fmt, w, h, mips, size = struct.unpack_from('<5I', d, m.start() + 10)
        if xnb != size + 30 or start + size > len(d) or not (0 < w <= 16384 and 0 < h <= 16384 and 0 < mips <= 15):
            pos = m.end(); continue
        if fmt == BC7 and size == sum(n for _, _, n in levels(w, h, mips)): yield start, w, h, mips
        pos = start + size


def chunks(data):   # (offset of the chunk's u8, compressed bytes) of each chunk; None for a package this tool leaves alone
    if len(data) < 4 or not struct.unpack('>I', data[:4])[0] & LZ4: return None
    out, pos = [], 4
    while pos < len(data):
        if data[pos] != 1 or pos + 5 > len(data): return None
        n, = struct.unpack_from('>I', data, pos + 1)
        if pos + 5 + n > len(data): return None
        out.append((pos, data[pos + 5:pos + 5 + n])); pos += 5 + n
    return out


def package_levels(data):   # (BC7 blocks, width, height) of every level of every convertible texture in a package
    for _, z in chunks(data) or []:
        d = lz4.block.decompress(z, uncompressed_size=CHUNK)
        for start, w, h, mips in textures(d):
            for lw, lh, n in levels(w, h, mips):
                yield d[start:start + n], lw, lh; start += n


def convert_package(data, convert):
    """(the package with each convertible BC7 level replaced by convert(blocks, width, height), the number of textures converted); unchanged chunks keep their bytes"""
    cs = chunks(data)
    if not cs: return data, 0
    out, count = bytearray(data[:4]), 0
    for _, z in cs:
        d = bytearray(lz4.block.decompress(z, uncompressed_size=CHUNK)); found = 0
        for start, w, h, mips in list(textures(d)):
            for lw, lh, n in levels(w, h, mips):
                new = convert(bytes(d[start:start + n]), lw, lh)
                if len(new) != n: raise ValueError(f'a {lw}x{lh} level came back as {len(new)} bytes instead of {n}')
                d[start:start + n] = new; start += n
            found += 1
        if found: z = lz4.block.compress(bytes(d), store_size=False)
        out += b'\x01' + struct.pack('>I', len(z)) + z; count += found
    return bytes(out), count


def ktx(blocks, w, h, gl_format):   # a one-level KTX 1 file
    return (b'\xabKTX 11\xbb\r\n\x1a\n' + struct.pack('<13I', 0x04030201, 0, 1, 0, gl_format, GL_RGBA, w, h, 0, 0, 1, 1, 0)
            + struct.pack('<I', len(blocks)) + blocks)


def ktx_level(path):   # (glInternalFormat, level 0's bytes, width, height) of a KTX 1 file
    b = Path(path).read_bytes()
    if b[:12] != b'\xabKTX 11\xbb\r\n\x1a\n': raise ValueError(f'not a KTX 1 file: {path}')
    gl_format, = struct.unpack_from('<I', b, 28); w, h = struct.unpack_from('<II', b, 36); kv, = struct.unpack_from('<I', b, 60)
    n, = struct.unpack_from('<I', b, 64 + kv)
    return gl_format, b[68 + kv:68 + kv + n], w, h


def bc7_astc(blocks, w, h, tc, cache):   # one BC7 level as ASTC 4x4 blocks of the same size
    key = hashlib.sha256(blocks + struct.pack('<II', w, h) + ' '.join(ENCODE).encode()).hexdigest()
    hit = Path(cache) / key[:2] / key
    if hit.exists(): return hit.read_bytes()
    with tempfile.TemporaryDirectory(prefix='pkg_astc.') as tmp:
        tmp = Path(tmp)
        (tmp / 'bc7.ktx').write_bytes(ktx(blocks, w, h, GL_BPTC))
        run([tc, '--mode=decompress', '--file_format=KTX', f'--output={tmp / "rgba.ktx"}', tmp / 'bc7.ktx'])
        run([tc, *ENCODE, '--file_format=KTX', '--disable_annotation', f'--output={tmp / "astc.ktx"}', tmp / 'rgba.ktx'])
        fmt, astc, aw, ah = ktx_level(tmp / 'astc.ktx')
    if (fmt, aw, ah, len(astc)) != (GL_ASTC_4x4, w, h, len(blocks)):
        raise ValueError(f'TextureConverter gave format {fmt:#x} {aw}x{ah}, {len(astc)} bytes for a {w}x{h} BC7 level of {len(blocks)} bytes')
    hit.parent.mkdir(parents=True, exist_ok=True)
    part = hit.with_name(hit.name + f'.{os.getpid()}.{threading.get_ident()}.part')   # per thread: two equal levels can be encoded at once
    part.write_bytes(astc); os.replace(part, hit)
    return astc


def read(img, offset, size):
    with open(img, 'rb') as f:
        f.seek(offset); return f.read(size)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Re-encode the BC7 textures of Supergiant packages as ASTC 4x4 in a copy of a RomFS image')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2), help='levels encoded at once (TextureConverter also uses threads)')
    ap.add_argument('--cache', help='encoded levels, reused by later runs (default: <out image>.cache)')
    ap.add_argument('image'); ap.add_argument('out')
    a = ap.parse_args(argv)
    tc = texture_converter()
    if not tc: sys.exit('[RED] Xcode\'s TextureConverter not found (xcrun --find TextureConverter): install Xcode and select it with xcode-select -s')
    cache = Path(a.cache or a.out + '.cache')
    stamp = Path(a.out + '.inputs.json')
    tool = hashlib.sha256(Path(__file__).read_bytes() + Path(romfs_image.__file__).read_bytes()).hexdigest()   # this tool and the repacker
    inputs = {'source': romfs_image.file_identity(a.image), 'tool': tool}
    try:
        if json.loads(stamp.read_text()) == dict(inputs, image=romfs_image.file_identity(a.out)): print(f'{a.out}: reused'); return 0
    except (OSError, ValueError): pass
    t0 = time.time()
    pkgs = sorted((p, o, s) for p, (isdir, o, s) in romfs_image.read_index(a.image).items() if not isdir and p.endswith('.pkg'))
    tmp = Path(a.out + '.pkgs'); shutil.rmtree(tmp, ignore_errors=True)
    replace, ntex, nlev = {}, 0, 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for p, o, s in pkgs:
            data = read(a.image, o, s)
            work = [pool.submit(bc7_astc, b, w, h, tc, cache) for b, w, h in package_levels(data)]
            for f in work: f.result()   # the encoding, in parallel; the rewrite below reads the cache
            if not work: continue
            out, n = convert_package(data, lambda b, w, h: bc7_astc(b, w, h, tc, cache))
            dst = tmp / p; dst.parent.mkdir(parents=True, exist_ok=True); dst.write_bytes(out)
            replace[p] = dst; ntex += n; nlev += len(work)
            print(f'{p}: {n} BC7 textures → ASTC 4x4 ({s / 2**20:.0f} → {len(out) / 2**20:.0f} MB)', flush=True)
    romfs_image.repack(a.image, a.out, replace)
    shutil.rmtree(tmp, ignore_errors=True)
    stamp.write_text(json.dumps(dict(inputs, image=romfs_image.file_identity(a.out))))
    print(f'{a.out}: {ntex} textures ({nlev} levels) in {len(replace)} of {len(pkgs)} packages re-encoded in {time.time() - t0:.0f} s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
