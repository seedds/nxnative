# NBA 2K26 v1.00 player lighting

The reported prototype used the North American Nintendo Switch cartridge base (title ID `0100CBF022E18000`, v0, display version `1.00`, Nintendo SDK `19.3.11`, Visual Concepts, NVN). Its main NSO build ID was `0ee9240a17640e135173b25808263edc`. The Mac run used host `f634de70` plus local extraction, startup and rendering changes. Clean-main gameplay remains unverified. [PR #2141](https://github.com/seedds/nxnative/pull/2141)

## Shader extraction

1. Start with `python3 scripts/build_ipa_from_nsp.py "<folder with the XCI>" -k "<prod.keys>" --platform mac` on your cartridge dump. The prototype extraction used `extract/v0/ExeFS/main` and `extract/v0/RomFS.img`. The stock build can stop at the shader split.
2. Scan the decompressed main NSO and the numbered RomFS archives for ZIP local headers (`50 4b 03 04`). Match names such as `VS.<16 hex digits>.shader`, with `HS`, `DS`, `GS`, `PS` and `CS` also possible. Headers can have gaps between them. Scan in bounded chunks with overlap at each boundary. Example archives include `0C` and `0Q`.
3. Method 8 uses raw DEFLATE. Method 27 uses legacy Oodle. The prototype used the game's `OodleLZ_Decompress` through the Mac host's `DM_OODLE_SERVE` mode, matching the game's `fuzzSafe=0` at `main+0x2f35134`. Main's generic Oodle tool uses `fuzzSafe=1`. In the prototype, `VS.2d02b4029169d2b3.shader` at byte 543677 in `0C` decoded to 4,585 bytes with CRC-32 `5400cc94`. Check the declared decoded length and CRC of each accepted member.
4. The decompressed wrapper starts with little-endian magic `0xc69e1ce8`, then the stage, control length and code length at offsets 4, 8 and 12. The control block starts at `0x18`, followed by the code. Validate them with `shader_split.check` and deduplicate with `skey(control, code)`. A repeat split with the candidate reader found 20,789 unique keys (1,863 vertex, 18,859 pixel, 62 compute and 5 geometry).
5. Build the extracted package and prototype profile with `dm/gfx/tools/dmsh_build.py`, `DM_GAME=0100CBF022E18000` and `DM_GAMES_DIR` pointing to the containing directory if the profile is outside the checkout. For iOS, set `IOSOUT` as well. Use the chain stamp from the full build.

## Startup and black players

- The prototype used a 4 GiB reservation, a `0xCD500000` application pool and module order `main subsdk0 subsdk1 sdk`. The first fault at `subsdk0+0x149fe0` writes through `nn::ro::detail::g_pLookupGlobalManualFunctionPointer`. The Switch runtime linker initializes the writable slot at `rtld+0x888`. Initializing it advanced startup to a loader traversal at `subsdk0+0x14b008`. The prototype implemented `nn::ro::LookupNextSymbol` by searching exports in loaded-module order after the caller.
- From a clean save, pass the startup prompts and choose **Play Now → Quick Play → Dallas Mavericks at Boston Celtics**. On the Mac prototype, the court, ball and HUD appeared while players were black silhouettes.
- The skipped compute shaders `d42a3793d0b14bf7` and `6ef81447e3c90695` write to a 128×128 cube array sampled by the player materials. Both read the destination handle in compute uniform slot 12 at byte offset 320. Before the prototype fix, all 12 mip-zero faces read back as zero.
- The translated MSL indexed a 32-entry image array using a descriptor ID clamped to 31, while the real IDs can reach the thousands. The prototype lowered accesses traced to a constant word in a known uniform binding to individual Metal image arguments, renumbered the remaining argument IDs, and resolved the full handle before binding the writable image. Main already provides the [`img_ubo` / `NVN_RB_IMGU` binding](../reference/shaders.md#images-and-buffer-textures-with-their-handle-in-a-uniform-buffer) for a handle read from a uniform. The prototype handled 58 of 59 rejected compute image shaders. `476175825ff26572` still needs analysis of its forwarded image index.

## Verification

- **Mac prototype.** After the correction, the opening tip and first quarter showed faces, skin tones and both uniforms. On a clean build, repeat the same matchup and compare a first-quarter frame with the uncorrected render.
- **iPad Air 11-inch (M3).** A tester installed the prototype IPA, copied the matching RomFS into the app's Documents folder and reported successful testing. The report provides no device log.
- **Open.** The reader and shader changes still need clean-main gameplay and cross-game checks. For an iPad retest once integrated, pack with `--platform ios --romfs separate`, sideload the IPA, copy `RomFS.img` to the app's Documents folder and repeat Dallas at Boston. Record tip-off, player appearance and controller input, and collect `Documents/log.txt`.
