# Xenoblade Chronicles X: Definitive Edition (`0100453019AA8000`)

Update 1.0.1 (`v65536`), SDK 19.3.2, Monolith's engine, NVN. The supplied XCI
contains both the base game and this update. Modules: `main`, `subsdk0`
(NintendoSDK_movie), `sdk`.

## Current status

Mac gameplay tested (2026-11-15, Apple M6) through avatar creation, the opening
movies and interactive scenes, name entry, and movement / camera control in
Starfall Basin with Elma. The field menu, autosave and a manual save write work.
Continue reloads the saved field, with movement and camera control rechecked
after rebasing. The test used the iPhone's 4.375 GB alias region. Combat,
further progression and iPhone validation remain open.

The opening field runs at about 30 fps: ten-second intervals report frame-time
p50 33.24 ms / p95 33.26–33.30 ms after loading. First-use shader and pipeline
loads still hitch. This is a new-port observation, with no working rendering
baseline for a speed comparison.

Resident memory settles near 2.6 GB / Metal allocation near 2.55 GB after the
opening field loads (frames 4,800–5,700); the allocator change reduces virtual
reservations, rather than the Horizon physical pool.

## Memory

The NPDM declares 32 MB of system resource. The profile reserves 5 GB, with
4.375 GB for alias memory. Monolith calls `nn::os::AllocateAddressRegion`
directly; the SDK's StandardAllocator gate cannot change its region. Turning
off virtual address memory offers no alternate heap path: initialization
discards `IsVirtualAddressMemoryEnabled`'s return value.

**Platform-limit workaround, game-bound:** `nninitStartup` reserves 60 GB at
`main+0x13f3cf0`. Its first arena (`main+0xbabe60`) reserves 4 GB + 128 KB and
commits 128 KB for bookkeeping. Shrinking only the outer region makes that
arena fail, followed by a null `memset`. Six coordinated cuts reserve a 4 GB
outer region and a 512 MB + 128 KB first arena, adjusting the bump increment,
subtraction and stored capacity together. Exact cuts are retained for main build
`3f2425864cf226844511065f77aa3e36`; other builds require the atomic function
match described below. At the first controllable field, the arena
has committed 95 MB (`0x5f00000`) of its `0x20020000`-byte capacity; the outer
region's bump reaches `0xe95a0000` inside its 4 GB capacity. Later-game
occupancy remains open.

`mem_map_limit on` supplies the existing Horizon physical-pool limit. The
wrapper returning to `main+0xbadc10` probes capacity in 1 MB chunks and frees
them. Without the limit, it successfully opens almost all 60 GB in the Mac's
alias region. The pool remains `0xCD500000`; the region changes address space,
not that physical limit.

The outer region's base, bump and capacity are at module offsets `0x277b5d0`,
`0x277b5d8`, `0x277b5e8`; the first arena's base, committed bytes and capacity
are at `0x277b618`, `0x277b620`, `0x277b628`. Read these after loading areas
when validating the smaller arena.

The binding is **game**: the existing function matcher requires both complete
functions, their constants and their direct callees to match uniquely, or
applies none of the six cuts. The cartridge's base 1.0.0 main
(`40229b9405f4dbed56764fd51b894f47`) supplies an independent static check: its
arena initializer is at `0xbabda0`, its outer trampoline at `0x13f3bf0`, with
the same complete-function fingerprints as 1.0.1. Initialization, physical
capacity probing, commitment and allocation paths were compared by disassembly.
A forged-build-ID copy of 1.0.1 rewritten through the matcher is byte-identical
to the tested fixed-cut output, including its gates and sites. The base is
enrollment evidence only; its assets and gameplay are not registered as tested.
An engine-wide recognizer with dynamic sizing would require shared-layout and
capacity-propagation proof across titles, new gates and cross-title validation.

## Files and shaders

The RomFS contains 53 files (13.35 GiB): `sts.arh` / `sts.ard`, 46 WebM movies
and five Wwise sound packs. Its ARH2 index has 104,824 hashed members. ARH2's
recorded disk sizes include XBC1 wrappers; offsets accumulate those sizes,
aligned to the index's alignment (16 bytes here). No guessed file names are
needed to find shaders.

