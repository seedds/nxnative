"""patch_nso.py --aot (docs/design/aot-recompiler.md 1.1, 1.2): the behaviour image and manifest the rewriter writes for the x86-64 recompiler, on the synthetic
module of aot_synth.py (no game files, no keys).
  ManifestRows (always): the manifest made by patch_nso.py's own writer (aot_data_rows, aot_manifest) from the module's tables, every row checked against the bytes
    of the image it names and of the NSO it comes from
  RewriterAot (where capstone and lz4 are installed, as patch_nso.py imports them: CI's tool tests): patch_nso.py on the NSO without and with --aot writes Apple's
    outputs byte for byte the same, and --aot writes exactly the image and manifest above; sites.tsv's T, J and W rows are the manifest's oracle rows
  NxaotReads (NXAOT=<nxaot executable>, as built in the nxnative-aot image): nxaot prints the manifest back unchanged, its discovery reproduces the oracle rows
    (design 12, item 5) with one stated difference, and nxaot build packs the module
    docker run ... nxnative-aot sh -c 'NXAOT=build/aot/nxaot python3 -m unittest -v dm/tools/tests/test_aot_manifest.py'
"""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import aot_synth as S

TOOLS = HERE.parent


def rows_of(text):
    """The manifest's rows: the header's fields, then [(kind, fields)]"""
    lines = text.splitlines()
    head = lines[0].split('\t')
    return head, [(ln.split('\t')[0], ln.split('\t')[1:]) for ln in lines[1:] if ln]


def u32(b, o): return struct.unpack_from('<I', b, o)[0]


