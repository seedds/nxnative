# Scope issue: port AnimalDuo's adaptive-view adapter to ACNH without general-code changes

This is a design document, not an implementation. It asks for scope agreement before any code is written, as `CONTRIBUTING.md` requires for a change that touches an existing path and runs to more than about 1,000 lines.

Status: proposed. No file in this repository has been changed. Nothing here has been compiled, assembled, tested or run on a device.

---

## 1. What and why

AnimalDuo is a downstream source snapshot of nxnative (base revision `b7a5cd43453722d448e07407d88a9c05ed294492`, 2026-10-27; local `main` is 923 commits later). It carries an adaptive-view adapter for Animal Crossing: New Horizons 3.0.3 (`main` build ID `ff1d1c05670db6021c85b624a710b963`) that keeps cached projections, scissors, captures, field ranges, keyboard and transition layouts consistent when the presented picture changes shape.

The upstream profile `dm/games/01006F8002326000/` already has `screen_shape`, a `FRAME_STEP` callback with 14 stub sites that make the game render a taller picture. The two are complementary, not duplicates:

- The upstream 14 sites own the size sources, the per-frame crop and render scale, the camera field of view, and the layouts' DrawInfo sizes.
- The adapter's 22 hooks and 29 caches own projection-cache invalidation, scissor and capture consistency, 3D instantiation ranges, near-plane unprojection, and UI details (footer, KeyGuide, inline keyboard, minimap, backdrop UVs).
- Precisely one site collides: `main+0x265140`, the single `ret` of `nn::ui2d::DrawInfo::SetProjectionMtx`.
- `main+0x27d4c0` (adapter footer recalculation, at `Pane::Calculate` entry) and `main+0x27d570` (upstream `duo_calc`, after `CalculateGlobalMatrix`) were disassembled and confirmed to coexist: one forces a dirty bit, the other writes the pane's global matrix. No conflict.

The port adds roughly 2,000 lines of profile-local ARM64 and Python. It changes **no general code**: `patch_nso.py`, `loader.c`, `main.c`, `front.h`, `hle_touch.c`, `nso_asm.py` and `test_patches.py` are untouched. It adds no `VIEW` rewriter table, no `VIEW_STATE_OFF`, no `DMVIEW01` block, no `hle_view.c`, and no `dm_view_state_apply()`.

The decision to eliminate general-code changes has one deliberate consequence: the adapter no longer supports arbitrary aspect ratios 0.75-3.5 or folding-display rotation. It consumes the aspect the host already publishes at `dm_fps_state +0x58` (`r = 1920 / oh`, limited by `DM_SCREEN_ASPECT` to 1.3-1.77 and quantised to multiples of 48). What remains is the guest-side consistency work, which is the part upstream does not have.

### Decisions already taken

| Decision | Choice |
|---|---|
| `main+0x265140` index 6 | Additive: `ldr s3; fadd s1,s3,s1; str`, with the adapter's value first divided by the vertical stretch (option B). Option A (plain `fadd`) is kept as a documented fallback. |
| State page | Reuse `FRAME_STEP`'s `dm_fps_state`; `+0x58` serves as both target aspect and folding capability. |
| General-code changes | Zero, including changes caused by implementation choices and changes caused by capability. |

### Explicitly out of scope

