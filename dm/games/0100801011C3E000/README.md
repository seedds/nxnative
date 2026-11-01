# Persona 5 Strikers (`0100801011C3E000`)

Base 1.0.0 (`v0`), SDK 8.3.2, Koei Tecmo engine on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.

**Current:** opening combat, movement, story progression to Cafe Leblanc, in-game saving, and loading a gameplay slot after a full application restart passed on an Apple M4 Mac with 16 GB memory. This is early-game validation, not a full playthrough. iOS is untested.

## Profile and building

[`game.tsv`](game.tsv) registers the three module build IDs. The application pool is `0xCD500000`, with a 4 GB reservation and 256 MB alias and stack regions; the NPDM declares no system resource. Rewritten modules require `mem_used 0x4600000` (70 MiB), above the default 64 MiB. No game-specific executable cuts or fixed-address host behavior are added.

Use your own base NSP and keys with the standard pipeline:

```sh
python3 scripts/build_ipa_from_nsp.py /path/to/NSP-folder -k /path/to/prod.keys --platform mac-app --gpu none
```

Two supplied license-only DLCs were detected (`0100801011C3F001` and `0100801011C3F002`); DLC notices appeared in the menus. Their gameplay content was not exhaustively tested.

## Bring-up fixes

- **Resolved:** rewriting `main` stopped at `jump table 496f20`. A signed table offset is loaded using a constant base, a copy of the base participates in the final add, and the target stays in `x18` through unrelated initialization before the indirect branch. The existing short-window recognizers could not prove this form. The general fallback recognizer is linked below; it does not hardcode this game's addresses or table destinations.
- **Resolved:** startup dereferenced a null SDK service object at `sdk+0x2a4fc0` while querying play history. The host covered the user-ID overload of `pdm::QueryApplicationPlayStatistics`, but not the system-wide three-argument overload. The latter now delegates to the same empty host play log with its correct argument positions. This supplies offline service behavior; it does not emulate a play-history database or ownership of another game.

Addresses above describe only the registered 1.0.0 builds and are diagnostic evidence, not runtime patch rules.

## Shaders and tested revision

All **23,092** split shader outputs compiled successfully for Mac. The shader tables here are the generated profile inputs, without game shader binaries. The tested app used upstream `4a1f68301877d0556237e7570eea04667d1b21b2` plus the two fixes, Xcode 27.1 and its pinned Metal compiler `32023.921.6`. That revision was selected because the subsequent upstream compiler migration requires Xcode 27.2 / Metal `32023.922.4`; the compiler check was not bypassed.

This submission is rebased onto newer upstream. **The complete current-chain shader rebuild and replay have not been performed.** Generated shader libraries and `c1_full.tsv` / `cb_extent.tsv` must be regenerated locally rather than copied from the tested earlier chain.

The Mac app was packed with `--gpu none` (no offline pipeline archive). Interactive validation used `DM_PSO_PREBUILD=0` so pipelines were built on demand. First-use stalls are possible; no formal before/after performance benchmark was performed.

## Validation

Testing on 2026-11-01:

1. Boot, movies, title and language menus, and DLC notices rendered.
2. New Game with English voices reached the opening Shibuya Crossing battle. Movement, standard and special attacks responded without a crash during the smoke test.
3. The player completed the opening and saved at July 24, “Prepare for tomorrow,” in Cafe Leblanc's attic (slot 1, 16 minutes shown).
4. With the game closed, the isolated committed save was backed up. A new application process loaded slot 1 and returned to the expected attic scene; movement and the save menu responded.
5. A second gameplay slot was saved without overwriting slot 1. The game displayed “Save complete,” and the host logged an atomic committed-save swap. The verified committed data was backed up again.

All development runs used isolated save directories. Explicit `DM_SAVE` / `DM_TEMP` overrides must point at existing writable directories; the first override attempt omitted their creation and mounted read-only after a clone failure. Creating the directories and relaunching corrected that test setup. The normal packaged launcher already creates its save directory.

The delivered bundle's code signature and normal launcher were verified. Packaging, signing and personal filesystem details are not part of this recipe.

## Found on this game

- [Carried signed-offset jump-table targets](../../../docs/reference/rewriter.md#carried-signed-offset-jump-table-targets).
- [System-wide application play statistics](../../../docs/reference/services.md#system-wide-application-play-statistics).

## Open

- Rebuild and replay with the current upstream shader chain and required compiler, then run the cross-game regression harness before treating this as current-chain validated.
- The opening logged a texture-type mismatch for `a9690e949779c0b3_b`, slot 4 (Metal type 6 versus bound type 5). No cause or visual fix has been established; shader compilation success alone is not proof of complete rendering correctness.
- Later battles, longer sessions, other languages, all DLC content, controller combinations, online features and iOS remain untested.