def branch_target(word, pc):   # b imm26
    assert word >> 26 == 0x05, hex(word)
    return pc + ((((word & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) << 2)


class ManifestRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        S.write(cls.tmp)
        cls.image = (cls.tmp / 'main.image.bin').read_bytes()
        cls.text = (cls.tmp / 'main.aot.tsv').read_text()
        cls.nso = (cls.tmp / 'main.nso').read_bytes()
        cls.head, cls.rows = rows_of(cls.text)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp)

    def kind(self, k): return [f for kk, f in self.rows if kk == k]

    def nso_image(self):
        """The NSO's segments at their memory offsets (it is uncompressed: flags 0)"""
        self.assertEqual(self.nso[:4], b'NSO0'); self.assertEqual(u32(self.nso, 0xC), 0)
        img = bytearray(S.DATA_END)
        for i in range(3):
            fo, mo, sz = struct.unpack_from('<III', self.nso, 0x10 + 0x10 * i)
            img[mo:mo + sz] = self.nso[fo:fo + sz]
        return img

    def test_header_and_layout(self):
        self.assertEqual(self.head, ['# nxaot-manifest 1', 'main', self.nso[0x40:0x50].hex(), hashlib.sha256(self.image).hexdigest()])
        kinds = [k for k, _ in self.rows]
        self.assertEqual(kinds, sorted(kinds, key=['layout', 'aux', 'data', 'obs', 'spin', 'cut', 'oracle-T', 'oracle-J', 'oracle-W'].index), 'the canonical order')
        self.assertEqual(set(kinds), {'layout', 'aux', 'data', 'obs', 'spin', 'cut', 'oracle-T', 'oracle-J', 'oracle-W'}, 'every row kind')
        (layout,) = self.kind('layout')
        t, r, d, b, n = (int(layout[i], 16) for i in (1, 3, 5, 7, 9))
        segs = [struct.unpack_from('<III', self.nso, 0x10 + 0x10 * i) for i in range(3)]
        self.assertEqual((t, r, d), (segs[0][2], segs[1][1], segs[2][1]))
        self.assertEqual(n, len(self.image))
        self.assertEqual(b, 0, 'the aux region took bss in: the image holds it as zeros')
        self.assertFalse(any(self.image[S.DATA_END:S.DATA_END + u32(self.nso, 0x3C)]), 'bss is zero')

    def test_image_is_the_module_with_its_cuts(self):
        img, nso = bytearray(self.image[:S.DATA_END]), self.nso_image()
        for off, old, new, _ in self.kind('cut'):
            off, old, new = int(off, 16), bytes.fromhex(old), bytes.fromhex(new)
            self.assertEqual(nso[off:off + len(old)], old, f'the cut at {off:#x}: the NSO holds the old bytes')
            self.assertEqual(img[off:off + len(new)], new, f'the cut at {off:#x}: the image holds the new ones')
            img[off:off + len(new)] = old
        self.assertEqual(img, nso, 'outside the cuts, the image is the NSO\'s segments')

    def test_aux(self):
        ((off, length, _, state),) = self.kind('aux')
        off, length, state = int(off, 16), int(length, 16), int(state, 16)
        self.assertEqual(off + length, len(self.image))
        self.assertFalse(any(self.image[state:state + 0x4000]), 'the state page is zero in the image')
        fs = [c for c in self.kind('cut') if 'frame step' in c[3]]
        self.assertEqual(len(fs), 1)
        site = int(fs[0][0], 16)
        self.assertEqual(branch_target(u32(self.image, site), site), off, 'the frame step\'s branch enters the aux region')
        adrp = u32(self.image, off)   # its first word: adrp x16 of the state page
        imm = ((adrp >> 29) & 3) | ((adrp >> 5) & 0x7FFFF) << 2
        self.assertEqual((off & ~0xFFF) + ((imm ^ 0x100000) - 0x100000) * 0x1000, state)
        self.assertEqual(branch_target(u32(self.image, off + length - 4), off + length - 4), site + 4, 'and goes back after the site')

    def test_data_rows(self):
        data = [(int(a, 16), int(b, 16)) for a, b in self.kind('data')]
        self.assertEqual(data[0], (4, 8)); self.assertEqual(u32(self.image, 4), S.MOD0, 'the MOD0 offset word')
        s, e = data[1]
        self.assertEqual(e, S.TEXT_END)
        words = [u32(self.image, a) for a in range(s, e, 4)]
        self.assertGreater(sum(w >> 24 == 0x01 for w in words) * 4, len(words), 'more than a quarter of the block is no instruction')
        for lo, hi in S.FN_RANGES: self.assertFalse(lo < e and s < hi, 'no function in a data block')

    def test_obs_and_spin(self):
        ((off, num, param, word),) = self.kind('obs')
        off = int(off, 16)
        self.assertEqual((int(num), int(param, 16), int(word, 16)), (S.OBS_NUMBER & 0xFF, S.OBS_NUMBER >> 8, u32(self.image, off)))
        self.assertIn(off, [lo for lo, _ in S.FN_RANGES], 'a gate at a function entry')
        ((head, edges),) = self.kind('spin')
        head, edges = int(head, 16), [int(x, 16) for x in edges.split(',')]
        for be in edges:
            w = u32(self.image, be)
            self.assertEqual(w >> 24, 0x34, 'a cbz back-edge')
            self.assertEqual(be + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000) * 4, head)

    def test_oracle_rows(self):
        ((br, base, width),) = self.kind('oracle-J')
        br, base = int(br, 16), int(base, 16)
        self.assertEqual(u32(self.image, br), 0xD61F0240, 'br x18'); self.assertEqual(width, '1')
        entries = self.image[S.BTAB:S.BTAB + 4]
        self.assertEqual([u32(self.image, base + 4 * e) for e in entries], [S.movz_w(0, 10 * (i + 1)) for i in range(4)], 'each case returns its value')
        ((br, targets),) = self.kind('oracle-T')
        br = int(br, 16)
        self.assertEqual(u32(self.image, br), 0xD61F0240, 'br x18')
        self.assertEqual([int(x, 16) for x in targets.split(',')], [S.TTAB + struct.unpack_from('<i', self.image, S.TTAB + 4 * i)[0] for i in range(4)])
        ((lo, hi),) = self.kind('oracle-W')
        lo, hi = int(lo, 16), int(hi, 16)
        self.assertEqual(u32(self.image, lo) & 0xFFFFFC00, 0x885FFC00, 'ldaxr w')
        self.assertEqual(u32(self.image, hi - 8) & 0xFFE0FC00, 0x8800FC00, 'stlxr w')
        self.assertEqual(u32(self.image, hi - 4) >> 24, 0x35, 'the cbnz after the store, which sites.tsv counts in the window')

    def test_writer_is_canonical(self):
        """nxaot's WriteManifest form: the rows' hex as %#llx (0x0 for zero), the obs number in decimal"""
        for _, f in self.rows:
            for v in (x for field in f for x in field.split(',')):
                if v.startswith('0x'): self.assertEqual(v, f'{int(v, 16):#x}')
        self.assertTrue(self.text.endswith('\n') and '\n\n' not in self.text)