Some decoded assets hold both Maxwell and NVN2 HCPS containers. The old reader
assumed a Maxwell control in every HCPS and discarded the complete archive job
when it reached a shorter NVN2 control. Reading the version and lengths from
each control recovers 22,838 shader keys: 12,435 Maxwell and 10,403 NVN2;
7,643 vertex, 15,190 pixel, two geometry and three compute. There are 18,029
variation pairs. Six keys come from `main`, the rest from the archive.

Full Mac and iOS builds translate all 22,838 shaders with no failures, zero
undecoded bindings and zero unfillable driver-bank reads. The map's
HCPS sources are identified as Monolith containers, rather than inferred as
Unity from their variation suffixes.

The unsigned 1.0.1 IPA is `XenobladeChroniclesXDefinitiveEdition.ipa` beside the
XCI, 13.49 GiB. Its 82 archive entries pass a complete payload CRC check.

## Running on the Mac

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`.
Set `DM_ALIAS_SIZE=0x118000000` to reproduce the iPhone layout. The tested run
used a visible window with the Metal HUD, real audio and `DM_PSO_PREBUILD=0`;
the optional all-pairs pipeline preparation was stopped after confirming it
was compiling normally. Its partially prepared archive remains reusable.

From the title, `A` starts a new game. `Plus`, then `A`, skips each opening
scene separately. For the default avatar: `A` for gender, `A` for base, `L`
wraps to Confirm, `A`, `Up`, `A` starts. `DM_KBD=Cross` supplies a test name
without the native input box; confirm it with `A`. Answer Nod, then skip the
remaining scenes to enter Starfall Basin. Move with the left stick and turn
the camera with the right stick. `Plus` opens the field menu; from Party,
`Up`, `Up`, `A` opens Save. The first empty slot accepts `A` directly.

The work folder's `run/save` holds the opening-field autosave and manual slot 1.
`run/save_ref` preserves a copy of those files for later tests.
Only the avatar settings save exists before reaching the field, so interrupting
the opening scenes does not make Continue available on the next boot.

## Checks

- Fifteen synthetic Monolith parser tests pass, including ARH2 member bounds
  and mixed Maxwell / NVN2 containers. The graphics Python suite passes 649
  tests (212 skipped), and the complete Metal graphics suite passes.
- The 1,619-test Python tool run had four initial failures: the new profile's
  table row was not yet added, and three .NET 10 fixtures could not use the
  installed .NET 11 runtime. Documentation and fixture reruns pass after adding
  the row and allowing .NET major-version roll-forward; 16 tool tests skipped.
  Python lint reports zero new findings against its 321-entry baseline.
- Required matcher, patch-registry, NRO-packing and documentation tests pass.
  The enrolled matcher metadata equals the independently verified scratch rule;
  registered 1.0.1 cuts and the IPA's patched bytes are unchanged.
- After rebasing onto the current main, Mac and iOS libraries were rebuilt for
  chain `8e41177b6eb3` (inputs `44f8fe0209f5`). The saved field reloads at about
  30 fps with no red diagnostics. All 158 targeted Python checks pass, including
  the upstream font and packaging changes; feedback, tiled-resolve and depth
  Metal checks pass. The IPA includes the current six shared-font stand-ins.
  An additional Mac-bundle test stops on immutable-`self` compile errors in the
  unchanged `dm/mac/Launcher.swift`; the direct Mac host and IPA build pass.
- Regression reach is moderate because the HCPS reader is shared. Xenoblade
  Chronicles 2 covers its ARH1 path: a fresh full extraction reproduces all
  four existing tables, 15,910 shader keys and 12,370 pairs, unchanged. Its
  `regress.py check --quick --games 0100E95004038000 --baseline HEAD` reached
  the five-minute cutoff during pipeline-interface analysis after compiling
  the shaders. No runtime baseline/candidate comparison completed. The log is
  `verification/xcx-regress.log` in this game's work folder.

## Found on this game

- [Monolith archive and shader containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers): ARH2 hashed entries and mixed Maxwell / NVN2 programs.

## Open

- iPhone testing, combat and further story progression.
- Later-area allocator occupancy and the optional complete pipeline prebuild.
