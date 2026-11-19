"""Owned camera code generation and symbolic relocation, without game data."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import sys
import unittest
from types import SimpleNamespace

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

class FrameStamps(unittest.TestCase):
    """The keyboard dock reads a host present counter and a stamp the game writes.

    The adapter keeps no host pointer: +0xa8 is the counter the host advances each present (hle_os.c dm_guest_frame_tick),
    +0xb0 the keyboard frame's stamp and +0xb8 the touch panel's. Lrecent_keyboard compares the first two, so a stamp written
    through a pointer, or to another slot, silently disables the dock (the value stays 0) -- or faults, when the counter is read
    as an address. These read the generated words, not the source, so a stale patches.py fails too."""

    def setUp(self):
        module=_load('patches.py')
        self.words,self.symbols=module.words,module.symbols

    def _at(self,name):return self.symbols[name]//4

    def test_stamps_are_values_not_pointers(self):
        # ldar x16,[x16] is the old host-pointer form: the counter itself would be dereferenced. Each hook's own body is
        # checked, up to the next symbol, so one hook's ldar cannot hide behind another's span.
        order=sorted((off,name) for name,off in self.symbols.items())
        for name in ('_dm_view_keyboard_window','_dm_view_keyboard_frame','_dm_view_touch_page'):
            at=self.symbols[name]//4
            end=min((off for off,_ in order if off>self.symbols[name]),default=self.symbols[name]+0x200)//4
            span=self.words[at:end]
            self.assertTrue(span,f'{name}: empty span')
            self.assertFalse([w for w in span if (w&0xfffffc00)==0xc8dffc00],f'{name}: atomic load through the stamp, which is a counter value')

    def test_keyboard_window_stamps_the_counter(self):
        # adrp/add state; ldr x16,[state+0xa8]; add x15,#56; stlr x16,[x15] -- the keyboard dialog's own stamp (+0x38).
        # It must not be +0xb0: that is the keyboard frame's stamp, the only one the dock compares, and the dialog also
        # draws on presents where the frame hook did not run.
        at=self._at('_dm_view_keyboard_window')
        tail=self.words[at:at+0x200]
        seq=None
        for i in range(len(tail)-4):
            if ((tail[i]&0x9f00001f)==0x9000000f and (tail[i+1]&0xffc003ff)==0x910001ef
                and tail[i+2]==0xf94055f0 and tail[i+3]==0x9100e1ef):
                seq=i
        self.assertIsNotNone(seq,'keyboard window: no ldr x16,[state+0xa8] then add x15,#56 (the +0x38 dialog stamp)')
        self.assertEqual(tail[seq+4]&0xfffffc00,0xc89ffc00,f'keyboard window: the stamp after add #56 must be stlr x16,[x15], got {tail[seq+4]:#x}')

    def test_stamp_offsets_are_the_free_slots(self):
        # The hooks pass the slot offset in x17 (Ltouch_stamp adds it to the page base): 8 = +0xb0, 16 = +0xb8.
        for name,slot in (('_dm_view_keyboard_frame',8),('_dm_view_touch_page',16)):
            at=self._at(name)
            span=self.words[at:at+0x40]
            self.assertIn(0xd2800000|(slot<<5)|17,span,f'{name}: mov x17,#{slot} (state + {slot:#x}) not found')


class ChainedStubSavesX30(unittest.TestCase):
    """A hand-emitted stub that calls and then returns must save x30.

    The bl writes its own return address into x30, so a stub that saved only its argument register returned to the
    instruction after that bl and looped, popping 16 bytes each pass until the store met an unmapped page. A stub that
    leaves through a tail b has no such contract: x30 is free after it. These assemble the profile's screen_shape stub
    with a fake context and read the words it emits, so the assertion holds for every stub of the build."""

    BASE, STATE = 0x8000000, 0x9000000

    def setUp(self):
        self.words, self.puts = [], []
        self.ctx = SimpleNamespace(here=lambda: self.BASE + 4 * len(self.words), emit=self.words.append,
                                   site=lambda off, orig: None, put=lambda off, words, why: self.puts.append((off, list(words))),
                                   state=self.STATE)

    def _stub(self, off):
        # the branch put at a site names its stub; a bl there calls one directly and has no stub of its own
        word = next(w for o, ws in self.puts if o == off for w in ws)
        if word & 0xfc000000 != 0x14000000: return None
        delta = word & 0x3ffffff
        if delta & 0x2000000: delta -= 0x4000000
        at = (off + 4 * delta - self.BASE) // 4
        # A branch may also go back into the game's own text (the capture manager's skipped destroy), which is no stub
        # this test can bound
        return at if 0 <= at < len(self.words) else None

    @staticmethod
    def _end(words, at):
        for i in range(at, len(words)):
            if words[i] == 0xd65f03c0: return i + 1, 'ret'
            if words[i] & 0xfc000000 == 0x14000000: return i + 1, 'b'
        return len(words), None

    @staticmethod
    def _saves_x30(words, at, end):
        # any store of x30 through sp: stp xN,x30,[sp,#imm] (pre, post or offset) or str x30,[sp,#imm]
        for w in words[at:end]:
            if w & 0xff800000 in (0xa9000000, 0xa9800000, 0xa8c00000) and (w >> 10) & 31 == 30 and (w >> 5) & 31 == 31: return True
            if w & 0xffc00000 == 0xf9000000 and w & 31 == 30 and (w >> 5) & 31 == 31: return True
        return False

    def test_every_stub_that_returns_from_a_call_saves_x30(self):
        patches.FRAME_STEP['ff1d1c05670db6021c85b624a710b963'](self.ctx)
        checked = 0
        for off, _ in self.puts:
            at = self._stub(off)
            if at is None: continue
            end, how = self._end(self.words, at)
            calls = [w for w in self.words[at:end] if w & 0xfc000000 == 0x94000000]
            if how != 'ret' or not calls: continue
            checked += 1
            self.assertTrue(self._saves_x30(self.words, at, end), f'{off:#x}: a stub that calls and returns must save x30')
        self.assertTrue(checked, 'no stub of this build calls and returns')

    def test_the_0x265140_chain_pairs_its_saves(self):
        # The chained stub at nn::ui2d::DrawInfo::SetProjectionMtx: after the upstream y-row division, stp x0,x30 / adrp
        # x15 / bl the adapter / ldp x0,x30 / ret.
        patches.FRAME_STEP['ff1d1c05670db6021c85b624a710b963'](self.ctx)
        at = self._stub(0x265140)
        self.assertIsNotNone(at, '0x265140: no stub branch at the chained site')
        end, how = self._end(self.words, at)
        self.assertEqual(how, 'ret', '0x265140: the chain must return, not branch on')
        body = self.words[at:end]
        pair = [i for i, w in enumerate(body) if w == 0xa9bf7be0]
        self.assertEqual(len(pair), 1, f'0x265140: one stp x0,x30,[sp,#-16]!, got {pair}')
        i = pair[0]
        self.assertEqual(body[i + 1] & 0x9f00001f, 0x9000000f, '0x265140: x15 is the state the adapter reads')
        self.assertEqual(body[i + 2] & 0xfc000000, 0x94000000, '0x265140: the bl follows the state')
        self.assertEqual(body[i + 3], 0xa8c17be0, f'0x265140: ldp x0,x30,[sp],#16 follows the bl, got {body[i + 3]:#x}')
        self.assertEqual(body[-1], 0xd65f03c0)


    def test_every_branch_targets_its_own_stub(self):
        # A branch's immediate names a word count from its own address. A stub that was lengthened and left its
        # branches alone still assembles, still runs, and jumps into the middle of another sequence: the +44 that
        # used to reach the trailing ret reaches the ldp before it, popping 16 bytes the stub never pushed. Each
        # target must therefore be an instruction of the stub it belongs to, and never the stub's tail branch.
        patches.FRAME_STEP['ff1d1c05670db6021c85b624a710b963'](self.ctx)
        # A hook jumps into the adapter's own body, whose internal branches are the assembly's labels, not a stub
        # this test can bound: only the stubs the profile emits here are checked.
        hooks = {off for off, _, _, _ in patches._view_hooks}
        checked = 0
        for off, _ in self.puts:
            if off in hooks: continue
            at = self._stub(off)
            if at is None: continue
            end, _ = self._end(self.words, at)
            for i in range(at, end):
                w = self.words[i]
                if w & 0xff000010 == 0x54000000: imm, cond = (w >> 5) & 0x7ffff, True      # b.cond
                elif w & 0x7e000000 == 0x36000000: imm, cond = (w >> 5) & 0x3fff, True     # tbz / tbnz
                elif w & 0x7e000000 == 0x34000000: imm, cond = (w >> 5) & 0x7ffff, True    # cbz / cbnz
                else: cond = False
                if not cond: continue
                if imm & (1 << (19 if w & 0x7e000000 == 0x36000000 else 18)): imm -= 1 << (20 if w & 0x7e000000 == 0x36000000 else 19)
                target = i + imm
                checked += 1
                self.assertTrue(at <= target < end, f'{off:#x}: a branch at word {i - at} leaves its stub ({target - at})')
        self.assertTrue(checked, 'no conditional branch in any stub of this build')


class DuoAnchorMatchesItsSource(unittest.TestCase):
    """The HUD adapter's words come from duo_anchor.c, and nothing regenerates them.

    view.s has a generator, a source hash and a test; duo_anchor.c's __text is pasted into patches.py by hand.
    Compiling the source here makes a change to one without the other a failure instead of a silent drift in
    the running game. Apple's clang is the same tool the comment above the words names."""

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'Apple ARM64 assembler required')
    def test_embedded_words_are_the_sources_text(self):
        import struct, subprocess, tempfile
        source = PROFILE / 'duo_anchor.c'
        with tempfile.TemporaryDirectory() as folder:
            obj = Path(folder) / 'duo_anchor.o'
            subprocess.run(['clang', '-arch', 'arm64', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                            '-fno-jump-tables', '-fno-asynchronous-unwind-tables', '-c', str(source), '-o', str(obj)], check=True)
            blob = obj.read_bytes()
        cursor, text = 32, None
        for _ in range(struct.unpack_from('<I', blob, 16)[0]):
            cmd, size = struct.unpack_from('<II', blob, cursor)
            if cmd == 0x19:
                for j in range(struct.unpack_from('<I', blob, cursor + 64)[0]):
                    at = cursor + 72 + 80 * j
                    if blob[at:at + 16].rstrip(b'\0') == b'__text':
                        text = struct.unpack_from('<Q', blob, at + 40)[0], struct.unpack_from('<I', blob, at + 48)[0]
            cursor += size
        self.assertIsNotNone(text, 'duo_anchor.o has no __text')
        length, offset = text
        words = list(struct.unpack('<%dI' % (length // 4), blob[offset:offset + length]))
        anchor = _load('patches.py')._duo_anchor   # read fresh: the module-level copy is the one this file loaded first
        embedded = [int(anchor[i:i + 8], 16) for i in range(0, len(anchor), 8)]
        self.assertEqual(len(words), len(embedded), 'duo_anchor.c and the words in patches.py differ in length: recompile it')
        for i, (got, want) in enumerate(zip(words, embedded)):
            self.assertEqual(got, want, f'duo_anchor.c word {i} is {got:#010x}, patches.py has {want:#010x}: recompile it')


if __name__=='__main__':unittest.main()
