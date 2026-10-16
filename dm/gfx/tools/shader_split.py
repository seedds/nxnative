#!/usr/bin/env python3
# 马力欧全部着色器 → dmsh_build.py 认的「包目录」+ 着色器映射表 + 翻译查询作答表（计算着色器的线程组 / 共享内存）。
# 来源（全量扫，不靠文件名）：RomFS 下每个文件（.zs 先 zstd 解压，Yaz0 先解压，SARC 逐成员展开）与 ExeFS main（NSO，lz4 解压）里的
#   · BNSH（'BNSH\0\0\0\0'，bfsha / 粒子特效 esetb / UI 布局 blarc / main 内嵌各自带）：
#       +0x60 grsc 块；+0x7c 变体数 u32；+0x80 变体表指针（指针都相对 BNSH 起点）。变体 0x40 字节，取 +0x10 二进制程序；
#       二进制程序 +0x08 起 6 个阶段指针：顶点、外壳、域、几何、像素、计算；阶段代码描述 +0x08 控制段指针、+0x10 代码区指针、+0x18 代码区长、+0x1c 控制段长。
#   · BAHS（agl 的 sharcb：agl / gsys 资源包、Secred 包里的 GameAglShader）：文件头 +0x1c 代码区起点、+0x18 代码区长；
#       表区里每条二进制项 16 字节 {代码偏移（相对代码区，0x100 对齐）, 0, 控制段长 0x880 / 0x88c（3D 世界 0x848）, 控制段相对本项地址的有符号偏移}。
#       控制段在多个程序间共用，所以按项配对，不能按控制段找代码；项靠「控制段魔数 + 代码区魔数 + 长度自洽」三重核对认出。
#   · Unity（UnityFS 资源包、Data 下的序列化文件，读法见 unityfs.py）：每个 Shader 对象的 Switch 平台子程序，程序代码 = 0x60 头 + 各阶段块。
#       头是 4 个 i32 ×6 数组，下标按 NVN 阶段（0 顶点 1 像素 2 几何 3 外壳 4 域 5 计算）：[0] 未用（0 或暂存大小）、[1] 块偏移（相对 +0x60，−1 = 无此阶段）、[2] 控制段长、[3] 代码区长；
#       块 = 控制段 + 代码区。一个程序的顶点 / 像素阶段即一对。游戏本体的序列化文件（data.unity3d、unity default resources）不带类型树，借资源包里同哈希的 Shader 类型树（第二遍）
#   · MSHD（MercurySteam，银河战士 生存恐惧 system/shd/*.bshdat）：每个阶段 = 控制段（+0x20 = 控制段长；后面有时跟一小段编译选项表）+ 下一个代码区头起的代码区；
#       控制段 0x880 之后是 NV 汇编文本，代码区之后是 SASS 反汇编文本。478 个文件全是「顶点、像素」成对排列（1、2 或 4 对），按顺序两两配对
# 代码区：0x30 头 + （图形）0x50 SPH + 指令；头魔数图形 0x12345678、计算 0x12345679。控制段 0x98761234 开头，+0x704 = 代码区长，
#   +0x700 = c1 常数段在代码区内的起点；计算着色器 +0x748 线程组 xyz（u32 ×3）、+0x754 共享内存字节（D2R 22 个计算着色器与 PC 反射逐条核对）。
# 阶段：BNSH 取阶段槽；BAHS 取 SPH 第 0 字 bits 10..13（1 顶点 2 外壳 3 域 4 几何 5 像素），计算头的为计算。
# 键 = sha256(控制段前 0x880 字节（控制段 +0x20 自报更短时取其长度，见 ctl_len）+ 代码区)[:16]，与宿主 prog_get 一致：本作控制段相同、代码不同的情况很多
#   （SecredUber 16600 个阶段着色器只有 3079 种控制段，其中 6367 处同控制段而代码不同），只哈希控制段会拿错库。
# 输出：<包目录>/<阶段>_<来源组>.bin，每条记录 = 'BTCDBTCD' + 头（+0x28 控制段长、+0x2c 代码区长）补到 0x38 + 代码区 + 控制段，同键只出一份；
#   映射表行与 D2R 的 shader_map.tsv 同列（第 5 列记首次出现的位置）；作答表每个计算着色器一行 {"local", "shared"}（共享内存按 16 字节向上取整，同 D2R 口径）。
# 用法：shader_split.py <RomFS 目录> <ExeFS main> <输出包目录> <输出映射表> <输出作答表> [<输出配对表>]   （解 main 要 lz4：用 dm 的 venv python 跑）   环境 J=并发（缺省 8）
#   带更新包：RomFS 与 main 都写成逗号分隔的列表（基础版在前，如 data/v0/RomFS,data/v327680/RomFS），输出两版的并集（见 main()）
#   Unity 的类型树字符串要 Unity 的公共字符串表，取自第一个 main
# 配对表：BNSH 每个变体（Unity 每个程序）的（顶点键, 像素键），dmsh_build 据此给顶点着色器补上像素着色器读而它没写的输出（见 dmsh_build.py vs_pad）
# All of Mario's shaders → the "package directory" dmsh_build.py understands + the shader map + the translation query answer table (thread group / shared memory of compute shaders).
# Sources (a full scan, not relying on file names): every file under RomFS (.zs is zstd-decompressed first, Yaz0 is decompressed, SARC is expanded member by member) and ExeFS main (NSO, lz4-decompressed), looking for
#   · BNSH ('BNSH\0\0\0\0', carried by bfsha / particle effect esetb / UI layout blarc / embedded in main):
#       +0x60 grsc block; +0x7c variation count u32; +0x80 variation table pointer (pointers are relative to the BNSH start). A variation is 0x40 bytes; the binary program is at +0x10;
#       the binary program has 6 stage pointers from +0x08: vertex, hull, domain, geometry, pixel, compute; the stage code descriptor has +0x08 control pointer, +0x10 code area pointer, +0x18 code area length, +0x1c control length.
#   · BAHS (agl's sharcb: the agl / gsys resource packs, GameAglShader in the Secred pack): file header +0x1c code region start, +0x18 code region length;
#       each binary entry in the table area is 16 bytes {code offset (relative to the code region, 0x100-aligned), 0, control length 0x880 / 0x88c (3D World 0x848), signed offset of the control relative to this entry's address}.
#       Control sections are shared between programs, so pairing goes by entry, never by finding code from a control; entries are recognized by a triple check of "control magic + code magic + consistent lengths".
#   · Unity (UnityFS bundles and the serialized files under Data, read by unityfs.py): the Switch subprograms of each Shader object; the program code = a 0x60 header + one block per stage.
#       The header is 4 i32 ×6 arrays indexed by NVN stage (0 vertex 1 pixel 2 geometry 3 hull 4 domain 5 compute): [0] unused (0 or a scratch size), [1] block offset (from +0x60, -1 = no such stage), [2] control length, [3] code length;
#       block = control section + code area. A program's vertex / pixel stages form a pair. The game's own serialized files (data.unity3d, unity default resources) carry no type trees and borrow the bundles' Shader type tree of the same hash (second pass)
#   · MSHD (MercurySteam, Metroid Dread system/shd/*.bshdat): each stage = a control section (+0x20 = control length; sometimes a small compiler option table follows) + the code area starting at the next code header;
#       NV assembly text follows the first 0x880 bytes of the control section, and SASS disassembly text follows the code area. All 478 files hold "vertex, pixel" in pairs (1, 2 or 4 pairs), paired two by two in order
# Code area: 0x30 header + (graphics) 0x50 SPH + instructions; header magic 0x12345678 for graphics, 0x12345679 for compute. The control section starts with 0x98761234, +0x704 = code area length,
#   +0x700 = start of the c1 constant segment within the code area; for compute +0x748 thread group xyz (u32 ×3), +0x754 shared memory bytes (checked one by one against PC reflection on D2R's 22 compute shaders).
# Stage: BNSH uses the stage slot; BAHS uses bits 10..13 of SPH word 0 (1 vertex 2 hull 3 domain 4 geometry 5 pixel), and a compute header means compute.
# Key = sha256(first 0x880 bytes of the control section (its own length when control +0x20 says it is shorter, see ctl_len) + code area)[:16], the same as the host's prog_get: in this game many shaders share a control section but differ in code
#   (SecredUber's 16600 stage shaders have only 3079 distinct control sections, with 6367 cases of same control, different code), so hashing the control alone picks the wrong library.
# Output: <package dir>/<stage>_<source group>.bin, each record = 'BTCDBTCD' + header (+0x28 control length, +0x2c code length) padded to 0x38 + code area + control section, one copy per key;
#   map rows have the same columns as D2R's shader_map.tsv (column 5 records where the key first appeared); the answer table has one row per compute shader {"local", "shared"} (shared memory rounded up to 16 bytes, as D2R did).
# Usage: shader_split.py <RomFS dir> <ExeFS main> <output package dir> <output map> <output answer table> [<output pair table>]   (decompressing main needs lz4: run with dm's venv python)   Environment J=concurrency (default 8)
#   With an update: give RomFS and main as comma-separated lists (base game first, e.g. data/v0/RomFS,data/v327680/RomFS); the output is the union of both versions (see main())
#   Unity type trees need Unity's common string table, which is taken from the first main
# Pair table: (vertex key, pixel key) of every BNSH variation (every Unity program); dmsh_build uses it to give vertex shaders the outputs their pixel shaders read but they do not write (see dmsh_build.py vs_pad)
import collections, concurrent.futures as cf, hashlib, json, os, re, struct, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../games')); import game as _game
GAME = _game.load()   # 当前游戏档案（DM_GAME，必给）：来源组规则与表头说明 | the current game profile (DM_GAME, required): source group rules and the table header note
GROUPS, NOTE = GAME.pairs('shader_groups'), GAME.get('shader_note')
GLSLC = GAME.get('shader_main_glslc', 'off') == 'on'   # main 里的 GLSLC 编译输出（glslc_shaders） | GLSLC compiler outputs in main (glslc_shaders)
SEPARABLE = GAME.get('shader_separable', 'off') == 'on'   # 单阶段程序、绘制时按阶段拼（separable_pairs） | single-stage programs combined per stage at draw time (separable_pairs)