def importable(name):
    try: return importlib.util.find_spec(name) is not None
    except ModuleNotFoundError: return False


@unittest.skipUnless(importable('capstone') and importable('lz4'), 'patch_nso.py imports capstone and lz4 (CI\'s tool tests install them)')
class RewriterAot(unittest.TestCase):
    def test_aot_is_additive_and_exact(self):
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp)
        S.write(tmp)
        env = dict(os.environ, DM_GAMES_DIR=str(tmp / 'profiles')); env.pop('DM_GAME', None)
        def rewrite(*args):
            r = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(tmp / 'main.nso'), *args], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout
        rewrite(str(tmp / 'plain'), 'main')
        out = rewrite(str(tmp / 'aot'), 'main', '--aot', str(tmp / 'aot_input'))
        self.assertIn('main aot:', out)
        plain, aot = sorted(os.listdir(tmp / 'plain')), sorted(os.listdir(tmp / 'aot'))
        self.assertEqual(plain, aot)
        for f in plain: self.assertEqual((tmp / 'plain' / f).read_bytes(), (tmp / 'aot' / f).read_bytes(), f'{f}: --aot changed Apple\'s output')
        self.assertEqual(sorted(os.listdir(tmp / 'aot_input')), ['main.aot.tsv', 'main.image.bin'])
        self.assertEqual((tmp / 'aot_input' / 'main.image.bin').read_bytes(), (tmp / 'main.image.bin').read_bytes())
        self.assertEqual((tmp / 'aot_input' / 'main.aot.tsv').read_text(), (tmp / 'main.aot.tsv').read_text())
        # The oracle rows are sites.tsv's T, J and W rows
        sites = [ln.split('\t') for ln in (tmp / 'aot' / 'main.sites.tsv').read_text().splitlines()]
        _, rows = rows_of((tmp / 'main.aot.tsv').read_text())
        self.assertEqual({(int(s[0], 16), int(s[2], 16)) for s in sites if s[1] == 'W'}, {(int(a, 16), int(b, 16)) for k, (a, b) in rows if k == 'oracle-W'})
        self.assertEqual({int(s[0], 16): int(s[2], 16) for s in sites if s[1] == 'J'}, {int(f[0], 16): int(f[1], 16) for k, f in rows if k == 'oracle-J'})
        self.assertEqual({int(s[0], 16): [int(x, 16) for x in s[2].split()] for s in sites if s[1] == 'T'},
                         {int(f[0], 16): [int(x, 16) for x in f[1].split(',')] for k, f in rows if k == 'oracle-T'})
        self.assertEqual({int(s[0], 16): [int(x, 16) for x in s[2].split()] for s in sites if s[1] == 'S'},
                         {int(f[0], 16): [int(x, 16) for x in f[1].split(',')] for k, f in rows if k == 'spin'})

    def test_usage(self):
        r = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), 'x.nso', 'out', 'main', '--aot'], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0); self.assertIn('usage', r.stderr)


NXAOT = os.environ.get('NXAOT', '')


