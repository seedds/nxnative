# Native material-refresh experiment

These files are a research prototype, not an enabled game-profile patch.

The observed OOT NSO build ID is `e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348`. A prepared program-set reference was aligned and valid while a material row still held its earlier tagged reference and old layout. The prototype calls the game's own refresh routine at `main+0x501a70` before the consumer at `main+0x6fc38`, only after its guarded selected source is ready and differs.

It does not mask reference tag bits. The stub preserves integer/SIMD state and status registers. Its branch placeholders must be resolved by a build-gated private-image patcher; the original game image and all game assets are intentionally absent.

The runtime experiment passes the initial CPU fault, and the separate uniform-record-size fix passes the subsequent malformed command stream. The refresh location is still unvalidated: enabling this native path has also produced texture-type warnings and invalid cached control lists. The normal OOT profile does not install it. No valid full motion field has been captured.

`material-refresh-fixture.s` is the same guarded control flow with a controlled C callee and a local return. It checks selection, aliases, non-ready sources, skipped cases and SIMD preservation. It does not prove the native refresh routine is safe at draw time.

On an Apple silicon Mac:

```sh
cd dm/games/0400486024DEC000/research/native-material-refresh
clang -O1 material-refresh-fixture.c material-refresh-fixture.s \
  -o /tmp/oot-material-refresh-fixture
/tmp/oot-material-refresh-fixture
```

The fixture passes 15 checks. Before integrating any hook, identify the native invalidation/preparation trigger, prove cache and command-list lifetimes, and gate all addresses by the correct build. Do not work around malformed records by dropping them.