CTL_MAGIC, GFX_MAGIC, CS_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}
ZH = {'vs': '顶点', 'hs': '外壳', 'ds': '域', 'gs': '几何', 'ps': '像素', 'cs': '计算'}

def red(msg): sys.exit('【红】' + msg)

def ctl_len(ctl):
    """控制段参与键的字节数：控制段 +0x20 是它自己的长度。马力欧、火纹 Engage、生存恐惧、十三机兵的控制段都不短于 0x880，取前 0x880；
    SDK 10 的超级马力欧 3D 世界 + 狂怒世界有 2,888 个只有 0x850 字节（ShaderData 的 bfsha、6 个布局），后面紧跟别的数据，只取它自己的长度（与宿主 keys_hash 一致）
    Number of control bytes in the key: control +0x20 holds the section's own length. Every control section of Mario, Fire Emblem Engage, Metroid Dread and 13 Sentinels is at least 0x880 bytes, so the first 0x880 are used;
    SDK 10's Super Mario 3D World + Bowser's Fury has 2,888 that are only 0x850 bytes (the bfsha in ShaderData, 6 layouts), with other data right after them, so only their own length is used (the same as the host's keys_hash)"""
    n = struct.unpack_from('<I', ctl, 0x20)[0]
    return n if 0x760 <= n < 0x880 else 0x880

