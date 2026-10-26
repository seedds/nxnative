# Pokémon Z-A offline ASTC pack preparation

`dm/tools/za_astc_pack.py` prepares audited BC7-to-ASTC 4x4 changes for
Pokémon Legends: Z-A's Trinity archive. It is an offline host tool. It does not
install anything, launch the game, or operate an Apple device. It supports both
same-slot replacement and relocated replacement packs. The latter is the normal
path when an ASTC file's Oodle stream is larger than its original FlatBuffer
vector.

The source `arc/data.trpfs` and `arc/data.trpfd` must come from the user's own
RomFS. Keep them and all derived game data outside this repository.

## Safety model

- `census`, `prove`, and `prepare` open the source archive read-only.
- Only BNTX `BC7_UNORM` and `BC7_sRGB` textures are eligible. BC7 and ASTC
  4x4 both use 16 bytes per 4x4 block, so the BNTX image storage layout and
  image size can be preserved while the BRTI format changes to ASTC 4x4.
- BC1 and BC4 are not converted because ASTC would double their 8-byte block
  storage. BC5 needs a different two-channel destination contract, and BC6H
  is HDR. They remain unchanged.
- Every mip and layer is deswizzled through the repository's NVN layout code,
  decoded, encoded by Arm astcenc, reswizzled, and deswizzled again. The tool
  rejects a layout mismatch. It also decodes the ASTC result and records PSNR
  and RMSE quality evidence.
- The converted file is compressed by the Oodle implementation linked into
  the user's own game module. A separate persistent guest-decoder process must
  reproduce the converted bytes exactly before a patch is admitted.
- A same-slot patch is emitted only when the new Oodle stream fits the
  existing FlatBuffer byte-vector capacity. The vector length is updated and
  unused tail bytes are zero-filled. An oversized stream is reported and
  skipped; no neighboring pack bytes move.
- `relocate` never moves an existing source pack physically. It copies each
  affected ONEPACK and appends new verified file vectors inside that copy. The
  compact payload is inserted immediately before the physical archive root,
  and the original root tail is shifted to the new physical end of file.
  Trinity also requires every adjacent root offset difference to equal the
  preceding descriptor size rounded to 16 bytes. The schema-4 manifest
  therefore rebuilds all pack offsets as one virtual aligned prefix sum while
  preserving their hash-sorted order, and emits `redirects.tsv` to map the
  complete virtual archive to the cloned source packs, inserted replacements,
  and shifted physical root. This admits oversized Oodle streams without
  physically rewriting every later pack in the immutable 4.75 GB source.
- A manifest binds the complete source archive SHA-256, every original range,
  every replacement range, and the astcenc executable. Treat a bundle with no
  patches, or a partial directory containing `FAILED`, as non-installable.

`DM_ROMFS_OVERLAY` selects complete physical files. `DM_ROMFS_REDIRECTS` adds
the byte-range translation required by a schema-4 virtual archive. A compact
bundle is therefore materialized as a physical archive, descriptor, and bound
redirect table before the guest starts. `apply-relocated` does this on a host:
it clones the base archive with APFS `clonefile`, extends the clone, copies the
source root tail to its shifted physical end-of-file position, streams
`append.bin` into the old root position, applies the virtual-root header and
shifted root-index edits, patches a copied descriptor, copies `redirects.tsv`,
verifies all three outputs, then atomically publishes a directory with
`READY.json`. Preserve the original archive. On a filesystem without cloning,
`--allow-full-copy` explicitly permits a full multi-gigabyte copy.

The tvOS frontend has a separate opt-in materializer for the same schema-4
`virtual-pack-layout-before-root` bundle. Earlier schemas are rejected. The
materializer is deliberately disabled unless a bundle path is configured. It
accepts a production bundle path relative to the app's writable container, or
an absolute path for development, validates the source identities and every
manifest-bound range, clones into a staging directory, writes `READY.json`
last, and atomically publishes the archive, descriptor, and redirect table as
one overlay. Only then does startup set both `DM_ROMFS_OVERLAY` and
`DM_ROMFS_REDIRECTS`. The file layer validates that the redirect rows cover
the virtual file continuously from byte zero to virtual EOF, translates
partial and cross-row reads, and reports the virtual size through file and
directory size queries. Invalid metadata, an out-of-bounds mapping, a gap, or
a short physical read fails closed. Removing the configured bundle/overlay
falls back to the immutable base. This repository workflow still does not
deploy it.