- `pso_manifest.tsv` union (separate change; static validation passed, runtime validation still needed).
- LDN local wireless (reference open PR #871, do not reimplement).
- Everything in AnimalDuo's host and iOS layers that upstream already has, is a graphical front end, or is a duplicate: pool index, `hle_thread.c` setjmp/longjmp workaround, smooth present, mods, save sync, companion UI, resident manager, vendored GPL third-party code.

---

## 2. Constraints and success criteria

| Item | Constraint |
|---|---|
| General code | Zero lines changed in `dm/tools/patch_nso.py`, `dm/host/loader.c`, `main.c`, `front.h`, `hle_touch.c`, `dm/tools/nso_asm.py`, `dm/tools/tests/test_patches.py` |
| New host files | None (no `hle_view.c`) |
| New rewriter table | None (no `VIEW`, no `VIEW_STATE_OFF`, no `DMVIEW01`) |
| State page | `FRAME_STEP`'s `FS_STATE_OFF` (this image: `0x5a0c000`), page size `0x1000`, upstream uses through `+0x9c` |
| Target aspect | `dm_fps_state +0x58` (`r = 1920/oh`; 1.43 gives 1.4285714) |
| `main+0x265140` | One rewrite site, one stub that chains upstream bit 6 then the adapter dock |
| Licence | `view.LICENSE` (MIT-0) stays with the new file; `view.s` keeps its MIT-0 attribution comment |

Success criteria, all verifiable:

1. `test_patches.py` passes: no `VIEW` table, no unwhitelisted uppercase name, no build ID claimed twice.
2. `main.cuts.tsv` has exactly one cut at `0x265140`; `0x27d4c0` and `0x27d570` each have one and do not overlap.
3. `main.fixups.tsv` has `FPS_STATE_OFF` and **no** `VIEW_STATE_OFF`.
4. `dmsh_chain.py --header` is not needed (no chain input change).
5. Mac routes at 1.43 and 16:9, keyboard open and closed, and `regress.py check --games` on five games.

---

## 3. Mechanism that makes zero general-code changes possible

Facts verified in the current tree:

| Fact | Source | Consequence |
|---|---|---|
| `FRAME_STEP` context has `state = FS_STATE_OFF` | `patch_nso.py:1671` | All adapter logic must live in the `FRAME_STEP` callback |
| `STUBS` context has **no** `state` | `patch_nso.py:1682` | Do not register `STUBS` |
| `test_patches.py:69` whitelist has no `VIEW` | `test_patches.py` | The `VIEW` table must be deleted |
| `test_patches.py:47` deduplicates per kind | `test_patches.py` | One build ID in one `FRAME_STEP` is fine |
| `here()` returns `PATCH_BASE + len(patch)` (absolute address) | `patch_nso.py:267` | `B`/`BL` take `here()` values directly |
| `stub_put` covers `len(words)` original instructions | `patch_nso.py:1655` | `0x265140` (one `ret`) takes exactly one `B` word |
| `stub_site` asserts the address is not already a rewrite site | `patch_nso.py:1650` | `site(0x265140, ...)` must be called once |
| `B`/`BL` reach is +/-128 MB; `0x265140` to `0x5a10000` is 91.9 MB | `nso_asm.py` | The chain stub is reachable |
| Upstream bit 9 pattern: emit machine code, then assemble the stub, then `put` | `patches.py:149-163` | Copy this ordering |
| `dm_fps_state` is read-write, used through `+0x9c`, 3,940 bytes free | `loader.c:344-366` | `+0xa0`..`+0xb8` are free for frame stamps |
| `gen_view_stubs.py` resolves the `_dm_view_state` symbol to its `state` argument | `patches.py:278`, `gen_view_stubs.py` | Only `ctx.state` changes; the 64 relocations stay as they are |

The adapter is therefore folded into the existing `screen_shape(ctx)` callback: the upstream 14 sites keep their code verbatim, the adapter's words and hooks are appended to the same patch area, and the one colliding site becomes a single chained stub.

---

## 4. File changes

| File | Action | Note |
|---|---|---|
| `dm/games/01006F8002326000/view.s` | Add (copy from AnimalDuo, then modify per section 5) | 1,658 lines of ARM64 |
| `dm/games/01006F8002326000/view.LICENSE` | Add | MIT-0 full text |
| `dm/games/01006F8002326000/gen_view_stubs.py` | Add (moved from AnimalDuo `dm/tools/`) | 63 lines; keep it profile-local so other games never load it |
| `dm/games/01006F8002326000/patches.py` | Modify | Merge `screen_shape` and the adapter's `view()`; delete the `VIEW` table |
| `dm/games/01006F8002326000/README.md` | Modify | One new section, marked current, per `AGENTS.md:15` |
| `docs/reference/rewriter.md` | Modify | New Symptom / Cause / Fix / Found on entry, then `python3 dm/tools/docs_symptoms.py` |
| `docs/agents/game-patches.md` | Regenerate | `python3 dm/tools/docs_patches.py` |
| `dm/host/tests/t_view_adapter.c` and three siblings | Add | Copy from AnimalDuo, remove the `hle_view.c` dependency |

Not added: `dm/tools/gen_view_stubs.py`, `dm/host/hle_view.c`, any `VIEW`-related fixup or header declaration.

---

## 5. `view.s` changes

### 5.1 State-page base: one change, not many

`gen_view_stubs.py`'s `relocated_words()` resolves the `_dm_view_state` symbol to the `state` argument passed at `patches.py:278`. Calling it with `STATE` (`FS_STATE_OFF`) is enough. All `adrp x15, _dm_view_state@PAGE` / `add ... @PAGEOFF` pairs stay verbatim; all 64 relocation entries keep their `_dm_view_state` names.

### 5.2 Field remapping

Replace the exact `[x15,#N]` offsets. Verify each replacement by counting.

| Old offset | Occurrences | New offset | Meaning |
|---|---|---|---|
| `[x15, #8]` | 10 | `[x15, #0x58]` | target aspect (`r`) |
| `[x15, #12]` | 19 | `[x15, #0x58]` | folding capability (same nonzero test) |
| `[x15, #16]` (`str w16`) | 1 | `[x15, #0xa0]` | layout match flag |
| `[x15, #24]` (`ldr x16`) | 3 | `[x15, #0xa8]` | frame stamp, now a direct value instead of a host pointer |
| `[x15, #32]` | 1 | `[x15, #0xb0]` | UI frame stamp |
| `[x15, #40]` / `stlr x16,[x15]` | see 5.3 | `[x15, #0xb8]` | second UI frame stamp |

`+0xa0`..`+0xb8` sit after `+0x9c` in the free area. No overlap with upstream fields.

`+0x58` is written only inside `loader.c`'s `DM_SCREEN_ASPECT` branch. When the variable is unset, `0`, or `auto`, `+0x58` stays zero and every adapter stub falls back to the original instructions. That matches the adapter's old "0 = no adaptation" meaning; it also means `auto` on iOS gets no adaptation, which must be recorded in the game note.

The quantisation difference is real but harmless for the comparisons used: `r = 1920/1344 = 1.428571` versus `DM_SCREEN_ASPECT = 1.43`, a 0.1% difference because `oh` is a multiple of 48. The `fcmp s1, 16/9` test is unaffected.

### 5.3 Remove the host frame-counter pointer

The old `Ltouch_stamp` read a host pointer from `+0x18` and wrote through it. With zero host changes, that pointer does not exist. Rewrite both helpers to use direct stamps:

```
Ltouch_stamp:
    adrp x15, _dm_view_state@PAGE
    add  x15, x15, _dm_view_state@PAGEOFF
    ldr  x16, [x15, #0xa8]
    add  x16, x16, #1
    add  x15, x15, x17          ; x17 = 32 or 40, unchanged
    stlr x16, [x15]
Ltouch_stamp_done:
    ret
```

Delete the `cbz x16, Ltouch_stamp_done` line (there is no pointer to be null any more).

```
Lrecent_keyboard:
    ldr  x16, [x15, #0xa8]
    ldr  x17, [x15, #0xb0]
    cbz  x17, Lrecent_done
    sub  x17, x16, x17
    cmp  x17, #2
    b.hi Lrecent_done
    mov  w8, #1
Lrecent_done:
    ret
```

Delete the old `ldar x16,[x16]` and `add x16,#1`. The `_dm_view_keyboard_frame` (`0x326817c`) and `_dm_view_touch_page` (`0x2af1258`) hooks stay unchanged and still call `Ltouch_stamp`, so the "inline keyboard rendered in the last two original presents" condition survives with no host help.

### 5.4 Index 6 additive write (decision 1, option B)

In `_dm_view_panel_projection`, replace `str s1, [x0, #0x18]` with:

```
    ldr s3, [x0, #0x18]        ; upstream bit 6 has already divided this by the stretch
    ldr s4, [x16, #0x84]       ; dm_fps_state +0x84 = vertical stretch
    fdiv s1, s1, s4            ; normalise the adapter's 1 - r/(16/9) to the same scale
    fadd s1, s3, s1
    str s1, [x0, #0x18]
```

`x16` holds `STATE` because the chained stub sets `ADRP(16, ..., STATE)` before calling the adapter (section 6). The adapter wrapper saves only `x0`/`x30`, so `x16` survives.

Option A (omit the `fdiv`) is a one-line fallback. Both must be documented in a comment and marked open until a device run confirms the dock direction. The mixed units in option A are a known difference, not a hidden bug.

### 5.5 Delete

- The `DMVIEW01` header, `ctx.site(ctx.state, "00"*64)` and `ctx.put(ctx.state, header, ...)`.
- Anything referencing `hle_view.c` or `dm_view_state_apply()`.

---

## 6. The `main+0x265140` chained stub

Disassembly of the AnimalDuo-rewritten `main.patched.bin` (build ID `ff1d1c05670db6021c85b624a710b963`, fixups containing `VIEW_STATE_OFF 0x4724200`) established:

- `0x265140` is `d65f03c0` (`ret`), the only return of `SetProjectionMtx`. `0x265158` and `0x265184` are `ret`s of two following functions, not alternate hook points.
- The upstream bit 6 stub divides seven matrix elements (indices 1, 4, 5, 6, 7, 9, 13) by the vertical stretch, gated on `|m5|/|m0|` being within 5% of 16/9.
- The adapter's `_dm_view_ui_projection` (at `code+1576`) is a wrapper: `add x0,x0,#0x10`, `bl _dm_view_panel_projection`, `ret`.
- `_dm_view_panel_projection` writes only index 6 (`DrawInfo+0x28`) with `1 - r/(16/9)`, gated on a verified 40-degree lens projection fingerprint, `r < 16/9`, and a recent inline keyboard.
- `Lscreen_projection` also writes m0 and m5. Since the upstream 16:9 test reads exactly those two, **the upstream test must run first**. The chained order guarantees this; the comment must say so.

The stub is assembled after the adapter words have been emitted, so the `BL` back into `view_base + symbols["_dm_view_ui_projection"]` has its target known. `bl_at = here()` is captured before emitting the `BL`, because `_bl(frm, to)` needs the address of the BL instruction itself.

---

## 7. `screen_shape(ctx)` complete pseudocode skeleton

### 7.1 Module level

```python
import hashlib, struct
from pathlib import Path
from nso_asm import ADRP, B, BL, STP_PRE, STRX, LDP_POST   # whitelisted by test_patches.py

# ==== BEGIN GENERATED VIEW CODE (written by gen_view_stubs.py; do not edit) ====
source_sha256 = '<sha256 of view.s>'
words = [ ... ]              # 1,478 words; changes after the offset remap
symbols = { '_dm_view_matrix': 0, '_dm_view_cache': 64, ...,
            '_dm_view_ui_projection': 1576, '_dm_view_panel_projection': 1596, ... }
relocations = [ (4, 3, '_dm_view_state'), (8, 4, '_dm_view_state'), ... ]   # 64 entries; _dm_view_state names unchanged
# ==== END GENERATED VIEW CODE ====

def _bl(frm, to): return 0x94000000 | (((to - frm) >> 2) & 0x3FFFFFF)

def relocated_words(words, relocations, symbols, address, state):
    """Moved down from AnimalDuo's nso_asm.py. Resolves _dm_view_state to state."""
    result = list(words)
    for offset, kind, name in relocations:
        pc = address + offset; word = result[offset // 4]
        if name == '_dm_view_state':            target = state
        elif name.startswith('_dm_return_'): target = int(name[len('_dm_return_'):], 16)
        elif name.startswith('_dm_code_'):   target = int(name[len('_dm_code_'):], 16)
        elif name in symbols:                target = address + symbols[name]
        else: raise ValueError('Unknown adapter relocation: ' + name)
        if kind == 2:    # B / BL
            assert word & 0x7c000000 == 0x14000000
            result[offset//4] = (word & 0xfc000000) | (B(pc, target) & 0x3ffffff)
        elif kind == 3:  # ADRP
            assert word & 0x9f000000 == 0x90000000
            result[offset//4] = ADRP(word & 31, pc, target)
        elif kind == 4:  # ADD (page offset)
            assert word & 0xffc00000 == 0x91000000
            result[offset//4] = (word & ~(0xfff << 10)) | ((target & 0xfff) << 10)
        else: raise ValueError('Unknown adapter relocation kind')
    return result

# Upstream constants, kept verbatim
_size_sites = [ ... ]                 # 5 entries
_duo_anchor = '...'                   # hex string
_duo_calc = 0x0
_pane_draw_self = 0x27e2c8
_tanf_plt, _atanf_plt = 0x3f03330, 0x3f03360

# Adapter tables copied from AnimalDuo view(), with the 0x265140 hook removed
_VIEW_HOOKS = [ (0x283e9c, 'e184ff97', '_dm_view_transition_fast_call', True), ... ]   # 21
_VIEW_CACHES = [ (0x15083c, 20, 8, '88224039'), ... ]                                   # 29
_VIEW_SITES = [ (0x4a65660, '68597e0300000000'),
                (0x474de58 + 21*8, struct.pack('<Q', 0x29a17c).hex()),
                (0x269134, '000d40bd602a00bd001140bd08710091682603a9602e00bd') ]
```

`relocated_words` is copied with its original `kind == 4` mask `0xffc00000`. Do not "fix" it.

### 7.2 The callback

```python
def screen_shape(ctx):
    here, emit, site, put, STATE = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state

    # [A] view.s must match the generated words
    src = Path(__file__).with_name('view.s')
    assert hashlib.sha256(src.read_bytes()).hexdigest() == source_sha256, \
        'regenerate view stubs after editing view.s'

    # [B] Upstream 14 sites, verbatim. Only 0x265140 differs: site() but no put().
    #   B1 _size_sites (bit 0-4)
    for at, ws, why in _size_sites:
        site(at, struct.pack('<I', ws[-1]).hex())
        s = here(); emit(ADRP(16, here(), STATE))
        for w in ws: emit(w)
        emit(B(here(), at + 4))
        put(at, [B(at, s)], why)

    #   B2 0x1527f00 / 0x1527f2c (bit 7)
    for at, orig, cvt, back in ((0x1527f00, 0x2d400500, 0x1e380010, 0x1e220201),
                                (0x1527f2c, 0x2d400d02, 0x1e380050, 0x1e220203)):
        site(at, struct.pack('<I', orig).hex())
        s = here(); emit(orig); emit(ADRP(16, here(), STATE))
        for w in (0xb9405611, 0x36380111, 0xb9404a11, 0x340000d1,
                  cvt, 0x1b117e10, 0x5280f011, 0x1ad10a10, back): emit(w)
        emit(B(here(), at + 4))
        put(at, [B(at, s)], 'resolution table entry ... (bit 7)')

    #   B3 0x31cee60 / 0x31cee9c (bit 8)
    for at, orig, ws in ((0x31cee60, 0xf2e890f8, (0x1e230230, 0x1e260211, 0xb3607e38)),
                         (0x31cee9c, 0xf2c08708, (0xb3607e28,))):
        site(at, struct.pack('<I', orig).hex())
        s = here(); emit(orig); emit(ADRP(16, here(), STATE))
        n = 3 + len(ws)
        emit(0xb9405611); emit(0x36400011 | (n << 5)); emit(0xb9404a11)
        emit(0x34000011 | ((n - 2) << 5))
        for w in ws: emit(w)
        emit(B(here(), at + 4))
        put(at, [B(at, s)], "layouts' DrawInfo ... (bit 8)")

    #   B4 0x7a895c / 0x7a89a8 (bit 5, field camera)
    for at in (0x7a895c, 0x7a89a8):
        site(at, '80ce42bd')
        s = here()
        for w in (0xbd42ce80, ADRP(16, here(), STATE), 0xb9405611, 0x362801b1,
                  0xb9404a11, 0x34000171, 0xf81f0fe0, 0x1e2c1001, 0x1e210800,
                  _bl(here(), _tanf_plt), ADRP(16, here(), STATE), 0xbd405e01,
                  0x1e210800, _bl(here(), _atanf_plt), 0x1e202800, 0xf84107e0,
                  B(here(), at + 4)): emit(w)
        put(at, [B(at, s)], 'field camera ... (bit 5)')

    #   B5 0x386a7a0 (bit 5, game camera)
    site(0x386a7a0, '0808291e')
    s = here()
    for w in (ADRP(16, here(), STATE), 0xb9404a11, 0x34000171, 0xb9405611,
              0x36280131, 0x1e2c1002, 0x1e220800, _bl(here(), _tanf_plt),
              ADRP(16, here(), STATE), 0xbd405e01, 0x1e210800,
              _bl(here(), _atanf_plt), 0x1e202800, 0x1e290808,
              B(here(), 0x386a7a4)): emit(w)
    put(0x386a7a0, [B(0x386a7a0, s)], 'game camera ... (bit 5)')

    #   B6 0x27d570 (bit 9, duo_calc) -- coexists with the adapter's 0x27d4c0
    code = here()
    for i in range(0, len(_duo_anchor), 8): emit(int(_duo_anchor[i:i+8], 16))
    site(0x27d570, '68624139')
    s = here()
    for w in (ADRP(16, here(), STATE), 0xb9405611, 0x36480111, 0xb9404a11,
              0x340000d1, 0xaa1303e0, 0x91020201,
              ADRP(2, here(), _pane_draw_self),
              0x91000042 | ((_pane_draw_self & 0xFFF) << 10),
              _bl(here(), code + _duo_calc), 0x39416268,
              B(here(), 0x27d574)): emit(w)
    put(0x27d570, [B(0x27d570, s)], 'Pane::Calculate ... duo_calc (bit 9)')

    #   B7 0x265140: site() ONLY. The original upstream put(B(...)) is removed.
    #       The chained stub in [D] owns this site.
    site(0x265140, 'c0035fd6')

    # [C] Three non-hook adapter site assertions
    for off, orig in _VIEW_SITES:
        site(off, orig)

    # [D] Adapter machine code, then the chained stub
    #     Order: emit all words first (view_base known), then assemble the stub,
    #     then put every hook / cache / the one 0x265140 branch.
    view_base = here()
    for w in relocated_words(words, relocations, symbols, view_base, STATE):
        emit(w)

    comb = here()
    emit(ADRP(16, here(), STATE))     # x16 = STATE, also used by section 5.4
    emit(0xb9404a11)                  # ldr w17,[x16,#0x48] output height
    emit(0x34000511)                  # cbz w17, ret (+41)
    emit(0xb9405611)                  # ldr w17,[x16,#0x54] sites mask
    emit(0x363004d1)                  # tbz w17,#6, ret (+39)
    # 16:9 test reads m0 / m5. MUST precede Lscreen_projection, which rewrites both.
    for w in (0xbd401001, 0x1e20c021, 0xbd402402, 0x1e20c042,
              0x5291c731, 0x72a7fc71, 0x1e270223, 0x1e230823,
              0x7ea3d443, 0x529999b1, 0x72a7a991, 0x1e270224,
              0x1e240824, 0x1e242060, 0x540002ec, 0xbd408600): emit(w)
    for i in (1, 4, 5, 6, 7, 9, 13):
        o = (0x10 + 4*i) // 4
        emit(0xbd400001 | (o << 10))  # ldr s1,[x0,#0x10+4i]
        emit(0x1e201821)              # fdiv s1,s1,s0
        emit(0xbd000001 | (o << 10))  # str s1,[x0,#0x10+4i]
    # Call the adapter's keyboard dock. The adapter uses x15 = STATE.
    emit(0xf81f0fe0)                  # str x0,[sp,#-16]!  save DrawInfo
    emit(ADRP(15, here(), STATE))     # x15 = STATE
    bl_at = here()                    # address of the BL instruction itself
    emit(_bl(bl_at, view_base + symbols['_dm_view_ui_projection']))
    emit(0xf84107e0)                  # ldr x0,[sp],#16
    emit(0xd65f03c0)                  # ret, replacing the original ret

    put(0x265140, [B(0x265140, comb)],
        'DrawInfo::SetProjectionMtx: y row divided by the stretch, then keyboard dock (bit 6, one chained stub)')

    # [E] 21 adapter hooks (0x265140 deliberately absent)
    for off, orig, sym, link in _VIEW_HOOKS:
        site(off, orig)
        put(off, [B(off, view_base + symbols[sym]) | (0x80000000 if link else 0)],
            'adaptive-view ' + sym)

    # [F] 29 adapter caches
    for off, src, dst, orig in _VIEW_CACHES:
        site(off, orig)
        body = here()
        for w in (STP_PRE(0,30,31,-16), STP_PRE(8,9,31,-16), STP_PRE(15,16,31,-16),
                  0xaa0003e0 | (src << 16)): emit(w)
        emit(B(here(), view_base + symbols['_dm_view_cache']) | 0x80000000)
        emit(STRX(8, 31, 2 if dst == 8 else 3))
        emit(LDP_POST(15,16,31,16)); emit(LDP_POST(8,9,31,16))
        emit(LDP_POST(0,30,31,16))
        emit(B(here(), off + 4))
        put(off, [B(off, body)], 'refresh cached projection after a view target change')

FRAME_STEP = { 'ff1d1c05670db6021c85b624a710b963': screen_shape }
BINDING = [ ... two entries ... ]
```

### 7.3 Six points that must not be got wrong

1. `site(0x265140, ...)` is called once. A second `site()` would assert because the address is already a rewrite site. That is why `_VIEW_HOOKS` has 21 entries, not 22.
2. `comb` sits after `view_base`. `B(0x265140, comb)` is about 92 MB, inside the 128 MB reach; `_bl(bl_at, view_base + 1576)` jumps back roughly 40 instructions.
3. `bl_at` is captured **before** `emit(_bl(...))`, because `_bl(frm, to)` needs the BL instruction's own address.
4. `put(0x265140, [B(...)])` passes exactly one word, matching the one original `ret`. The dozens of chained instructions live in the patch area.
5. The 16:9 test is emitted before the `BL`. `Lscreen_projection` rewrites m0/m5, so reordering would silently corrupt the test. Record this order constraint in a comment.
6. `x16` and `x15` are both set to `STATE`: `x16` by the upstream-style `ADRP(16, ...)`, `x15` before the call. The adapter wrapper saves only `x0`/`x30`, so neither is clobbered.

---

## 8. Interface contract with `view.s`

The skeleton depends on `view.s` satisfying all of these after the section 5 edits:

| Contract | How to check |
|---|---|
| `symbols["_dm_view_ui_projection"]` still exists and points at the wrapper | `gen_view_stubs.py` output |
| `relocations` `_dm_view_state` entries unchanged | generator resolves them against `ctx.state` |
| Every `[x15,#8]` and `[x15,#12]` became `[x15,#0x58]` | replacement script, then count |
| `Ltouch_stamp` / `Lrecent_keyboard` no longer use a host pointer; stamps at `+0xa8/+0xb0` | source review |
| `_dm_view_panel_projection` index 6 is `ldr s3 / ldr s4 / fdiv / fadd / str` | disassembly of regenerated words |
| `source_sha256` matches the edited `view.s` | generator recomputes it |

Regenerate after **every** `view.s` edit, or the `[A]` assert fails:

```
python3 dm/games/01006F8002326000/gen_view_stubs.py \
    dm/games/01006F8002326000/view.s \
    dm/games/01006F8002326000/patches.py
```

The generator replaces only the text between `# BEGIN GENERATED VIEW CODE` and `# END GENERATED VIEW CODE`.

---

## 9. Verification

| # | Check | Expected |
|---|---|---|
| 1 | `python3 dm/tools/tests/test_patches.py` | Pass; one build ID in `FRAME_STEP`, no `VIEW`, no unwhitelisted uppercase name |
| 2 | `main.cuts.tsv` at `0x265140` | Exactly one cut, new word = `B` to `comb` |
| 3 | `main.cuts.tsv` at `0x27d4c0` / `0x27d570` | One cut each, no overlap |
| 4 | `main.fixups.tsv` | `FPS_STATE_OFF` present, `VIEW_STATE_OFF` absent |
| 5 | Disassemble `comb` | 16:9 test before the `BL`; trailing `ret`; `x16`/`x15` both reach `FS_STATE_OFF` |
| 6 | Disassemble `view_base + 1576` | Index 6 is the four-instruction additive form, not a single `str` |
| 7 | `source_sha256` | Matches `view.s` |
| 8 | `make -C dm/host/tests`, `make -C dm/gfx/tests GAME=01006F8002326000 run` | Pass |
| 9 | `python3 dm/tools/docs_patches.py`, `python3 dm/tools/docs_symptoms.py`, `python3 dm/tools/tests/test_docs.py` | Generated pages and links up to date |
| 10 | `scripts/regress.py check --games` on the five games the change is most likely to reach | Report naming the games and why, in the PR |
| 11 | Mac at 1.43 and 16:9, keyboard open and closed, island and menu routes | No `[RED]` line, dock position correct |

---

## 10. Docs, commits and PR split

| PR | Content | Size |
|---|---|---|
| Issue (this document) | Scope agreement under `CONTRIBUTING.md:35` | -- |
| PR 1 | `view.s`, `view.LICENSE`, `gen_view_stubs.py`, `patches.py`, four host tests | about 2,700 lines |
| PR 2 (may fold into PR 1) | `README.md` section, `docs/reference/rewriter.md`, regenerated `game-patches.md` / `symptoms.md`, `BINDING` | about 150 lines |

- Each commit: `area/component: summary`, plain English, saying what changed and why, the symptom, the cause, the fix and what was checked.
- No `Front-ends:` line: no setting, no `settings.tsv` change, no change to how saves are read.
- No chain-tool change, so no `dmsh_chain.py --header`; the PR text states "No chain input changes".
- Game files, keys and extractions stay out of the repository per `CONTRIBUTING.md:11`.

---

## 11. Open and unverified (reported explicitly)

1. Option B in section 5.4 (divide by `+0x84` before adding) is mathematically equivalent to docking after the upstream stretch, but the dock direction has **not** been confirmed on a device. Option A is the documented fallback. Both stay marked open until checked.
2. `+0x58` is 0.1% away from the literal `DM_SCREEN_ASPECT`, and is zero when `DM_SCREEN_ASPECT` is unset, `0` or `auto`. The latter means no adaptation on an iOS `auto` layout; record it in the game note rather than compensating here.
3. `Lscreen_projection` rewriting m0/m5 versus the upstream 16:9 test is safe only because of the chained order. That order needs an assertion or an explicit comment so a later refactor cannot invert it.
4. The four `link=True` hooks keep AnimalDuo's `B | 0x80000000` (BL) encoding; whether each site is genuinely a call rather than a branch was not re-verified in this pass.
5. This document is a **design spec**. `view.s` has not been edited, `gen_view_stubs.py` has not been run, `words` / `relocations` / `source_sha256` have not been regenerated, `patch_nso.py` has not been run, no test has been executed, and nothing has been validated on a device.
6. Disassembly was performed on a local `main.patched.bin` produced by AnimalDuo's rewriter (its fixups contain `VIEW_STATE_OFF 0x4724200`). The upstream `screen_shape`-only patched image was not available locally; the upstream bit 6 instruction sequence was taken from `dm/games/01006F8002326000/patches.py` instead.
7. `pso_manifest.tsv` union and the LDN reference to PR #871 are separate work and are deliberately not part of this issue.

---

## 12. Evidence index

| Claim | Source |
|---|---|
| 923 commits between the AnimalDuo base and local `main` | `git rev-list --count b7a5cd43..HEAD` |
| One colliding site, 12 upstream-only, 21 adapter-only hooks | AST comparison of both `patches.py` files |
| `0x265140` is the only `ret` of `SetProjectionMtx` | `llvm-objdump` on `main.patched.bin` at `0x265100..0x2651b0` |
| `0x27d4c0` only forces a dirty bit; `0x27d570` writes the pane matrix | disassembly at `0x27d4a0..0x27d5c0` and the stub at `0x6197cec` |
| `_dm_view_ui_projection` / `_dm_view_panel_projection` behaviour | disassembly at `0x6197704..0x6197760`, constants at `0x6197560` (16/9) |
| `FRAME_STEP` ctx has `state`, `STUBS` does not | `patch_nso.py:1671` / `:1682` |
| `test_patches.py` whitelist excludes `VIEW` | `dm/tools/tests/test_patches.py:69` |
| `pso_manifest.tsv` is not a chain input | `dmsh_chain.py:52` `INPUT_TABLES` |
| `hle_thread.c` workaround already superseded upstream | `hle_bigstack.c`, `hle_svc.c:378`, commits `2d3fb83b`, `3e940796` |
| Pool index already upstream | `nvn_mtl.m:988` `pidx_cmp` / `qsort(g_pidx, ...)` |