def skey(ctl, code):
    return hashlib.sha256(ctl[:ctl_len(ctl)] + code).hexdigest()[:16]

def check(where, st, ctl, code):
    """控制段与代码区自洽；不自洽返回原因 | control section and code area consistent; returns the reason if not"""
    if ctl[:4] != CTL_MAGIC or len(ctl) < ctl_len(ctl): return '控制段异常'
    # 计算代码区可以没有头（SDK 10 的超级马力欧 3D 世界 + 狂怒世界与 D2R：指令从 +0 起，dmsh_build 只在有 0x12345679 头时去头） | a compute code area may have no header (SDK 10's Super Mario 3D World + Bowser's Fury and D2R: instructions start at +0; dmsh_build strips only a 0x12345679 header)
    if st == 'cs' and code[:4] in (CS_MAGIC, GFX_MAGIC) and code[:4] != CS_MAGIC or st != 'cs' and code[:4] != GFX_MAGIC: return '代码区头魔数与阶段不符'
    if struct.unpack_from('<I', ctl, 0x704)[0] != len(code): return '控制段 +0x704 与代码区长不符'
    return None

def bnsh_shaders(d, base, label):
    if d[base + 0x60:base + 0x64] != b'grsc': red(f'{label} BNSH @{base:#x} 没有 grsc 块')
    nvar, pvar = struct.unpack_from('<IQ', d, base + 0x7c)
    out = []
    for v in range(nvar):
        prog = struct.unpack_from('<Q', d, base + pvar + 0x40 * v + 0x10)[0]
        if not prog: continue
        for si, st in enumerate(STAGES):
            sc = struct.unpack_from('<Q', d, base + prog + 8 + 8 * si)[0]
            if not sc: continue
            pctl, pcode, ncode, nctl = struct.unpack_from('<QQII', d, base + sc + 8)
            ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
            why = check(label, st, ctl, code)
            if why: red(f'{label} BNSH @{base:#x} 变体 {v} {st}：{why}')
            out.append((f'{label}#BNSH@{base:#x}#var{v}', st, ctl, code))
    return out

