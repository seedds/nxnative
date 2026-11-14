"""Owned camera code generation and symbolic relocation, without game data."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import sys
import unittest

TOOLS=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(TOOLS))
sys.path.insert(0,str(TOOLS))  # nso_asm
import importlib.util
PROFILE = TOOLS.parent/'games/01006F8002326000'
def _load(name):
    spec=importlib.util.spec_from_file_location(name, PROFILE/name)
    m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
from nso_asm import ADDI,ADRP,B
gen=_load('gen_view_stubs.py'); assemble=gen.assemble
patches=_load('patches.py'); relocated_words=patches.relocated_words

class Relocations(unittest.TestCase):
    def test_mixed_symbols_and_link_bit(self):
        words=[0x94000000,0x9000000f,0x910001ef,0x14000000]
        reloc=[(0,2,'_dm_return_00009000'),(4,3,'_dm_view_state'),(8,4,'_dm_view_state'),(12,2,'local')]
        got=relocated_words(words,reloc,{'local':64},0x10000,0x2ffe0)
        self.assertEqual(got,[B(0x10000,0x9000)|0x80000000,ADRP(15,0x10004,0x2ffe0),ADDI(15,15,0xfe0),B(0x1000c,0x10040)])
        self.assertEqual(words,[0x94000000,0x9000000f,0x910001ef,0x14000000])
        self.assertEqual(relocated_words([0x14000000],[(0,2,'_dm_code_00100000')],{},0xff000,0),[B(0xff000,0x100000)])

    def test_reject_invalid_instruction_range_or_symbol(self):
        for words,reloc in [([0],[(0,2,'local')]),([0],[(0,3,'local')]),([0],[(0,4,'local')]),([0x14000000],[(2,2,'local')]),([0x14000000],[(4,2,'local')]),([0x14000000],[(-4,2,'local')])]:
            with self.subTest(reloc=reloc,words=words),self.assertRaises(AssertionError):relocated_words(words,reloc,{'local':0},0,0)
        with self.assertRaises(ValueError):relocated_words([0],[(0,9,'local')],{'local':0},0,0)
        with self.assertRaises(ValueError):relocated_words([0],[(0,2,'missing')],{},0,0)
        with self.assertRaises(AssertionError):relocated_words([0x14000000],[(0,2,'_dm_code_10000000')],{},0,0)
        with self.assertRaises(AssertionError):relocated_words([0x90000000],[(0,3,'_dm_view_state')],{},0,1<<40)

    @unittest.skipUnless(sys.platform=='darwin' and shutil.which('clang'),'Apple ARM64 assembler required')
    def test_generated_profile_matches_owned_source(self):
        source=PROFILE/'view.s';profile=source.with_name('patches.py')
        spec=importlib.util.spec_from_file_location('test_owned_view',profile)
        assert spec is not None and spec.loader is not None
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        words,symbols,relocations=assemble(source)
        self.assertEqual(module.source_sha256,hashlib.sha256(source.read_bytes()).hexdigest())
        self.assertEqual((module.words,module.symbols,module.relocations),(words,symbols,relocations))
        # Every registered branch and page reference must relocate in a normal image.
        relocated=relocated_words(words,relocations,symbols,0x6190000,0x4724200)
        self.assertEqual(len(relocated),len(words))

if __name__=='__main__':unittest.main()
