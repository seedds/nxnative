# NBA 2K26 v1.00 on NxNative

## Scope and inputs

- Nintendo Switch North American cartridge base, title ID `0100CBF022E18000`, display version `1.00`, Nintendo SDK `19.3.11`.
- Main NSO build ID `0ee9240a17640e135173b25808263edc`. All offsets below are relative to the loaded module named beside them.
- The gameplay prototype used NxNative `f634de70` plus local diagnostic changes. GitHub main was at `c4ffb9df` when preparing this report. The prototype gameplay results do not validate that newer revision.
- Use your own cartridge dump and keys. No game files, keys, extracted shader code, compiled shaders, or IPA accompany this report.

## Reproduce shader extraction

1. Run `python3 scripts/build_ipa_from_nsp.py "<folder with the XCI>" -k "<prod.keys>" --platform mac` to get the extraction and rewritten modules. On an unmodified checkout the stock shader split may stop. The inputs to the extraction experiment are the resulting `extract/v0/ExeFS/main` and `extract/v0/RomFS.img`.
2. Scan the decompressed main NSO and the ZIP-headed numbered RomFS archives for ZIP local headers (`50 4b 03 04`). Check for member names of the form `VS.<16 hex digits>.shader` and the equivalent `HS`, `DS`, `GS`, `PS`, and `CS` prefixes. The archives can have gaps between records, so walk by signature in bounded chunks with overlap at chunk boundaries. Examples of archive names include `0C` and `0Q`.
3. Read the declared compressed bytes. Method 8 is raw DEFLATE. Legacy Oodle method 27 can be decoded with the game's own `OodleLZ_Decompress` through the Mac host's `DM_OODLE_SERVE` tool mode. Follow the game's wrapper for the legacy stream: it passes `fuzzSafe=0` at `main+0x2f35134`. As a check, the record `0C` at offset `543677`, `VS.2d02b4029169d2b3.shader`, decodes to 4,585 bytes with CRC-32 `5400cc94`. Validate length and CRC for every accepted record.
4. Check the little-endian wrapper magic `0xc69e1ce8` at offset 0. The stage, control length and GPU-code length are at offsets 4, 8 and 12. The control block begins at `0x18` and the GPU code follows it. Feed those blocks through `shader_split.check` and deduplicate using the splitter's `skey(control, code)`. The expected census is **20,789 unique shaders**: 1,863 vertex, 18,859 pixel, 62 compute and five geometry. A fresh extraction with the candidate reader on the prototype base reproduced this census.
5. Pass the generated package and profile tables to `dm/gfx/tools/dmsh_build.py`. Set `DM_GAME=0100CBF022E18000` and, if the profile is outside the checkout, `DM_GAMES_DIR` to the directory containing it. For iPad also set `IOSOUT`. Run the full build and use the chain stamp it generates.

## Reproduce startup and the black-player defect

- Prototype memory settings were a 4 GiB reservation and an application pool of `0xCD500000`. Keep modules `main subsdk0 subsdk1 sdk` in that order.
- The first startup fault was at `subsdk0+0x149fe0`. It writes through `nn::ro::detail::g_pLookupGlobalManualFunctionPointer`, whose writable slot is set up by the Switch runtime linker at `rtld+0x888`. Initializing that slot advanced startup to a loader traversal at `subsdk0+0x14b008`. The prototype implemented `nn::ro::LookupNextSymbol` as an export lookup in loaded-module order after the calling module.
- With the full shader package and the startup changes, open **Play Now → Quick Play → Dallas Mavericks at Boston Celtics**. The court, ball and HUD appear. Player bodies are black silhouettes before the image binding correction.
- Trace compute dispatches for keys `d42a3793d0b14bf7` and `6ef81447e3c90695`. Both read a destination texture handle from compute uniform slot **12**, byte offset **320**. The prototype pass trace identified both as producers for a 128×128 cube array sampled by player lighting. Before the fix, the dispatches were skipped and the array's 12 mip-zero faces read back as zero.
- The translated MSL indexed a 32-entry image array with a masked descriptor ID clamped to 31. The actual IDs can be in the thousands. When every index can be traced to a constant word in a known uniform binding, lower the array accesses to individual Metal image arguments and renumber the remaining argument IDs consecutively. Resolve the image using the full handle before preparing and encoding it. Current main already has the `KIND_IMG_UBO` / `NVN_RB_IMGU` path for a uniform-sourced image. Use that path when integrating the lowering. Leave unresolved patterns visible for diagnosis.
- The prototype handled 58 of 59 rejected compute shaders. Key `476175825ff26572` still needs analysis of its forwarded image index.

## Verification

- **Mac prototype:** Dallas at Boston reached the opening tip and active first-quarter play. Faces, skin tones and both teams' uniforms were visible. Repeat the route on a fresh build and compare a first-quarter frame with the uncorrected render.
- **iPad Air 11-inch M3:** the user installed the prototype IPA, copied its separate RomFS and reported successful testing. There is no device log in this report and the Mac gameplay observations above are separate.
- **Repeat on iPad:** build for `iphoneos` with `--romfs separate` once the reader and renderer changes are integrated. Sideload the generated IPA, copy the matching `RomFS.img` into the app's Documents folder, then repeat Dallas at Boston. Record tip-off, player appearance, physical controller input and the app's `Documents/log.txt`.
- Clean end-to-end builds and cross-game regression testing against current main remain open.
