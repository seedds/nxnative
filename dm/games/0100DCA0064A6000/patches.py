# Luigi's Mansion 3 (0100DCA0064A6000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_STUBS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py.
from nso_asm import ADDI, ADRP, B, LDRX

# Ghost Gallery / billboard skeleton lookup crash (GitHub issue #672): SIGSEGV / SIGBUS at main+0x4723a0 / +0x4723c8 (and +0x47241c in normal play) on 1.4.0.
# Inside RenderManager::RenderFrame (main+0x464920 on 1.4.0, main+0x41a4b0 on 1.0.0), the pass over ECS component list 0x3cd (vtable 0x16babf0: screen-space glow/halo
# billboards) projects up to 5 bones of each entity's model ([comp + 0xb8] -> vtable +0x98 -> skeleton instance) into screen space and builds a 4-vertex quad.
# For each bone hash in the list it takes the skeleton handle ([x0]), indexes the engine's handle table (main+0x18868e0 on 1.4.0, main+0x17dd8d8 on 1.0.0; 0x20000
# pointers indexed by handle & 0x1ffff, u16 generation per slot at table + 0xffff8 + slot * 2 matched against handle >> 17), binary-searches the skeleton's sorted
# {hash, index} pairs (count +0x14, offset +0xc) and reads the bone matrix at [x0 + 0x10] + index * 64.
#
# Two bugs in this reader:
# 1. Unlike 113 other skeleton handle lookups in main (including component 0x3cd's own activation helper main+0x48b7b0), it does not validate the handle's generation or
#    the slot pointer. When ProcessBundleUnloadQueue releases a ghost's skeleton resource while its component 0x3cd is still registered, the handle-free routine stores
#    an intrusive free-list pointer (next_slot | 1) in the slot and bumps its generation (initialized to 0x12b3 at startup), so the unchecked lookup dereferences an odd
#    non-pointer — or, if the slot was reallocated, an unrelated engine object.
# 2. When a skeleton has 0 bone pairs or a bone hash is not found, the search sets the index to -1 and falls into the matrix read, where zero-extending w28/w27 adds
#    0xffffffff * 64 (+256 GiB) and faults immediately (+0x47241c).
#
# The stub validates x0 != 0, handle generation != 0 and matching table + 0xffff8 + slot * 2, slot pointer != 0 and even, and bone count != 0 before resuming the binary
# search at `ldr w11, [x9, #0xc]`. Any invalid handle, 0-entry skeleton, or missing bone hash branches to a clean component-skip exit (restoring the clobbered component
# loop index and x22 = 0x100000000 and branching to the loop's existing clean skip target 0x4730b0 / 0x427960) rather than advancing the inner bone loop, which would
# still fall through to build a quad from uninitialized stack coordinates.
#
# Both complete RenderManager::RenderFrame functions and the handle-free function (asserting the same handle-table base and +0xffff8 generation-table layout) are matched
# by MATCH_STUBS for 1.4.0 and 1.0.0. What queues a Vault ghost bundle for unload while its component 0x3cd is still active remains open in README.md.


def _cbz(is64, reg, frm, to):
    off = to - frm
    assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0
    return (0xB4000000 if is64 else 0x34000000) | (((off >> 2) & 0x7FFFF) << 5) | reg


def _bne(frm, to):
    off = to - frm
    assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0
    return 0x54000001 | (((off >> 2) & 0x7FFFF) << 5)


def _tbnz_w(reg, bit, frm, to):
    off = to - frm
    assert -(1 << 15) <= off < (1 << 15) and off % 4 == 0
    return 0x37000000 | ((bit & 0x1F) << 19) | (((off >> 2) & 0x3FFF) << 5) | reg


