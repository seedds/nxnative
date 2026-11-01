# 02 — Surveying a new title

Do this before touching the host. Every item answers "what will break, and how much work is it".

**The first pass is automatic.** Run `scripts/build_ipa_from_nsp.py` on the title's NSP folder. When no game profile (`dm/games/*/game.tsv`) has its title ID, the script does no build. It extracts only the code modules plus a RomFS manifest (`nx_extract --manifest`: path, size and first 32 bytes of every file, with no game files written). It then writes `<NSP folder>/<title ID>_survey/survey.md`, which covers:

- game name and display version (from the NACP). Newer SDKs compress the NACP's title area: a u16 size, then raw deflate that inflates to 32 language entries of 0x300 bytes (Mario 1.2.1 does this). LibHac 0.19 does not decode it; `nx_extract` and the script do;
- base / update / DLC NSPs;
- every module with its build ID and segment sizes;
- program ID;
- SDK version and components, the Unity version;
- engine / API markers (NVN, Vulkan, EGL, IL2CPP, sead / agl, Wwise, CRIWARE, …);
- RomFS folders, header magics and extensions;
- the `insn_census.py` counts per module;
- `nso_syms.py` imports vs. host hooks;
- a short list of what that means for a port;
- the provisional profile keys `dm/games/profile_guess.py` reads off these facts, each with its reason.

Fire Emblem Engage, for example, came out as:
- Unity 2020.3 IL2CPP on SDK 12.3.7 with NVN;
- an extra `subsdk0` module (Nintendo's movie player);
- 27,152 UnityFS bundles holding the shaders;
- 101 `nn::hid` imports, plus `irsensor`, `ro` and `swkbd`.

The tables below are what to check by hand afterwards.

## Identity and versions

| Question | How |
|---|---|
| Program ID | NCA header / `main.npdm`. The host reports it from `svcGetInfo(ProgramId)` (`hle_svc.c`, `DM_PROGRAM_ID` overrides); some SDK code checks it. |
| SDK version | Strings in `sdk`: `SDK MW+Nintendo+NintendoSDK_nnSdk-16_2_1-Release` style. SDK 16 changed several object sizes and service paths compared with D2R's SDK. |
| NVN API version | The game calls `nvnDeviceGetInteger(API_MAJOR/MINOR)` and refuses to run if the host reports an older version. Mario required 55.x with minor ≥ 15 (`DM_NVN_API`). Its device setup *returns silently* on a failed check, so a missing frame loop with no error is a hint. |
| Modules | ExeFS listing. The loader takes up to 8 modules, in the order given; a game profile's `modules` key lists them for the rewriter, packaging and the host's default load list. Only `main` + `sdk` have been run so far. |

## Imports vs. host hooks

`dm/tools/nso_syms.py <repo dm/host dir> <gameA ExeFS | -> <gameB ExeFS>` compares the dynamic imports of the new title (B) with the title the host already supports (A) and with the host's hook names. Use `-` for A when no supported title's ExeFS is at hand; the survey mode does.

- Mario imported 1,107 symbols, 692 of which D2R never imported (biggest gaps: `nn::audio` 197, `nn::hid` 48). 172 of the host's 186 hook names existed in Mario's `sdk`.
- Not every import needs a hook. Many SDK functions run natively inside `sdk`; only the ones that end in an IPC to a system service (they reach `nn::sm::Initialize` / `svcSendSyncRequest`) must be replaced by a host hook on the **public entry point**. A missing one shows up at run time as an abort in `nn::sm::Initialize` with a backtrace: hook the outermost public function in that backtrace.

## SVCs and instructions to rewrite

- `dm/tools/insn_census.py <module.nso> [--sites tsv]` counts every site the rewriter must handle: `svc`, `mrs/msr tpidr*`, `x18` use, exclusive windows (`ldxr`/`stxr`), timers (`cntpct`, `cntfrq`), cache maintenance, and system registers.
- Run `python3 dm/tools/patch_nso.py <nso> <out dir> <name>` and check that its counts match the census. New system registers appear with new SDKs: SDK 16 reads `CTR_EL0` (macOS traps it), now rewritten to a constant.
- Mario's SVC set was a subset of D2R's. A new SVC needs a case in `hle_svc.c` / `hle_thread.c`; the default case logs `SVC %#x … (placeholder returns success)` once and returns success, which is wrong for anything that returns data.

## NVN surface

- First check that the game draws with NVN at all. Importing `nvnBootstrapLoader` is not enough: Digimon Story Cyber Sleuth: Complete Edition imports it and EGL, makes an NVN device for a helper beside its movie player, and renders everything with NVIDIA's desktop OpenGL (`eglBindAPI(EGL_OPENGL_API)`, a glad loader asking `eglGetProcAddress` for 2,956 names). Read the graphics setup that calls `nvnBootstrapLoader` / `eglGetDisplay` / `vkCreateInstance`, and count which loaded entry points the code really reaches: for a glad or NVN C loader that stores each pointer in a global, the GOT slots of those globals that code outside the loader loads (Digimon: 79 GL functions, 30 NVN ones, all in that helper; Balatro's LÖVE: about 150 GL functions, whose globals its code reaches through registers, which `dm/tools/loader_census.py` follows). `profile_guess.py` decides `graphics gl` when `main` imports `eglCreateWindowSurface` and names `glShaderSource`; the host's GL layer is `dm/gfx/gl/hle_gl.m` ([04](04-graphics.md)).
- Find how the game obtains NVN entry points. Some games write the slots with `adrp`-to-slot code; Mario's loader (`main+0x1070910`) calls `GetProcAddress` per name and stores through the GOT (`dm/tools/nvn_slots_got.py <main.bin> <loader> <text end> <out tsv>`). The output lists every fetched function and every call site.
- Mario fetched 534 functions and called 236 of them; 79 were new to the host (CommandBuffer 31, Texture 15, Buffer 10, Sampler 10). Names the host doesn't know at all go into `dm/gfx/nvn/names.txt` → `gen_protos.py`.