## Prerequisites

Build the Z-A host so the tool can use the game's Oodle implementation:

```sh
make -C dm/host GAME=0100F43008C44000 dmhost
```

Supply the matching rewritten `main` and `sdk` modules through `--modules`.
Build or obtain an official Arm astcenc 5.x executable and pass its path with
`--astcenc`. The tool can compile its small NVN texture-layout helper with the
system `clang`; `--library` can instead select a prebuilt helper.

The examples below use `/private/tmp` intentionally. Replace the RomFS and
module placeholders with local paths, but keep outputs and caches outside the
source tree.

## Read-only census

The census decodes every selected Trinity file and records BNTX texture
geometry, formats, image bytes, source offsets, and errors in JSON:

```sh
python3 dm/tools/za_astc_pack.py census \
  /path/to/romfs/arc/data.trpfs \
  /private/tmp/za-texture-census.json \
  --descriptor /path/to/romfs/arc/data.trpfd \
  --host dm/host/dmhost \
  --modules /path/to/rewritten/modules
```

Use `--pack-index`, `--file-index`, or `--limit-packs` for a bounded audit.
Unsupported compression records appear in `errors`; they are not silently
treated as textures.

An exact runtime route can be selected from `DM_FSLOG` reads. Pair the trace
with a completed census so only known BC7 file records in intersected packs are
decoded:

```sh
python3 dm/tools/za_astc_pack.py census \
  /path/to/romfs/arc/data.trpfs \
  /private/tmp/za-texture-census.json \
  --descriptor /path/to/romfs/arc/data.trpfd \
  --host dm/host/dmhost \
  --modules /path/to/rewritten/modules \
  --fslog /path/to/route-fs.tsv
```

`--fslog-before SECONDS` can bound the selection to an earlier point. A read
crossing two pack ranges selects both; malformed rows and traces with no pack
intersection are rejected.

## One-file proof

`prove` requires exactly one selected file and saves the original raw file,
converted raw file, and recompressed Oodle stream as evidence when the patch
fits:

```sh
python3 dm/tools/za_astc_pack.py prove \
  /path/to/romfs/arc/data.trpfs \
  /private/tmp/za-astc-proof \
  --descriptor /path/to/romfs/arc/data.trpfd \
  --host dm/host/dmhost \
  --modules /path/to/rewritten/modules \
  --pack-index 12777 --file-index 0 \
  --astcenc /path/to/astcenc \
  --cache /private/tmp/za-astc-cache
```

The default Oodle compressor is Mermaid (`9`) at level 7. The compressor is
selectable for controlled experiments, but a different compressor is not a
shortcut around verification: the resulting stream must fit and pass the
same exact guest-decoder round trip. Keep the compressor choice and quality
thresholds recorded with test results.

The default quality floor is deliberately strict: every converted
subresource must meet 40 dB PSNR and at most 2.55 8-bit RMSE. Small mips and
high-frequency textures may be retained even when they are structurally
valid. Lower `--minimum-psnr` or raise `--maximum-rmse` only as an explicit,
reviewed visual-quality decision; the manifest records per-subresource
measurements.

## Prepare more than one patch

`prepare` applies the same conversion, quality, capacity, and exact-decode
gates to all selected records:

```sh
python3 dm/tools/za_astc_pack.py prepare \
  /path/to/romfs/arc/data.trpfs \
  /private/tmp/za-astc-bundle \
  --descriptor /path/to/romfs/arc/data.trpfd \
  --host dm/host/dmhost \
  --modules /path/to/rewritten/modules \
  --astcenc /path/to/astcenc \
  --cache /private/tmp/za-astc-cache
```

