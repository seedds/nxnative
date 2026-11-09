#!/usr/bin/env python3
# Test saves for The Legend of Zelda: Ocarina of Time (Switch 2, 0400486024DEC000, v131072): copies of a save whose slots all load Link at a chosen place, so a scene can be
# measured or checked (scripts/bench.py --save, a headless run, the Mac app launched with DM_SAVE) without playing there first.
# Where a save loads: on Continue / Load the game's load sequence (Sequence/TitleToMainWithLoadSave and Sequence/Retry) reads the stage from Sequence_CurrentBanc, the position from
# PlayerStatus.SavePos and the facing from PlayerStatus.SavePosRadY (radians, 0 faces +Z), changes to that stage with the position as an override, and Sequence/SceneBeginning
# searches a safe spot near it. There is no fixed restart point as on the N64: a save loads wherever it was made, dungeons included. Age is Sequence_IsChildhood; child and adult
# overworld stages force their own on arrival (GameBancParam IsForceSwitchChildOrAdult / IsChildWhenChangeBanc), dungeons keep it.
# The files: slot_NN/progress.sav and caption.sav are a header (01020304 002D80EB, the header's end) and {u32 hash, u32 value or data offset} pairs, hash = MurmurHash3 x86_32 of
# the field name (enum values too), then the data area that strings, vectors and arrays point into. No checksum, encryption or compression: fields are rewritten in place.
# Every slot of the copy gets the same place, so Continue and every Load entry go there. Items, quest flags and dungeon state are the base slot's, unless the options change them;
# a child save loaded as an adult gets the adult kit (Master Sword and Hylian Shield owned and equipped, the child's gear kept for the other age).
# Credits: the save-format work builds on Traiver's OOT-SW2SE save editor (MIT, https://github.com/Traiver/OOT-SW2SE): its parser and field catalog showed the file layout and many field names, and its
# catalog's hashes confirmed the hash function (MurmurHash3).
# Usage: python3 scripts/ocarina_test_save.py <id>_build folder [options]   (the folder the NSP script made: the extracted RomFS is read for the game's tables)
#   --list (the presets) / --stages [filter] (stages and their named start positions) / --show <save folder> (where each slot of a save loads)
#   --place <name> | --all (the checkpoints) | --name <name> --stage <stage> (--start <start position> | --pos X,Y,Z [--facing degrees, 0 = +Z, 90 = +X])
#   --base <save folder>: the save to copy, only read (default <id>_build/run/save); --out <folder>: where <name>/save goes (default <id>_build/test_saves)
#   --age child|adult, --slot slot_NN (the base slot to copy; default the newest that fits), --warp-songs (the Ocarina of Time and the six warp songs), --item <actor>,
#   --wear <armor actor>, --set FIELD=VALUE (an inline progress field by name or 0xHASH: Bool, Int, UInt, Float, Enum by value name), --no-adult-kit, --force (replace a test save
#   this script made: it carries TEST-SAVE.json)
# The game's tables (stages, start positions, save field types) are read from the RomFS once into <out>/game_tables.json, again when the image changes.
import argparse, datetime, json, math, re, shutil, struct, sys
from pathlib import Path
from typing import Any, NoReturn

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs as rfs

TITLE = '0400486024DEC000'
MARKER = 'TEST-SAVE.json'
MAGIC, VERSION, FIRST_PAIR = 0x01020304, 0x002D80EB, 40
HEADER_END = {'progress': 0x175D8, 'caption': 0x2A0}   # the header ends of this version's progress.sav and caption.sav

# The fields written. Quoted names are the game's own (their hashes match); the others are unnamed in the game's files and were matched by comparing autosaves made in two stages.
P_STAGE = 0x1D6189DA         # String64 "Sequence_CurrentBanc": the stage a load goes to
P_POS = 0xC884818D           # Vector3 "PlayerStatus.SavePos" (x, y up, z)
P_FACING = 0x1EA93BD8        # Float "PlayerStatus.SavePosRadY": facing in radians
P_LAST_STAGE = 0x4A6BDB7D    # String32: the stage of P_LAST_POS (equal to P_STAGE in every autosave seen)
P_LAST_POS = 0x8773F02D      # Vector3: a recent position within 2 m of SavePos
P_MAP_POS = 0x960AD56A       # Vector3 "Player_Pos_For_Map": the map's player marker
P_MAP_ROT = 0xA4AEE8CD       # Float "Player_Rot_For_Map": the marker's rotation in degrees, facing + 180
P_LOCATIONS = 0xC458F1EB     # String64Array (4): the first entry is the stage's location name
P_IS_CHILD = 0xCEA220D3      # Bool "Sequence_IsChildhood"
P_QUEST = 0x874C7ED4         # String64: the main quest step ("Goto_HyruleCastle")
P_EQUIP_WEAPON = 0xA95A3521  # String64 "PlayerStatus.EquipWeapon"
P_EQUIP_SHIELD = 0xEE463235  # String64 "PlayerStatus.EquipShield"
P_OTHER_WEAPON = 0xA1F7EE2B  # String64 "PlayerStatus.AnotherEraEquipWeapon": the other age's sword
P_OTHER_SHIELD = 0x2342B619  # String64 "PlayerStatus.AnotherEraEquipShield"
P_EQUIP_ARMOR = 0x008D7F0C   # String64 "PlayerStatus.EquipArmor": the worn tunic, empty = the default one
C_POS = 0x8B26618E           # caption.sav Vector3: SavePos for the Load list
C_LOCATION = 0xF74E0E8E      # caption.sav String64: the location name
C_STAGE_PATH = 0x0CCCC6CD    # caption.sav String64: "Folder/Stage"
C_AUTOSAVE = 0x25F03CAA      # caption.sav Bool: 1 autosave, 0 manual save
C_TIME = (0x9811A3F7, 0xDFD840D3, 0xBD46F485, 0x23F3D75E, 0x27853BF7)   # caption.sav Int: year, month, day, hour, minute

