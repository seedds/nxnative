#!/usr/bin/env python3
# scripts/ocarina_test_save.py: the place, kit and tunic fields it writes into a save's slots, on synthetic progress.sav / caption.sav files built here (no game data needed)
import contextlib, importlib.util, io, json, struct, tempfile, unittest
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('ots', REPO / 'scripts/ocarina_test_save.py'); assert spec and spec.loader
ots = importlib.util.module_from_spec(spec); spec.loader.exec_module(ots)
H = ots.murmur3
SIZE = {'S64': 64, 'S32': 32, 'V3': 12, 'A4': 4 + 4 * 64}


def sav(kind, fields):
    """A synthetic .sav: the header, {hash, value-or-offset} pairs, then the data blocks; fields = [(hash, 'I' | 'F' | 'S64' | 'S32' | 'V3' | 'A4', initial value)]."""
    end = ots.HEADER_END[kind]
    head, data = bytearray(end), bytearray()
    struct.pack_into('<III', head, 0, ots.MAGIC, ots.VERSION, end)
    for i, (h, t, v) in enumerate(fields):
        o = ots.FIRST_PAIR + 8 * i
        if t == 'I': struct.pack_into('<Ii', head, o, h, v)
        elif t == 'F': struct.pack_into('<If', head, o, h, v)
        else:
            struct.pack_into('<II', head, o, h, end + len(data)); blk = bytearray(SIZE[t])
            if t == 'V3': struct.pack_into('<3f', blk, 0, *v)
            elif t == 'A4': struct.pack_into('<I', blk, 0, 4); blk[4:4 + len(v)] = v.encode()
            else: blk[:len(v)] = v.encode()
            data += blk
    return bytes(head + data)


def child_slot(defeated=1):
    """progress.sav / caption.sav of a child slot in Kokiri Forest after the Deku Tree, with every field the script writes."""
    items = [x for i in ('Weapon_Sword_Master', 'Weapon_Shield_Hylian', 'Item_Ocarina_Time', 'Armor_Adult_Goron')
             for x in ((H(f'PouchData.{i}.StockNum'), 'I', 0), (H(f'IsGet.{i}'), 'I', 0), (H(f'HavePlayedGetEvent.{i}'), 'I', 0))]
    songs = [(H(f'{p}Item_Melody_{s}'), 'I', 0) for s in ots.WARP_SONGS for p in ('IsGet.', 'HavePlayedGetEvent.')]
    flags = [(H(n), 'I', 0) for n in ots.ADULT_FLAGS] + [(H(f'HavePlayedEvent.{e}'), 'I', 0) for e in ots.WARP_EVENTS] + [(H('Gohma_DefeatBoss'), 'I', defeated)]
    p = sav('progress', [(ots.P_STAGE, 'S64', 'KokiriForest_Child'), (ots.P_POS, 'V3', (25.5, 111.7, 0.9)), (ots.P_FACING, 'F', -1.48),
                         (ots.P_LAST_STAGE, 'S32', 'KokiriForest_Child'), (ots.P_LAST_POS, 'V3', (25.5, 111.7, 0.9)), (ots.P_MAP_POS, 'V3', (25.5, 111.7, 0.9)),
                         (ots.P_MAP_ROT, 'F', 95.0), (ots.P_LOCATIONS, 'A4', 'KokiriForest'), (ots.P_IS_CHILD, 'I', 1), (ots.P_QUEST, 'S64', 'Goto_HyruleCastle'),
                         (ots.P_EQUIP_WEAPON, 'S64', 'Weapon_Sword_Kokiri'), (ots.P_EQUIP_SHIELD, 'S64', 'Weapon_Shield_Deku'), (ots.P_OTHER_WEAPON, 'S64', ''),
                         (ots.P_OTHER_SHIELD, 'S64', ''), (ots.P_EQUIP_ARMOR, 'S64', '')] + items + songs + flags)
    c = sav('caption', [(ots.C_POS, 'V3', (25.5, 111.7, 0.9)), (ots.C_LOCATION, 'S64', 'KokiriForest'), (ots.C_STAGE_PATH, 'S64', 'KokiriForest/KokiriForest_Child'),
                        (ots.C_AUTOSAVE, 'I', 1)] + [(h, 'I', v) for h, v in zip(ots.C_TIME, (2026, 11, 8, 19, 32 + defeated))])   # the slot after the fight is the newer one
    return p, c


TABLES = {'stages': {'HyruleField_Child': {'folder': 'HyruleField', 'location': 'HyruleField', 'age': 'child'},
                     'HyruleField_Adult': {'folder': 'HyruleField', 'location': 'HyruleField', 'age': 'adult'},
                     'DeathMountainCrater_Adult': {'folder': 'DeathMountainCrater', 'location': 'DeathMountainCrater', 'age': 'adult'},
                     'InsideTheDekuTree': {'folder': 'InsideTheDekuTree', 'location': 'InsideTheDekuTree', 'age': None}},
          'start_pos': {'HyruleField_Child': {'HyruleField_FrontOfMarket': {'pos': [-0.04, 150.66, 32.16], 'facing_deg': 0.0}},
                        'HyruleField_Adult': {'HyruleField_FrontOfMarket': {'pos': [-0.04, 150.66, 32.16], 'facing_deg': 0.0}},
                        'DeathMountainCrater_Adult': {'FireBolero': {'pos': [0.38, 111.69, -8.68], 'facing_deg': 0.0}}},
          'fields': {f'{H("PouchData.Weapon_Sword_Master.StockNum"):08x}': ['Int', 0]}}


def args(base, out, **kw):
    a = dict(place=None, name=None, stage=None, start=None, pos=None, facing=None, face_toward=None, age=None, slot=None, warp_songs=False, item=None, wear=None,
             set=None, no_adult_kit=False, force=False, base=Path(base), out=Path(out))
    a.update(kw); return SimpleNamespace(**a)


