# Divinity: Original Sin 2 - Definitive Edition (`010027400CDC6000`)

Update 1.0.12 (v786432), SDK 9.3.0, NVN, Larian's engine and Wwise. Modules `main`, `subsdk0` and `sdk`; the two SDK modules have the same build ID. The RomFS has 47 Larian packages/parts, 10.58 GB. **Plays on the Mac** (M6, 16 GB, macOS 27.2, 2026-11-15): title, New Game, character creation, opening ship, Magister Siwan's dialogue, player movement, an autosave and reloading it with Continue. Gameplay testing stayed under ten minutes. No iPhone or iPad run.

## What it needs

The NSP script's provisional profile, with the memory split and depth mode measured by its Mac checks. No game-specific host code or rewriter cuts. The general [UDP loopback service](../../../docs/reference/services.md#udp-loopback-works-offline) lets RakNet initialize offline; there is no networking profile flag.

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `main` imports both SDK modules |
| `mem_reserve 0x140000000`, `mem_alias 0x60000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | a 5 GB block; the measured heap request is `0xbd400000` (2.96 GB), so the heap region is 3 GB and the alias region 1.5 GB. The address-region checks reach alias +1.25–1.27 GB in the first 15 seconds of `nnMain` |
| `sys_resource 0x200000` | `game.tsv` | the NPDM declares 2 MB of system resource, enabling virtual address memory |
| `depth_mode zero` | `game.tsv` | the game sets near = 0 at boot; offline pipeline bases use the same convention |

## Shaders

12,329 (6,132 vertex, 6,192 pixel, 5 compute), with 6,147 program pairs. `main` contributes 73 and the Larian packages 12,256. The archive reader takes about one second on an M6, against 39 seconds for the earlier scan that found only the executable's shaders. All 93 missing shader keys captured during the first boot occur in the package extraction.

The shader archives are `Engine.pak`, `EngineShaders.pak`, `CommunityMods.pak` and `Materials.pak`. Their indices also name compiled programs for other platforms; only GLSLC stages are taken. See [Larian LSPK packages and BSHD programs](../../../docs/reference/shaders.md#larian-lspk-packages-and-bshd-programs) for the format and reader.

## Driving it on the Mac

`scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app` makes a self-contained, double-clickable `Divinity Original Sin 2.app` beside the NSPs. The tested build is 11.9 GB, with all shaders, the M6's pipeline archive and the recorded prewarm pipelines; its ad-hoc signature verifies with `codesign --verify --deep --strict`. The app uses its normal per-title save and log locations in the player's Library.

The work folder `010027400CDC6000-work` beside the NSPs holds `run/boot.sh <log> [seconds]` (window and Metal HUD, sound, cloud save sync off). It uses the NSP script's build beside it, a separately named `dos2host`, and `DM_CTL=run/ctl.txt`. Append button presses or `shot <absolute PNG path>` to that file while the host runs. Saves and logs stay under `run/`.

From a fresh save: A at the title, B to decline Steam cross-save, then Story, Single Player, a difficulty and Begin. Wait for each menu transition. Character creation's tutorial needs A held for about a second; Plus starts the game, followed by A to confirm. The opening leads to the ship and Magister Siwan. A advances dialogue; the left stick moves the character. Tutorial acknowledgements also need a held A.

In the opening hold, the host's frame-5100 sample reports frame time p50 33.34 / p95 33.71 ms and GPU p50 6.45 ms, with about 1.93 GiB resident memory. These are diagnostic samples with sound and scripted frame captures, not a controlled performance comparison. Sound was enabled, but not listened to or dumped. The prewarm list records the menus, character creation and opening ship; later areas are not covered.

## Validation

The host and graphics unit suites pass. The LSPK parser has 13 synthetic tests; the focused parser suite, documentation/profile/chain checks and Python lint pass. The broader Python run's extraction fixtures need `DOTNET_ROLL_FORWARD=Major` to use the installed .NET 11 runtime; their ten-test rerun passes. Both networking tests pass with ASan/UBSan and ThreadSanitizer.

LLVM 23.1.4 reports no finding in the changed networking sources and tests. The full 313-source ratchet is not green: 215 findings against 211 in the baseline, with nine file/check pairs above and three below. The above-baseline graphics files are unchanged by this port; no unrelated baseline was altered.

Regression reach is uncertain because the shared descriptor service changes. The Binding of Isaac: Rebirth covers the older SDK path; Fast Fusion covers SDK 20 startup and menus, with fewer shaders than the other available titles. Full regression runs cover both. The first Fast Fusion baseline hung on entering a race, and another baseline received an external SIGTERM; its comparison was repeated on a menu route using a private executable name. These runs were on a busy Mac and are correctness checks, not performance measurements.

Isaac's comparison passes. Fast Fusion's uninterrupted baseline and candidate both finish (3,445 / 3,457 frames, about 59.2 fps, all 2,976 shaders unchanged). The latest harness report is `REGRESSION` because its candidate logs the known cube-view warning and that single menu baseline does not. The exact warning also occurs in this unchanged revision's earlier quick and race baselines and is already listed in [Fast Fusion's open findings](../040041E0212AE000/README.md#open). The report was not edited or its baseline counters suppressed.

After rebasing onto `f226affd`, the rebuilt Mac host reloads the autosave into the opening ship. The 48 documentation/profile/chain/LSPK checks, both socket tests with ASan/UBSan, and Python lint pass again; the rebuilt app's signature verifies. The broader graphics and regression runs above preceded the rebase.

## Found on this game

- [Larian LSPK packages and BSHD programs](../../../docs/reference/shaders.md#larian-lspk-packages-and-bshd-programs): black rendering with the executable's shaders alone.
- [UDP loopback works offline](../../../docs/reference/services.md#udp-loopback-works-offline): New Game stayed on the loading artwork with an empty progress bar. Before the fix both socket attempts failed; afterward the first socket succeeded and the bind, send and receive calls executed, reaching the opening ship.
- [Time zone rules, calendar conversion and the network clock](../../../docs/reference/services.md#time-zone-rules-calendar-conversion-and-the-network-clock): found earlier on the Android host fork; the Mac host already has the general fix.
- [Alpha-test comparisons without an alpha export](../../../docs/reference/rendering.md#alpha-test-comparisons-without-an-alpha-export): the depth-pass warnings described below.

## Alpha-test warnings

**Resolved on the Mac (2026-11-15).** Character creation used to warn on `faca851c03a852eb`, `ATM_Wind_Tree_AlphaTest_2S_PBR_ST_DEP_NX.bshd`; directly reloading the ship warned on `7ffa2f2d8d5c5c26`, `Base_AlphaTest_PBR_ST_DEP_NX.bshd`. Both programs have an original output map of zero, no colour outputs and their own fragment discard. The host's comparison-eligibility check requested an alpha-test entry despite the absent alpha export. The general [output-map fix](../../../docs/reference/rendering.md#alpha-test-comparisons-without-an-alpha-export) leaves their shader bodies intact and needs no profile rule or shader rebuild.

Fresh-save character creation and Continue into the opening ship both render with zero red lines. A one-frame pass dump from each scene confirms the affected shader actually draws. The test profile is separate from the established autosave; testing stayed under ten minutes. The Mac app was rebuilt and its signature verified.

The expanded alpha replay passes 1,216 checks (168 failed before the fix), including an RGB-only export with padded MSL alpha and the unconditional `NEVER` case. The eight alpha Python tests compile the variants for Mac and iPhone; all pass, as do the 31 documentation/chain checks and Python lint. The full graphics suite passes its 167 native targets and two final Python targets; six optional reflection-cache sample checks are skipped because their fixtures are absent. Targeted LLVM 23.1.4 lint reports no new findings. Regression level is moderate because RGB-only exports with an existing side variant also change pipeline selection: a full Isaac comparison passes with all 19 shaders unchanged and no new red lines. This is the light local NVN regression game; the run was busy, so its timings are not performance evidence.

## Open

- Later areas, combat, manual saving, multiplayer and online features are untested.
- No iPhone run or IPA yet. Later memory use in the alias region is unmeasured.
- Only update 1.0.12 is registered; the base game alone is untested.