@unittest.skipUnless(NXAOT and os.access(NXAOT, os.X_OK), 'NXAOT names no nxaot executable (the nxnative-aot image builds it: dm/tools/aot/README.md)')
class NxaotReads(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        S.write(cls.tmp)
        cls.tsv = cls.tmp / 'main.aot.tsv'

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp)

    def nxaot(self, *args):
        r = subprocess.run([NXAOT, *args], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_manifest_round_trip(self):
        """nxaot's reader accepts every row (the SHA-256 and the layout against image.bin among them) and its writer prints the text back unchanged"""
        self.assertEqual(self.nxaot('manifest', str(self.tsv)), self.tsv.read_text())

    def test_discovery_reproduces_the_oracle_rows(self):
        """Design 12, item 5: the recovered tables are the rewriter's. The windows' end differs: sites.tsv's W rows (oracle-W) end past the cbz / cbnz after the store,
        nxaot's past the store"""
        out = self.tmp / 'discover.tsv'
        self.nxaot('discover', str(self.tsv), '--tsv', str(out))
        rows = [ln.split('\t') for ln in out.read_text().splitlines()]
        jt = {int(r[1], 16): r for r in rows if r[0] == 'jt'}
        win = {int(r[1], 16): int(r[2], 16) for r in rows if r[0] == 'win'}
        _, mrows = rows_of(self.tsv.read_text())
        image = (self.tmp / 'main.image.bin').read_bytes()
        for k, f in mrows:
            if k == 'oracle-T':
                self.assertIn(int(f[0], 16), jt)
                self.assertEqual([int(x, 16) for x in jt[int(f[0], 16)][12].split(',')], [int(x, 16) for x in f[1].split(',')])
            elif k == 'oracle-J':
                r = jt[int(f[0], 16)]
                self.assertEqual((int(r[6], 16), r[4]), (int(f[1], 16), f[2]), 'the base (the target of entry 0) and the entries\' width')
            elif k == 'oracle-W':
                lo, hi = int(f[0], 16), int(f[1], 16)
                self.assertIn(lo, win)
                self.assertTrue(win[lo] == hi or (win[lo] + 4 == hi and u32(image, win[lo]) >> 25 & 0x3F == 0x1A), f'window {lo:#x}: {win[lo]:#x} against {hi:#x}')
        summary = next(r for r in rows if r[:2] == ['summary', 'coverage'])
        self.assertEqual(summary[summary.index('uncovered') + 1], '0')

    def test_build(self):
        self.nxaot('build', str(self.tsv), '-o', str(self.tmp / 'main.o'), '--shared', str(self.tmp / 'main.aot.so'))
        self.assertGreater((self.tmp / 'main.aot.so').stat().st_size, 0)


class HostFnModule(unittest.TestCase):
    """aot_synth.py --hostfn (CTest t_aot_hostfn): main's manifest through patch_nso.py's writer against its image, the NRO sub's header, symbols and relocations against
    its bytes, and nro.tsv's row against the NRO; with NXAOT, nxaot reads both, covers every word and packs both; with llvm-mc-18, the module's own encoders"""
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        S.write_hostfn(cls.tmp)
        cls.image = (cls.tmp / 'main.image.bin').read_bytes()
        cls.sub = (cls.tmp / 'sub.nro').read_bytes()

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp)

    def test_manifest(self):
        head, rows = rows_of((self.tmp / 'main.aot.tsv').read_text())
        self.assertEqual(head, ['# nxaot-manifest 1', 'main', S.HF_BUILD_ID, hashlib.sha256(self.image).hexdigest()])
        self.assertEqual(rows[0], ('layout', ['text_end', '0x1000', 'ro_off', '0x1000', 'data_off', '0x2000', 'bss', f'{S.HF_BSS:#x}', 'image_size', f'{len(self.image):#x}']))
        data = [(int(f[0], 16), int(f[1], 16)) for k, f in rows if k == 'data']
        self.assertEqual(data[0], (4, 8), 'the MOD0 offset word')
        self.assertEqual(data[1][1], S.HF_TEXT_END)
        self.assertEqual(set(self.image[data[1][0]:S.HF_TEXT_END]), {0}, 'the gap after the last function: zeros, no instruction')
        self.assertEqual({k for k, _ in rows}, {'layout', 'data'}, 'no cut, gate, frame step or oracle row: the module has no patch file')

    def test_main_imports_and_tables(self):
        """main's GOT relocations name HF_GOT_SYMS in order; sub_value is the one jump slot; the heap pointer is a relative relocation to bss"""
        dyn = {}
        for o in range(S.HF_DYNAMIC, S.HF_DYNAMIC + 16 * 16, 16):
            tag, v = struct.unpack_from('<qQ', self.image, o)
            if not tag: break
            dyn[tag] = v
        symtab, strtab = dyn[6], dyn[5]
        name = lambda i: self.image[strtab + u32(self.image, symtab + 24 * i):].split(b'\0')[0].decode()
        rela = [struct.unpack_from('<QQq', self.image, dyn[7] + 24 * i) for i in range(dyn[8] // 24)]
        jmp = [struct.unpack_from('<QQq', self.image, dyn[23] + 24 * i) for i in range(dyn[2] // 24)]
        got = {off: (info & 0xFFFFFFFF, name(info >> 32)) for off, info, _ in rela + jmp if info & 0xFFFFFFFF != 1027}
        self.assertEqual([got[S.HF_GOT + 8 * i][1] for i in range(len(S.HF_GOT_SYMS))], S.HF_GOT_SYMS)
        self.assertEqual([s for _, (t, s) in got.items() if t == 1026], ['sub_value'])
        self.assertIn((S.HF_HEAPPTR, 1027, S.HF_HEAP), rela)

    def test_nro(self):
        b = self.sub
        self.assertEqual(b[0x10:0x14], b'NRO0')
        size, flags, text_off, text_size, ro_off, ro_size, data_off, data_size, bss = struct.unpack_from('<9I', b, 0x18)
        self.assertEqual((size, text_off, text_size, ro_off, data_off, data_off + data_size, bss), (len(b), 0, 0x1000, 0x1000, 0x2000, len(b), S.SUB_BSS))
        self.assertEqual(u32(b, 4), S.SUB_MOD0)
        self.assertEqual(b[S.SUB_MOD0:S.SUB_MOD0 + 4], b'MOD0')
        self.assertEqual(branch_target(u32(b, 0), 0), 0xD0, 'the entry branch: to sub_value')
        rows = [ln.split('\t') for ln in (self.tmp / 'nro.tsv').read_text().splitlines() if ln and ln[0] != '#']
        self.assertEqual(rows, [['sub', hashlib.sha256(b).hexdigest(), str(len(b))]])

    def test_expected_results(self):
        self.assertEqual(S.HF_EXPECTED[:12], [19, 0x40200000, 1030, 1056, 123, 0, 42, 0, 0, 1012, 77, 0], 't_aot_hostfn.c EXPECTED holds these numbers')

    @unittest.skipUnless(shutil.which('llvm-mc-18'), 'llvm-mc-18 is not installed (the nxnative-aot image has it)')
    def test_encoders(self):
        src = self.tmp / 'enc.s'
        src.write_text(''.join(t + '\n' for t, _ in S.HF_ENCODINGS))
        r = subprocess.run(['llvm-mc-18', '-triple=aarch64', '-show-encoding', str(src)], capture_output=True, text=True, check=True)
        got = [int(''.join(reversed(m.split(','))).replace('0x', ''), 16) for m in re.findall(r'encoding: \[([^\]]+)\]', r.stdout)]
        self.assertEqual(got, [w for _, w in S.HF_ENCODINGS])

    @unittest.skipUnless(NXAOT and os.access(NXAOT, os.X_OK), 'NXAOT names no nxaot executable')
    def test_nxaot_reads_and_packs(self):
        def nxaot(*args):
            r = subprocess.run([NXAOT, *args], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout
        tsv = self.tmp / 'main.aot.tsv'
        self.assertEqual(nxaot('manifest', str(tsv)), tsv.read_text())
        for mod in (tsv, self.tmp / 'sub.nro'):
            out = self.tmp / f'{mod.stem}.discover.tsv'
            nxaot('discover', str(mod), '--tsv', str(out))
            summary = next(ln.split('\t') for ln in out.read_text().splitlines() if ln.startswith('summary\tcoverage'))
            self.assertEqual(summary[summary.index('uncovered') + 1], '0', mod.name)
            nxaot('build', str(mod), '-o', str(self.tmp / f'{mod.stem}.o'), '--shared', str(self.tmp / f'{mod.stem}.aot.so'))


if __name__ == '__main__':
    unittest.main()