def bahs_shaders(d, label):
    region, rsz = struct.unpack_from('<I', d, 0x1c)[0], struct.unpack_from('<I', d, 0x18)[0]
    if region + rsz > len(d): red(f'{label} BAHS 代码区越界')
    out = []
    for a in range(0, region - 16, 4):
        off, z, csz, rel = struct.unpack_from('<IIIi', d, a)
        if z or not (csz in (0x880, 0x88c) or 0x760 <= csz < 0x880) or off % 0x100: continue   # 控制段长：马力欧 0x880 / 0x88c，3D 世界的 BAHS 第 9 版 0x848 | control length: Mario 0x880 / 0x88c, 3D World's BAHS version 9 0x848
        c = a + rel
        if c < 0 or c + csz > region or d[c:c + 4] != CTL_MAGIC: continue
        n = struct.unpack_from('<I', d, c + 0x704)[0]
        if off + n > rsz: continue
        code, ctl = d[region + off:region + off + n], d[c:c + csz]
        st = 'cs' if code[:4] == CS_MAGIC else SPH_STAGE.get((struct.unpack_from('<I', code, 0x30)[0] >> 10) & 0xf) if code[:4] == GFX_MAGIC else None
        if not st or check(label, st, ctl, code): continue
        out.append((f'{label}#BAHS@{a:#x}', st, ctl, code))
    return out

def mshd_shaders(d, label):
    out, c = [], d.find(CTL_MAGIC)
    while c >= 0:
        nctl = struct.unpack_from('<I', d, c + 0x20)[0]
        g = d.find(GFX_MAGIC, c + nctl)
        if nctl < 0x880 or g < 0 or g - c - nctl > 0x400: red(f'{label} MSHD 控制段 @{c:#x}：长 {nctl:#x}，之后找不到代码区头')
        ncode = struct.unpack_from('<I', d, c + 0x704)[0]
        ctl, code = d[c:c + nctl], d[g:g + ncode]
        st = SPH_STAGE.get((struct.unpack_from('<I', code, 0x30)[0] >> 10) & 0xf)
        if st != ('vs', 'ps')[len(out) % 2]: red(f'{label} MSHD @{c:#x}：第 {len(out)} 个阶段是 {st}，不是顶点 / 像素交替')
        why = check(label, st, ctl, code)
        if why: red(f'{label} MSHD @{c:#x} {st}：{why}')
        out.append((f'{label}#MSHD#var{len(out) // 2}', st, ctl, code))
        c = d.find(CTL_MAGIC, g + ncode)
    if len(out) % 2: red(f'{label} MSHD：{len(out)} 个阶段，不成对')
    return out

def glslc_shaders(d, label):
    """GLSLC 编译输出（十三机兵防卫圈 main 只读段里 838 个，各一个阶段）：魔数 0x19866891 起，+0x44 = 整份长；份内恰有一个控制段，代码区紧跟其后（控制段 +0x20 = 控制段长）。
    魔数是 4 字节，别的游戏的 main 里也有（火焰纹章 Engage 有 11 份），所以只在档案 shader_main_glslc 开时读，且只收三重核对都过的
    GLSLC compiler outputs (838 in 13 Sentinels' main read-only segment, one stage each): from the magic 0x19866891, +0x44 = the whole output's length; each output holds exactly one control section with the code area right after it (control +0x20 = control length).
    The magic is only 4 bytes and other games' mains have it too (Fire Emblem Engage has 11), so this runs only when the profile's shader_main_glslc is on, and keeps only outputs that pass the triple check"""
    out = []
    for m in re.finditer(b'\x91\x68\x86\x19', d):
        h = m.start(); size = struct.unpack_from('<I', d, h + 0x44)[0]
        c = d.find(CTL_MAGIC, h, h + size) if 0x880 < size <= len(d) - h else -1
        if c < 0 or d.find(CTL_MAGIC, c + 4, h + size) >= 0: continue
        nctl, ncode = struct.unpack_from('<I', d, c + 0x20)[0], struct.unpack_from('<I', d, c + 0x704)[0]
        if nctl < 0x880 or c + nctl + ncode > h + size: continue
        ctl, code = d[c:c + nctl], d[c + nctl:c + nctl + ncode]
        st = 'cs' if code[:4] == CS_MAGIC else SPH_STAGE.get((struct.unpack_from('<I', code, 0x30)[0] >> 10) & 0xf) if code[:4] == GFX_MAGIC else None
        if not st or check(label, st, ctl, code): continue
        out.append((f'{label}#GLSLC@{h:#x}', st, ctl, code))
    if not out: red(f'{label}：档案开了 shader_main_glslc，却没有一份 GLSLC 输出')
    return out

