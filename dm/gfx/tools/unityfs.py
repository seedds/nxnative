#!/usr/bin/env python3
# Unity 资源包读取（够取着色器用）：UnityFS 包 → 序列化文件 → 按类型树解出对象 → Shader 的各平台子程序。
# Unity asset bundle reader (enough for shaders): UnityFS bundle → serialized files → objects decoded by their type trees → each platform's subprograms of a Shader.
#   · UnityFS（'UnityFS\0'，格式 6..8）：大端头 + 块信息（LZMA / LZ4 / LZ4HC 或不压）+ 数据块；块信息里列出文件节点（偏移、长度、路径）。
#   · 序列化文件（格式 17..22）：元数据（类型表连类型树、对象表）+ 对象数据。类型树字符串的高位置 1 时指 Unity 内置的公共字符串表，
#     这张表不随资源走，取自游戏自己的 main（'AABB\0AnimationClip\0…'，见 common_strings）。
#   · Shader（类 48）：platforms[i] 平台的 compressedBlob 由 offsets[i][s] / compressedLengths[i][s] / decompressedLengths[i][s] 分段 LZ4 压缩；
#     解开的段内：u32 子程序数，每项 12 字节 {偏移, 长度, 段号}（2019.3 起）；子程序 = 版本、程序类型、统计、关键字、程序代码字节……
#   · UnityFS ('UnityFS\0', format 6..8): big-endian header + block info (LZMA / LZ4 / LZ4HC or stored) + data blocks; the block info lists the file nodes (offset, size, path).
#   · Serialized file (format 17..22): metadata (type table with type trees, object table) + object data. A type tree string offset with the high bit set refers to Unity's built-in common string table,
#     which is not shipped with the assets; it is taken from the game's own main ('AABB\0AnimationClip\0…', see common_strings).
#   · Shader (class 48): the compressedBlob of platform platforms[i] is LZ4-compressed in segments offsets[i][s] / compressedLengths[i][s] / decompressedLengths[i][s];
#     inside a decompressed segment: u32 subprogram count, then 12-byte entries {offset, length, segment} (2019.3 and later); a subprogram = version, program type, stats, keywords, program code bytes, ...
# 只依赖 lz4（与 shader_split 解 NSO 同一个包）| Only depends on lz4 (the same package shader_split uses for NSOs)
import lzma, struct

CLASS_SHADER = 48

def _lz4(raw, size):
    import lz4.block
    return lz4.block.decompress(raw, uncompressed_size=size)

def _decomp(raw, size, kind):
    if kind == 0: return raw
    if kind == 1:   # Unity 的 LZMA：5 字节属性 + 裸流 | Unity's LZMA: 5 property bytes + raw stream
        props = raw[0]; dict_size = struct.unpack_from('<I', raw, 1)[0]
        lc, lp, pb = props % 9, (props // 9) % 5, props // 45
        f = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dict_size, 'lc': lc, 'lp': lp, 'pb': pb}])
        return f.decompress(raw[5:], size)[:size]
    if kind in (2, 3): return _lz4(raw, size)
    raise ValueError(f'UnityFS 未知压缩 {kind}')

def _cstr(d, p):
    e = d.index(b'\0', p); return d[p:e].decode('utf-8', 'replace'), e + 1

def bundle_files(d):
    """UnityFS 包 → [(路径, 字节)] | UnityFS bundle → [(path, bytes)]"""
    if d[:8] != b'UnityFS\0': raise ValueError('不是 UnityFS')
    fmt = struct.unpack_from('>I', d, 8)[0]
    _, p = _cstr(d, 12); _, p = _cstr(d, p)
    size, csz, usz, flags = struct.unpack_from('>qIII', d, p); p += 20
    if fmt >= 7: p = (p + 15) & ~15
    if flags & 0x80: info_raw = d[len(d) - csz:]
    else: info_raw = d[p:p + csz]; p += csz
    info = _decomp(info_raw, usz, flags & 0x3f)
    q = 16; nb = struct.unpack_from('>i', info, q)[0]; q += 4
    blocks = []
    for _ in range(nb):
        u, c, f = struct.unpack_from('>IIH', info, q); q += 10; blocks.append((u, c, f))
    nn = struct.unpack_from('>i', info, q)[0]; q += 4
    nodes = []
    for _ in range(nn):
        off, sz, f = struct.unpack_from('>qqI', info, q); q += 20
        name, q = _cstr(info, q); nodes.append((off, sz, name))
    if flags & 0x200: p = (p + 15) & ~15   # BlockInfoNeedPaddingAtStart
    out = bytearray()
    for u, c, f in blocks:
        out += _decomp(d[p:p + c], u, f & 0x3f); p += c
    return [(name, bytes(out[off:off + sz])) for off, sz, name in nodes]