CHILD_WEAPONS, CHILD_SHIELDS = ('Weapon_Sword_Kokiri',), ('Weapon_Shield_Deku',)
ADULT_ITEMS = ('Weapon_Sword_Master', 'Weapon_Shield_Hylian')
ADULT_FLAGS = ('Market_Child_FoundMasterSword', 'Market_Child_Firsttime_GotoAdult')   # the first sword pull: the Temple of Time does not replay it
WARP_ITEMS = ('Item_Ocarina_Time',)
WARP_SONGS = ('ForestMinuet', 'FireBolero', 'WaterSerenade', 'ShadowNocturne', 'SpiritRequiem', 'LightPrelude')
# The songs' lesson scenes, marked as played so Sheik does not stop Link at the pads: the six cutscenes (Spilit and Shine are the game's names for Requiem and Prelude) and the stage
# triggers that request them (EventRequestActor in Sacred Forest Meadow and Death Mountain Crater, the Ice Cavern chest)
WARP_EVENTS = ('Dm_MelodyGet_Forest', 'Dm_MelodyGet_Fire', 'Dm_MelodyGet_Water', 'Dm_MelodyGet_Shadow', 'Dm_MelodyGet_Spilit', 'Dm_MelodyGet_Shine',
               'ForestTemple_SageOfForest_MelodyGet', 'SageOfFire_MelodyGetFire', 'SageOfWater_MelodyGet_Water')

# Presets: `start` names a start position of the stage (Banc/BancBasedResource/StartPos.byml.zs), `pos` a point from the stage's actor placements; `face` is a point (x, z) Link
# turns toward; `require` picks the newest base slot with those field values; `warp` adds the warp kit. All but dmt-adult and temple-of-time were checked headless with a screenshot on an M2 Pro (2026-11-09) from
# a child save after the Deku Tree.
CHECKPOINTS = {
    'hyrule-field': dict(stage='HyruleField_Child', start='HyruleField_FrontOfMarket', desc='Hyrule Field, on the Castle Town drawbridge, field ahead'),
    'hyrule-field-adult': dict(stage='HyruleField_Adult', start='HyruleField_FrontOfMarket', warp=True, desc='Hyrule Field, the same spot as an adult'),
    'dmt-goron': dict(stage='DeathMountainTrail_Child', pos=(-38.24, 109.43, -13.13), face=(-36.25, -13.30),
                      desc="Death Mountain Trail, 2 m in front of the Goron by Dodongo's Cavern, facing it (it sits at its AnchorAction1); A talks to it"),
    'warp-fire': dict(stage='DeathMountainCrater_Adult', start='FireBolero', warp=True, wear='Armor_Adult_Goron',
                      desc='Death Mountain Crater, the Bolero of Fire pad by the Fire Temple, in the Goron Tunic (without it the heat ends a run in under a minute)'),
    'zora-domain': dict(stage='ZoraDomain_Child', start='ZoraDomain_FromZoraRiver', desc="Zora's Domain, the entrance from Zora's River"),
    'zora-domain-adult': dict(stage='ZoraDomain_Adult', start='ZoraDomain_FromZoraRiver', warp=True, desc="Zora's Domain, the same spot, frozen"),
    'warp-forest': dict(stage='SacredForestMeadow_Adult', start='ForestMinuet', warp=True, desc='Sacred Forest Meadow, the Minuet of Forest pad by the Forest Temple'),
    'warp-water': dict(stage='HyruleField_Adult', start='WaterSerenade', warp=True, desc='Lake Hylia, the Serenade of Water pad by the Water Temple'),
    'warp-shadow': dict(stage='Graveyard_Adult', start='ShadowNocturne', warp=True, desc='Graveyard, the Nocturne of Shadow pad by the Shadow Temple'),
    'warp-spirit': dict(stage='DesertColossus_Adult', start='SpiritRequiem', warp=True, desc='Desert Colossus, the Requiem of Spirit pad by the Spirit Temple'),
}
EXTRAS = {
    'kokiri-house': dict(stage='KokiriForest_Child', pos=(-46.0869, 118.4389, 15.3939), facing=108.57, desc="Kokiri Forest, Link's porch (the stage's default start)"),
    'hyrule-castle-vines': dict(stage='HyruleCastle', pos=(27.9, 106.6, -21.3), face=(29.5, -21.75),
                                desc='Hyrule Castle, at the foot of the climbable vines, facing them (stick up climbs; the castle comes into view at the top)'),
    'kakariko': dict(stage='KakarikoVillage_Child', start='KakarikoVillage_Entrance', desc='Kakariko Village, at the gate from Hyrule Field'),
    'castle-town': dict(stage='Market_Child', start='GameOverRespawn', desc='Castle Town market square, facing the fountain'),
    'lost-woods-from-goron-city': dict(stage='GoronCity_Child', pos=(23.5, 104.94, -2.85), facing=100.0,
                                       desc='Goron City, in the tunnel to the Lost Woods past the breakable rock: about 5 s of stick up loads the Lost Woods'),
    'lost-woods': dict(stage='LostWood_Child', start='LostWood_GoronCitySide_after', desc='Lost Woods, where the Goron City tunnel comes out'),
    'dodongos-cavern': dict(stage='DodongoCavern', start='EntrancePos', facing=180.0, desc="Dodongo's Cavern, inside the entrance, facing in"),
    'gohma': dict(stage='InsideTheDekuTree', pos=(-2.3771, 46.9615, 67.6421), facing=77.64, require={'Gohma_DefeatBoss': 0},
                  desc="Gohma's room before the fight (needs a base slot saved there)"),
    'gohma-after': dict(stage='InsideTheDekuTree', start='StartPos_Get_StoneOfForest', require={'Gohma_DefeatBoss': 1}, desc="Gohma's room after the fight"),
    'dmt-adult': dict(stage='DeathMountainTrail_Adult', pos=(-38.24, 109.43, -13.13), face=(-36.25, -13.30), warp=True, desc='Death Mountain Trail as an adult, the dmt-goron spot'),
    'temple-of-time': dict(stage='Market_Adult', start='LightPrelude', warp=True, desc='Temple of Time, the Prelude of Light pad'),
}
PRESETS = {**CHECKPOINTS, **EXTRAS}


