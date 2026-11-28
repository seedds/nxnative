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

The opening field runs at about 60 fps with the profile's native interval
setting; [Frame rate](#frame-rate) records the comparison with the original
30 fps mode. First-use shader and pipeline loads still hitch.

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

## Frame rate

`fps_target 60` selects the engine's native one-vsync interval. The standard
launcher offers 30 and 60 fps; `DM_FPS_TARGET=30` restores the two-vsync setting,
and `DM_FPS_TARGET=0` leaves it to the game. No present-interval profile override
or frame-step state page is needed. The
[shared engine recognizer](../../../docs/reference/frame-rate.md#monoliths-native-frame-interval)
finds the setting from reviewed timing functions and their relocated globals,
without a title or build ID. Its binding is engine; the selected default is
profile data. Horizon's clock and NVN presentation alone cannot raise the
game's internal wait.

The setting controls the nominal simulation step as well as the wait. In 1.0.1
the central update at `main+0x25060` derives its rate from the interval, clamps
measured elapsed seconds to one–two nominal frames, and passes both seconds
and `30 × seconds` to the game. Animation and task updates consume those
seconds. A second update at `main+0x937c10` constructs the same timing values.
The recognizer resolves the interval to `main+0x1da33a0` in 1.0.1 and
`main+0x1da3380` in the supplied base; the base remains static evidence only.

**Game-bound camera correction:** the orbit camera applies configured angular
gain once per update, independently of that elapsed-time step. Raising the
interval alone doubles its turn speed. A function-matched stub at the shared
yaw/pitch gain load (`main+0xab510`, base `+0xae358`) multiplies the loaded gain
by the game's own `30 × clamped elapsed seconds`, after the stick response
curve. Raw input, the configured gain and saved sensitivity settings stay in
their original units. The complete orbit function and three timing functions
must match; their globals come from the matched instructions. No match means
`NATIVE_FPS_BLOCKED` and the host retains the native frame rate. This local
correction uses `MATCH_STUBS`, without declaring a host-controlled frame step.

On the Apple M6, the original setting gives frame-time p50 33.24 ms, versus
16.62 ms with the native 60 fps interval. Raising only the host's presentation
cap leaves 33.24 ms unchanged. The same two-second walk ends at the same spot
at both rates. After the camera correction, a half-second horizontal turn
changes yaw by 1.367 radians at 30 and 1.361 at 60 (0.4% difference); menus and
returning to the field work, with no red diagnostics. The work folder's
`run/fps*-verified*` records the walk and initial camera mismatch;
`run/camera*-fixed*` and `fps/camera-comparison.json` record the corrected turn.
iPhone performance, combat, event cameras and later progression remain
unmeasured at 60 fps. Camera bank/easing and event/shake effects have not been
globally retimed.

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
  The allocator matcher metadata equals the independently verified scratch rule;
  its six registered 1.0.1 allocator cuts are unchanged.
- The initial port's Mac and iOS libraries were rebuilt for
  chain `8e41177b6eb3` (inputs `44f8fe0209f5`). The saved field reloads at about
  30 fps with no red diagnostics. All 158 targeted Python checks pass, including
  the upstream font and packaging changes; feedback, tiled-resolve and depth
  Metal checks pass. The IPA includes the current six shared-font stand-ins.
  An additional Mac-bundle test stops on immutable-`self` compile errors in the
  unchanged `dm/mac/Launcher.swift`; the direct Mac host and IPA build pass.
- The 60 fps change passes 281 targeted Python checks, 38 native frame-limit
  cases and 160 synthetic AArch64 stub executions. The engine finder resolves
  both supplied XCX builds, Xenoblade 2 and Xenoblade 3; sixteen distinct
  non-Monolith builds give no match or red diagnostics. An actual rewrite of
  an equivalent-instruction variant deliberately missing the camera fingerprint
  retains the original gain load and emits `NATIVE_FPS_BLOCKED`, while the
  supplied base matches its complete camera correction. Neither gets a
  frame-step state page. Python lint has zero new findings.
- Regression reach is moderate: the shared Monolith finder and native frame-limit
  path can reach another title. Xenoblade Chronicles 2 covers those paths and
  ARH1; its fresh extraction reproduces all four tables, 15,910 shader keys and
  12,370 pairs unchanged. The final quick regression passes within five minutes:
  15,910/15,910 shaders on both revisions, unchanged MSL, no new interface failures
  or red diagnostics, and screenshot dHash differences of zero. Baseline/candidate
  frame p50 is 33.24/33.23 ms, p99 33.26/34.81 ms, fps 30.1/29.8. Reports are in
  `regress-harness/reports/20261115-164020-check/` in this game's work folder.

## Found on this game

- [Monolith archive and shader containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers): ARH2 hashed entries and mixed Maxwell / NVN2 programs.
- [Monolith's native frame interval](../../../docs/reference/frame-rate.md#monoliths-native-frame-interval): one setting controls the internal wait and elapsed-time simulation step.
- [Function-matched ordinary stubs](../../../docs/reference/rewriter.md#function-matched-ordinary-stubs): elapsed-time camera correction without a host frame-step state page; incomplete matching disables native frame targets.

## Open

- iPhone testing, combat and further story progression.
- Later-area allocator occupancy and the optional complete pipeline prebuild.