## Data containers that matter to the host

| What | Where to look | Mario |
|---|---|---|
| Shaders | Every container that embeds NVN shader binaries, not just the obvious archive. Scan all of RomFS and `main` (see [04](04-graphics.md)) | `Shader/*.bfsha` (BNSH), `Lib/agl/*.sarc` + `Shader/Secred.sarc` (agl BAHS `sharcb`), `Effect/*.esetb` (BNSH), `Layout/*.blarc` (BNSH), and two BNSH inside `main` |
| Heap budget | A heap-definition file, if the engine has one | `System/HeapSize/*.HeapDef.byml`: per-subsystem heaps in KB, and a 1.75 GB `VirtualAddressMemory` mapped on demand into the alias region (drives the address-space partition, [memory.md](../reference/memory.md#partitioning-the-guest-block-between-alias-region-stacks-and-heap)) |
| Resource sizes | Resource size table | `System/Resource/ResourceSizeTable.*.rsizetable` (RESTBL, CRC32 of the path → decompressed size) |
| Fonts | Font archives and use of the system shared font | `Font/*.bfarc`; the shared font goes through `nn::fontll::ScalableFontEngineHelper::Decode` (BFTTF) |
| Languages | Per-language archives | `Mals/<Region><lang>.sarc`, `Voice/Resource/<Region><lang>` |

Archives the game reads itself need nothing from the host but plain file access. 13 Sentinels keeps everything in six CRI CPK files (`ROBO_*.CPK`, 4.3 GB) that CRI File System opens through `nn::fs`. To look inside one: the header and TOC are `@UTF` tables (big-endian columns), masked with CRI's byte stream (`m = 0x655f`, then `m *= 0x4115` per byte); file offsets in the TOC are relative to the smaller of `TocOffset` and `ContentOffset`, and files whose `ExtractSize` differs from `FileSize` are CRILAYLA-compressed. Its listing showed no shader container, which is how the shaders were found in `main` instead ([04](04-graphics.md)).

## Ghidra and REA

- Convert with `nso2elf.py`, create one persistent headless project per module, and analyse once (Mario `main`, 28 MB: ~35 min, 138k functions). Ghidra's image base for these ELFs is 0x100000, so `FUN_xxxx` = module offset + 0x100000.
- Decompile on demand in seconds with a script (`ghidra/scripts/Decompile.java` in the workspace):

  ```bash
  JAVA_HOME=<jdk21> $GHIDRA/support/analyzeHeadless $W/ghidra/proj <name> -process <module>.elf \
      -noanalysis -readOnly -scriptPath <repo>/dm/tools/ghidra -postScript Decompile.java 0x107dbc0,0x3707b0
  ```

  `analyzeHeadless` needs `JAVA_HOME` set even when Java is on PATH. Code only reachable through vtables may have no function defined at an address; use `objdump -d --start-address=… --stop-address=… <module>.elf` instead.
- REA (MCP) can open the same ELF, but its snapshots keep evidence only, so every new session re-runs the 35-minute analysis; the persistent Ghidra project is faster for repeated lookups. Register REA with `GHIDRA_INSTALL_DIR`, `JAVA_HOME` and a long `REA_GHIDRA_STARTUP_TIMEOUT_MS`.
- Dynamic symbols (imports / exports with names) come from the NSO's `.dynsym`; read them directly (MOD0 → dynamic section → `DT_SYMTAB` / `DT_STRTAB`) when you need an address for a named SDK function, e.g. `nn::fontll::ScalableFontEngineHelper::Decode` in `sdk`.