def _make_skeleton_guard(lookup_off, bne_off, not_found_off, not_found_orig, skip_target_off, idx_reg, idx_sp_off):
    def stub(ctx):
        other = ctx.others.get('handle free')
        if other is None:
            return
        lookup = ctx.base + lookup_off
        bne_site = ctx.base + bne_off
        not_found = ctx.base + not_found_off
        skip_target = ctx.base + skip_target_off
        table = ctx.address(lookup + 8)
        assert table == ctx.address(other + 0x24), 'handle table address disagrees with handle free routine'

        ctx.site(lookup, '080040b9')        # ldr w8, [x0]
        ctx.site(bne_site, 'a1000054')      # b.ne +0x14 (falls into matrix read with index -1)
        ctx.site(not_found, not_found_orig) # mov w28/w27, #-1

        start = ctx.here()
        skip = start + 18 * 4
        ctx.emit(_cbz(True, 0, ctx.here(), skip))          # cbz x0, skip
        ctx.emit(0xB9400008)                               # ldr w8, [x0]
        ctx.emit(0xD3517D0A)                               # ubfx x10, x8, #17, #15 (handle generation)
        ctx.emit(_cbz(False, 10, ctx.here(), skip))        # cbz w10, skip
        ctx.emit(0x92404108)                               # and x8, x8, #0x1ffff (slot index)
        ctx.emit(ADRP(9, ctx.here(), table))               # adrp x9, table
        ctx.emit(ADDI(9, 9, table & 0xFFF))                # add x9, x9, #(table & 0xfff)
        ctx.emit(0x8B08052B)                               # add x11, x9, x8, lsl #1
        ctx.emit(0x321D43EC)                               # mov w12, #0xffff8 (generation table offset)
        ctx.emit(0x786C696B)                               # ldrh w11, [x11, x12]
        ctx.emit(0x6B0B015F)                               # cmp w10, w11
        ctx.emit(_bne(ctx.here(), skip))                   # b.ne skip (stale or reallocated slot)
        ctx.emit(0xF8687929)                               # ldr x9, [x9, x8, lsl #3]
        ctx.emit(_cbz(True, 9, ctx.here(), skip))          # cbz x9, skip (null slot)
        ctx.emit(_tbnz_w(9, 0, ctx.here(), skip))          # tbnz w9, #0, skip (free-list pointer)
        ctx.emit(0xB940152A)                               # ldr w10, [x9, #0x14] (bone pair count)
        ctx.emit(_cbz(False, 10, ctx.here(), skip))        # cbz w10, skip (empty skeleton)
        ctx.emit(B(ctx.here(), lookup + 0x1C))             # resume at `ldr w11, [x9, #0xc]`
        assert ctx.here() == skip
        ctx.emit(LDRX(idx_reg, 31, idx_sp_off // 8))       # restore component loop index clobbered in bone loop
        ctx.emit(0xD2C00036)                               # movz x22, #1, lsl #32 (0x100000000)
        ctx.emit(B(ctx.here(), skip_target))               # skip component cleanly without submitting a quad

        ctx.put(lookup, [B(lookup, start)], 'billboard skeleton lookup (#672): check handle generation, pointer and bone count')
        ctx.put(bne_site, [0x54000081], 'billboard skeleton lookup (#672): b.ne to missing-bone skip instead of index -1')
        ctx.put(not_found, [B(not_found, skip)], 'billboard skeleton lookup (#672): skip component cleanly when bone hash is not found')

    return stub


_name = 'RenderManager::RenderFrame billboard skeleton lookup validates handle and skips missing bones (#672)'
MATCH_STUBS = {'main': [
    # Update 1.4.0 (v327680): main+0x464920..+0x478dc0 (lookup +0xda34, skip target +0xe790, index x16 at [sp, #0xf0]).
    {
        'name': _name,
        'size': 0x144A0,
        'fingerprints': ['e4d34b17afc79741ba32931565877eeaf4ad03e34f5f22a0dac8362888e63e05'],
        'others': {
            'handle free': {
                'size': 0xE0,
                'fingerprints': ['f77feb3dc49209cac60bfa2a2cafd4987a7aac6fb35ac82f2f97d439accd7046'],
            },
        },
        'stub': _make_skeleton_guard(0xDA34, 0xDACC, 0xDADC, '1c008012', 0xE790, 16, 0xF0),
    },
    # Base 1.0.0 (v0): main+0x41a4b0..+0x42b7bc (lookup +0xc758, skip target +0xd4b0, index x23 at [sp, #0xe8]).
    {
        'name': _name,
        'size': 0x1130C,
        'fingerprints': ['cb6fb03a8c91ee98f2f03bca6e0ccdd7828ea94349b42140715151018e394ca2'],
        'others': {
            'handle free': {
                'size': 0xE0,
                'fingerprints': ['e26519e154670dae9f408cc447c7c45e935508ed3e0a32b3124046c38cc39c2a'],
            },
        },
        'stub': _make_skeleton_guard(0xC758, 0xC7F0, 0xC800, '1b008012', 0xD4B0, 23, 0xE8),
    },
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': _name, 'now': 'game', 'goal': 'game', 'purpose': 'runtime',
     'why': "the game's own RenderManager::RenderFrame billboard pass omits the handle generation check used by 113 other skeleton lookups and dereferences index -1 on a missing bone hash; function-matched (MATCH_STUBS) across 1.0.0 and 1.4.0 with handle-table layout assertion",
     'checked': 'base 1.0.0 (v0) and update 1.4.0 (v327680) mains: RenderManager::RenderFrame, handle-free routine, handle-table initialization and component 0x3cd verified by disassembly, full rewrites and Unicorn replay of valid, freed/odd, reallocated/wrong-generation, zero-generation, null-slot, zero-count and missing-bone paths (2026-11-28)'},
]