def yaz0(s):
    """Yaz0 解压（超级马力欧 3D 世界 + 狂怒世界：RomFS 几乎全是 .szs = Yaz0 包着的 SARC）。纯 Python 每核约 20–45 MB/s，全 RomFS 4.7 GB 并发下不到一分钟，不另装依赖。
    组头字节逐位：1 = 原样 1 字节；0 = 回拷，2 字节 {长度高 4 位, 距离 12 位}（距离 +1），长度 0 时再读 1 字节 + 0x12，否则 +2。重叠回拷按周期展开
    Yaz0 decompression (Super Mario 3D World + Bowser's Fury: almost all of RomFS is .szs = SARC wrapped in Yaz0). Pure Python runs at about 20–45 MB/s per core, under a minute for the whole 4.7 GB RomFS in parallel, and needs no extra dependency.
    Group header bits: 1 = one literal byte; 0 = back-reference, 2 bytes {length high nibble, 12-bit distance} (distance + 1); length 0 reads one more byte + 0x12, else + 2. Overlapping copies are expanded by their period"""
    n = int.from_bytes(s[4:8], 'big'); out = bytearray(); si = 16
    while len(out) < n:
        h = s[si]; si += 1
        for _ in range(8):
            if h & 0x80:
                out.append(s[si]); si += 1
            else:
                b1, b2 = s[si], s[si + 1]; si += 2
                dist, ln = ((b1 & 15) << 8 | b2) + 1, b1 >> 4
                if ln: ln += 2
                else: ln = s[si] + 0x12; si += 1
                st = len(out) - dist
                out += out[st:st + ln] if dist >= ln else (out[st:] * (ln // dist + 1))[:ln]
            if len(out) >= n: break
            h <<= 1
    return bytes(out[:n])

def sarc_members(d):
    hl, bom, size, doff = struct.unpack_from('<HHII', d, 4)
    n = struct.unpack_from('<H', d, hl + 6)[0]
    names = hl + 12 + 16 * n + 8
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', d, hl + 12 + 16 * i)
        nm = d[names + (attr & 0xffffff) * 4:].split(b'\0')[0].decode('utf-8', 'replace') if attr >> 24 else f'#{i}'
        yield nm, d[doff + s:doff + e]

NV_STAGES = ['vs', 'ps', 'gs', 'hs', 'ds', 'cs']   # NVN 阶段序 | NVN stage order
COMMON, TREES = None, {}   # Unity 公共字符串表、借用的类型树（进程池初始化时给） | Unity common strings and borrowed type trees (given when the pool starts)

def _pool_init(common, trees):
    global COMMON, TREES
    COMMON, TREES = common, trees

def unity_program(code, where, stage=None):
    """Unity 的 Switch 程序代码 → [(阶段, 控制段, 代码区)] | Unity's Switch program code → [(stage, control, code)]"""
    # 2019.3 之前（Cuphead，Unity 2017.4）：0x10 头 {?, ?, 控制段长, 代码区长} + 控制段 + 代码区；
    # 一个子程序只含一个阶段，阶段不在代码里，取自 Shader 的 m_ParsedForm（见 unity_stages）
    # before 2019.3 (Cuphead, Unity 2017.4): a 0x10 header {?, ?, control size, code size} + control section + code area;
    # a subprogram carries a single stage, which is not in the code but taken from the Shader's m_ParsedForm (see unity_stages)
    if len(code) >= 0x14 and code[0x10:0x14] == CTL_MAGIC:
        nctl, ncode = struct.unpack_from('<2i', code, 8); a = 0x10; st = stage or 'vs'
        end = a + nctl + ncode
        ctl, cd = code[a:a + nctl], code[a + nctl:end]
        why = check(where, st, ctl, cd) if end <= len(code) else '块越界'
        if why: red(f'{where} {st}：{why}')
        if end != len(code): red(f'{where}：程序代码 {len(code):#x} 字节，控制段 + 代码区只到 {end:#x}')
        return [(st, ctl, cd)]
    h = struct.unpack_from('<24i', code, 0); out = []; end = 0x60
    for i, st in enumerate(NV_STAGES):
        off, nctl, ncode = h[6 + i], h[12 + i], h[18 + i]
        if off == -1: continue
        a = 0x60 + off; ctl, cd = code[a:a + nctl], code[a + nctl:a + nctl + ncode]
        why = check(where, st, ctl, cd) if a + nctl + ncode <= len(code) else '块越界'
        if why: red(f'{where} {st}：{why}')
        out.append((st, ctl, cd)); end = max(end, a + nctl + ncode)
    if end != len(code): red(f'{where}：程序代码 {len(code):#x} 字节，各阶段块只到 {end:#x}')
    return out

def unity_stages(o):
    """Shader 的 m_ParsedForm → {子程序序号: 阶段}；旧布局的子程序阶段只能从这里取（新布局在程序代码头里）
    Shader's m_ParsedForm → {blob index: stage}; only the older layout needs it (the newer one carries the stages in the program code header)"""
    m = {}
    for sub in (o.get('m_ParsedForm') or {}).get('m_SubShaders', []):
        for p in sub.get('m_Passes', []):
            for field, st in (('progVertex', 'vs'), ('progFragment', 'ps'), ('progGeometry', 'gs'), ('progHull', 'hs'), ('progDomain', 'ds')):
                for s in (p.get(field) or {}).get('m_SubPrograms', []):
                    m.setdefault(s.get('m_BlobIndex'), st)
    return m

def unity_shaders(d, label, out, trees):
    """UnityFS 包或序列化文件里的 Shader → out；没有类型树可用时抛 NoTree | Shaders in a UnityFS bundle or serialized file → out; raises NoTree when no type tree is available"""
    import unityfs
    if COMMON is None: red(f'{label}：是 Unity 资源，但 main 里没有 Unity 公共字符串表')
    sers = unityfs.bundle_files(d) if d[:8] == b'UnityFS\0' else [('', d)]
    for node, x in sers:
        if not unityfs.is_serialized(x): continue
        for cid, pid, o in unityfs.serialized_objects(x, COMMON, {unityfs.CLASS_SHADER}, trees):
            name = o['m_ParsedForm']['m_Name'] if 'm_ParsedForm' in o else o.get('m_Name', '?')
            sm = unity_stages(o)
            for plat, k, sp in unityfs.shader_programs(o):
                where = f'{label}{":" + node if node else ""}#{name}#var{plat}.{k}'
                code = unityfs.subprogram(sp)[4]
                out.extend((where, st, ctl, cd) for st, ctl, cd in unity_program(code, where, sm.get(k)))

def scan_blob(d, label, out):
    if d[:4] == b'Yaz0': d = yaz0(d)
    if d[:4] == b'SARC':
        for nm, m in sarc_members(d): scan_blob(m, f'{label}/{nm}', out)
        return
    if d[:4] == b'BAHS': out.extend(bahs_shaders(d, label)); return
    if d[:4] == b'MSHD': out.extend(mshd_shaders(d, label)); return
    for m in re.finditer(b'BNSH\0\0\0\0', d):
        size = struct.unpack_from('<I', d, m.start() + 0x1c)[0]
        if m.start() + size <= len(d) and d[m.start() + 0x60:m.start() + 0x64] == b'grsc': out.extend(bnsh_shaders(d, m.start(), label))

def group(label):
    """来源组（包文件名的后半） | source group (second half of the package file name)"""
    if label.startswith('main'): return 'main'
    for k, g in GROUPS:   # 游戏档案 shader_groups，按顺序取第一个 | the profile's shader_groups, first match wins
        if k in label: return g
    return 'other'

def ps_inputs(code):
    """像素着色器读的通用属性：SPH 第 6–13 字是 32 个属性各 8 位的输入图（Ryujinx ShaderHeader.ImapTypes），非 0 = 读；翻译出的 MSL 输入就是 user(locn<属性号>)
    Generic attributes a pixel shader reads: SPH words 6–13 are the input map, 8 bits for each of the 32 attributes (Ryujinx ShaderHeader.ImapTypes), non-zero = read; the translated MSL inputs are exactly user(locn<attribute>)"""
    return {i for i in range(32) if struct.unpack_from('<I', code, 0x30 + 4 * (6 + (i >> 2)))[0] >> ((i & 3) * 8) & 0xff}

def separable_pairs(keys):
    """单阶段程序（十三机兵防卫圈：每份 GLSLC 输出建一个只有一个阶段的程序，绘制时 BindProgram 按阶段位把顶点、像素程序拼起来），真配对只在绘制时才有。
    vs_pad 只用配对像素着色器读的位置的并集：所以每个顶点着色器配上一组「覆盖」——读到的位置合起来等于全部像素着色器读的位置的最少几个像素着色器（贪心，同样多时取键小的），
    补完的顶点着色器能接任何一个像素着色器。全配对要 376 × 375 行，覆盖只要几行 ×376
    Single-stage programs (13 Sentinels: each GLSLC output becomes a program with one stage, and BindProgram's stage bits combine vertex and pixel programs per draw) have their real pairs only at draw time.
    vs_pad uses only the union of the locations the paired pixel shaders read, so every vertex shader is paired with a "cover": the fewest pixel shaders whose read locations together equal those of all pixel shaders (greedy, ties go to the smaller key);
    padded that way, a vertex shader links with any pixel shader. All pairs would be 376 × 375 rows; the cover needs a few rows × 376"""
    ins = {k: ps_inputs(v[3]) for k, v in keys.items() if v[1] == 'ps'}
    left, cover = set().union(*ins.values()) if ins else set(), []
    while left:
        k = min(ins, key=lambda k: (-len(ins[k] & left), k)); cover.append(k); left -= ins[k]
    return {(v, p) for v in keys if keys[v][1] == 'vs' for p in cover}, cover

def var_pairs(found):
    """BNSH 同一变体里的（顶点键, 像素键） | (vertex key, pixel key) within the same BNSH variation"""
    var = collections.defaultdict(dict)
    for where, st, ctl, code in found:
        if '#var' in where: var[where][st] = skey(ctl, code)
    return {(v['vs'], v['ps']) for v in var.values() if 'vs' in v and 'ps' in v}

def scan_file(args):
    """→ (相对路径, {键: …}, 配对, 额外)；额外 = 本文件的 Unity 类型树，或 'defer'（缺类型树，第二遍再扫） | → (relative path, {key: …}, pairs, extra); extra = this file's Unity type trees, or 'defer' (no type tree, scanned again in the second pass)"""
    path, rel = args
    d = subprocess.run(['zstd', '-d', '-c', '-q', path], capture_output=True, check=True).stdout if path.endswith('.zs') else open(path, 'rb').read()
    if d[:4] == b'Yaz0': d = yaz0(d)
    found = []; trees = {}
    if d[:8] == b'UnityFS\0' or (d[8:11] == b'\0\0\0' and __import__('unityfs').is_serialized(d)):
        import unityfs
        try: unity_shaders(d, '/' + rel, found, trees if not TREES else dict(TREES))
        except unityfs.NoTree: return rel, {}, set(), 'defer'
    elif b'BNSH\0\0\0\0' not in d and b'BAHS' not in d and d[:4] != b'MSHD' and not (d[:4] == b'SARC' and b'Yaz0' in d): return rel, {}, set(), {}   # SARC 里可能还有 Yaz0 成员 | a SARC may hold Yaz0 members
    else: scan_blob(d, '/' + rel, found)
    uniq = {}
    for where, st, ctl, code in found:
        key = skey(ctl, code)
        if key in uniq and (uniq[key][2] != ctl or uniq[key][3] != code): red(f'键 {key} 相同而控制段或代码区不同（{where}）')
        uniq.setdefault(key, (where, st, ctl, code))
    return rel, uniq, var_pairs(found), trees

def nso_image(path):
    import lz4.block
    d = open(path, 'rb').read()
    flags = struct.unpack_from('<I', d, 0xc)[0]; segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return bytes(img)

def main():
    romfs_l, main_l, pkg, mapout, factsout = sys.argv[1:6]
    pairsout = sys.argv[6] if len(sys.argv) > 6 else None
    # 多个版本（基础版 + 更新）：RomFS 与 main 各用逗号分隔、一一对应，输出取并集（键是内容哈希，各版共用的只出一份）。
    # 第一个版本的来源列不变；之后的版本以其工作区目录名（如 v327680）为前缀：/v327680/Shader/…、main@v327680，排在第一个版本之后，原有行的包内序号不变。
    # Several versions (base game + update): the RomFS and main arguments are comma-separated and paired one to one; the output is their union (keys are content hashes, shaders shared by versions appear once).
    # The first version's source column is unchanged; later versions are prefixed with their workspace directory name (e.g. v327680): /v327680/Shader/…, main@v327680. They sort after the first version, so existing rows keep their package indices.
    romfs_v, main_v = romfs_l.split(','), main_l.split(',')
    if len(romfs_v) != len(main_v): red(f'RomFS {len(romfs_v)} 个、main {len(main_v)} 个，要一一对应')
    files, results, common = [], [], None
    for i, (romfs, mainnso) in enumerate(zip(romfs_v, main_v)):
        tag = os.path.basename(os.path.dirname(os.path.abspath(romfs))) if i else ''
        files += sorted((os.path.join(dp, f), (tag + '/' if tag else '') + os.path.relpath(os.path.join(dp, f), romfs)) for dp, _, fs in os.walk(romfs) for f in fs)
        img = nso_image(mainnso)
        if not i:
            import unityfs
            try: common = unityfs.common_strings(img)
            except ValueError: common = None   # 不是 Unity 游戏 | not a Unity game
        found = []; scan_blob(img, 'main' + ('@' + tag if tag else ''), found)
        if GLSLC: found += glslc_shaders(img, 'main' + ('@' + tag if tag else ''))
        results.append(('main', {skey(c, k): (w, s, c, k) for w, s, c, k in found}, var_pairs(found), {}))
    keys = {}
    # 第一遍：全部文件；Unity 资源包带类型树，顺手收下 Shader 的树。第二遍：没带类型树的 Unity 序列化文件借这些树再扫
    # First pass: every file; Unity bundles carry type trees, and the Shader trees are collected on the way. Second pass: Unity serialized files without type trees are scanned again with the borrowed trees
    J = int(os.environ.get('J', '8'))
    with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(common, {})) as ex: results += list(ex.map(scan_file, files, chunksize=16))
    trees, defer = {}, []
    for rel, uniq, pr, extra in results:
        if extra == 'defer': defer.append(rel)
        elif extra: trees.update(extra)
    if defer:
        byrel = {r: p for p, r in files}
        with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(common, trees)) as ex: again = list(ex.map(scan_file, [(byrel[r], r) for r in defer]))
        for rel, uniq, pr, extra in again:
            if extra == 'defer': red(f'{rel}：Unity 序列化文件没有类型树，资源包里也没有同哈希的 Shader 类型树可借')
        results = [r for r in results if r[3] != 'defer'] + again
    per_src = collections.Counter(); pairs = set()
    for rel, uniq, pr, _ in results:
        pairs |= pr
        for key, v in uniq.items():
            if key in keys:
                if keys[key][2] != v[2] or keys[key][3] != v[3]: red(f'键 {key} 相同而控制段或代码区不同（{v[0]} / {keys[key][0]}）')
                continue
            keys[key] = v; per_src[group(v[0])] += 1
    if SEPARABLE:
        sp, cover = separable_pairs(keys); pairs |= sp
        print(f'单阶段程序：每个顶点着色器配 {len(cover)} 个像素着色器作覆盖（{" ".join(cover)}） | single-stage programs: every vertex shader is paired with a cover of {len(cover)} pixel shader(s)')
    os.makedirs(pkg, exist_ok=True)
    for f in os.listdir(pkg):
        if f.endswith('.bin'): os.unlink(os.path.join(pkg, f))
    recs = collections.defaultdict(list); rows = []; facts = {}; stat = collections.Counter()
    for key in sorted(keys, key=lambda k: (group(keys[k][0]), keys[k][0], k)):   # 同输入逐字节同样的输出 | the same input gives byte-identical output
        where, st, ctl, code = keys[key]
        name = f'{st}_{group(where)}.bin'
        rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); rec += code + ctl
        if rec.find(CTL_MAGIC) != 0x38 + len(code): red(f'键 {key}：代码区里出现控制段魔数，dmsh_build 会切错')
        if rec.count(b'BTCDBTCD') != 1: red(f'键 {key}：记录内出现记录分隔符')
        rows.append((name, len(recs[name]), ZH[st], key, where)); recs[name].append(bytes(rec)); stat[st] += 1
        if st == 'cs':
            x, y, z = struct.unpack_from('<3I', ctl, 0x748); sh = struct.unpack_from('<I', ctl, 0x754)[0]
            if not x * y * z: red(f'计算着色器 {key} 控制段 +0x748 线程组为 0（{where}）')
            facts[key] = {'local': [x, y, z], 'shared': (sh + 15) // 16 * 16}
    for name, rr in recs.items():
        with open(os.path.join(pkg, name), 'wb') as f: f.write(b''.join(rr))
    with open(mapout, 'w') as f:
        kinds = ' / '.join(k for k, hit in (('BNSH / BAHS', any('#BNSH@' in r[4] or '#BAHS@' in r[4] for r in rows)), ('Unity Shader', any('#var' in r[4] and '#BNSH@' not in r[4] and '#MSHD#' not in r[4] for r in rows)),
                                            ('MSHD', any('#MSHD#' in r[4] for r in rows)), ('GLSLC', any('#GLSLC@' in r[4] for r in rows))) if hit)
        f.write(f'# kind: evidence  {NOTE}（RomFS + main 里的 {kinds}）→ 控制段 + 代码区键（dm/gfx/tools/shader_split.py）\n')
        f.write('# 第 5 列记每个键首次出现的位置；本作没有 PC 版配对\n')
        f.write('package\tindex\tstage\tdm_ctl_sha16\tsource\n')
        for r in rows: f.write('\t'.join(map(str, r)) + '\n')
    with open(factsout, 'w') as f:
        f.write('# 翻译查询的作答表（dm/gfx/tools/shader_split.py 生成，勿手改）：键 <TAB> 事实 JSON。本作没有 PC 版可配对：只有计算着色器的线程组 / 共享内存（取自控制段），其余查询按翻译器缺省\n')
        for k in sorted(facts): f.write(k + '\t' + json.dumps(facts[k], sort_keys=True, separators=(',', ':')) + '\n')
    if pairsout:
        with open(pairsout, 'w') as f:
            f.write('# kind: evidence  BNSH 变体里的顶点 / 像素着色器配对（dm/gfx/tools/shader_split.py 生成，勿手改）：顶点键 <TAB> 像素键 | vertex / pixel shader pairs within BNSH variations (generated, do not edit): vertex key <TAB> pixel key\n')
            for a, b in sorted(pairs): f.write(f'{a}\t{b}\n')
    print(f'着色器 {len(rows)} 个（' + '，'.join(f'{ZH[s]} {n}' for s, n in sorted(stat.items())) + '）；来源 ' + '，'.join(f'{g} {n}' for g, n in per_src.most_common()) + f'；计算着色器作答 {len(facts)} 行' + (f'；变体配对 {len(pairs)} 对' if pairsout else ''))

if __name__ == '__main__': main()