def common_strings(image):
    """游戏 main 里 Unity 的公共字符串表 → {偏移: 字符串} | Unity's common string table in the game's main → {offset: string}"""
    a = image.find(b'AABB\0AnimationClip\0AnimationCurve\0')
    if a < 0: raise ValueError('main 里找不到 Unity 公共字符串表（AABB\\0AnimationClip…）')
    tab, p = {}, a
    while True:
        e = image.index(b'\0', p)
        if e == p: break
        s = image[p:e]
        if not all(0x20 <= ch < 0x7f for ch in s): break
        tab[p - a] = s.decode(); p = e + 1
    return tab

class Node:
    __slots__ = ('type', 'name', 'size', 'flags', 'kids')
    def __init__(s, t, n, z, f): s.type, s.name, s.size, s.flags, s.kids = t, n, z, f, []

class Reader:
    def __init__(s, d, le): s.d, s.p, s.e = d, 0, '<' if le else '>'
    def u(s, fmt):
        v = struct.unpack_from(s.e + fmt, s.d, s.p); s.p += struct.calcsize(s.e + fmt); return v[0] if len(v) == 1 else v
    def align(s): s.p = (s.p + 3) & ~3

PRIM = {'SInt8': 'b', 'UInt8': 'B', 'char': 'B', 'bool': '?', 'SInt16': 'h', 'short': 'h', 'UInt16': 'H', 'unsigned short': 'H',
        'SInt32': 'i', 'int': 'i', 'UInt32': 'I', 'unsigned int': 'I', 'Type*': 'i', 'SInt64': 'q', 'long long': 'q', 'UInt64': 'Q',
        'unsigned long long': 'Q', 'FileSize': 'Q', 'float': 'f', 'double': 'd'}

def _read(r, n):
    if n.type in PRIM and not n.kids:
        v = r.u(PRIM[n.type])
    elif n.type == 'string':
        ln = r.u('i'); v = r.d[r.p:r.p + ln]; r.p += ln
        try: v = v.decode('utf-8')
        except UnicodeDecodeError: pass
        if n.kids and n.kids[0].flags & 0x4000: r.align()
    elif n.kids and n.kids[0].type == 'Array':
        arr = n.kids[0]; cnt = r.u('i'); el = arr.kids[1]
        if el.type in ('UInt8', 'char', 'SInt8') and not el.kids: v = r.d[r.p:r.p + cnt]; r.p += cnt
        elif el.type in PRIM and not el.kids:
            f = PRIM[el.type]; w = struct.calcsize(f); v = list(struct.unpack_from(r.e + f * cnt, r.d, r.p)); r.p += w * cnt
        else: v = [_read(r, el) for _ in range(cnt)]
        if arr.flags & 0x4000: r.align()
    elif n.type == 'TypelessData':
        cnt = r.u('i'); v = r.d[r.p:r.p + cnt]; r.p += cnt
    else:
        v = {k.name: _read(r, k) for k in n.kids}
    if n.flags & 0x4000: r.align()
    return v

class NoTree(ValueError):
    """序列化文件没带类型树，且没有同类同哈希的树可借 | the serialized file has no type tree and no tree of the same class and hash to borrow"""

