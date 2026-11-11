# Rewriter

The NSO rewriter (`dm/tools/patch_nso.py`) and its per-build tables (`dm/tools/sdk_patches.py`, a profile's `patches.py`). Each entry: symptom, cause, fix, the games it was found on. The [symptom index](../agents/symptoms.md) lists every symptom.

## Disassembler dependencies

### Capstone API version

- **Symptom:** `NameError: name 'CS_ARCH_ARM64' is not defined` at the start of `patch_nso.py`; now `[RED] Capstone 6.… is unsupported` from the NSP script or `patch_nso.py`.
- **Cause:** Capstone 6 renames the architecture to `CS_ARCH_AARCH64` and the operand constants (`ARM64_OP_*`); the rewriter uses the Capstone 5 API. PyPI's Capstone 6 releases are pre-releases (6.0.0a…), so a plain `pip install capstone` gets 5.x; a `--pre` install or another package's requirement brings in 6.
- **Fix:** install `capstone>=5,<6` (README and CI). `patch_nso.py` checks the major version (`cs_version()`) before it disassembles anything, and the NSP script checks it before building or surveying, so the extraction is not wasted; both print the install command for the Python that runs them. Capstone 6 is not supported.
- **Found on:** [Celeste](../../dm/games/01002B30028F6000/README.md#found-on-this-game).

## Patch discovery

### Repository patches missing under bracketed paths

- **Symptom:** a build made from a checkout or folder under a name such as `Game [0400486024DEC000][v131072]` silently loses the repository's per-game cuts, observation gates and frame-step stubs, although its build ID is registered; no error, as the SDK and engine patch files, named directly, still load. Under the same kind of folder, `nso_match.py --registered` finds no cuts, a provisional profile is guessed with no host hooks, and the shader split finds no NRO captured from a boot (`DM_NRO_DIR`, in the NSP folder): no CRA file table, no shaders from NROs.
- **Cause:** these tools passed a folder path to `glob.glob` unescaped, and glob reads `[...]` in it as a character class, so the search matched nothing. The `DM_GAMES_DIR` search already escaped its folders.
- **Fix:** every folder given to `glob.glob` is escaped (`glob.escape`) before its pattern is joined: `patch_nso.py`'s `PATCH_FILES`, `nso_match.registered_cuts`, `nso_syms.host_hooks` and `shader_split.boot_nros`. The wildcards still select the same files under a plain path. `pathlib`'s `Path.glob`, which the other tools use, never reads the folder as a pattern. `boot_nros` also returns nothing without `DM_NRO_DIR`; the CRA lookup searched the working folder then.
- **Check:** `test_patches.ExternalRegistry.test_repository_patches_under_bracketed_directory` loads a frame-step registration through the registry under a bracketed parent; `test_bracketed_paths.py` checks the other three. The first three fail without the fix.
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md#found-on-this-game), a build in a bracketed folder ([PR #1826](https://github.com/seedds/nxnative/pull/1826)).

## Patches across updates

### Function-matched cuts for unregistered builds

- **Symptom:** an updated game loses a needed cut because its NSO build ID changed, although the relevant code still matches a verified version; the rewriter reports `[RED] function patch ... skipped` when that match cannot be established, and the NSP script ends with `[RED] main: no function patch carries the registered builds' fixes to this build` or `[RED] main: function patches not applied` (Super Smash Bros. Ultimate 13.0.1: a static-initialiser abort in `nninitStartup`, GitHub issue #375).
- **Cause:** fixed-address `CUTS` correctly apply only to their registered builds. Copying an old address, or matching a short instruction sequence, does not identify the updated function.
- **Fix:** a profile can opt individual cuts into `MATCH_CUTS` in its `patches.py`. The shared matcher, `dm/tools/nso_match.py`, selects a unique complete unwind function by length and SHA-256 fingerprint. It retains constants, registers and internal branch destinations; normalizes relocation addresses; and checks the normalized bodies of direct callees or their imported symbol names. Original bytes are checked again before a cut is applied. Missing boundaries, unknown callees, changed code and ambiguous matches produce a red diagnostic and no cut. Every locator fails closed this way, and a new one must too: exactly one match with its assumptions checked, or no change (`dm_ue_cvar` in the host as well: a console variable whose name is registered twice is not written). That is what makes a looser binding safe on builds nobody has seen ([Binding](../agents/game-patches.md#binding)). Overlapping rules stop the rewrite. Every accepted cut is recorded in `<module>.cuts.tsv` with `[function match]` in its reason. Rules with the same `name` are one patch's reviewed implementations, for a function a build changed (another size, or the cut at another offset in it). Exactly one of them must match: the log names the implementation that did, and a red line appears only when none matches or more than one does. Before this, each implementation logged a red line on every build it did not match.
  - **A patch of several functions** (`{'name', 'functions': [{'size', 'fingerprints', 'cuts'}, ...]}`) applies whole or not at all: every function must match exactly once, at distinct places, or none of its cuts applies, and the red line names each function that did not match. Use it when the cuts depend on each other, as a constant used in several functions does (a region size, the place it derives, the bound checks of its end): applying some of them leaves code that disagrees about the layout, which fails later and less clearly than the original abort. Before this, rules were independent, so a constant split over several functions could not be enrolled safely.
  - **What an unregistered build kept:** for a module whose build is not registered, the rewriter writes `<module>.match.tsv`: the cuts of the registered builds of that module (the repository profile's `build` rows) and each patch's result. The NSP script reports it with the build: a red line when registered cuts have no function patch at all, or when a patch did not apply; the build itself goes on, since a derived profile is best-effort.
- **SDK-wide patches:** `dm/tools/sdk_patches.py`'s `MATCH_CUTS` (`{'sdk': [rule, ...]}`) apply to every game's `sdk` of a build with no `CUTS`, whatever the profile; a build that must keep Nintendo's code registers an empty `CUTS` list. Before, they skipped a build the selected profile's `build` rows name, which kept a registered row for every such build even where the rules gave the same cuts (18 builds' rows were removed when that changed). A patch that finds no reviewed implementation is logged as a plain line and listed by the NSP script without red, since a module may not need it ([The SDK's address-region node area](memory.md#the-sdks-address-region-node-area)). The NSP script's rewrite step keys its reuse on the profile's `build` rows as well. A callee that is one of the SDK's system call wrappers (`svc #n; ret`, assembly with no unwind entry) is identified by its call number.
- **Engine-wide patches:** `dm/tools/engine_patches.py`'s `MATCH_CUTS` (`{'main': [rule, ...]}`) are loaded and applied the same way, to every game's module of that name with no `CUTS`, a plain line where nothing matches: fixes of middleware code a game ships (Unity's [stream buffer handshake](threads.md#unitys-stream-buffer-handshake-loses-a-wake-up-on-apple-cores), from FINAL FANTASY CRYSTAL CHRONICLES and Raging Loop's Unity 2018). A game's own `patches.py` keeps what only that game needs; engine discovery by name (`ue_malloc.py`, `unity_bucket.py`, ...) needs no fingerprint at all.
- **Scope:** the profile selected by `DM_GAME`, keyed by module name. A derived profile still uses that repository profile's rules, even when the repository has no matching rules or patch file; an external-only provisional profile can supply its own. External files cannot replace or enroll a repository profile's matching rules. Other games' rules never participate. A build with registered `CUTS` uses those instead, including an explicitly empty list. SDK patches, observation gates, `FRAME_STEP` stubs and fixed host addresses retain their existing build-ID gates; frame-step stubs can be placed by their function instead ([Frame-step stubs placed by their function](#frame-step-stubs-placed-by-their-function)). The rewrite cache includes the selected profile's patch file even for unknown builds, the selected game ID and the matching tool. Repeated search roots or symlinks to the same patch file are loaded and counted once; distinct files claiming the same build still stop the rewrite.
- **Limits:** this is an opt-in code-identity check, not automatic compatibility for every game update. Direct callee bodies are checked one level deep; their callees, indirect-call targets and external data layouts are not proven unchanged. Enroll only a cut whose assumptions remain valid under those changes. Prefer fixing the general host behavior when possible. A replacement must contain no PC-relative instruction, except a direct branch whose target stays inside the matched function: the fingerprint keeps internal branches exact, so such a branch means the same on every matching build. A branch out of the function needs a different mechanism. Failure leaves the game potentially without its workaround, and stays visible in the rewrite log.
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md#memory), whose former resource-heap allowance was missing on 1.0.1. The shared matcher remains, but Wonder's allowance and matching rule were later removed after the general command-record fix described in its game note. Same-name implementations: [Taiko Risshiden V DX](../../dm/games/0100346017304000/README.md#frame-rate), whose frame-limiter cut has one implementation for 1.1.1 and one for the base game, where the function is shorter. A patch of several functions: [Super Smash Bros. Ultimate](../../dm/games/01006A800016E000/README.md#memory), whose 11 engine-region cuts lie in four functions. SDK-wide: the AddressRegionManager node area and the 2 MB alignment of large regions (SDK 20's), from the SDK builds of 20 games ([The SDK's address-region node area](memory.md#the-sdks-address-region-node-area)). Branches inside a replacement, and functions and callees holding the word `0xe7ffdefe` (a trap left after a call that does not return, which Capstone cannot decode; read as `brk`, still in the digest; 1,238 of them in this game's `main`): [Pokémon Legends: Arceus](../../dm/games/01001F5010DFA000/README.md#crash-in-the-capture-demo).

To enroll a cut for any game's own module:

1. Start from a verified fixed cut and its original NSO. Run `python3 dm/tools/nso_match.py <module.nso> <offset> <old hex> <new hex> <reason>` with the tools' Python environment. It emits a rule containing `name`, `size`, `fingerprints` and function-relative `cuts`, and rejects an ambiguous reference. For cuts that must apply together, `python3 dm/tools/nso_match.py <module.nso> --registered <name>` makes one patch of every cut registered for that NSO's build ID, grouped by function. Keep game files and scratch outputs in the game's NSP work folder.
2. Check the function and callees in another known build using `nso_fnmap.py` and disassembly. Check that the cut still has the same effect. `Image.match_patch(rule)` must resolve the reviewed locations. If a reviewed implementation differs, generate its fingerprint separately; combine fingerprints only when function size and relative cuts agree, and otherwise add the implementation as another rule under the same name. Retain exact `CUTS` for known builds.
3. Add the rule under `MATCH_CUTS = {'main': [rule]}` (or the appropriate non-SDK module) in that game's `patches.py`. Record the evidence and remaining runtime validation in the game's note. Do not transplant a workaround into unrelated games.
4. Verify an unregistered build-ID copy through the rewriter, comparing its output with the known fixed cut, and run `test_nso_match.py`, `test_patches.py` and `test_nro_pack.py`. The matching tests use synthetic modules from two profiles to check relocation, rejection and isolation without game files.

### Frame-step stubs placed by their function

- **Symptom:** a game that steps its time by a fixed amount per frame needs the host's frame step ([the frame step](frame-rate.md#a-fixed-frame-step-plays-in-slow-motion-on-a-slow-mac)), but its stubs could only be keyed by the main's build ID: every update needed a new port, and the binding page listed each such patch as `build` bound, blocked for want of a function-matched form.
- **Cause:** `FRAME_STEP` stubs branch out of the game's function into the patch area and back, and read the state page by `adrp`: addresses `MATCH_CUTS` replacements may not hold, since a cut's bytes must mean the same on every build that matches.
- **Fix:** `MATCH_FRAME_STEP = {'main': [{name, size, fingerprints, stub}]}` in a game's `patches.py`. `nso_match.locate_stub` finds the one complete unwind function of that size and fingerprint, before any cut changes the image, by the same rules as a function-matched cut (relocations normalized, direct callees checked, rules of one name as reviewed implementations, exactly one must match). `patch_nso.py` then gives the main its state page and runs `stub(ctx)` with `ctx.base` the function's start, so the stub writes its sites at offsets inside the function and assembles its body in the patch area with the addresses of this build; each site's original bytes are still asserted (`ctx.site`), and the cuts it lists say `[function match]`. No match, or two, gives `[RED] frame step … skipped` and no state page, so the host keeps the game's own present interval ([A present cap applies only with a frame step or an engine limit the host writes](frame-rate.md#a-present-cap-applies-only-with-a-frame-step-or-an-engine-limit-the-host-writes)). A build with `FRAME_STEP` keeps those stubs. Only the profile `DM_GAME` selects takes part, as for `MATCH_CUTS`.
- **Several functions:** a rule's `others = {label: {size, fingerprints}}` names more functions its stub patches or reads, each found the same way (`ctx.others[label]`, `None` with a red line when not exactly one matches); a stub that needs all of them leaves everything out when one is missing, as a part alone runs the game at the wrong speed. A global the stub needs comes from the code of a matched function: `ctx.address(offset)` is the data address the `adrp` there and the `add`, load or store after it make, read from the original text (the fingerprints mask the page). Tears of the Kingdom's frame step (six functions) rewrites 1.4.3 byte-identical to the build entry it replaced; Hyrule Warriors: Age of Calamity's (four functions, the KTGL device global from its frame task's own load, the frame-rate block from its writer's store) places its five sites on v458752, which had no frame step.
- **Anchored on an import:** a fingerprint covers the whole function, so an update that changes anything in it, even one member offset, needs a new one. Where the site sits in the one function that calls a particular imported symbol, a rule can name that symbol instead: `{name, calls, find, stub, reviewed}`. `nso_match.Image.callers` finds the complete unwind functions that `bl` its PLT entry; exactly one must. `find(fn)` (an `nso_match.Function`: its words, the calls' offsets, the text, `uses(registers)`, `branch_targets()`) then checks everything the stub relies on in that build's code and returns what the stub needs (`ctx.found`), or raises `MatchError`: a red line and no stub, as for a fingerprint that does not match. `reviewed` lists what `find` returned on the builds checked by hand, which the binding page and `test_patches.py` run the stub with. Such a rule holds over updates that change the function elsewhere, and still finds exactly one place with its assumptions checked, or none. Hades II's render size (the one caller of `nn::vi::CreateLayer`; one 1280x720 constant after the call, once in the text, no branch onto its second instruction, none of the stub's registers used in the function) rewrites v0 and 1.0.141667 byte-identical to the two fingerprinted rules it replaced, and locates in 0.16 s.
- **Limits:** Breath of the Wild (44 sites in 34 functions) and ASTRAL CHAIN (112 sites in 15 functions, its last update from 2019) stay build-bound by choice; Paper Mario: The Thousand-Year Door's cut and stub could move. A comparison with each engine's other games (2026-11-09) found none of these frame-step functions shared, so no engine-wide rule can hold them.
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md#frame-rate) and [Mario Kart World](../../dm/games/0400C3F00006E000/README.md#frame-rate), the first frame steps with a `game` binding; [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md#frame-rate) and [Hyrule Warriors: Age of Calamity](../../dm/games/01002B00111A2000/README.md#frame-rate), the first with several functions (2026-11-09).
- **Check:** `test_nso_match.py` (`test_frame_step_rule_locates_its_function_or_nothing`: a moved function, two matches, none, malformed rules); `test_patches.py` (each rule's sites lie inside its function; the registry scopes the rules as `MATCH_CUTS`). Both mains rewritten (2026-11-08): the stub at the reviewed site, its branches back into the function, the state page the fixups name.

### A frame step state page where the patch area starts

- **Symptom:** the rewrite of a main that gets frame step stubs (`FRAME_STEP` or a `MATCH_FRAME_STEP` match) stops with `AssertionError: frame step state page`; the same main rewrites without the stubs.
- **Cause:** the state page is the page after the key page, and an appended patch area starts on the first 64 KB boundary after the key page. When the key page ends on such a boundary, that boundary is the state page itself (Hades II's v0 main: key page at `0x590c000`, so the state page and the patch area both at `0x5910000`), and the assertion that the state page lies below the patch area fails. One main in four lays out this way; none with a frame step had before.
- **Fix:** `patch_nso.py` moves the patch area 64 KB later for a main that gets the state page and would start its patch area on it, decided after the frame step's function is found and before anything is written to the patch area. Every other module keeps its layout, byte for byte. Render-scale stubs (state +0x10) now also give their largest scale: `ctx.render_scale = n`, written as `RENDER_SCALE n` in the fixups table, holds `DM_RENDER_SCALE` to 0.5–n in the loader and sets the launcher slider's top (`settings.py`); the stubs that only lower the size give 1.
- **Found on:** [Hades II](../../dm/games/0100A00019DE0000/README.md#rendering): its render size stub, the first state page of a main without a frame step, on v0 (issue [#772](https://github.com/seedds/nxnative/issues/772); padsiwono reported the assertion on update 1.0.131248).
- **Check:** v0 and 1.0.141667 mains rewritten (2026-11-10): v0's patch area at `0x5920000`, its state page at `0x5910000`; 1.0.141667's layout unchanged (patch area `0x5b10000`, state page `0x5b08000`); both ran through the opening into Erebus with the render size stub at 1.5 (v0) and 0.5 to 3.0 (1.0.141667).

### Cheats carried across game updates

- **Symptom:** after a game update, the Cheats menu lists only `Cheats for build … (another version of the game)`, and nothing runs. Or a cheat file converted to the new build by hand writes into the wrong place: a table instead of a variable, or code that moved.
- **Cause:** Atmosphère cheats name fixed offsets into the main NSO, valid for the one build the file is named after. An update moves code and data by different amounts in different parts of the image; a single shift is a guess.
- **Fix:** `scripts/cheat_anchor.py` anchors each address to the game's code and compiles the cheats for any build ([Cheats that survive game updates](../development.md#cheats-that-survive-game-updates)):
  - data through the adrp pairs (and relocated `.data` slots) that reference the nearest address at or below it;
  - code sites through `nso_match.py`'s function fingerprints, then the tool's own fallbacks (the body digest, then a unique masked window), with a code site's original words always checked;
  - ASM code caves through the padding before the read-only data.

  The result is plain Atmosphère text, so it runs on any engine.
- **Found on:**
  - FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition: the 1.0.1 cheatslips file, anchored on the cartridge's `main`, compiles to exactly the hand-verified +0x43680 on 1.0.3 (12 of 12 cheats, 4 of 4 references each). The 1.0.3 file (5 ASM cheats with a code cave, 7 stat cheats, Mog's fatigue) compiles back to identical writes on 1.0.3 and to all 18 entries on the cartridge's build (code sites: 4 exact, 3 by body digest, 2 by window).
  - [13 Sentinels: Aegis Rim](../../dm/games/01003FC01670C000/README.md#cheats): a 1.10 Ryujinx list compiles back to identical writes and to 1.00, where every site lies 0x330 lower and the hook site was found by its body digest.

## Thread registers

### System register accesses through x18

- **Symptom:** a crash (`sig=11`) in the patch area a few seconds into a run, at an `x18` site's load, with an address a little above a small stale value (`addr=0x33220`, the thread's `K18` slot at `0x33200`), shortly after the game read `tpidr_el0` into `x18`.
- **Cause:** the guest's `x18` lives in the thread slot `K18`, since the Apple kernel may change the register at any instruction, and every `x18` site loads and stores the slot. The system register sites did not: `mrs x18, tpidr_el0` / `tpidrro_el0` put the value in the register only, `msr tpidr_el0, x18` stored the register into `KTLS`, and the gate for `mrs x18, cntpct_el0` / `cntvct_el0` wrote the saved `x18`, which `dm_gate` does not restore. The next `x18` site read whatever `K18` held before. `mrs x18, ctr_el0` stopped the rewrite with an assertion.
- **Fix:** with `x18` as the register, the rewriter copies between thread slots (`site_slot_copy`: `KTLS` or `KTLR` into `K18`, `K18` into `KTLS`, the cache type constant into `K18`), carrying the value in `x15` with `x16` / `x17` for the slot addresses, all saved in the red zone; the value never passes through `x18`. The host's counter gate writes `K18` for a destination of `x18`. The other registers keep their blocks, so every other rewrite is byte-identical: none of 203 modules on hand has one of these instructions.
- **Check:** `test_x18_sysreg.py` checks that no block names `x18`, and runs each block on a thread slot area with unicorn (optional); it fails without the fix.
- **Found on:** The Hinokami Chronicles 2 (`0100AD80208A8000`, 1.70, SDK 22.0.0; not in the [games table](../../dm/games/README.md#games); [issue #2123](https://github.com/seedds/nxnative/issues/2123)): one `mrs x18, tpidr_el0` in `main`, at `+0x23f5ec0`. Its reporter's version of the fix ran 19 minutes into a battle; this one awaits their run.

## Jump tables

### Offset jump tables through x18 read at rewrite time

- **Symptom:** `jump table <addr>` assertion in `patch_nso.py`, in a Unity / IL2CPP `main`.
- **Cause:** the compiler used `x18` for jump-table shapes other than the ones the rewriter knew. One of them is an offset table: `ldrsw x18, [xB, x18, lsl #2]; add x18, x18, xB; br x18`. (The same `main` also uses `x18` for an indirect call and an `alloca`: see [Indirect calls through x18](#indirect-calls-through-x18) and [Stack pointer writes through x18](#stack-pointer-writes-through-x18).)
- **Fix:** the rewriter reads the offset table's targets at rewrite time (`site_br_tab` in `patch_nso.py`).
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md) `main`: 7 jump tables (beside 1 call and 1 `sp` write).
- **Check:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md)'s output stays byte-identical.

### Offset jump tables with the bounds check on another register

- **Symptom:** `jump table <addr>` assertion in `patch_nso.py`, in a `subsdk`.
- **Cause:** the offset-table form `ldrsw x18, [xB, xI, lsl #2]; add x18, x18, xB; br x18` with the bounds check on another register: the index in `w0` (`sub w0, w18, #0xd1; cmp w0, #0x31`), a copy (`cmp w21, #7; …; mov w18, w21`) or a masked copy (`and w16, w18, #0x3f; cmp w16, #0xe; …; and x18, x18, #0x3f`). The entry may also be loaded into another register, with only the `add` writing `x18` (Cuphead, Unity 2017.4: `cmp w17, #7; ldrsw x17, [x15, x17, lsl #2]; add x18, x17, x15`).
- **Fix:** `jump_offsets` follows `mov` copies back to the `cmp` and accepts a check on the same `and` of the same, unchanged source.
- **Found on:** [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md) (GLSLC and movie modules); [Cuphead](../../dm/games/0100A5C00D162000/README.md) (the entry loaded into another register).
- **Check:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md)'s output stayed byte-identical.

### Index copied before the bounds check

- **Symptom:** `jump table <addr>` assertion where the index is a copy made before the check (`mov w0,w18`, then 41 instructions holding two other jump tables on `x0`, then `cmp w18,#4; b.hi; ldrsw x18,[x11,x0,lsl #2]; add x18,x18,x11; br x18`).
- **Cause:** every other form follows copies made after the bounds check.
- **Fix:** `check_before_copy` accepts the `cmp` and `b.hi` right before the load when a `mov wI, wS` is the last writer of both registers; capstone lists a `cmp`'s first operand as written, which the search skips. The search follows the function boundary rather than an arbitrary instruction count, stopping at undecodable gaps and calls that can clobber either register. It runs only when every other form has failed.
- **Found on:** [DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition](../../dm/games/01006C300E9F0000/README.md) `main+0x2dd8cd0`; [Mario + Rabbids Kingdom Battle](../../dm/games/010067300059A000/README.md) `subsdk0+0x7340b8`, whose copy is 71 instructions before the load, across two switches preserving both registers (beyond the former 64-instruction limit).
- **Note:** before fixing one site, a census of all of them saves a run per site: a copy of `patch_nso.py` whose assert prints and continues found this one alone among the 25,508 `x18` sites. That `main` has no unwind entries for its first 70 MB, so the rewriter disassembles them as one function and takes 6 minutes (see [Long functions analysed in pieces](#long-functions-analysed-in-pieces)).

### 64-bit bounds check of the index

- **Symptom:** `jump table <addr>` assertion in `patch_nso.py` on an offset table whose index is checked with a 64-bit compare (`cmp x22, #0x10; …; b.hi; adrp x18 + add x18; mov x0, x18; ldrsw x18, [x0, x22, lsl #2]; add x18, x18, x0; br x18`).
- **Cause:** `jump_offsets` took only a 32-bit `cmp wI, #N` as the bounds check. A 64-bit compare bounds the index as well (the load uses the whole `xI`); here the table base also reaches the load through a copy, so only the far-base pass reads it.
- **Fix:** in its last pass (`far_base`), `jump_offsets` accepts `cmp xI, #N` on the index too. The earlier passes are unchanged, so modules that rewrote before come out the same.
- **Found on:** [OCTOPATH TRAVELER](../../dm/games/010057D006492000/README.md) `main+0x1676f58` (1.0.5), the only such site in that `main`.

### Index zero-extended after a 32-bit bounds check

- **Symptom:** `jump table <addr>` assertion in `patch_nso.py` where the index is made with a 64-bit `and` by `0xffffffff` after the check (`cmp w8, #4; b.hi; and x18, x8, #0xffffffff; ldrsw x18, [x16, x18, lsl #2]; add x18, x18, x16; br x18`).
- **Cause:** `jump_offsets` took the `and` as a mask and looked for the same mask on the checked register, which has none, so no form matched.
- **Fix:** a 64-bit `and` with `0xffffffff` zero-extends the source's low 32 bits. `jump_zero_extended` recognizes a 32-bit `cmp` of that source followed by `b.hi` out of the dispatch. The source stays unchanged up to the mask, the index up to the load, the loaded entry and base up to the add, and `x18` up to the branch. Calls, system calls, traps, other intervening control flow, undecodable gaps, and known entries past the check are rejected; the table must fit in the image and every target must be a decoded instruction outside the guarded run (or its check).
- **Check:** this is a separate fallback after every earlier recognizer fails. Adding the rule inside `jump_offsets` before its existing searches could change a previously recognized table's extent by selecting a nearer compare. The fallback preserves those earlier results; synthetic full-rewriter tests cover both the new five-entry form and an existing eight-entry match with a nearer five-entry check.
- **Found on:** Mario Tennis Aces (0100BDE00862A000) `main+0x35837c` (1.0.0), a five-entry table whose targets all lie in the function ([PR #778](https://github.com/seedds/nxnative/pull/778)). The contributor reported that `main`, `subsdk0` and `sdk` rewrite with zero-extension recognition; gameplay validation remains open.

### Index masked in the block that branches to the dispatch

- **Symptom:** `jump table <addr>` assertion in a Unity / IL2CPP `main` whose dispatch checks a fresh mask of another register than the one it indexes with.
- **Cause:** the dispatch block masks the source again for its bounds check, while the index register was masked from the same source in the block that branches to it: `and x6,x17,#0x7ff; ubfx w18,w17,#0xb,#2; cbz w18,L`, then `L: and w18,w17,#0x7ff; cmp w18,#3; b.hi; ldrsw x18,[x27,x6,lsl #2]; add x18,x18,x27; br x18`, with the base `x27` from an `adrp` + `add` about 1,500 instructions back. A sibling builds its base inside the block and moves registers before the `br` (`adrp x24` + `add`, a `mov`, the load and the add, then 10 register moves). No copy links the two registers, so `jump_offsets` and `check_before_copy` find no check.
- **Fix:** `jump_masked_pred` accepts the site when the block (at most 32 instructions) is `and wC,wS,#M; cmp wC,#N; b.hi` followed by straight-line code holding the load and the add; it is entered by branches alone (the instruction before it is `b` / `br` / `ret`, no branch lands inside it after its `and`, no jump table already resolved in the function reaches it); every branch into it follows, on a straight-line path, `and xI,xS,#M` with the check's source and mask; and the index, the base and `x18` are unchanged where the form needs them. The base is found as `far_base` finds it and every target must be an instruction of the function. It runs only after every other form has failed.
- **Guards (resolved):** direct calls and resolved jump tables must not enter between a predecessor's mask and its branch, since that bypasses the index assignment; direct calls into the dispatch are refused too. Undecodable words break either straight-line run, and table targets must be decoded instruction addresses, not just aligned addresses inside the function. The base uses `copied_base`, which follows `adrp` + `add` and register copies while rejecting 32-bit writes that destroy a 64-bit base.
- **Found on:** FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition 1.0.0 (Unity 2018.4.2f1, SDK 8.2.1) `main+0x2d103cc` and `+0x2d10590`, in one function; the same form in its update 1.0.3.
- **Check:** it runs only where the rewriter asserted before, so modules that rewrote before come out the same; other games' modules were not on hand to compare.

### Zero-guarded complemented jump-table index

- **Symptom:** `jump table 6eea88` or `jump table 6eeac0` assertion in `patch_nso.py` on Return of the Obra Dinn 1.0.1.
- **Cause:** Unity's IL2CPP output uses `tst w15,#7; b.eq exit; mvn w15,w15; and x15,x15,#7; ldrsw x15,[xB,x15,lsl #2]; add x18,x15,xB; br x18`. The zero test excludes one masked value before complementing it: the reachable indices are 0–6, so the table holds seven entries. Treating the `tst` immediate as a `cmp` bound reads an eighth word from the next table, introducing a target the guest cannot reach through this dispatch.
- **Fix:** the last fallback, `jump_masked_not`, accepts this contiguous guard / complement / low-bit-mask form, reads exactly the mask's number of entries, follows the table base through `copied_base`, and requires decoded instruction targets in the function. It rejects intervening register writes or control flow, direct branches / calls and already resolved tables entering after the guard, and its own table targets bypassing the guard. Earlier recognizers are unchanged.
- **Found on:** [Return of the Obra Dinn](../../dm/games/010032E00E6E2000/README.md) 1.0.1 (`main` build `9e93a6566aaa27418dd2435d8d5dffe5`), two dispatches in the function at `main+0x6ee8c0`.
- **Check:** `dm/tools/tests/test_jump_copies.py` exercises the exact seven-entry extent with an invalid next word, mismatched guards / masks / sources, register clobbers, incoming edges, undecodable words and invalid targets. The extracted update's two sites resolve to seven targets each; this is static verification, with gameplay validation recorded in the game note.

### Offset table base set far back in the function

- **Symptom:** `jump table <addr>` assertion on an offset table whose base was set far back (`cmp w18,#5; b.hi; ldrsw x18,[x24,x18,lsl #2]; add x18,x18,x24; br x18`).
- **Cause:** the function's first block loads six table bases with `adrp` + `add` into registers that its loop keeps (`x24` 441 instructions before this load), and a later block one more (`x3`, 549 before the load at `+0x6db70c`); `jump_offsets` follows the base's writer chain 256 instructions back. The function-wide search that `jump_adr_offsets` uses for a hoisted base (the one `adrp` + `add` pair that fits) found two for `x3`, the other a local table of a `br x17` further on.
- **Fix:** where nothing writes the base in those 256, its last attempt (`far_base`) follows the chain to the function's start (`table_base` `far`), through no call when the base is caller-saved, and accepts the table only when every target is an instruction of the function. It runs only where every other form has failed.
- **Found on:** ASTRAL CHAIN 1.0.1 (SDK 6) `main+0x6db4a0`: three of that `main`'s five `br x18` sites need it.
- **Check:** modules that rewrote before come out the same (Tears of the Kingdom, Age of Calamity, Cuphead, DARK SOULS: REMASTERED, Luigi's Mansion 3 and Metroid Dread `main`s: byte-identical).

### Far table base passed through other registers

- **Symptom:** `jump table <addr>` assertion on an offset table whose base was set far back and moved through other registers (`ldrb w18,[x14]; sub w18,w18,#1; cmp w18,#6; b.hi; ldrsw x18,[x7,x18,lsl #2]; add x18,x18,x7; br x18`).
- **Cause:** `x7` is built by `adrp` + `add` 688 instructions before, then parked in `x30` around a loop (`mov x30,x7` … `mov x7,x30`), so the far chain meets a `mov` and stops; the function-wide search finds a second `adrp x7` + `add` pair (another table further on) whose seven targets also lie in the function.
- **Fix:** the last attempt now follows register copies back to the `adrp` + `add` (`copied_base`, through no call while the register followed is caller-saved; the same target check). It runs only where the rewriter asserted before.
- **Found on:** [Super Smash Bros. Ultimate](../../dm/games/01006A800016E000/README.md) 13.0.5 (SDK 8.2.1) `main+0x2b74458`: that `main`'s only unhandled site.
- **Check:** [Mario Kart 8 Deluxe](../../dm/games/0100152000022000/README.md)'s three modules and Smash's `sdk` and movie module come out byte-identical.

### Offset table base initialized after the dispatch

- **Symptom:** `jump table ae95f8` assertion in `patch_nso.py` while rewriting Blasphemous 1.0.8.
- **Cause:** the function enters an initialization block later in address order, then branches backward to `cmp w18,#9; b.hi; ldrsw x18,[x17,x18,lsl #2]; add x18,x18,x17; br x18`. The `adrp x17` / `add x17,x17,#lo` pair is after the dispatch, so the backward writer searches find no base.
- **Fix:** a final `jump_offsets` fallback uses `forward_table_base` only when that adjacent full-width pair is the base register's sole initialization in the function. It rejects other writes, calls that can clobber the base, known branches or table targets entering the add without its adrp, out-of-image tables and targets that are not decoded instructions of the function. Earlier recognizers are unchanged.
- **Found on:** [Blasphemous](../../dm/games/0100698009C6E000/README.md), `main+0xae95f8`, ten table-relative targets.
- **Check:** `dm/tools/tests/test_jump_copies.py` covers the exact table extent, base clobbers, incoming edges, undecodable targets and out-of-image tables. The extracted function resolves to the ten targets read directly from its table.

### A table base register that another loop holds a small constant in

- **Symptom:** `AssertionError: jump table 59dc90` in `patch_nso.py` on Cuphead 1.3.7 `main`, a build its profile names, since e5620b1a.
- **Cause:** the function interleaves four dispatch loops that share registers. `x12` is the base of one (`adrp x12` + `add x12,x12,#0xa54` at `+0x59dba4`) and the constant 1 of another (`mov w12,#1` at `+0x59dbdc`, in a block that leaves by `b` to its own dispatch); only the value of `w8` keeps the paths apart. `jump_offsets` walks the base's writers back in address order. Since e5620b1a it sees a write of `wN` as a write of the base (correctly), so the walk met the `mov` first and gave up, and no later form takes the site.
- **Fix:** general (`patch_nso.py` `null_base`): before the walk has found the base's `add`, a write of a constant that puts the table inside page zero (`mov` of an immediate or of `wzr` / `xzr`, the constant plus 4 bytes an entry at most 0x1000) is passed. Horizon never maps page zero, so the load would fault through that value on the console too: no path of a working game carries it to the load. Any other writer still ends the chain, and a constant between the `adrp` and the `add` is the base's own input.
- **Found on:** [Cuphead](../../dm/games/0100A5C00D162000/README.md) 1.3.7 `main+0x59dc90` (GitHub issue #1532). Its ten targets are instructions of the function, and three of them are the same as those of the two sibling tables in `x15` and `x17` that the rewriter already took.
- **Check:** `dm/tools/tests/test_jump_copies.py` `NullPageBase` (fails on the rewriter before). The other modules on hand rewrite byte-identically.

### Offset tables relative to an adr

- **Symptom:** `jump table <addr>` assertion, entries relative to an `adr`.
- **Cause:** `cmp wI, #N; b.hi; …; adr x18, #L; ldrsw xE, [xT, xI, lsl #2]; add x18, x18, xE; br x18`: target = L + entry, not table + entry.
- **Fix:** `jump_adr_offsets`, tried only where the table-relative form is not found. The bounds check can be on a register copied into the index (`mov w16, w15`), and the table base can be built far from the dispatch; then every `adrp` + `add` pair in the function that builds that register is tried, and a table is accepted only if exactly one makes every target an instruction of the function.
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) `main+0x393bb0` and others.

### ADR-relative jump tables with 64-bit bounds

- **Symptom:** `AssertionError: jump table 30ee50c` while rewriting an ADR-relative dispatch with `cmp x9,#4`.
- **Cause:** `jump_adr_offsets` recognized only 32-bit bounds comparisons, although a comparison of the full 64-bit index also bounds the table. Its backward copy tracking could also retain a destination whose old value had been overwritten.
- **Fix:** accept either comparison width for the tracked index. When following a 32-bit index copy backward, replace the destination with its source, so an earlier check of the overwritten destination cannot justify the load. Other writes still invalidate the match; table and target validation are unchanged.
- **Found on:** [DRAGON QUEST I & II HD-2D Remake](../../dm/games/0100D7C01F254000/README.md), `main+0x30ee50c` (five targets).
- **Check:** `dm/tools/tests/test_jump_copies.py` exercises both comparison widths, unrelated checks, overwritten indices and a bounds check on the copied source with real AArch64 instructions.
- **Resolved:** the recognizer assumed the comparison had an effective unsigned bounds guard without checking the branch or the flags; it checks both now ([A compare that guards nothing taken as a jump table's bound](#a-compare-that-guards-nothing-taken-as-a-jump-tables-bound)). This game's actual dispatch has `b.hi` with unchanged flags. That every incoming path passes the check is still not proven.

### Vector registers read as writes of the jump-table index

- **Symptom:** `AssertionError: jump table 3ce6c` while rewriting `main`, on an ADR-relative dispatch that the existing form describes: `ldrb w0, [x18, #5]; cmp w0, #5; b.hi; ldr s0, [sp, #8]; ldr s1, [x18, #0xc]; adr x18, #L; ldrsw x1, [x17, x0, lsl #2]; add x18, x18, x1; br x18`.
- **Cause:** the backward walk from the load to the bounds check refuses the site when an instruction writes the index. It named each written register `'w' + name[1:]`, so `ldr s0` became a write of `w0`, the index, and `jump_adr_offsets` gave up. `half_count`, `check_before_copy` and the masked form of `jump_offsets` had the same test. It also failed the other way: capstone's `reg_name` calls x30 `lr` and x29 `fp`, so a real write of an index in either went unseen. `jump_masked_not` missed those writes too.
- **Fix:** all of them ask `written_w`, which names only general-purpose registers, with `lr` and `fp` as w30 and w29. A vector or floating-point register (`s`, `d`, `q`, `b`, `h`, `v`) never changes the index.
- **Found on:** Tomodachi Life: Living the Dream (`010051F0207B2000`) 1.0.0, `main+0x3ce6c` (six targets, table at `main+0x2a5967c`), by the pull request's author: with the fix the game rewrites, packs as a Mac app from a provisional profile and boots to its first screen. The other forms were found by reading the code and shown on synthetic sites.
- **Check:** `dm/tools/tests/test_jump_copies.py` puts vector loads and x29 / x30 writes between the check and the load in each form, including `half_count`.

### Overwritten jump-table indices and bases taken as unchanged

- **Symptom:** no failure seen on a game. The rewriter could read a jump table with the wrong size or base, so a case's target or landing pad would be missing or wrong. In the other direction, a halfword table whose base is in x30 got all 65,536 landing pads (`J` rows with 65536 in `main.sites.tsv`), which only makes the patch area larger.
- **Cause:** `jump_offsets` and `half_count` followed `mov w9,w0` as a copy into the index but kept `w9` as the index too, so `cmp w9,#4` before the copy counted as its check. The table-base writer chains and `jump_adr_offsets`' search for the `adr` compared written names with `xN`, so a write of `wN`, which replaces the register as well, went unseen, as did `lr` and `fp`. A compare of `xN` counted as a write there.
- **Fix:** a copy replaces the index, as `jump_adr_offsets` already did. Those searches ask `writes_reg`, which sees either width and the `lr` / `fp` names and leaves compares out. `jump_offsets` also refuses a site whose index is written after its check, such as `add w18,w18,#1`, or a caller-saved index across a call. A write that keeps the value, such as `sxtw x0,w0`, is refused too: no game on the drive has one, and a refusal stops the rewrite with a `jump table` assertion instead of reading the table wrongly.
- **Found on:** reading the code while fixing [Vector registers read as writes of the jump-table index](#vector-registers-read-as-writes-of-the-jump-table-index). Of the 51 `main`s that rewrote in a comparison of old and new, only [Absolum](../../dm/games/0100A83022A22000/README.md)'s changed: its four tables at `main+0x1ed2dc`, `+0x1ed4fc`, `+0x1ed71c` and `+0x1ed8cc`, based in x30, went from 65,536 pads to 445, 373, 301 and 257, each its table's largest entry + 1, and the patch area from 3.7 MB to 570 KB.
- **Check:** `dm/tools/tests/test_jump_copies.py` `CopyAfterCheck`, `IndexChangedAfterCheck`, `HalfCount`, `test_table_base_sees_every_write_of_the_register` and `test_a_32_bit_write_after_the_adr_replaces_its_value`.

### A compare that guards nothing taken as a jump table's bound

- **Symptom:** no failure seen on a game. `jump_offsets` and `jump_adr_offsets` could read a table with the wrong size and report nothing: on a synthetic site with a second compare between the real check and the load (`cmp w18,#4; b.hi exit; cmp w18,#1; ldrsw x18,[x4,x18,lsl #2]`) they returned 2 targets where the check allows 5, so three cases had no landing pad.
- **Cause:** the walk back from the table load took the nearest `cmp` of the index as the bounds check without looking at what acts on it. A compare bounds the index only through the branch that follows it.
- **Fix:** `guarded_count` (`patch_nso.py`) takes a compare only when, between it and the load, the first branch on an unsigned condition comes with the compare's flags still in place (no instruction that sets them and no call in between; a store, or a `cbz`, changes nothing): `b.hi` out of the dispatch gives N + 1 entries and `b.hs` / `b.cs` gives N, which had been read as N + 1; `b.ls` or `b.lo` / `b.cc` into the dispatch counts the same way when the instruction before its target is a `b`, `br` or `ret`, so the failed check does not fall into it. A compare with no such branch only sets the flags, and the walk goes on to the check before it. With none the site is refused, goes on to the other recognizers and ends in the `jump table` assertion, as before for a site with no compare. `half_count` keeps its own form (`b.hi`, `b.hs` or `b.cs` right after the compare).
- **Found on:** reading the code (GitHub issue #977). Of the 27 modules of 6 games rewritten before and after ([Super Smash Bros. Ultimate](../../dm/games/01006A800016E000/README.md) 13.0.5, [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md), [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md), [Pokémon Pokopia](../../dm/games/0400A3A01EE4C000/README.md), [Mario Kart World](../../dm/games/0400C3F00006E000/README.md), [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md)), 165 sites reach these recognizers with a compare, all in Smash's `main`, `rtld` and `subsdk0`: every one is `cmp; b.hi` on the way to the load, one with a store between the two (`main+0x39be740`), so none is refused or resized.
- **Check:** `dm/tools/tests/test_jump_copies.py` `GuardedCheck` (7 of its 8 cases fail on the rewriter before).
- **Open:** no module with `b.hs`, or with a branch into the dispatch, was on the drive, so those two forms are tested synthetically only. A game whose dispatch has another form of check stops at the `jump table` assertion, where it rewrote before with a size nothing had checked.

### Byte tables indexed by x30 (capstone register names)

- **Symptom:** `jump table <addr>` assertion on a byte table indexed by x30 (`adr x18, #L; ldrb w30, [x14, x12]; add x18, x18, x30, lsl #2; br x18`).
- **Cause:** capstone names x30 `lr` (and x29 `fp`), so the index register looked like no register and the `ldrb` never matched.
- **Fix:** the byte / halfword table check maps those names back to 30 / 29.
- **Found on:** [Hyrule Warriors: Age of Calamity](../../dm/games/01002B00111A2000/README.md) `main+0x138216c`.

### Byte tables whose add is far from the br

- **Symptom:** `jump table <addr>` assertion on a byte table whose `add` is far from the `br` (`adr x18,#L; ldrb w0,[x17,x15]; add x18,x18,x0,lsl #2`, then 17 register moves before `br x18`).
- **Cause:** the byte / halfword search looked 16 instructions back.
- **Fix:** `jump_bytes_far` looks 32 back, requires that nothing writes x18 between the `add` and the `br`, and runs only when every other form has failed, so sites that rewrote before come out the same.
- **Found on:** [Octopath Traveler 0](../../dm/games/01005270232F2000/README.md) `main+0x37f9bc8`: one of the 12 `br x18` sites in this `main`.
- **Note:** a census of all the `br x18` sites first (each shape within 16 / 32 / 48 instructions) saves a 12-minute rewrite per site.

### Indirect calls through x18

- **Symptom:** `jump table <addr>` assertion in `patch_nso.py`, in a Unity / IL2CPP `main`, at a `blr x18`.
- **Cause:** the compiler made an indirect call through `x18` (`blr x18`).
- **Fix:** the rewriter calls through `x16` with the original return address, so C++ unwinding still finds its frames (`site_blr`).
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md) `main`: 1 call (beside 7 jump tables and 1 `sp` write).
- **Check:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md)'s output stays byte-identical.

### Indirect tail calls through x18 in IL2CPP thunks

- **Symptom:** `jump table <addr>` assertion on a `br x18` with no table in front of it.
- **Cause:** not a jump table but an indirect tail call: an IL2CPP thunk loads the method pointer with `ldr x18, [x0, #0x18]`, moves its stack arguments with every other scratch register (`ldr x16, [sp]`, `ldp x17, x15, [sp, #8]`, …) and ends with `br x18`.
- **Fix:** when neither table form is found, the last write of `x18` is a plain load and the `br` ends the function, it becomes `x16 = guest x18; br x16` (`tail_call` / `site_br_call`; at a call `x16` / `x17` are free, as for `blr x18`). It is tried only where the rewriter used to stop.
- **Found on:** [Ace Attorney Investigations Collection](../../dm/games/010005501E68C000/README.md) (Unity 2022.3 IL2CPP): `main+0x1d271bc` and `+0x204ebec`.
- **Check:** every module the rewriter accepted before comes out byte-identical.

### Unframed pointer tail calls followed by zero trap padding

- **Symptom:** `AssertionError: jump table d29e68` while rewriting Moving Out 2 1.3.315.
- **Cause:** the function loads its method pointer with `ldp x18,x17,[x17,#0x138]`, moves caller stack arguments and ends with `br x18`, followed by one `udf #0` padding word before the next unwind entry. The terminal-tail rule accepted only `ldr` and required the branch to be the last decoded instruction.
- **Fix:** for a tail followed by zero trap padding, accept a plain paired pointer load as well as a single load, with contiguous decoding, straight-line argument moves and memory transfers, and no writes to SP, FP or LR, including their 32-bit aliases and writeback. The last write to either `x18` or `w18` must be the pointer load. Branches, calls, nonzero traps, decoding gaps and framed thunks do not match this fallback. The terminal-tail rule and other nonterminal-tail checks are unchanged.
- **Found on:** Moving Out 2 1.3.315, `main` build `c552b6464e7348a7bb261a708b353137`, at `+0xd29e68`. This is a rewrite-stage finding, not gameplay validation or a game profile.
- **Check:** `dm/tools/tests/test_tail_call.py` covers synthetic paired loads, zero padding, argument stores and rejection of control flow, frame changes, pointer clobbers and missing instructions.

### Loaded method-pointer tail call before an error block

- **Symptom:** `jump table 53e9c` while rewriting Warm Snow 2.1.4's Unity IL2CPP `main`, or `jump table 108bb70` on Ori and the Blind Forest: Definitive Edition.
- **Cause:** a framed IL2CPP method loads a method pointer into `x18` (`ldr`, or `ldp` with the next field), moves its arguments, restores its frame and executes `br x18`, with code after the `br`: error blocks in a small thunk, the function's other paths in a larger one. The terminal-tail recognizer requires the jump to end the function.
- **Fix:** keep the terminal rule, which proves no frame, unchanged. For a `br x18` with code after it, `tail_call` proves that the frame is gone, because a jump inside a function needs that function's frame. A wrong accept turns a jump table into a call without any error, so what is not proven stays at `jump table <addr>`. It holds for both frames:
  - **Pointer:** the last write of `x18` is `ldr x18, [xN, #imm]` or `ldp x18, xM, [xN, #imm]`.
  - **The path to the `br`:** no call or jump after that load or the frame restore, whichever is first, no branch or store after the restore, and no branch from anywhere lands past the restore. A guard may land before it: real code skips a reload of the pointer that way.
  - **Frame registers:** nothing else on the way writes `sp`, `x29` or `x30`, except that `x29` may be set once to the saved pair's address, and no branch re-enters the frame's setup.
  - **Stack stores:** a fixed offset from `sp` or the set `x29`, whose bytes stay clear of the saved pair; a register offset or writeback is rejected, and so is any exclusive or atomic access, whatever its base, which the rule does not model.
  - **After the `br`:** no branch comes back to the `br` or ahead of it, and no direct call lands in the function.
  - **A 16-byte thunk** (`stp x29, x30, [sp, #-0x10]!; mov x29, sp`, restored by the instruction before the `br`) proves more, as Warm Snow's does: no calls, no stores through other bases, every guard goes to the code after the `br`, at least one does, and that code is error blocks that hold a call out of the function, end in a call, a trap or a branch among themselves, and hold no `ret`, `br` or `blr`.
  - **A function** (`sub sp, sp, #N` first, `add sp, sp, #N` right before the `br`, `x29, x30` saved to and restored from one slot inside the N bytes) is ordinary compiled code: its body may branch, loop and call, as IL2CPP's lazy init does, its stores through other bases cannot be proven apart from its callees' and are allowed, and any of its other paths may follow the `br`. A jump through another register in its body has unknown targets and is rejected.
- **Found on:** Warm Snow 2.1.4, `main` build `306fc054203275803dcbceba7fb7c392`, at `+0x53e9c`. Ori and the Blind Forest: Definitive Edition, `main` build `228b944fcb08ae56180369be66727558`, at `+0x108bb70`, `+0x108ca70`, `+0x13ff8f0`, `+0x14008a0`.
- **Check:** `dm/tools/tests/test_tail_call.py` covers both forms and rejects each broken proof with synthetic AArch64 instructions; no game assets are required by the tests. The function form was measured on real framed tail calls through other registers in Unity mains, whose shapes it must accept.

## Exclusive windows

### Branches within an exclusive window

- **Symptom:** `AssertionError: retry block overlaps a window 34b790` while rewriting Trails in the Sky 2nd Chapter 1.0.4 `main`.
- **Cause:** a conditional branch to the store inside its own exclusive window was mistaken for an out-of-line retry. The same window also contains an unconditional branch, which the window copier did not relocate.
- **Fix:** branches within the same window stay in its copy. Unconditional branches are relocated to the copy or an exit stub; for windows containing one, a control-flow check rejects exits with a live reservation. Direct entries into newly accepted windows from another function are rejected. A retry copy cannot branch into the uncopied middle of a window. Indirect entries outside the existing jump-table checks are not proven absent.
- **Found on:** Trails in the Sky 2nd Chapter 1.0.4 (01007A2027548000). Its `main` rewrites with the fix; gameplay remains unverified.

### Exit stub order in exclusive windows

- **Symptom:** job system hangs; a queue claims thousands of tasks but every slot is empty.
- **Cause:** in an `ldxr … cmp … b.ne out … stxr … cbnz` window, the rewritten stub emitted its exits in the wrong order, so successful stores took the failure path.
- **Fix:** emit the fall-through exit first (`patch_nso.py` `site_window`).
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md): 82 such windows. D2R had no such windows.

### Out-of-line retry blocks of atomic updates

- **Symptom:** `branch into window <addr>` assertion.
- **Cause:** the compiler moved an atomic update's retry path out of line: the failed `stxr` branches to a block `ldxr; cmp; b.eq <the stxr>` that re-enters the window at its store. That block cannot jump into the window's patch copy (the `x18` stand-in would be loaded between its exclusive load and the store).
- **Fix:** the retry block gets a copy of its own: the retry block and the window's tail from the store on, under one prologue (`retry_block` / `site_retry_window`).
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) 1.4.3 `main`, 8 sites.

### A window never spans a second exclusive load

- **Symptom:** branches to an `ldxr … stxr` loop's head landed mid-window.
- **Cause:** before this rule, a retry block's `ldxr` opened a window that ran on into the next, unrelated `ldxr … stxr` loop (`main+0x5a4674`).
- **Fix:** a window never spans a second exclusive load.
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) 1.4.3 `main` (with [Out-of-line retry blocks of atomic updates](#out-of-line-retry-blocks-of-atomic-updates)).

### adrp inside an exclusive window

- **Symptom:** `AssertionError: 0x…` in `site_window` on an `adrp` inside an exclusive window (`ldaxr x9,[x9]; cmp x9,x25; b.ne out; adrp x10 + add x10` (the same address again, for the store); `mov x18,x26; stlxr w9,x8,[x10]`, then loads and `cbnz w9` back).
- **Cause:** a window that uses x18 is copied into the patch area, and an `adrp` there would build another page.
- **Fix:** it is re-encoded for the copy's address with the same target page (x18 as its stand-in), in both window copiers (`window_adrp`).
- **Found on:** [Pikmin 4](../../dm/games/0100B7C00933A000/README.md) 1.1.1 (Unreal Engine 4.26.2) `main+0x43da14c`.
- **Check:** only windows that stopped the rewrite change, so every other module rewrites byte for byte as before.

## Patch area reach

### Halfword jump table landing pads sized to the largest entry

- **Symptom:** `B()` assertion at a spin wait: a branch into the patch area beyond the ±128 MB a `b` reaches.
- **Cause:** each halfword jump table (`ldrh wE, [xT, xI, lsl #1]; …; adr x18, #L; add x18, x18, xE, lsl #2; br x18`) got a landing pad for every one of the 65,536 values an entry can hold (768 KB a table), whatever the switch's size: that `main` has 51 of them, 40 MB, and the spin-wait stubs placed after them were out of reach.
- **Fix:** `half_count` reads the table as `jump_adr_offsets` does (the index's bounds check, following register copies, then the base from `adrp` + `add`, or the one candidate in the function whose every target is an instruction of it) and pads only up to its largest entry; a table it cannot read keeps all 65,536 (7 of the 51). The patch area went from 42 MB to 7.9 MB.
- **Found on:** [Animal Crossing: New Horizons](../../dm/games/01006F8002326000/README.md) 3.0.3 `main`, 63 MB of text.
- **Check:** in the other titles' modules only such tables changed: one in the `sdk` of several SDK 11–20 builds (936 to 1,320 pads), 1 to 6 in some `main`s (Tears of the Kingdom, Breath of the Wild, Silksong, Monster Hunter Rise, Absolum, STEINS;GATE RE:BOOT) and one in some movie and middleware `subsdk`s.

### Patch area before the module base for a large bss

- **Symptom:** `B()` range assertion in `patch_nso.py` (`site_plain`), only for one module.
- **Cause:** its bss is 1.8 GB, the engine's static heap, so a patch area appended after the image starts 1.9 GB from the code and no `B` (±128 MB) reaches it.
- **Fix:** when an appended patch area would start beyond 112 MB, the rewriter puts it 32 MB *before* the module base (negative `PATCH_OFF`). `patched.bin` then holds only text + ro + data (`FILE_IMAGE_LEN`) followed by the patch bytes, and the loader maps the bss and the key page as zero pages (`IMAGE_SIZE`). The Mac loader, the iOS embedding (`gen_embed.py`: no bss in the executable) and `QueryMemory` handle both layouts.
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md) `main`.
- **Check:** Mario's and Engage's output is byte-identical to before.

### Frame-step state with a front patch area

- **Symptom:** `frame step state page` assertion when a `FRAME_STEP` profile is added to a module with a large BSS.
- **Cause:** frame-state allocation required a patch area after the image, although the stubs can address the state page through ADRP with either layout.
- **Fix:** for a front patch area, reserve a zero-filled 16 KB state page immediately after the key page by extending `IMAGE_SIZE`. The existing Mac and iOS loaders map it with the BSS; no padding is added to the game binary. Ordinary modules retain their existing layout.
- **Found on:** [ASTRAL CHAIN](../../dm/games/01007300020FA000/README.md#frame-rate).

### Patch area right before the base for a long text

- **Symptom:** `B()` range assertion, then `patch area of … bytes does not fit in the … before the base`.
- **Cause:** with the patch area 32 MB before the base, sites near the end of the text are more than 128 MB away.
- **Fix:** when the text ends within 32 MB of the reach (`BIG_TEXT`), the patch area goes right before the base: the stubs follow the sites in address order, so each site's stub is about as far from it as the text is long. The first pass tries the gap that keeps every site in reach (2.6 MB here); when the patch area is larger (3.6 MB, 34,322 sites), the rewriter runs itself again with exactly that size (`DM_PATCH_FRONT`, set only by the rerun) and `B()` checks every branch.
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) `main`: 125.3 MB of text.

### Two patch areas for a text longer than a branch reaches

- **Symptom:** `main: patch area of 0x6b9d38 bytes does not fit in -0x39c0000 before the base; rewriting again with 0x6c0000`, then a `B()` range assertion in `site_window`: the gap before the base came out negative.
- **Cause:** a text longer than the ±128 MB a `b` reaches has no place for one patch area from which every site is in reach: before the base the last sites are too far, after the image the first ones.
- **Fix:** such a module (`TWO` in `patch_nso.py`: the text alone ends past the reach) gets two patch areas. The front one, 16 MB before the base, takes the sites below `SPLIT` (128 MB less the front area and 1 MB of margin for a stub's exits past its site); the back one, right after bss, the key page and the frame step page (`BACK_OFF`, 64 KB aligned), takes the others. A site's stubs and their branches back stay in its own area (`use_area`); when the front sites need more room, the rewriter runs again with exactly that size, and `B()` checks every branch. The file is the image, the front area padded to 16 KB, then the back area; `fixups.tsv` adds `BACK_OFF` and `BACK_LEN`, and `IMAGE_SIZE` covers the back area. The Mac loader copies it to `BACK_OFF` and seals it read-only and executable, the iOS embedding gives it a fourth section (`gen_embed.py`: `__TEXT,__dm<name>x2`) that the loader remaps there, and `svcQueryMemory` reports it as CodeData+RX like a patch area after the image (data and bss end at it). A module that does not need it is rewritten byte for byte as before.
- **Found on:** [EA SPORTS FC 27](../../dm/games/0100C49025D3E000/README.md) `main` (1.8d.310c): 185.6 MB of text, 177,058 sites; 3.9 MB of stubs in front and 3.2 MB at `BACK_OFF` `0xdec0000`.

### Split point within the back area's reach

- **Symptom:** a `B()` range assertion in `site_plain` on the branch back to the site, in a module that takes two patch areas.
- **Cause:** `SPLIT` only kept the front area in reach. The back area sits after ro, data and bss, so when those are large it is more than 128 MB from the sites just above a split set by a 16 MB front area: here `SPLIT` was `0x6f00000` and the back area at `0xfa40000` reaches down to `0x7a40000`.
- **Fix:** `SPLIT` is also no lower than `BACK_MIN`, `BACK_OFF` less the reach plus the same 1 MB of margin. When that lifts it, the front area's first pass takes the gap up to `BACK_MIN` instead of 16 MB, so the front area still reaches every site below the split; the rerun with exactly the size its sites need works as before, and `B()` checks every branch. A module whose back area already reaches the 16 MB split (EA SPORTS FC 27) is rewritten byte for byte as before; one where no split reaches stops with a message naming `BACK_MIN`.
- **Found on:** Hello Kitty Party Land `main` (1.0.2): 151.8 MB of text and 98 MB of ro, data and bss after it; `SPLIT` `0x7b40000`, 1.7 MB of stubs in a 3.75 MB front area (no rerun) and 123 KB at `BACK_OFF` `0xfa40000`.

## Cuts and observation gates

### Observation gates on any relocatable instruction

- **Symptom:** `observation gate neither at a function entry nor after a bl`; `observation gate neither at a function entry, after a bl nor before a call` on an epilogue.
- **Cause:** the rewriter assumed the host may change x9–x17 in a gate. So a gate was placed only where they carry nothing: a function entry, after a `bl`, or (from 2026-10-22, `scratch_dead`) where the straight-line code ahead writes them before reading them. The assumption never held. Since the first host, the gate frame (`dm/host/entry.S`, `_dm_gate`) has saved x0–x28, the vector registers and the flags and restored them, and the gate block borrows only x29 and x30, which it restores from the stack.
- **Fix:** since 2026-10-31 there is no placement rule: any instruction `site_obs` can relocate takes a gate. A register changes only when the callback writes it (`OBS_STDALLOC_INIT`'s x2, `OBS_UE_REGION`'s size register and x1).
- **Found on:**
  - [DRAGON QUEST VII Reimagined](../../dm/games/0100A9D01C446000/README.md): the gate on `mov x0, x19` at `main+0x32758e4`, after a cut `bl` and an `adrp`.
  - [Pikmin 4](../../dm/games/0100B7C00933A000/README.md): the gate on `ldp x29, x30, [sp, #0x10]` right after the cut allocator constructor call, `main+0x353f78c`.
  - Both gates were removed on 2026-10-30: the engine-wide allocator rule puts its gate on the cut call itself ([Unreal's FMallocBinned3 replaced by the host allocator](memory.md#unreals-fmallocbinned3-replaced-by-the-host-allocator)).
  - The engine region's gate sits on a store that reads x10 (`str x1, [x10, #0x280]`, Octopath Traveler 0 `main+0x12e20`), which the old rule refused: [Unreal's engine address region and its options](memory.md#unreals-engine-address-region-and-its-options).
- **Note:** the gate table takes the original instruction as a word value (`aa1303e0`), the cut table as bytes (`e00313aa`).

## Functions, unwind entries and data in text

### Code before the first unwind entry is one function

- **Symptom:** `jump table <addr>` assertion on an ordinary byte table near the start of the text (`adr x18; ldrb w0,[x17,x16]; add x18,x18,x0,lsl #2; br x18`).
- **Cause:** the module has no unwind entry before `main+0x6a75d4`, and `func_of` made every site before the first FDE a function of one instruction, so no form saw the `adr` in front of the `br`.
- **Fix:** the text before the first FDE is now one function, from the end of the data block before it (or the text's start), as a gap between two FDEs already was (`func_of`).
- **Found on:** [FINAL FANTASY RESONANCE](../../dm/games/0100606026F12000/README.md) 1.0.1 `main` (Unreal Engine 5.6.1): `main+0x219234`.
- **Check:** of the 120 modules of 33 games on hand, only DRAGON QUEST VII Reimagined's `main` has other `x18` sites there (110 before `+0xb310`, none in a window or a `br x18`), and its rewrite is byte-identical.

### Long functions analysed in pieces

- **Symptom:** the rewrite never finishes, the Mac swaps, the rewriter's footprint reaches 100 GB.
- **Cause:** a stretch without FDEs is one function to `func_of`, and its detailed disassembly costs about 880 bytes an instruction once built (2.6 KB while capstone builds it): 21.6 million instructions. In FINAL FANTASY RESONANCE's `main`, 82 MB at `main+0x6c4f88`–`+0x59220c0` and the 6.65 MB before the first FDE have no unwind entries. Octopath Traveler 0 (71 MB), DRAGON QUEST XI S (68 MB) and DRAGON QUEST VII Reimagined (two of 23 MB) have such stretches too; they got through on a quieter Mac with the compressor and swap.
- **Fix:** a function longer than 256 KB is now analysed in pieces (`pieces` in `patch_nso.py`), cut only right after a `ret` / `br` / `b` where no conditional branch and no `b` within 64 KB crosses, so a window, a jump table's bounds check and table base, and a retry block stay in one piece. After all pieces, every farther `b` / `bl` and every jump table target is checked against the windows and retry blocks of all pieces. Counting `b` up to 1 MB left only 21,385 cut points in the 82 MB (tail calls between neighbours) and pieces of up to 6.5 MB; with 64 KB there are 185,228, and no piece is over 256 KB (the two stretches: 28 and 344 pieces). The rewrite then takes 13 minutes at about 2.7 GB resident on a loaded Mac.
- **Found on:** [FINAL FANTASY RESONANCE](../../dm/games/0100606026F12000/README.md) 1.0.1 `main`; also [Octopath Traveler 0](../../dm/games/01005270232F2000/README.md), [DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition](../../dm/games/01006C300E9F0000/README.md), [DRAGON QUEST VII Reimagined](../../dm/games/0100A9D01C446000/README.md).
- **Check:** DRAGON QUEST VII Reimagined's `main` came out byte-identical to its rewrite by the previous `patch_nso.py`. FINAL FANTASY RESONANCE's came out the same as with `b` counted up to 1 MB (3 and 136 pieces), and DRAGON QUEST XI S's (292 pieces) and Octopath Traveler 0's the same as with 1 MB pieces and `b` within 256 KB (75 pieces for DQ XI S), so the analysis does not depend on where the cuts fall.

### Disassembly goes on after undecodable words

- **Symptom:** `KeyError` at `site_br` for a `br x18` that is an ordinary byte jump table.
- **Cause:** capstone stops at the first word it cannot decode, and the function held a trap word (`0xe7ffdefe`, `main+0x6b34540`) before the jump table, so the table was never analysed.
- **Fix:** function disassembly now goes on after undecodable words (`disasm_fn`).
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) `main+0x6b346d0`.

### Data blocks at the start of the text

- **Symptom:** `RuntimeError: 788 ldr x18, #0xd1eb0` in `patch_nso.py`, or any rewrite site in data.
- **Cause:** the text segment starts with 1.7 MB of data (`main+0x0`–`+0x1b5000`, high-entropy, no FDE covers it, and no `adrp` or relocation points into it): 62% of its words do not decode, and 6,858 of the rest read as instructions that use `x18`, plus three "`svc`" words with numbers such as `#0x91e7`. The linear sweep rewrote them all.
- **Fix:** `dm/tools/nso_code.py` finds such data blocks: a stretch of at least 4 KB between the `.eh_frame` FDE ranges in which more than a quarter of the words do not decode. The sweep skips them (`DATA_BLOCKS`, logged as `data blocks left alone`), and so does `insn_census.py`, whose counts then match the rewriter's again (11,744 `x18` sites in Absolum's `main`). Code without unwind information is still swept: it decodes (Absolum's zlib at `main+0x75261c`, libunwind's register restore at `main+0x28dbd4`).
- **Found on:** [Absolum](../../dm/games/0100A83022A22000/README.md) `main` (.NET NativeAOT).
- **Check:** no module of the 19 other games on hand has a data block, and their rewrites are byte-identical.

### NRO metadata inside the text segment

- **Symptom:** `RuntimeError: 44 ldr x18, #0xfffffffffff40370` while rewriting an NRO.
- **Cause:** an NRO's text segment includes its metadata header. The arbitrary build ID at `+0x40` can decode as instructions; `magic_0898_2015.nro` contains a literal load into `x18` at `+0x44`. The ordinary 4 KB data-gap heuristic cannot exclude this 120-byte header.
- **Fix:** `nso_code.data_blocks` explicitly excludes `+8..+0x80` when the image has the `NRO0` header. The initial entry branch and MOD0 pointer remain intact, and scanning resumes at the first possible function. NSO scanning is unchanged.
- **Found on:** [FINAL FANTASY X/X-2 HD Remaster](../../dm/games/0100BC300CB48000/README.md), 1,432 registered NROs.
- **Check:** `test_nro_header.py` runs the real rewriter on synthetic NROs whose build IDs decode as an `x18` load or `br x18`, checks the header byte-for-byte, and checks that an actual `x18` instruction immediately after it is rewritten. Code before the first unwind entry is covered too: per-function analysis must not reenter the header even when the linear scans exclude it.

## Stack and system registers

### Stack pointer writes through x18

- **Symptom:** `jump table <addr>` assertion in `patch_nso.py`, in a Unity / IL2CPP `main`, at an `alloca` (`mov sp, x18`).
- **Cause:** the compiler wrote `sp` from `x18` for an `alloca` (`mov sp, x18`).
- **Fix:** the rewriter restores its scratch registers relative to the old `sp` (`site_sp`).
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md) `main`: 1 `sp` write (beside 7 jump tables and 1 call).
- **Check:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md)'s output stays byte-identical.

### Stack arguments reserved with sub sp, sp, xN

- **Symptom:** `CRASH sig=10 addr=0` on an `stp` relative to `sp`, with `sp` not a multiple of 16 (`main+0x2bbba4`, `sub sp, sp, #0xc0` then `stp d15, d14, [sp, #0x60]` with sp ending in `…9dc`).
- **Cause:** Horizon leaves stack alignment checking off for user code; Apple's kernel faults on an sp-based access while sp is misaligned. The game's script engine calls native functions through six hand-written bridges (`main+0x17c140` and five more beside it): each reserves the callee's stack arguments with `sub sp, sp, x19`, x19 being their sizes added up (4 for one int, `[x0 + 0x238]`), and gives them back with `add sp, sp, x19` after the call, so the callee ran with sp 4 bytes off.
- **Fix:** the rewriter now sends every `sub sp, sp, xN` (extended-register form, no shift) through a stub that rounds xN up to 16 first (`site_sp_align`); the `add` then gives back the same amount. Compiled code keeps such sizes 16-aligned, so there it changes nothing. `insn_census.py` counts the sites as `sub sp, sp, xN`.
- **Found on:** [Paper Mario: The Thousand-Year Door](../../dm/games/0100ECD018EBE000/README.md) `main` (its AngelScript call bridges).
- **Check:** of the 22 games' modules on hand only this `main` has the form (6 sites), so every other rewrite is byte-identical.

### System register reads that macOS traps

- **Symptom:** SIGILL / trap on a system register read.
- **Cause:** a newer SDK reads a register macOS traps (`CTR_EL0` in SDK 16's `FlushDataCache`).
- **Fix:** rewrite to a constant (`patch_nso.py`, the `ctr` site kind).

## Relocations and module layout

### Packed relative relocations (DT_RELR)

- **Symptom:** every pointer in `.data` / the GOT is a module offset, crashes everywhere.
- **Cause:** SDK 20 modules pack their relative relocations as `DT_RELR` (tag 36, size tag 35): `main` has 223,994 of them and its `DT_RELA` only symbol relocations. Each 8-byte entry is either an even offset (relocate it, move to the next slot) or an odd bitmap whose bits 1–63 stand for the following 63 slots; the addend is the slot's own value.
- **Fix:** `relr_apply` in `loader.c`; `DM_LOADHASH` and `nvn_slots_got.py` read it too. The loader's log line counts them as `relr=`.
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md) 1.9.0 (SDK 20).

### Regression rewrites register copied profiles twice

- **Symptom:** `regress.py check` stops before gameplay with `CUTS of build ... in both ...`, naming a revision profile and its copied harness profile.
- **Cause:** shader builds use a writable profile copy for generated tables. Passing that copy to the rewriter exposes its `patches.py` alongside the revision's original, registering every cut twice.
- **Fix:** registered games rewrite with the revision's original profile; provisional games retain their external profile path. Shader builds continue using their writable copies. The harness regression covers both selections.
- **Found on:** [NieR:Automata geometry review](../../dm/games/010056B015FE8000/README.md#geometry-optimization-review), when comparing rewrites after reconciling PRs #336 and #416.

## Rewriter performance

### Parallel scans preserve rewrite output

- **Symptom:** packing spends most of the rewrite step on the x18 scan and the spin-wait search, using one core per module. Cancelling a parallel rewrite can leave its workers running; `DM_REWRITE_J=0` or a negative value can silently omit spin gates, and the NSP script can ignore an explicit worker limit.
- **Cause:** both searches analyse an unchanged image, but ran sequentially. A default SIGTERM exits without unwinding the pool's context manager. The spin search's job count assumed a positive worker count, and the NSP script always replaced the caller's setting with its automatic budget.
- **Fix:** `patch_nso.py` scans code ranges in 1 MB pieces and distributes spin candidates across forked workers. Each pool inherits the image and analysis state at that pass; workers return addresses, and the parent rebuilds accepted spin regions in address order before emitting patches. SIGTERM exits through Python's cleanup so the pool terminates and joins its children. `DM_REWRITE_J` must be a positive integer (`1` runs serially), checked before loading modules; the NSP script honours an explicit value and otherwise chooses its budget from the core and module counts. These changes apply to every NSO and NRO; patch selection and runtime semantics stay the same.
- **Found on:** [Breath of the Wild](../../dm/games/01007EF00011E000/README.md#found-on-this-game).
- **Check:** synthetic tests cover SIGTERM during active pool jobs, invalid counts and explicit worker limits through the packing script. The original speed measurements and module comparisons are in [PR #761](https://github.com/seedds/nxnative/pull/761); their timings are contributor measurements, not local regression results. Local game validation is recorded in Breath of the Wild's note; other games and NROs have no new local runtime validation here.

### A pool's shutdown hangs on a worker that has just started

- **Symptom:** a rewrite never ends, using no CPU: `patch_nso.py` waits in `wait4` and one forked worker of the same command in `sem_wait`, in `multiprocessing/queues.py` `get`, having run no job. It is rare and needs small jobs (about 1 in 6,000 pools of tiny jobs under load). Separately, a SIGTERM could be ignored until the rewrite ended, or leave workers behind.
- **Cause:** workers inherited the Python SIGTERM handler that lets the parent exit through the pool's context manager. `Pool.terminate` takes the job queue's lock for good, sends each worker one SIGTERM and joins it with no timeout, so it needs that signal to end the worker. A handler only sets a flag, and Python clears flags in a new child, so a worker just forked lost the signal, then waited for that lock forever. Resetting SIGTERM in the pool's initializer alone does not help: the signal can land before the initializer runs. In the parent, the handler's `sys.exit` could also be raised during the pool's teardown, where it stopped a terminate halfway or was discarded in a destructor.
- **Fix:** `in_parallel` blocks SIGTERM across the fork, which keeps it pending in the kernel, and each worker restores the default action before unblocking it. In the parent SIGTERM acts only while waiting for results. A finished pool leaves by `close` and `join`, so no SIGTERM is sent.
- **Open:** cancelling a rewrite still ends in `Pool.terminate`, and a worker it kills while sending a result never releases the result queue's lock, so the parent can hang (Python's own limit; 2 in about 25,000 cancellations of a process doing nothing but pools, far rarer in a real rewrite, which spends its time in analysis). A rewrite that finishes never reaches it.
- **Found on:** the Python tool tests (`discover -s dm/tools/tests`), which hung for 2 hours in a test's small NRO; no game was seen to hang.
- **Check:** `test_rewrite_parallel.py` checks that SIGTERM is blocked from the fork on and that a worker has the default action unblocked before any job, and sends SIGTERM during pool start and during teardown. The race itself is too rare and too slow to repeat in a test.

## Checking a rewriter change

Rewrite every extracted module of every title with the old and the new `patch_nso.py` and compare all outputs; only `build_src.sha256` (the rewriter's own hash) may differ for titles the change is not meant for.