Review `manifest.json`. In particular, inspect `patches`, `converted`,
`quality_retained`, and `skipped_files`; a completed command does not imply
that every BC7 texture was converted.

## Prepare virtual-layout relocated packs

Use `relocate` for a deployable compact bundle. This example limits work to
BC7 files from a prior census whose packs were read by a captured route:

```sh
python3 dm/tools/za_astc_pack.py relocate \
  /path/to/romfs/arc/data.trpfs \
  /private/tmp/za-astc-route-bundle \
  --descriptor /path/to/romfs/arc/data.trpfd \
  --host dm/host/dmhost \
  --modules /path/to/rewritten/modules \
  --census /private/tmp/za-texture-census.json \
  --fslog /path/to/route-fs.tsv \
  --astcenc /path/to/astcenc \
  --cache /private/tmp/za-astc-cache \
  --minimum-psnr 35 --maximum-rmse 4 \
  --oodle-compressor 13 --oodle-level 7
```

The compact runtime inputs are `manifest.json`, `append.bin`, and
`redirects.tsv`. Despite its historical filename, `append.bin` is inserted at
`archive_root_offset`, before the physical root tail; it is not appended after
EOF. A schema-4 manifest binds the source root offset and hash, the inserted
range, the shifted physical root offset and hash, the virtual root and EOF,
the complete output hashes, and the redirect-table identity and dimensions.
The first redirect row maps the unchanged header/prefix, one row maps each pack
to either its immutable source range or replacement range, and the final row
maps the virtual root to the shifted physical root. Rows cover the virtual file
without gaps.

`data.relocated.trpfd` is a review/convenience artifact for host verification;
the materializer can derive the same descriptor from the manifest's exact
edits. The manifest records the complete canonical input and output hashes,
each old and new eight-byte field, every appended pack hash, quality evidence,
and both tool recipes. A trace-selected bundle only covers reads observed by
that trace; it is not evidence about later, unvisited scenes.

## Apply to a new archive

Apply a reviewed bundle to a distinct output path:

```sh
python3 dm/tools/za_astc_pack.py apply \
  /private/tmp/za-astc-bundle \
  /path/to/romfs/arc/data.trpfs \
  /path/to/separate-overlay/arc/data.trpfs
```

Before writing, `apply` verifies the source size, full SHA-256, and every
source range. It creates `<output>.partial`, writes and `fsync`s the exact
manifest ranges, reads them back, and renames the verified result. It refuses
to overwrite an existing output or partial file. The immutable source is
hashed again in the final report.

This workflow creates a host-side candidate for later testing. Device
installation and runtime validation are separate, explicit operations.

## Apply and verify a relocated bundle on the host

Publish a reviewed relocated bundle to a new directory:

```sh
python3 dm/tools/za_astc_pack.py apply-relocated \
  /private/tmp/za-astc-route-bundle \
  /path/to/romfs/arc/data.trpfs \
  /path/to/romfs/arc/data.trpfd \
  /private/tmp/za-astc-route-materialized
```

The output contains `data.trpfs`, `data.trpfd`, `redirects.tsv`, an apply
journal, and `READY.json`. Its physical archive layout is `source prefix +
inserted replacement-pack payload + shifted root tail`, while its header and
root index describe the virtual sequential layout. The command refuses
pre-existing output, validates the complete source identities before cloning,
streams the payload instead of holding it in memory, `fsync`s all published
inputs, validates the inserted range, shifted root, redirect table, and
complete final hashes, and removes an unpublished partial directory on
failure.

Do not run a normal mmap-based `census` directly on the materialized physical
archive: its root offsets are virtual by design. Verification must read through
the same redirect layer, and runtime traces refer to virtual offsets. A valid
redirect table continuously covers `[0, virtual_archive_bytes)`, stays within
`final_archive_bytes`, and maps the root row to
`final_archive_root_offset`. The host file layer enforces these boundaries and
fails closed instead of falling back to the same physical offset.