def serialized_objects(d, common, want=None, trees=None):
    """序列化文件 → 迭代 (类号, 路径 ID, 对象字典)；want = 只解这些类号。
    trees = {(类号, 类型哈希): 类型树}：带类型树的文件把树记进去，不带的（游戏本体 data.unity3d 等）按同类同哈希借用——同一 Unity 版本的类布局一致，哈希相同即布局相同
    serialized file → yields (class ID, path ID, object dict); want = only decode these class IDs.
    trees = {(class ID, type hash): type tree}: files with type trees record theirs, files without (the game's own data.unity3d and so on) borrow one of the same class and hash -- the same hash means the same layout"""
    msz, fsz, ver, doff = struct.unpack_from('>IIII', d, 0)
    if not 17 <= ver <= 22: raise ValueError(f'序列化文件格式 {ver} 未支持')
    p = 16; le = d[p] == 0; p += 4
    if ver >= 22: msz, fsz, doff, _ = struct.unpack_from('>Iqqq', d, p); p += 28
    r = Reader(d, le); r.p = p
    _, r.p = _cstr(d, r.p); r.u('i'); tt = r.u('?')
    tlist = []
    for _ in range(r.u('i')):
        cid = r.u('i'); r.u('B'); r.u('h')
        if cid == 114: r.p += 16   # 脚本 ID | script ID
        h = d[r.p:r.p + 16]; r.p += 16
        root = None
        if tt:
            nn, sb = r.u('i'), r.u('i')
            # 2019.3 之前（序列化格式 < 21）的 TypeTreeNode 没有末尾的 refTypeHash(u64)：节点 24 字节，不是 32
            # before 2019.3 (serialized format < 21) the TypeTreeNode has no trailing refTypeHash(u64): the node is 24 bytes, not 32
            nfmt = 'HBBIIiiiQ' if ver >= 21 else 'HBBIIiii'
            raw = [r.u(nfmt) for _ in range(nn)]
            buf = d[r.p:r.p + sb]; r.p += sb
            def st(o):
                if o & 0x80000000: return common.get(o & 0x7fffffff, f'?{o & 0x7fffffff}')
                return buf[o:buf.index(b'\0', o)].decode('utf-8', 'replace')
            stack = []
            for row in raw:
                _v, lv, _tf, to, no, bs, _ix, mf = row[:8]
                nd = Node(st(to), st(no), bs, mf)
                del stack[lv:]
                if stack: stack[-1].kids.append(nd)
                else: root = nd
                stack.append(nd)
            if ver >= 21: n = r.u('i'); r.p += 4 * n   # 类型依赖 | type dependencies
            if trees is not None and (want is None or cid in want): trees.setdefault((cid, h), root)
        elif trees is not None: root = trees.get((cid, h))
        tlist.append((cid, h, root))
    objs = []
    for _ in range(r.u('i')):
        r.align(); pid = r.u('q'); start = r.u('q') if ver >= 22 else r.u('I'); size = r.u('I'); ti = r.u('i')
        objs.append((pid, start, size, ti))
    for pid, start, size, ti in objs:
        cid, h, root = tlist[ti]
        if want and cid not in want: continue
        if root is None: raise NoTree(f'类 {cid}（类型哈希 {h.hex()}）没有类型树可用')
        o = Reader(d, le); o.d = d[doff + start:doff + start + size]
        yield cid, pid, _read(o, root)

def is_serialized(d):
    """像不像序列化文件（大端头：格式 19..22，22 起长度在 +0x14 之后）| does it look like a serialized file (big-endian header: format 19..22; from 22 the sizes follow +0x14)"""
    if len(d) < 48: return False
    ver = struct.unpack_from('>I', d, 8)[0]
    if not 17 <= ver <= 22 or d[16] not in (0, 1): return False
    if ver >= 22: msz, fsz, doff = struct.unpack_from('>Iqq', d, 20)
    else: msz, fsz, _, doff = struct.unpack_from('>IIII', d, 0)
    return fsz == len(d) and 0 < msz < fsz and doff <= fsz

def shader_programs(sh):
    """Shader 对象 → 迭代 (平台号, 子程序序号, 子程序字节) | Shader object → yields (platform, subprogram index, subprogram bytes)"""
    blob = sh['compressedBlob']
    for i, plat in enumerate(sh['platforms']):
        offs, clen, dlen = sh['offsets'][i], sh['compressedLengths'][i], sh['decompressedLengths'][i]
        if isinstance(offs, list):
            # 2019.3 起：每平台多段，表项 12 字节 {偏移, 长度, 段号}
            # 2019.3 and later: several segments per platform, 12-byte table entries {offset, length, segment}
            segs = [_lz4(blob[o:o + c], u) for o, c, u in zip(offs, clen, dlen)]
            if not segs: continue
            s0 = segs[0]; n = struct.unpack_from('<I', s0, 0)[0]
            for k in range(n):
                off, ln, seg = struct.unpack_from('<III', s0, 4 + 12 * k)
                yield plat, k, segs[seg][off:off + ln]
        else:
            # 2019.3 之前：每平台一段，表项 8 字节 {偏移, 长度}，子程序都在这一段里（Cuphead，Unity 2017.4）
            # before 2019.3: one segment per platform, 8-byte table entries {offset, length}, all subprograms inside that segment (Cuphead, Unity 2017.4)
            seg = _lz4(blob[offs:offs + clen], dlen)
            n = struct.unpack_from('<I', seg, 0)[0]
            for k in range(n):
                off, ln = struct.unpack_from('<II', seg, 4 + 8 * k)
                yield plat, k, seg[off:off + ln]

def subprogram(b):
    """子程序头 → (版本, 程序类型, 关键字, 局部关键字, 程序代码, 代码后偏移) | subprogram header → (version, program type, keywords, local keywords, program code, offset after the code)"""
    r = Reader(b, True)
    ver, ptype = r.u('i'), r.u('i'); r.p += 12
    if ver >= 201608170: r.p += 4
    def strs():
        out = []
        for _ in range(r.u('i')):
            ln = r.u('i'); out.append(b[r.p:r.p + ln].decode('utf-8', 'replace')); r.p += ln; r.align()
        return out
    kw = strs(); lkw = strs() if ver >= 201806140 else []
    ln = r.u('i'); code = b[r.p:r.p + ln]; r.p += ln; r.align()
    return ver, ptype, kw, lkw, code, r.p
