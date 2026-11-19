# Patches of this profile (010052A00B5D2000) — what each one is, and how to rebuild the module it produces

[`patches.py`](patches.py) here is the profile `dm/tools/patch_nso.py` rewrites this game's modules with. Two `STUBS`
entries, one build ID (`7e19fa8490f09d9d2a71f74f5903211d` = `libharry2.nro`, Years 5–7). Both are `nro0` only;
`libharry1.nro` (`nro1`, Years 1–4) and the launcher (`nro2`) carry no guest patch beyond the rewriter's own gates.

| function | site | old | new | what it fixes |
|---|---|---|---|---|
| `publish_record` | `nro0+0xd3138` | `08040011` (`add w8,w0,#1`) | `b #0x11e4a80` | the pass table's publish moved to record time — without it Years 5–7 crashes at ~frame 3,900 (two threads, two locks that do not order the record and the replay) |
| `blit_guard` | `nro0+0x6d9f64` | `2bfeff97` (`bl #0x6d9810`) | `b #0x11e4a9c` | the HUD entity's whole-texture blit appended into the glyph run (the glyph/atlas lattice): skipped when the open UI run carries another texture |

`publish_record`'s stub comes first in the patch area and `blit_guard`'s follows it, so adding `blit_guard` did not move
the earlier stub (`0x11e4a80..0x11e4a9c` unchanged). The patch area starts at `0x11c0000`; 150,224 bytes are written
(`patch 150224` in the rewriter's summary).

## `blit_guard`'s stub, instruction for instruction

52 bytes at `0x11e4a9c..0x11e4acc`, reached by the `b` the site now holds, and branching back to `0x6d9f68` (the
instruction after the original call):

```
adrp x8,#0x1153000 ; add x8,x8,#0x9c8      &ui_batch[0]
ldr  w9,[x8,#0x10] ; cbz w9,emit           vertices in the open run; empty → the blit starts its own
ldr  x9,[x8,#0x30] ; adrp x10,#0x11b0000
ldr  x10,[x10,#0xbb8] ; cmp x9,x10 ; b.eq emit
b skip                                      another element's texture in the run → do not append
emit: mov x0,x19 ; bl #0x6d9810
skip: b #0x6d9f68
```

The helper (`0x6d9810`) and the emitter (`0x1053b0`) are untouched: the blit still runs whenever the open run is empty
or already carries its own handle. `x8/x9/x10` are dead at the site (execution resumes at `0x6d9f68` with
`ldr x0,[x19,#8]`; `x19` is callee-saved by the callee), so no live register is clobbered.

## Rebuild

```
DM_GAME=010052A00B5D2000 DM_GAMES_DIR="$PWD/dm/games" \
  /opt/homebrew/bin/python3.14 dm/tools/patch_nso.py \
  <build>/nro_v131072/nro0.nro <outdir> nro0
```

* `outdir/nro0.patched.bin` = `6729d1b3c0857fbb93ed6d94ceb4f4a5105f4fc112f376dcab18527d7c1a2496`, 18,762,448 bytes
  (both patches) — what `Lego HP NX GLYPHFIX.app` carries, byte for byte
* `outdir/nro0.cuts.tsv` lists both sites and the bytes
* removing `blit_guard` from `stubs()` reproduces `56f3bf4fdbb712f3c4f7d1c32d921efd38ec6abde304616ab29df82150ce9820`,
  18,762,396 bytes — the module the app carried before this patch

`DM_GAMES_DIR` must point at the **profile** directory's parent (here `dm/games`, which holds one directory per game):
`patch_nso.py` globs `$DM_GAMES_DIR/*/patches.py`. Pointing it at this folder silently loads nothing and yields a module
with no `STUBS` at all (28 bytes shorter) — check `stub N` and `cuts N` in the rewriter's summary line (`stub 2` is
both patches; `stub 0` is neither).

`./scripts/build_lego_hp.sh module` does exactly this and checks the hash. `docs/guestpatch-glyph-batch.md` in the host
repository (`nxnative`, branch `lego-hp57-artifact`) is the mechanism note; this profile's [`README.md`](README.md) has
the artifact, the diagnosis, the numbers and the residual.
