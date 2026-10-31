# 06 — Building an IPA on an Intel Mac

The iPhone build itself is host-independent: extraction, the rewriter, the shader split, the
translator and the iOS shader libraries are plain tools plus Apple's Metal compiler, and all of
them run on x86_64. What an Intel Mac cannot do is **run the game's own arm64 code**, which is
what the Mac host (`dm/host/dmhost`) is for. This page lists the two fixes that make the rest work
and the steps that must be skipped or substituted.

Everything below was used to pack Super Mario Odyssey (0100000000010000, a 5.6 GB XCI, 27,210
shaders) on an Intel Mac and sign the result for a phone.

## What works, what does not

| Stage | Intel Mac |
|---|---|
| extract (`nx_extract`), rewrite (`patch_nso.py`), shader split, iOS shaders, app build, IPA pack | works |
| shader translator `dmsh_xlate` | works after §1 (its launcher is arch-specific) |
| Oodle / LZ5 decode with the game's own code, run-time shader capture, provisional heap check | needs `dmhost` (arm64): skip, or substitute a native decoder (§2) |

## Prerequisites

```bash
python3 -m pip install lz4 capstone fonttools     # nso.py, shader_split.py, fonttools for icons
brew install xcodegen dotnet zstd spirv-cross     # + Xcode with the Metal toolchain
```

`scripts/build_ipa_from_nsp.py` looks for tools in `/opt/homebrew/bin`, `/usr/local/bin`,
`/usr/local/share/dotnet` and `~/.dotnet`, so an Intel Homebrew (`/usr/local`) is covered.

## 1. The shader translator launcher

Symptom: `Bad CPU type in executable` from `dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate`. The
translator is a .NET app (`dmsh_xlate.dll`) behind a native apphost launcher; the committed
launcher is arm64 only.

```bash
tools/intel-mac/build_dmsh_xlate.sh
```

The script builds the x86_64 apphost with `dotnet build -r osx-x64`, joins it to the committed
arm64 launcher with `lipo`, checks `--source-sha` against `dmsh_chain.lock` and installs the
universal binary (the previous one is kept as `dmsh_xlate.arm64.bak`). This is safe because
`dmsh_chain.lock` fingerprints the DLL and the translator's source sha, not the launcher: the
same sources must produce the same `xlate_source` (`16bbef6c762573ee` at the time of writing).

Do not commit the rebuilt launcher. Run the script on the machine that builds, and leave the
tracked binary alone so arm64 users keep the arm64-only launcher they already have.

## 2. Steps that run the game's own code

The NSP script stops where it would boot the game on the Mac: a run-time shader capture, a decode
with the game's own Oodle or LZ5 decoder, or a provisional profile's heap check. Those steps are
not needed for every title, but an Oodle-compressed title (Pokémon Legends: Z-A's Trinity packs,
Bayonetta 3's PKZ, Unreal's shader archives) cannot be split without one.

Two ways out:

- **Skip.** With a profile that ships its shader tables, nothing on the iOS path calls the host.
  Games whose shaders are generated at run time (Super Mario Galaxy, Three Houses) cannot be built
  this way.
- **Substitute a native decoder.** `scripts/build_ipa_from_nsp.py` accepts

  ```bash
  OODLE_NATIVE=/path/to/native-decoder OODLE_NATIVE_DLL=/path/to/dir scripts/build_ipa_from_nsp.py ...
  ```

  `OODLE_NATIVE` replaces `dmhost` as `DM_OODLE_HOST` and must speak the same protocol as
  `dm/host/hle_oodle.c` `DM_OODLE_SERVE`: on stdin, `u64 compressed length`, `u64 raw length`,
  then the bytes; on stdout, `u64 bytes decoded` (0 on failure) and the decoded bytes. Requests
  are served until stdin ends. `OODLE_NATIVE_DLL` is passed as `DM_OODLE_MODULES`.

The decoder used here is a small PE loader that maps Windows' `oo2core_win64.dll` (Oodle 2.9.14,
buildable from `Sainan/noodle`) into the process and calls `OodleLZ_Decompress` with
`__attribute__((ms_abi))` for the Windows x64 calling convention. Four things cost time and are
worth knowing before writing another one:

1. Read `.reloc` entries **from the file** (`image base + raw pointer`). The mapped image skips
   the section's file bytes, so reading relocations after mapping gives garbage.
2. Fill the IAT. Point every `KERNEL32` import at a stub that returns 0 — the decoder path used
   here takes decoder memory from the caller and never touches the CRT. An unfilled IAT jumps to
   the hint/name RVA and crashes.
3. `mprotect` per segment: `.text` RX, data RW. Mapping the whole image RX makes `.data`/BSS
   unwritable and the process dies with SIGBUS.
4. `read()` on a pipe returns short. Loop until the request is complete; a 64 KB pipe buffer will
   cut anything larger.

## 3. Disk and time

A 5–6 GB title peaks at roughly source (5.6) + extract (5.2) + tmp (2–3) + IPA (5.8) ≈ 19–21 GB on
one volume. What worked:

- build with the source on one volume and `-o` pointing the IPA at another;
- after extraction, move the source off the build volume (`mv`, not `rm` — the script only reads
  it during extraction and a rebuild needs it back);
- the IPA is packed by `tools/pack_ipa.py` straight from `extract/`, with no `Payload/` copy, so
  no second 6 GB copy appears at pack time.

An interrupted run leaves `shader/pkg.partial` behind and the next run fails in `shader_split.py`
with `FileNotFoundError: .../pkg.partial/tables/shader_map.tsv`. Create the directory and run
again — the metallib cache (`shader/cache`, several GB, keyed by shader content) is untouched, so
a 2.7-hour compile is not repeated:

```bash
mkdir -p "$W/shader/pkg.partial/tables"
```

## 4. Signing and installing from the Mac

`codesign` can sit for 15 minutes in `SecKeyCopyDigestForMessage` on a bundle with thousands of
romfs files. `zsign` signs the same bundle in about a minute; two of its quirks matter:

- signing **with `-o`** while repacking leaves the executable named `DmIOS.cstemp`. Sign the
  `.app` directory without `-o`, then `zip -qr0 out.ipa Payload/` **from the parent of
  `Payload/`**;
- the bundle id must equal the profile's `application-identifier` suffix, and the device's UDID
  must be in the profile's `ProvisionedDevices` (`xcrun devicectl device info details`).

Install straight from the directory, no repack needed:

```bash
xcrun devicectl device install app --device <id> Payload/DmIOS.app
```

## Limits

This page is about producing the IPA. Nothing here makes a game run on Intel: the Mac host still
needs arm64, and the IPA still needs a device.