class OcarinaTestSave(unittest.TestCase):
    def setUp(self):
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))   # make() prints one line per save
        self.tmp = tempfile.TemporaryDirectory(); d = Path(self.tmp.name)
        self.base, self.out = d / 'save', d / 'out'
        for i, defeated in ((0, 1), (1, 0)):   # slot_00 newest (after Gohma), slot_01 older (before)
            s = self.base / f'slot_0{i}'; s.mkdir(parents=True)
            p, c = child_slot(defeated); (s / 'progress.sav').write_bytes(p); (s / 'caption.sav').write_bytes(c)
        (self.base / 'option.sav').write_bytes(b'opt'); (self.base / 'account.sav').write_bytes(b'acc')

    def tearDown(self): self.tmp.cleanup()

    def slot(self, name, i=0): return ots.read_slot(self.out / name / 'save' / f'slot_0{i}')

    def test_hashes(self):
        self.assertEqual(H('PlayerStatus.SavePos'), ots.P_POS); self.assertEqual(H('Sequence_CurrentBanc'), ots.P_STAGE); self.assertEqual(H('Sequence_IsChildhood'), ots.P_IS_CHILD)

    def test_child_place(self):
        ots.make(args(self.base, self.out, place='hyrule-field'), TABLES)
        for i in (0, 1):
            p, c = self.slot('hyrule-field', i)
            self.assertEqual(p.string(ots.P_STAGE, 64), 'HyruleField_Child'); self.assertEqual(p.string(ots.P_LAST_STAGE, 32), 'HyruleField_Child')
            self.assertEqual([round(v, 2) for v in p.vec3(ots.P_POS)], [-0.04, 150.66, 32.16]); self.assertEqual(p.f32(ots.P_FACING), 0.0)
            self.assertEqual(p.f32(ots.P_MAP_ROT), 180.0); self.assertEqual(p.u32(ots.P_IS_CHILD), 1)
            self.assertEqual(p.string(ots.P_EQUIP_WEAPON, 64), 'Weapon_Sword_Kokiri')   # a child place keeps the child's gear
            self.assertEqual(c.string(ots.C_STAGE_PATH, 64), 'HyruleField/HyruleField_Child'); self.assertEqual(c.string(ots.C_LOCATION, 64), 'HyruleField')
            self.assertEqual(len(p.b), len((self.base / 'slot_00/progress.sav').read_bytes()))
        self.assertEqual((self.out / 'hyrule-field/save/option.sav').read_bytes(), b'opt')
        self.assertEqual(json.loads((self.out / 'hyrule-field' / ots.MARKER).read_text())['base_slot'], 'slot_00')

    def test_adult_kits_and_tunic(self):
        ots.make(args(self.base, self.out, place='warp-fire'), TABLES)
        p, _ = self.slot('warp-fire')
        self.assertEqual(p.u32(ots.P_IS_CHILD), 0)
        self.assertEqual((p.string(ots.P_EQUIP_WEAPON, 64), p.string(ots.P_EQUIP_SHIELD, 64)), ('Weapon_Sword_Master', 'Weapon_Shield_Hylian'))
        self.assertEqual((p.string(ots.P_OTHER_WEAPON, 64), p.string(ots.P_OTHER_SHIELD, 64)), ('Weapon_Sword_Kokiri', 'Weapon_Shield_Deku'))
        self.assertEqual(p.string(ots.P_EQUIP_ARMOR, 64), 'Armor_Adult_Goron')
        for name in ('PouchData.Weapon_Sword_Master.StockNum', 'IsGet.Weapon_Shield_Hylian', 'PouchData.Item_Ocarina_Time.StockNum', 'PouchData.Armor_Adult_Goron.StockNum',
                     'IsGet.Item_Melody_FireBolero', 'HavePlayedEvent.SageOfFire_MelodyGetFire', *ots.ADULT_FLAGS):
            self.assertEqual(p.i32(H(name)), 1, name)

    def test_require_picks_an_older_slot(self):
        ots.make(args(self.base, self.out, place='gohma'), TABLES)
        self.assertEqual(json.loads((self.out / 'gohma' / ots.MARKER).read_text())['base_slot'], 'slot_01')

    def test_custom_place_and_set(self):
        ots.make(args(self.base, self.out, name='crater', stage='DeathMountainCrater_Adult', start='FireBolero', facing=90.0, no_adult_kit=True,
                      set=['PouchData.Weapon_Sword_Master.StockNum=3']), TABLES)
        p, _ = self.slot('crater')
        self.assertAlmostEqual(p.f32(ots.P_FACING), 1.5707964, places=6); self.assertEqual(p.string(ots.P_EQUIP_WEAPON, 64), 'Weapon_Sword_Kokiri')
        self.assertEqual(p.i32(H('PouchData.Weapon_Sword_Master.StockNum')), 3)

    def test_refusals(self):
        (self.out / 'kakariko').mkdir(parents=True)   # a folder this script did not make is never replaced
        with self.assertRaises(SystemExit): ots.make(args(self.base, self.out, name='kakariko', stage='HyruleField_Child', start='HyruleField_FrontOfMarket', force=True), TABLES)
        ots.make(args(self.base, self.out, place='hyrule-field'), TABLES)
        with self.assertRaises(SystemExit): ots.make(args(self.base, self.out, place='hyrule-field'), TABLES)   # exists: needs --force
        ots.make(args(self.base, self.out, place='hyrule-field', force=True), TABLES)
        with self.assertRaises(ValueError): ots.SaveFile(b'\0' * ots.HEADER_END['progress'], 'progress', 'bad')


if __name__ == '__main__':
    unittest.main()