def fail(msg) -> NoReturn:
    print(f'error: {msg}', file=sys.stderr); sys.exit(1)


def murmur3(data, seed=0):
    """MurmurHash3 x86_32 of a str or bytes: the game's hash for field names and enum values."""
    if isinstance(data, str): data = data.encode()
    c1, c2, h, n = 0xcc9e2d51, 0x1b873593, seed, len(data)
    def mix(k): k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff; return (k * c2) & 0xffffffff
    for i in range(n // 4):
        h ^= mix(struct.unpack_from('<I', data, i * 4)[0])
        h = ((h << 13) | (h >> 19)) & 0xffffffff; h = (h * 5 + 0xe6546b64) & 0xffffffff
    tail = data[n // 4 * 4:]
    if tail: h ^= mix(int.from_bytes(tail, 'little'))
    h ^= n; h ^= h >> 16; h = (h * 0x85ebca6b) & 0xffffffff; h ^= h >> 13; h = (h * 0xc2b2ae35) & 0xffffffff
    return h ^ (h >> 16)


def wrap_deg(d):
    """An angle in degrees folded into (-180, 180]."""
    d = math.fmod(d, 360.0)
    return d - 360.0 if d > 180.0 else d + 360.0 if d <= -180.0 else d


# ---- the game's tables, from the RomFS ----

def zstd_mod():
    """The zstd module: compression.zstd from Python 3.14, its backport before (pip install backports.zstd), the same API (as dm/gfx/tools/shader_split.py)."""
    try: from compression import zstd
    except ImportError:
        try: from backports import zstd   # type: ignore[no-redef]
        except ImportError: fail(f'Python {sys.version_info[0]}.{sys.version_info[1]} has no compression.zstd (3.14+): pip install backports.zstd')
    return zstd


class RomFS:
    """The files of an extracted RomFS: a DMROMFS1 image (dm/tools/romfs.py) or a folder."""

    def __init__(self, path):
        self.path = Path(path)
        self.img = rfs.Image(self.path) if rfs.is_image(self.path) else None

    def read(self, rel):
        """Bytes of one file; raises FileNotFoundError when there is none."""
        if self.img is None: return (self.path / rel).read_bytes()
        a, b, _, _, _, _ = self.img.entry(self.img.find(rel))
        with open(self.img.path, 'rb') as f:
            f.seek(a); return f.read(b)

    def listdir(self, rel):
        """The names in one folder of the RomFS."""
        if self.img is None: return sorted(p.name for p in (self.path / rel).iterdir())
        return [n for n, _ in self.img.children(self.img.find(rel))]


class Byml:
    """A minimal BYML v7 reader, either byte order (the GameDataList is big-endian, the stage files little-endian)."""

    def __init__(self, data):
        if data[:2] not in (b'YB', b'BY'): raise ValueError('not a BYML file')
        self.b, self.e = data, '<' if data[:2] == b'YB' else '>'
        _, kt, st, self.root = struct.unpack_from(self.e + 'HIII', data, 2)
        self.keys, self.strs = (self._strings(kt) if kt else []), (self._strings(st) if st else [])

    def _u32(self, o): return struct.unpack_from(self.e + 'I', self.b, o)[0]

    def _head(self, o):   # (node type, element count)
        w = self._u32(o)
        return (self.b[o], w >> 8) if self.e == '<' else (w >> 24, w & 0xffffff)

    def _strings(self, o):
        _, n = self._head(o)
        offs = struct.unpack_from(self.e + f'{n + 1}I', self.b, o + 4)
        return [self.b[o + offs[i]:self.b.index(b'\0', o + offs[i])].decode() for i in range(n)]

    def _value(self, t, v) -> Any:
        e, b = self.e, self.b
        if t == 0xA0: return self.strs[v]
        if t == 0xD0: return bool(v)
        if t == 0xD1: return struct.unpack(e + 'i', struct.pack(e + 'I', v))[0]
        if t == 0xD2: return struct.unpack(e + 'f', struct.pack(e + 'I', v))[0]
        if t == 0xD3: return v
        if t in (0xD4, 0xD5, 0xD6): return struct.unpack_from(e + {0xD4: 'q', 0xD5: 'Q', 0xD6: 'd'}[t], b, v)[0]
        if t == 0xFF: return None
        if t in (0xC0, 0xC1, 0x20): return self.node(v)
        raise ValueError(f'unsupported BYML value type 0x{t:02x}')

    def node(self, o) -> Any:
        """The array (list) or dictionary (dict) at offset o (Any: the shape is the file's)."""
        t, n = self._head(o)
        if t == 0xC0:
            types, vo = self.b[o + 4:o + 4 + n], (o + 4 + n + 3) & ~3
            return [self._value(types[i], self._u32(vo + 4 * i)) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                ent = o + 4 + 8 * i; w = self._u32(ent)
                key, typ = (w & 0xffffff, self.b[ent + 3]) if self.e == '<' else (w >> 8, w & 0xff)
                out[self.keys[key]] = self._value(typ, self._u32(ent + 4))
            return out
        if t == 0x20: return {self._u32(o + 4 + 8 * i): self._value(self.b[o + 4 + 8 * n + i], self._u32(o + 8 + 8 * i)) for i in range(n)}
        raise ValueError(f'unsupported BYML node type 0x{t:02x}')

    def tree(self) -> Any: return self.node(self.root)


def sarc_files(data):
    """{name: bytes} of a little-endian SARC (Pack/ZsDic.pack.zs holds the zstd dictionaries)."""
    if data[:4] != b'SARC': raise ValueError('not a SARC archive')
    data_off, count = struct.unpack_from('<I', data, 0x0C)[0], struct.unpack_from('<H', data, 0x1A)[0]
    names = 0x14 + 12 + 16 * count + 8
    out = {}
    for i in range(count):
        _, attr, start, end = struct.unpack_from('<IIII', data, 0x14 + 12 + 16 * i)
        no = names + (attr & 0xffff) * 4
        out[data[no:data.index(b'\0', no)].decode()] = data[data_off + start:data_off + end]
    return out


def stage_age(param, name):
    """The age a stage forces on arrival: 'adult' (IsChildWhenChangeBanc false, or an _Adult stage), 'child' (IsForceSwitchChildOrAdult, or _Child), None (kept: dungeons)."""
    if param.get('IsChildWhenChangeBanc') is False or name.endswith('_Adult'): return 'adult'
    if param.get('IsForceSwitchChildOrAdult') or name.endswith('_Child'): return 'child'
    return None


def build_tables(romfs):
    """The tables make() needs, from the RomFS: stages (scene folder, location name, forced age), their start positions, and the save fields' types."""
    zstd = zstd_mod()
    dicts = sarc_files(zstd.decompress(romfs.read('Pack/ZsDic.pack.zs')))
    unzs = lambda rel: zstd.decompress(romfs.read(rel), zstd_dict=zstd.ZstdDict(dicts['zs.zsdic']))
    gdl_name = next((n for n in romfs.listdir('GameData') if re.fullmatch(r'GameDataList\.Product\.\d+\.byml\.zs', n)), None) or fail('no GameData/GameDataList in the RomFS')
    gdl = Byml(unzs('GameData/' + gdl_name)).tree()
    fields = {f'{e["Hash"]:08x}': [typ, e['SaveFileIndex']] for typ, entries in gdl['Data'].items() if typ != 'Bool64bitKey'
              for e in entries if e.get('SaveFileIndex') in (0, 2)}   # 0 progress.sav, 2 caption.sav
    raw = {n.split('.')[0]: Byml(romfs.read('Banc/GameBancParam/' + n)).tree() for n in romfs.listdir('Banc/GameBancParam') if n.endswith('.game__banc__GameBancParam.bgyml')}
    stem = lambda p: p.rsplit('/', 1)[-1].split('.', 1)[0] if p else None
    def resolved(name, depth=0):   # a stage's parameters with its $parent chain under them
        own, parent = raw[name], stem(raw[name].get('$parent'))
        return {**resolved(parent, depth + 1), **own} if parent in raw and depth < 8 else dict(own)
    stages = {}
    for name in raw:
        p = resolved(name); main = (p.get('Banc') or {}).get('Main', '')
        stages[name] = {'folder': main.split('/')[-2] if main.count('/') >= 2 else None, 'location': stem(p.get('BaseLocation')), 'age': stage_age(p, name)}
    start_pos = {stage: {n: {'pos': [round(x, 4) for x in v['Trans']], 'facing_deg': round(math.degrees(v['Rot'][1]), 3)} for n, v in entries.items()}
                 for stage, entries in Byml(unzs('Banc/BancBasedResource/StartPos.byml.zs')).tree().items()}
    return {'gamedata_list': gdl_name, 'stages': stages, 'start_pos': start_pos, 'fields': fields}


def load_tables(build, out):
    """<out>/game_tables.json, made from the build folder's RomFS when missing or when the RomFS changed (its path, size and time)."""
    ext = sorted(build.glob('extract/v*'), key=lambda p: int(p.name[1:]) if p.name[1:].isdigit() else -1)
    romfs_path = Path(str(rfs.source(ext[-1]))) if ext else fail(f'no extracted RomFS (extract/v*) in {build}')   # rfs.source gives romfs.py's own Path
    st = romfs_path.stat(); ident = f'{romfs_path.resolve()} {st.st_size} {st.st_mtime_ns}'
    cache = out / 'game_tables.json'
    if cache.is_file():
        t = json.loads(cache.read_text())
        if t.get('romfs') == ident: return t
    print(f'reading the game tables from {romfs_path.resolve()}')
    t = {'romfs': ident, **build_tables(RomFS(romfs_path))}
    out.mkdir(parents=True, exist_ok=True); cache.write_text(json.dumps(t) + '\n')
    return t


# ---- the save files ----

class SaveFile:
    """One progress.sav or caption.sav in memory; every accessor raises ValueError on a missing field or an offset outside the file, so a damaged file is refused, not written."""

    def __init__(self, data, kind, path):
        self.b, self.kind, self.path = bytearray(data), kind, path
        if len(self.b) < HEADER_END[kind]: raise ValueError(f'{path}: too short for a {kind}.sav')
        head = struct.unpack_from('<III', self.b, 0)
        if head != (MAGIC, VERSION, HEADER_END[kind]): raise ValueError(f'{path}: not a supported {kind}.sav ({" ".join(f"{w:08x}" for w in head)})')
        self.pairs = {}
        for o in range(FIRST_PAIR, HEADER_END[kind], 8):
            h = struct.unpack_from('<I', self.b, o)[0]
            if h and self.pairs.setdefault(h, o + 4) != o + 4: raise ValueError(f'{path}: duplicate field {h:08x}')

    def _slot(self, h):
        if h not in self.pairs: raise ValueError(f'{self.path}: field {h:08x} missing')
        return self.pairs[h]

    def _data(self, h, length):   # the field's data block, checked to lie in the data area
        o = struct.unpack_from('<I', self.b, self._slot(h))[0]
        if o < HEADER_END[self.kind] or o + length > len(self.b): raise ValueError(f'{self.path}: field {h:08x} points outside the file')
        return o

    def u32(self, h): return struct.unpack_from('<I', self.b, self._slot(h))[0]
    def i32(self, h): return struct.unpack_from('<i', self.b, self._slot(h))[0]
    def f32(self, h): return struct.unpack_from('<f', self.b, self._slot(h))[0]
    def set_u32(self, h, v): struct.pack_into('<I', self.b, self._slot(h), v & 0xffffffff)
    def set_f32(self, h, v): struct.pack_into('<f', self.b, self._slot(h), v)
    def vec3(self, h): return struct.unpack_from('<3f', self.b, self._data(h, 12))
    def set_vec3(self, h, xyz): struct.pack_into('<3f', self.b, self._data(h, 12), *xyz)

    def string(self, h, size):
        o = self._data(h, size)
        return bytes(self.b[o:o + size]).split(b'\0', 1)[0].decode('utf-8', 'replace')

    def set_string(self, h, s, size):
        """A NUL-padded string into a fixed-size field (String32 / String64); refuses one that does not fit."""
        raw = s.encode('ascii')
        if len(raw) >= size: raise ValueError(f'{s!r} is too long for a {size}-byte field')
        o = self._data(h, size); self.b[o:o + size] = raw + bytes(size - len(raw))

    def set_string_array(self, h, values, size=64):
        """A String64Array rewritten in place: the values first, the other elements emptied."""
        count = struct.unpack_from('<I', self.b, self._data(h, 4))[0]
        o = self._data(h, 4 + count * size) + 4
        if len(values) > count: raise ValueError(f'field {h:08x} holds only {count} strings')
        for i in range(count):
            raw = values[i].encode('ascii') if i < len(values) else b''
            if len(raw) >= size: raise ValueError(f'{values[i]!r} is too long for a {size}-byte element')
            self.b[o + i * size:o + (i + 1) * size] = raw + bytes(size - len(raw))


def read_slot(slot):
    """(progress, caption) of one slot folder."""
    return (SaveFile((slot / 'progress.sav').read_bytes(), 'progress', slot / 'progress.sav'),
            SaveFile((slot / 'caption.sav').read_bytes(), 'caption', slot / 'caption.sav'))


def slot_dirs(save):
    """The slot_NN folders of a save that hold both files."""
    return [d for d in sorted(save.glob('slot_[0-9][0-9]')) if (d / 'progress.sav').is_file() and (d / 'caption.sav').is_file()]


def field_hash(name): return int(name, 16) if re.fullmatch(r'0x[0-9a-fA-F]{1,8}', name) else murmur3(name)


def give_item(p, item):
    """Own an item the way a pickup records it: PouchData.<item>.StockNum at least 1, IsGet.<item> and HavePlayedGetEvent.<item> (no get cutscene)."""
    stock = murmur3(f'PouchData.{item}.StockNum')
    p.set_u32(stock, max(1, p.i32(stock)))
    for flag in (f'IsGet.{item}', f'HavePlayedGetEvent.{item}'):
        if murmur3(flag) in p.pairs: p.set_u32(murmur3(flag), 1)


def adult_kit(p):
    """A child slot's equipment fitted to an adult: the Master Sword and Hylian Shield owned and equipped, the child's sword and shield as his other-age gear, the first sword pull done."""
    for item in ADULT_ITEMS: give_item(p, item)
    weapon, shield = p.string(P_EQUIP_WEAPON, 64), p.string(P_EQUIP_SHIELD, 64)
    if weapon in CHILD_WEAPONS or not weapon: p.set_string(P_OTHER_WEAPON, weapon, 64); p.set_string(P_EQUIP_WEAPON, 'Weapon_Sword_Master', 64)
    if shield in CHILD_SHIELDS or not shield: p.set_string(P_OTHER_SHIELD, shield, 64); p.set_string(P_EQUIP_SHIELD, 'Weapon_Shield_Hylian', 64)
    for flag in ADULT_FLAGS: p.set_u32(murmur3(flag), 1)


def warp_kit(p):
    """The Ocarina of Time and the six warp songs, their lesson scenes marked as played."""
    for item in WARP_ITEMS: give_item(p, item)
    for song in WARP_SONGS:
        for flag in (f'IsGet.Item_Melody_{song}', f'HavePlayedGetEvent.Item_Melody_{song}'): p.set_u32(murmur3(flag), 1)
    for event in WARP_EVENTS: p.set_u32(murmur3(f'HavePlayedEvent.{event}'), 1)


def parse_set(spec, tables):
    """'FIELD=VALUE' -> (hash, u32 word, text) for an inline progress field, typed from the game's tables."""
    if '=' not in spec: fail(f'--set {spec!r}: expected FIELD=VALUE')
    name, value = spec.split('=', 1); h = field_hash(name)
    info = tables['fields'].get(f'{h:08x}')
    if not info or info[1] != 0: fail(f'--set {name}: not a progress.sav field of this game ({h:08x})')
    typ = info[0]
    try:
        if typ == 'Bool': word = {'1': 1, 'true': 1, '0': 0, 'false': 0}[value.lower()]
        elif typ in ('Int', 'UInt'): word = int(value, 0); assert (-2**31 <= word < 2**31) if typ == 'Int' else (0 <= word < 2**32)
        elif typ == 'Float': word = struct.unpack('<I', struct.pack('<f', float(value)))[0]
        elif typ == 'Enum': word = int(value, 16) if value.lower().startswith('0x') else murmur3(value)   # an enum value is the hash of its name
        else: fail(f'--set {name}: type {typ} is not supported (Bool, Int, UInt, Float, Enum)')
    except (KeyError, ValueError, AssertionError): fail(f'--set {name}: {value!r} is not a valid {typ}')
    return h, word, f'{name}={value}'


def resolve_place(a, tables):
    """The place to write as a dict (name, stage, pos, facing, require, desc, warp, items, wear), from --place or --name / --stage / --start or --pos."""
    if a.place:
        pr = PRESETS.get(a.place) or fail(f'unknown place {a.place!r} (--list)')
        pl: dict[str, Any] = dict(name=a.place, stage=pr['stage'], desc=pr['desc'], require=pr.get('require'), warp=pr.get('warp', False), wear=pr.get('wear'))
        start, pos, facing, face = pr.get('start'), pr.get('pos'), pr.get('facing'), pr.get('face')
    else:
        if not (a.name and a.stage and (a.start is not None or a.pos)): fail('give --place, or --name with --stage and --start or --pos')
        pl: dict[str, Any] = dict(name=a.name, stage=a.stage, desc='', require=None, warp=False, wear=None)
        start, pos, facing, face = a.start, a.pos, None, None
    pl['warp'] = pl['warp'] or a.warp_songs; pl['wear'] = a.wear or pl['wear']
    pl['items'] = list(a.item or []) + ([pl['wear']] if pl['wear'] and pl['wear'] not in (a.item or []) else [])
    facing = a.facing if a.facing is not None else facing; face = a.face_toward or face
    if pl['stage'] not in tables['stages']: fail(f'unknown stage {pl["stage"]!r} (--stages)')
    if start is not None:
        table = tables['start_pos'].get(pl['stage'], {})
        if start not in table: fail(f'{pl["stage"]} has no start position {start!r}; it has: {", ".join(map(repr, table))}')
        pos = pos or table[start]['pos']
        if facing is None and not face: facing = table[start]['facing_deg']
    if face: facing = math.degrees(math.atan2(face[0] - pos[0], face[1] - pos[2]))
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]*', pl['name']): fail(f'{pl["name"]!r}: use lowercase letters, digits and dashes')
    pl['pos'], pl['facing'] = tuple(float(v) for v in pos), wrap_deg(0.0 if facing is None else facing)
    return pl


def pick_source(base, want, require):
    """The base slot to copy: --slot, or the newest (by its caption's time) whose fields match require."""
    slots = slot_dirs(base) or fail(f'{base}: no slot_NN folder with progress.sav and caption.sav')
    if want: return next((d for d in slots if d.name == want), None) or fail(f'{base}: no {want}')
    ok = []
    for d in slots:
        p, c = read_slot(d)
        if all(p.i32(field_hash(k)) == v for k, v in (require or {}).items()): ok.append((tuple(c.i32(h) for h in C_TIME), d.name, d))
    return max(ok)[2] if ok else fail(f'{base}: no slot has {require}; pick one with --slot')


def patch(p, c, pl, tables, kits, sets):
    """One slot pointed at the place, with the kits, items, tunic and --set values applied."""
    info = tables['stages'][pl['stage']]; location = info['location'] or info['folder'] or pl['stage']
    p.set_string(P_STAGE, pl['stage'], 64); p.set_vec3(P_POS, pl['pos']); p.set_f32(P_FACING, math.radians(pl['facing']))
    p.set_string(P_LAST_STAGE, pl['stage'], 32); p.set_vec3(P_LAST_POS, pl['pos'])
    p.set_vec3(P_MAP_POS, pl['pos']); p.set_f32(P_MAP_ROT, wrap_deg(pl['facing'] + 180.0)); p.set_string_array(P_LOCATIONS, [location])
    p.set_u32(P_IS_CHILD, 0 if pl['age'] == 'adult' else 1)
    if 'adult' in kits: adult_kit(p)
    if 'warp' in kits: warp_kit(p)
    for item in pl['items']: give_item(p, item)
    if pl['wear']: p.set_string(P_EQUIP_ARMOR, pl['wear'], 64)
    for h, word, _ in sets: p.set_u32(h, word)
    c.set_vec3(C_POS, pl['pos']); c.set_string(C_LOCATION, location, 64); c.set_string(C_STAGE_PATH, f'{info["folder"]}/{pl["stage"]}', 64)


def make(a, tables):
    """<out>/<name>/save from the base save (only read), built beside the target and swapped in; returns its path."""
    pl = resolve_place(a, tables)
    base, out = a.base.resolve(), a.out.resolve()
    if not base.is_dir(): fail(f'no base save at {base}')
    root = out / pl['name']
    if root == base or root in base.parents: fail(f'the base {base} is inside the output {root}')
    if root.exists() and not (a.force and (root / MARKER).is_file()): fail(f'{root} exists' + ('; --force replaces it' if (root / MARKER).is_file() else f' and has no {MARKER}: left alone'))
    src = pick_source(base, a.slot, pl['require']); src_p, src_c = read_slot(src)
    forced = tables['stages'][pl['stage']]['age']
    pl['age'] = a.age or forced or ('child' if src_p.u32(P_IS_CHILD) else 'adult')
    if forced and pl['age'] != forced: print(f'warning: {pl["stage"]} makes Link {forced} on arrival; the game overrides --age {pl["age"]}')
    kits = [k for k, on in (('adult', pl['age'] == 'adult' and src_p.u32(P_IS_CHILD) and not a.no_adult_kit), ('warp', pl['warp'])) if on]
    sets = [parse_set(s, tables) for s in a.set or []]
    tmp = out / f'.{pl["name"]}.part'
    if tmp.exists(): shutil.rmtree(tmp)
    (tmp / 'save').mkdir(parents=True)
    for f in sorted(base.glob('*.sav')): shutil.copy2(f, tmp / 'save' / f.name)   # option.sav, account.sav unchanged
    for d in slot_dirs(base):
        p, c = read_slot(src)
        c.set_u32(C_AUTOSAVE, read_slot(d)[1].u32(C_AUTOSAVE))   # each slot keeps its autosave / manual label
        patch(p, c, pl, tables, kits, sets)
        (tmp / 'save' / d.name).mkdir()
        (tmp / 'save' / d.name / 'progress.sav').write_bytes(p.b); (tmp / 'save' / d.name / 'caption.sav').write_bytes(c.b)
        check = read_slot(tmp / 'save' / d.name)[0]   # read back
        if check.string(P_STAGE, 64) != pl['stage'] or any(abs(x - y) > 1e-3 for x, y in zip(check.vec3(P_POS), pl['pos'])) or len(check.b) != len(src_p.b):
            fail(f'read-back check failed in {tmp / "save" / d.name}')
    mark = {k: pl[k] for k in ('name', 'desc', 'stage', 'age', 'items', 'wear')}
    mark.update(pos=[round(v, 4) for v in pl['pos']], facing_deg=round(pl['facing'], 2), kits=kits, set=[s[2] for s in sets], base_slot=src.name,
                base_saved='{:04d}-{:02d}-{:02d} {:02d}:{:02d}'.format(*(src_c.i32(h) for h in C_TIME)), quest_step=src_p.string(P_QUEST, 64),
                made=datetime.datetime.now().isoformat(timespec='seconds'))
    (tmp / MARKER).write_text(json.dumps(mark, indent=1) + '\n')
    if root.exists(): old = out / f'.{pl["name"]}.old'; shutil.rmtree(old, ignore_errors=True); root.rename(old); tmp.rename(root); shutil.rmtree(old)
    else: tmp.rename(root)
    x, y, z = pl['pos']
    print(f'{root / "save"}: {pl["stage"]} at {x:.2f} {y:.2f} {z:.2f}, facing {pl["facing"]:.1f}, {pl["age"]}{"".join(", " + k + " kit" for k in kits)}, from {src.name} ({mark["quest_step"]})')
    return root / 'save'


def describe(slot):
    """One line: where a slot loads, when it was saved, age and quest step."""
    p, c = read_slot(slot); x, y, z = p.vec3(P_POS)
    when = '{:04d}-{:02d}-{:02d} {:02d}:{:02d}'.format(*(c.i32(h) for h in C_TIME))
    return (f'{slot.name}  {when} {"auto" if c.u32(C_AUTOSAVE) else "manual":6s} {p.string(P_STAGE, 64):26s} {x:8.2f} {y:7.2f} {z:8.2f}  facing '
            f'{math.degrees(p.f32(P_FACING)):6.1f}  {"child" if p.u32(P_IS_CHILD) else "adult"}  {p.string(P_QUEST, 64)}')


def main():
    ap = argparse.ArgumentParser(description='Ocarina of Time test saves: a save copied so that every slot loads Link at one place')
    ap.add_argument('build', help='the <id>_build folder the NSP script made (its extracted RomFS is read)')
    ap.add_argument('--list', action='store_true', help='the presets'); ap.add_argument('--stages', nargs='?', const='', metavar='FILTER', help='stages and start positions')
    ap.add_argument('--show', metavar='SAVE', help='where each slot of a save loads')
    ap.add_argument('--place'); ap.add_argument('--all', action='store_true', help='make the checkpoints')
    ap.add_argument('--name'); ap.add_argument('--stage'); ap.add_argument('--start', help="a named start position of the stage ('' = the default one)")
    ap.add_argument('--pos', type=lambda s: tuple(float(v) for v in s.split(',')), help='X,Y,Z (Y is up)')
    ap.add_argument('--facing', type=float, help='degrees: 0 faces +Z, 90 faces +X'); ap.add_argument('--face-toward', type=lambda s: tuple(float(v) for v in s.split(',')), metavar='X,Z')
    ap.add_argument('--age', choices=('child', 'adult')); ap.add_argument('--slot'); ap.add_argument('--warp-songs', action='store_true')
    ap.add_argument('--item', action='append'); ap.add_argument('--wear'); ap.add_argument('--set', action='append', metavar='FIELD=VALUE')
    ap.add_argument('--no-adult-kit', action='store_true'); ap.add_argument('--force', action='store_true')
    ap.add_argument('--base', help='the save to copy (default <id>_build/run/save)'); ap.add_argument('--out', help='where <name>/save goes (default <id>_build/test_saves)')
    a = ap.parse_args()
    build = Path(a.build).expanduser().resolve()
    if not build.name.upper().startswith(TITLE): fail(f'{build.name}: not a {TITLE}_build folder')
    a.base = Path(a.base).expanduser() if a.base else build / 'run/save'
    a.out = Path(a.out).expanduser() if a.out else build / 'test_saves'
    if a.list:
        for title, group in (('checkpoints (--all)', CHECKPOINTS), ('extras', EXTRAS)):
            print(title); [print(f'  {k:28s} {v["stage"]:26s} {v["desc"]}') for k, v in group.items()]
        return 0
    if a.show:
        for d in slot_dirs(Path(a.show).expanduser()): print(describe(d))
        return 0
    tables = load_tables(build, a.out)
    if a.stages is not None:
        for stage, info in tables['stages'].items():
            if a.stages.lower() in stage.lower():
                print(f'{stage}  (folder {info["folder"]}, location {info["location"]}, age {info["age"] or "kept"})')
                for s, v in tables['start_pos'].get(stage, {}).items(): print(f'    {s!r:50s} {v["pos"][0]:8.2f} {v["pos"][1]:7.2f} {v["pos"][2]:8.2f}  facing {v["facing_deg"]:7.1f}')
        return 0
    try:
        if a.all:
            for name in CHECKPOINTS: a.place = name; make(a, tables)
        else: make(a, tables)
    except ValueError as e: fail(str(e))
    return 0


if __name__ == '__main__':
    sys.exit(main())
