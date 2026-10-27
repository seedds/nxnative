# Shaders

Finding, keying and translating shaders (`dm/gfx/tools/shader_split.py`, `dmsh_build.py`, `dmsh_xlate`, `Program.cs`) and their pipelines. Each entry: symptom, cause, fix, the games it was found on. The [symptom index](../agents/symptoms.md) lists every symptom.

## Containers by engine

### BNSH in NintendoWare archives and in main

- BNSH (`BNSH\0\0\0\0` + `grsc` block at +0x60) is found inside `bfsha`, particle effects (`esetb`, VFXB), UI layouts (`blarc`), and `main` itself.
- Variation table at +0x80 (count at +0x7c); binary program at variation +0x10; six stage pointers from program +0x08 (vs, hs, ds, gs, ps, cs).
- Stage code descriptor: control ptr +0x08, code ptr +0x10, code size +0x18, control size +0x1c. All pointers are relative to the BNSH start.

### Nintendo EPD (sead / agl / gsys): BNSH, agl sharcb and resource packs

- [Super Mario 3D World + Bowser's Fury](../../dm/games/010028600EBDA000/README.md): bfsha (BNSH) and agl sharcb (BAHS version 9, [next entry](#bahs-in-agl-sharcb)) in `ShaderData/*.szs`, particle effects (BNSH, including 25 compute shaders) in `EffectData`, six layouts and `SystemData/AglResource.szs` (BAHS), and two BNSH in `main`: 11,080 shaders. All bind with Mario's texture handle layout (`0x20 + 8 × slot`); the one failure is a geometry shader (fur fins, `RenderFinFur`), which Metal cannot express ([Geometry shaders with no Metal stage](#geometry-shaders-with-no-metal-stage)).
- [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md): 19,592 shaders: `Shader/*.sbfsha` (BNSH) and the gsys / ksys / terrain resource packs inside `Pack/Bootup_Graphics.pack` and `Pack/TitleBG.pack`, the particle effects in `Effect/*.sesetlist` (11,373 shaders, 101 compute), layouts (`.sblarc`) and 37 in `main`. All bind with Mario's texture handle layout; the full build had no failure and no unresolved binding.

### BAHS in agl sharcb

- **How it is stored:** agl `sharcb`, the engine utility shaders: code region at header +0x1c (size +0x18). Binary entries in the table area are 16 bytes `{code offset, 0, control size (0x880/0x88c; 0x848 in BAHS version 9, Super Mario 3D World), signed offset from this entry to the control section}`.
- **Pairing:** control sections are shared between programs, so pair by entry, never by searching for code near a control.
- **Found on:** [Super Mario 3D World + Bowser's Fury](../../dm/games/010028600EBDA000/README.md) (BAHS version 9).

### Zstd files recognised by their magic

- **Symptom:** `shader_split.py` finds 0 shaders in a RomFS of zstd files named `.zst`.
- **Cause:** the split decompressed only `.zs` files.
- **Fix:** it now goes by the zstd magic (`28 b5 2f fd`) whatever the name.
- **Found on:** [Paper Mario: The Thousand-Year Door](../../dm/games/0100ECD018EBE000/README.md) (8,726 of the RomFS's 9,959 files, the shader archives among them). Of the other 21 games' RomFS on hand only Super Mario Galaxy's `GDDR3.zst` (a Vulkan game, no NVN split) has such a file.

### HAL's `.cmp` containers: a size-prefixed frame

- **Symptom:** the game asks for shaders the map does not have (`[RED] shader ... is not in the shader map`, hundreds of lines), and the split finds far fewer shaders than the game uses (121 against 27,288 on Kirby and the Forgotten Land).
- **Cause:** HAL keeps its assets in `.cmp` containers: a 4-byte little-endian uncompressed size, then a frame (zstd, or LZ4 in the `ubin/` map data). The split decompressed a zstd file by its magic at offset 0, so a frame that starts at offset 4 was read raw, and the shaders inside the BFRES archives of those containers were never seen.
- **Fix:** the split opens a size-prefixed frame as well (`unsized`: zstd, with a dictionary as for a plain zstd file, or LZ4) when the file's bytes 4 to 8 are a frame magic and bytes 0 to 4 are not the zstd magic. A frame whose header records a size other than the prefix is not such a container, and the file is read as it is: Tears of the Kingdom's 1,202 `Cave/…/C.crbin.<hash>/*.quad` files are a 4-byte hash (the one in their folder name) and a zstd frame, the only files with a frame at +4 in the RomFS of 39 other games on hand. A frame that records no size must decompress to exactly the prefix's size; otherwise (it does not decompress, it holds more or less, or it names a dictionary `Pack/ZsDic.pack.zs` does not have) the split prints a `[RED]` line and reads the file as it is.
- **Found on:** [Kirby and the Forgotten Land](../../dm/games/01004D300C5AE000/README.md): 7,201 `.cmp` files, 5,121 of them zstd frames with 19,364 BNSH inside (11.2 GB decompressed) and 2,080 LZ4 frames, all of the latter in `ubin/` (0.3 GB, and no BNSH); the same layout is in 430 `.cmpbin` files (76 zstd in `lyt/`, 354 LZ4 in `snd/`). Every zstd frame records its size, the same as its prefix; no LZ4 frame does, and each decompresses to its prefix's size. The split now finds 27,288 shaders in the base game (18,476 in `gfx/`, 8,313 in `ptcl/`, 378 in `lyt/`, 67 in the loose `shader` files, 54 in `main`), against 121 before, and 27,302 with update 1.1.0 (v65536: 14 more in `lyt/`).

### BNSH in Bezel Engine archives

- **How it is stored:** the shaders are BNSH in the `.bfsha` entries of the `.bea` archives (zstd entries, [01](../agents/01-extract.md)), most of them the engine's own in `System.nx.bea` (`_BezelSystemResources/<system>/shader/*.bfsha`: PBR shadow maps 846, post effects 770, …), the rest in the layouts' `__ArchiveShader.bnsh` and the per-archive shader packs (`.bnbshpk`), plus one BNSH in `main`.
- **Reader:** `shader_split.py` scans each archive entry as a file of its own (`bea_members`).
- **Found on:** [Big Brain Academy: Brain vs. Brain](../../dm/games/0100620012D6E000/README.md) (NintendoWare Bezel Engine 1.56): 2,125 shaders in about 90 s.

### BNSH entries of GFPAK archives

- **Symptom:** a provisional split finds only a fraction of the shaders (3,126 of Pokémon Sword's 41,516).
- **Fix:** `shader_split.py` `gfpak_members` reads the BNSH entries of GFPAK archives (LZ4 entries, [01](../agents/01-extract.md)).
- **Found on:** [Pokémon Sword](../../dm/games/0100ABF008968000/README.md): 41,516 shaders, nearly all BNSH entries of the 7,699 GFPAK archives under `bin/archive` (the models' and effects' shader archives), beside loose BNSH files (`bin/battle/waza/model/**/*.bnsh_vsh` / `_fsh`, `bin/graphics`, `system_resource/shader`) and two in `main`. The YEBIS shaders it also draws are in `main` ([Shaders in a zlib stream in main](#shaders-in-a-zlib-stream-in-main-yebis-tfxo)).

### BNSH in Trinity packs

- **How it is stored:** every shader is a BNSH inside the Oodle-compressed files of `arc/data.trpfs` ([01](../agents/01-extract.md)).
- **Reader:** for any game with Trinity packs (`ONEPACK` files), `trpfs_jobs` / `trinity_shaders` in `shader_split.py` name the packs from `data.trpfd`, decode every file with the game's own Oodle in the Mac host (`DM_OODLE_HOST` / `DM_OODLE_MODULES`, set by the NSP script) and scan it like a loose file.
- **Copies that differ:** the layout archives' control sections are 104 KB long, and their stage descriptors run 456 bytes past the control's own length (control +0x20), with bytes that differ between copies of one shader; `same_shader` compares copies only up to the control's own length.
- **Found on:** [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md) (Game Freak's engine): 28,603 shaders (11,633 vertex, 16,845 pixel, 125 compute) and 26,051 variation pairs: the material shaders in `system_resource/gfx2/shader/NX64/env_ikkaku` (`material/standard.bnsh`, `material/transparent.bnsh`, `system/light.bnsh`, and the `special/imgui.trsha` pack with the engine's system shaders), particle effects (`.ptcl` / `.trtml` in `ik_effectdata`, 19,842 of the shaders), UI layouts (`__ArchiveShader.bnsh` in the `.truiv` packs of `uidata`) and the 40 BNSH in `main`.

### GLSLC outputs in main and in loose files

- **How it is stored:** 13 Sentinels (Vanillaware's engine) has no shader file in RomFS, whose six CRI CPK archives hold textures, scripts, CRI audio and USM movies: 838 GLSLC compiler outputs sit in `main`'s read-only segment, each referenced from a `{pointer, size, 0}` table in `.data`. Other engines keep whole outputs in loose RomFS files of their own formats (below).
- **Layout:** an output starts with the magic `0x19866891`, its total size is at +0x44, and it holds exactly one control section with the code area right after it (control +0x20 = control length); a debug section with the NV assembly and the GLSL source follows. The stage comes from SPH word 0, as for BAHS. Outputs that hold several stages, and SDK 4's outputs, are read through their section headers ([GLSLC outputs holding several stages](#glslc-outputs-holding-several-stages), [GLSLC outputs read through their section headers](#glslc-outputs-read-through-their-section-headers)).
- **Reader:** `shader_split.py` reads them in every game's `main`, accepting only outputs that pass the control / code / length check. Loose RomFS files are searched the same way: it reads GLSLC outputs in any RomFS file no other reader takes. When the reader was written, of the other profiles' files only Metroid Dread's MSHD files held any, which `mshd_shaders` reads ([MSHD in MercurySteam's bshdat files](#mshd-in-mercurysteams-bshdat-files)).
- **Found on:**
  - [13 Sentinels: Aegis Rim](../../dm/games/01003FC01670C000/README.md): 838 in `main`.
  - [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md): 11 in `main`, Unity's own.
  - [FINAL FANTASY TACTICS - The Ivalice Chronicles](../../dm/games/010038B015560000/README.md): 257 whole GLSLC outputs in the engine's technique files (`*.tec`: `resources_nx/release/master/shader/*.tec`, magic `TEC\0`, each output whole after a table of parameter names) beside 49 in `main`.
  - [Absolum](../../dm/games/0100A83022A22000/README.md): 163 GLSLC outputs in `shaders.nx.bin` (`rom:/shaders.nx.bin`: a u32 count, then per entry a u32 hash, a u32 size and one whole output): 162 unique shaders, 83 pixel and 79 vertex, every vertex shader paired with the single-stage cover ([Single-stage programs paired with a cover](#single-stage-programs-paired-with-a-cover)). Its `.xnb` effects are XNA's D3D9 effects, MOJOSHADER linked in, which the GPU does not run.
  - [Catherine: Full Body](../../dm/games/0100BF00112C0000/README.md): 3,050 loose files in `data/shaders_binary` (`*.vs` / `*.ps`, one stage each, one output per file), read like FINAL FANTASY TACTICS' technique files; its vertex shaders are in two parts ([VertexA and VertexB parts merged into one vertex shader](#vertexa-and-vertexb-parts-merged-into-one-vertex-shader)).

### GLSLC outputs holding several stages

- **Symptom:** the split found no shader in the game (0 shaders).
- **Cause:** an output may hold several stages compiled together as one program (Baba Is You: the 120 outputs in `Assets.dat`, each a pixel and a vertex shader, then a reflection section), which the single-stage reader does not take.
- **Fix:** such an output is read by its section headers (+0x4c section count, section *i*'s header at +0x90 + 0x90 *i*, type 0 = GPU code; `glslc_program`; the same headers SDK 4's outputs are read through, [GLSLC outputs read through their section headers](#glslc-outputs-read-through-their-section-headers)), and its stages are paired with each other, not as single-stage programs. The NSP script now stops at the split when it finds none.
- **Found on:** Baba Is You (no profile).

### GLSLC outputs in a zip archive, and those NROs compile at run time

- **Symptom:** an NVN game's split finds none of its shipped shaders, and the ones it compiles at run time were not captured: the game's renderer is in NROs that call `glslcCompile`, and its whole RomFS is one zip.
- **Cause:** Portal keeps all its files in `game.zip` (41,416 members, Deflate or stored). The Source engine's D3D9 layer (`materialsystem.nro`) links each vertex and pixel shader combination into one program; the programs of the game's precache list (`portal/glbaseshaders.cfg`, 219 entries) ship compiled as `nxcontent/nvsb/<hash>.nvsb` (`NVSB`, version 0x65, the file's length at +8, a 0x60-byte header, then one GLSLC output holding a vertex and a pixel shader). A deflated member hides the GLSLC magic from the loose-file scan. The game compiles the rest with `glslcCompile` (its movie shaders and its own blits: 10 outputs within 30 s of boot), and the capture rule looked only for `main` holding GLSL sources and importing `glslcCompile`.
- **Fix:** `shader_split.py` reads zip archives member by member in parallel ranges (`zip_jobs` / `zip_shaders`; `zipfile`), each member scanned like a loose file, whole GLSLC outputs included. `profile_guess.py` `shader_sources` asks for the run-time GLSLC capture when an NRO the game loads imports `glslcCompile` and a subsdk defines it (`glslc_capture`). Portal's map: 242 keys from the `.nvsb` files and 13 from the capture, 229 vertex and pixel pairs.
- **Found on:** [Portal](../../dm/games/01007BB017812000/README.md). No other profile's RomFS has a zip archive holding shaders.

### G2S containers in KTGL RDB archives

- **How it is stored:** each material's shaders are a G2S container (`_S2G3000`, type KTID `0x7bcd279f`; 23,108 entries in `asset/system.rdb.bin*`, 2.3 GB inflated) of GLSLC outputs, as in 13 Sentinels' `main`, inside zlib-chunked IDRK entries ([01](../agents/01-extract.md)). One `_DOK0000` container (type `0x20a6a0bb`, the engine's system resources) holds 151 more bare GLSLC outputs, 21 of them drawn in the first battle.
- **Reader:** `shader_split.py` maps the RDB files and inflates only entries that start with those two magics (`idrk_shaders`), and the 117 outputs in `main` as in every game. Many outputs carry 0x850-byte control sections and the compute ones have no code header, so `glslc_shaders` accepts controls from 0x760 bytes and takes the stage of headerless code from the thread group size at control +0x748. Pairs come from the single-stage cover (`separable_pairs`).
- **Found on:** [Hyrule Warriors: Age of Calamity](../../dm/games/01002B00111A2000/README.md) (Koei Tecmo's KTGL): 40,726 shaders (37,043 pixel, 3,440 vertex, 241 compute, 2 geometry), Mario's texture handle layout (`0x20 + 8 × slot`). [ROMANCE OF THE THREE KINGDOMS 8 REMAKE](../../dm/games/0100C2A01A730000/README.md)'s newer KTGL keeps them (`_S2G5000` / `6000` / `7000`, and `_DOK0000`) in `.fdata` files whose IDRK entries are LZ4 or zlib in chunks of another form ([01](../agents/01-extract.md)): 59,214 outputs there and 149 in `main`, all of them built.
- **Note:** the older KTGL of Three Houses packs the same G2S containers differently ([SPKG packages in KTGL LINKDATA](#spkg-packages-in-ktgl-linkdata)).

### SPKG packages in KTGL LINKDATA

- **How it is stored:** shader packages (`SPKG`: `patch*/nx/shader/*.spkg` and 132 entries of `DATA1.bin`) of G2S containers as in the [RDB entry](#g2s-containers-in-ktgl-rdb-archives), one `_SPK4100` package without G2S (the post effects, `DATA1.bin` entry 549), and map and model archives carrying packages of their own.
- **Reader:** `kt_shaders` inflates every LINKDATA entry and loose `*.gz` / `*.spkg` file ([01](../agents/01-extract.md)), takes every GLSLC output that passes the triple check, and scans its SARCs for BNSH; `DATA1.bin` is split into 64 scan jobs by entry range (`linkdata_jobs`).
- **Symptom:** 24 shaders before the control-length fix, 1,355 after.
- **Cause:** its GLSLC (output version 0x1d.2) writes the 0x848 / 0x850-byte controls and headerless compute code described in the RDB entry, which `glslc_shaders` used to drop. A few outputs of version 0x1d.0 keep something else at control +0x20 (odd values such as 0x136b), so a control length must be a multiple of 4; the section-header reading now takes 8 of them ([GLSLC outputs read through their section headers](#glslc-outputs-read-through-their-section-headers)).
- **Found on:** [Fire Emblem: Three Houses](../../dm/games/010055D009F78000/README.md) (the older KTGL).
- **Note:** nine more shaders exist only at run time ([Built-in shaders compiled with the game's own GLSLC](#built-in-shaders-compiled-with-the-games-own-glslc)).

### GLSLC outputs in FromSoftware's BND3 and dvdbnd archives

- **How it is stored:** RomFS is two archives, `dvdbnd0.bdt` and `dvdbnd1.bdt` (3.6 GB), each indexed by the `.bhd5` beside it: `BHD5`, 0xff (little-endian), u32 1, u32 file size, u32 bucket count, u32 0, u64 bucket table offset; each 16-byte bucket `{u32 count, u32, u64 offset}` points at 16-byte entries `{u32 path hash, u32 size, u64 offset}` (paths only hashed). 5,184 entries are DCX containers (`DCX\0`, big-endian header, `DFLT` = a zlib stream at +0x4c), most of them BND3 archives (28-byte entries `{u8 flags, i32 size, u64 offset, i32 ID, u32 name offset, i32 unpacked size}` in formats 0x5c / 0x7c). Six BND3 archives in `dvdbnd0.bdt` hold the shaders, one GLSLC output per entry (`.vpo` vertex, `.fpo` pixel, named `Shader/FRPG_Sfx/NVNShaderBinary/ps/…` or by base name in the two FLVER material archives, 2,602 + 486 entries).
- **Reader:** `bhd5_entries`, `undcx`, `bnd3_members`, `bdt_shaders` in `shader_split.py`, in 64 jobs by entry range, about 10 s. Only archives that contain a GLSLC magic are parsed (the others use format 0x78).
- **Note:** SDK 4's GLSLC needs its section headers read ([GLSLC outputs read through their section headers](#glslc-outputs-read-through-their-section-headers)).
- **Found on:** [DARK SOULS: REMASTERED](../../dm/games/01004AB00A260000/README.md).

### MSHD in MercurySteam's bshdat files

- **How it is stored:** `system/shd/*.bshdat`, the only place the game's shaders live (nothing in `main` or the `.pkg` packs). Each stage is a control section, whose length is at control +0x20, sometimes followed by a small compiler-option table (`fastmath true`), then the code area at the next code header. Every file holds vertex / pixel stages in alternating order (1, 2 or 4 programs) and is paired two by two.
- The control section carries NV assembly text after its first 0x880 bytes, and the SASS disassembly follows each code area, so the binding layout can be read straight from the files (`TEX` / `TEXS` handle immediates).
- **Reader:** `mshd_shaders`.
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md) (MercurySteam's engine): 478 files, 1,342 stages, 1,010 unique keys (359 vertex, 651 pixel), 659 pairs.

### SDF definitions in RE Engine PAKs

- **How it is stored:** shader definitions (`SDF\0`, 2,685 files in the PAK view, [01](../agents/01-extract.md)): u16 version 1, u16 entry count, u64, then 0x108-byte entries from +0x10. Entry +0x10 holds six u64 offsets (vertex, hull, domain, geometry, pixel, compute; 0 = none), each to `{u32 control length, u32 code length}` followed by the control section (0x880 bytes) and the code area; compute code has no header. An entry's stages are one program, so its vertex / pixel stages are a pair. Empty definitions are 16 bytes of version 0.
- **Reader:** `kpka_jobs` maps the PAKs, takes the newest copy of each path, peeks at 4 decompressed bytes and inflates only SDF entries (`sdf_shaders`).
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md): 63,111 shaders (30,233 vertex, 21,475 pixel, 11,271 compute, 53 geometry, 33 hull, 46 domain) and 102,345 pairs, in 5 s with 12 jobs.

### Shader records in Snowdrop SDF data slices

- **Symptom:** the split finds no shaders in a RomFS made of `.sdfdata`, `.sdftoc` and `.sdfver` files.
- **Cause:** Snowdrop stores the shaders in individually compressed records inside its `BERG` data slices. These SDF archives are a different format from RE Engine's shader definitions above. A shader record has a 20-byte header `{u32 0xabcd00c6, flags, 0, raw length, stored length}`, then one zstd frame; reflection data precedes the NVN sections inside it.
- **Fix:** `shader_split.py` recognizes `BERG` by magic and reads the length-delimited compressed records (`snowdrop_shaders`). Every control section is followed immediately by its code, with lengths from control +0x20 and +0x704; they are byte-packed, so a control need not be aligned. A single record can hold more than one shader. Each section must pass the control / code / stage checks. Programs hold one stage, and the vertex shaders take the [single-stage cover](#single-stage-programs-paired-with-a-cover). Scanning for the record header avoids mistaking a coincidental zstd magic inside compressed data for another asset.
- **DLC:** additional shader records ship in the DLC's data slices. The NSP script includes each extracted DLC RomFS as an extra shader root, without an ExeFS module, and fingerprints its index and extraction timestamps for split reuse. Vulkan and OpenGL builds include the DLC roots too. When installing or removing optional DLC changes an NVN shader union, the script uses an external profile with new shader tables only after every non-DLC shader row and fact matches the registered tables; package indices may change. The registered module builds, settings and build-gated cuts are preserved. A cached proof of this comparison permits repeat builds to reuse the split.
- **Found on:** [Mario + Rabbids Sparks of Hope](../../dm/games/0100317013770000/README.md) (Snowdrop, SDF version 22).

### SPK shader package in an MT Framework ARC

- **How it is stored:** the engine's one shader package, `sc\NX_64\root` (`SPK\0`, type hash `0x02358e1a`) in `nativeNXx64/sa/NX_64/root.arc` ([01](../agents/01-extract.md)). After the package's tables every shader is a code area followed directly by its control section, back to back to the end of the file: 230 vertex shaders, then 230 pixel shaders.
- **Reader:** each control section names its code length (+0x704), so the code is the bytes right before it, and every block must start where the previous one ended (`mtspk_shaders`). The tables that say which vertex and pixel shaders the engine combines are not read: the vertex shaders are paired with the single-stage cover (one pixel shader reads every input any of them reads).
- **Note:** `CustomShaderPackage.mfx` (`MFX\0`, the effect descriptions) holds no shader code, and neither `main` nor any other ARC entry does.
- **Found on:** [The Great Ace Attorney Chronicles](../../dm/games/010036E00FB20000/README.md) (Capcom's MT Framework): all 460 build, with Mario's binding layout; every binding decodes.

### PKZ version 1 archives

- **Symptom:** `[RED] ... PKZ version 0x10000: only 0x20000 is known` stops the shader split.
- **Cause:** the PKZ reader accepted only the version first found on Bayonetta 3. Version `0x10000` uses the same 0x20-byte header, 0x20-byte entries and relative name offsets; the last header word is zero rather than a v2 alignment. The named compression method still determines how each entry is decoded.
- **Fix:** `shader_split.py` accepts both `0x10000` and `0x20000`, retaining rejection of unknown versions. The existing Zstandard decoder and DAT/shader scanners handle the contents. Synthetic directory fixtures cover both versions in `dm/gfx/tests/test_pkz.py`.
- **Found on:** [ASTRAL CHAIN](../../dm/games/01007300020FA000/README.md).

### PlatinumGames' shader objects

- PlatinumGames' shader objects: `VSO\0` / `PSO\0` / `CSO\0` / `HSO\0` / `DSO\0` (the stage), u32 compiler version, u32 control length, u32 control offset (0x18), u32 code length, u32 code offset, and nothing else. The engine makes a program of each and binds them per stage at draw time, so they are single-stage programs, paired with a cover pixel shader (`shader_split.py` `pg_shader`; the archives: [01](../agents/01-extract.md)). It finds its built-in ones by name (`hwfillcolor.vso`, …) and checks each object's GPU code version against the driver's (the NVN layer reports the translator's supported GPU-code range: [Newest GPU code version reported by the driver](nvn-objects.md#newest-gpu-code-version-reported-by-the-driver), SDK 13.3.2).
- **Found on:** [Bayonetta 3](../../dm/games/01004A4010FEA000/README.md): 8,024 objects in `root_000.pkz` (`shader.dat`, `shader2.dat`, `shadereff*.dat`, `autoshadereff.dat`); 6,424 distinct keys (4,288 pixel, 2,047 vertex, 78 compute, 6 domain, 5 hull).

### Size-delimited PlatinumGames shader objects

- **Symptom:** the split finds zero shaders and refuses the shader build, although the CPK's DAT archives contain the objects passed to NVN at boot.
- **Cause:** another PlatinumGames wrapper stores a binding table before the code and control sections, rather than the stage-tagged object above or a BNSH file: u32 prefix length (including that word), binding bytes, u32 code length, code, u32 control length, control. The observed prefix is `0x88` bytes and the control section `0x880` bytes.
- **Fix:** `shader_split.py` `pg_sized_shader` recognizes the self-described section boundaries and NVN magics (`0x12345678` for graphics code, `0x12345679` for compute, `0x98761234` for control). The prefix must be aligned and in bounds, both sections must fit, the record must end with its control section, and the control's code length at `+0x704` must agree. Graphics stages come from SPH; the compute magic identifies compute. The reader emits `#PGSO` records so the existing single-stage cover pairing applies. Recognition needs no title or filename rule, and the shader translation chain is unchanged.
- **Found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): the objects in five DAT shader archives inside `data000.cpk`; the game note records the census and the runtime-key checks.

### BNSH files in PlatinumGames DAT archives inside CRI CPK archives

- PlatinumGames' engine in its Wii U form keeps its shaders as BNSH files of one stage each, the `.vso` / `.pso` members of PlatinumGames DAT archives (`shader.dat`, `shader2.dat`, `shadereff.dat`) inside a CRI CPK archive, all CRILAYLA-compressed ([01](../agents/01-extract.md) has the CPK layout). `dm/gfx/tools/cpk.py` reads the @UTF tables (and the masked ones of Unicorn Overlord's CPKs, the same tables as before once unmasked), the TOC and CRILAYLA (a decoder over a bit string, about 29 MB/s); `shader_split.py` `cpk_jobs` splits the CPKs into jobs and scans each entry (a CPK it cannot read is scanned as one file, with a `[RED]` line).
- Each file is one variation with one stage, so the split pairs nothing; as in Hades II, the vertex shaders write every output their pixel shaders read and no pipeline failed to link.
- **Found on:** [Bayonetta 2](../../dm/games/01007960049A0000/README.md): 1,856 distinct keys (1,547 pixel, 309 vertex) in `shader.dat`, `shader2.dat` and `shadereff.dat` of `data000.cpk` (the split about 2.5 minutes); [Bayonetta](../../dm/games/010076F0049A2000/README.md): 458 distinct keys (236 pixel, 222 vertex), the members of `shader.dat` in `data00.cpk` (about 1.7 minutes). All build for both.
- **Not checked:** 13 Sentinels and Catherine: Full Body's CPKs (no files on the drives).

### Monolith ARH1, XBC1 and HCPS/SLCT shader containers

- **Symptom:** archive shaders are absent from the map; the two boot shaders in `main` are reported as raw controls with no identified code area.
- **Cause:** Monolith uses ARH1/ARD archives and XBC1 streams. HCPS shader containers live inside models, map streams and standalone shader files; a standalone SLCT in `main` omits the HCPS header.
- **Fix:** `shader_split.py` reconstructs paths from the ARH1 XOR trie, reads bounded archive entries in parallel, and decodes stored/zlib/zstd XBC1 streams recursively. The ARH stored size excludes an XBC1 wrapper's 48 bytes. Decoded members are searched for both HCPS and complete GLSLC outputs (effect asset packs); compressed literals are visited only after decoding. HCPS's SLCT/NVSD descriptors locate each variant's control and code ranges; a vertex bundle can include geometry, so stages are split by each control's own lengths. Text-bearing controls advance at eight-byte alignment. Standalone SLCT sections use offsets relative to their own start. Source-only HCPS containers with empty code/control ranges hold no NVN binaries. Pairs stay within their program variant. Synthetic fixtures cover wrapper bounds, embedded geometry, variants, standalone offsets and compressed streams containing literal container magics.
- **Nested effect compression:** Firehawk WIPAC resources can wrap their `BlShaderArchive` GLSLC programs in `cmp\0zlib`: an eight-byte signature, little-endian stored/raw u32 lengths, then zlib. This layer lives inside the decoded XBC1 entry. The recursive reader checks both lengths and the end of the stream; it excludes these compressed ranges from raw container scans too. The four initially missing geometry/pixel keys and two additional vertex/pixel keys from Xenoblade Chronicles 3's opening match `/effect/fld/ma01a.wipac` after this decoding. Synthetic tests cover nested literal streams and incorrect sizes.
- **Found on:** [Xenoblade Chronicles 3](../../dm/games/010074F013262000/README.md#files-and-shaders), whose note records the census and runtime key checks.
- **Format references:** [ARH1 layout](https://github.com/roccodev/ard-tools/blob/master/docs/arh1.md), [xc3_lib shader structures](https://github.com/ScanMountGoat/xc3_lib/blob/main/xc3_lib/src/spch.rs). The reader's lengths and keys were checked against the supplied game files.

### Needle shader containers in main and GFD archives

- **Symptom:** the split finds no shaders, although `main` contains NVN control sections; it reports that no data record or code-address pair identifies their code areas.
- **Cause:** Needle keeps independent shader programs in `HNSHV002` containers, embedded in `main` and in GFD's GSC files inside CRI CPK archives. These are neither GLSLC outputs nor BNSH files. A container can hold many variants separated by reflection metadata.
- **Fix:** `shader_split.py` `needle_shaders` uses the container's big-endian whole length at +8, then the little-endian NVN section descriptors: 0x90 bytes immediately before each control section, with control offset, code offset, code length and control length at +0x30. Both regions must fit within that container, and the control/code headers and lengths must agree. Graphics stages come from SPH; headerless compute stages from the control's thread-group dimensions. Each program is paired with the single-stage cover, since its draw-time partner is not recorded by this wrapper. Synthetic fixtures check embedded variants, compute, length mismatches and container boundaries.
- **Found on:** [Persona 5 Royal](../../dm/games/01005CA01580E000/README.md#shaders), with 0x850-byte control sections. The game note records counts and runtime coverage.

### Shaders in a zlib stream in main (YEBIS .tfxo)

- **Symptom:** shaders the game draws are missing from the map (`DM_NVN_SHDUMP`), in no RomFS file and no module, stored or LZ4, and not compiled at run time.
- **Cause:** Silicon Studio's YEBIS post-effect library (bloom, glare, depth of field, vignette; `C:/workspace/YEBIS_draft/src/GPU/…` in `main`) keeps its shaders in a zlib stream in `main`: YEBIS's `.tfxo file`, a text header (the shader count, `hlsl`, `TexUtil_NVN_NX`, the technique list) and the shaders, each a control section (its own length 0x850 on SDK 7) directly followed by its code area.
- **Fix:** `shader_split.py` inflates it and reads the shaders as single-stage programs (`tfxo_streams`, `tfxo_shaders`).
- **Found on:** [Pokémon Sword](../../dm/games/0100ABF008968000/README.md): after the [GFPAK reader](#bnsh-entries-of-gfpak-archives), 38 shaders the opening drew were still missing; none was a patched copy of a split shader (the nearest ones of the same length differ in hundreds of bytes). `main` imports `glslcCompile` from `subsdk1` but never calls it and holds no GLSL source. The only zlib stream in `main` that inflates (`main+0x1c98ed9`, 39.5 MB) holds 3,155 shaders (3,015 distinct, 3,112 pixel and 43 vertex); they covered all 38.
- **Check:** a temporary trace in `nvnProgramSetShaders` (the key's code and control addresses, a backtrace and the heap around them) showed control sections packed back to back with technique names such as `ps_BrightPassT1_THRESHOLD_LUMINANCE_S1` beside them; the names are in `main`, and so is the data.

### Loose control sections and code areas in main (Bink 2)

- **Symptom:** two `[RED] shader … is not in the shader map` lines and `program has no vertex shader` as the opening movie starts; every movie drew white.
- **Cause:** the movie player (Bink 2's Unity plugin) builds its programs from 32 control sections and code areas in `main`'s read-only segment that are in no GLSLC output and no BNSH file. 26 are 0x20-byte records in two tables in `.data` (`{code area pointer, control section pointer, u32 code length, u32 control length}`, both pointers relative relocations); the code takes the other 6 by address (`adrp` + `add` to the control section and, a few instructions away, to the code area it copies with `memcpy`), the code area right after the control section's own length (+0x20, 16-byte aligned) or right before it, and two control sections share one code area. The 8 compute shaders' code areas have no header (SDK 10).
- **Fix:** the reader (`raw_shaders`) takes them. Both neighbours can pass the header and length checks, so it takes only a code area the code builds the address of, the nearest one to the control section's.
- **Found on:** [Advance Wars 1+2: Re-Boot Camp](../../dm/games/0100300012F2A000/README.md) (Unity 2020.2 with Bink 2's Unity plugin). Of 25 `main`s on hand no other has such control sections.

### SNVN program records in Retro Studios' RFRM forms

- **Symptom:** the provisional split finds only `main`'s 294 GLSLC outputs; the 3D scene and the title logo are magenta, with `[RED] shader … is not in the shader map` lines.
- **How it is stored:** Retro's files are RFRM forms (`RFRM`, big-endian u64 data length, u64, then the form type and two u32 versions; chunks inside are a fourcc, u64 length, two u32 versions, then the data). `MaterialArchive.arc` is one `MTRL` form holding an `SDX ` chunk (the same materials' D3D bytecode, 3,468 DXBC programs) and an `SNVN` chunk (the NVN ones). The `.pak` files are `PACK` forms whose `ADIR` chunk lists the resources (u32 count, then 36-byte entries: fourcc type, 16-byte GUID, big-endian u64 offset and length); each `MTRL` resource is an RFRM form whose data is one zlib stream holding another `MTRL` form with an `SNVN` chunk. An SNVN program record is a big-endian u32 code length, u32 control length, a byte, u32 stream length, then a zlib stream that inflates to the program's code areas back to back followed by their control sections in the same order (0x840 bytes each on SDK 4). The chunk's tables differ between files, so the reader finds the records by their own lengths: a stream whose header ends where it says and that inflates to exactly code + control bytes.
- **Startup archive in `main`:** the first programs the game builds (the `CGraphicsUtilCache` quads that draw the title logo) come from a startup material archive compressed into `main` (`NMaterialStartup::get_startup_archive_compressed_buffer`, 5,870 bytes; `NMaterialArchive::ImportStartupArchive` inflates it at boot). Its `CBufferedCompression` header is a type byte (1: LZSS on 8-bit units, `CLZSSDecompressor::DecompressBuffer8`) and three zero bytes; the data inflates to an `MTRL` form like `MaterialArchive.arc`'s. Its programs share code areas with `MaterialArchive.arc`'s but differ in control +0x7d0 (a per-program value), so their keys are their own.
- **Reader:** `rfrm_shaders`, `snvn_records`, `retro_main_forms` and `retro_lzss8` in `shader_split.py`; each record is one program, so its stages pair with each other.
- **Found on:** [Donkey Kong Country: Tropical Freeze](../../dm/games/0100C1F0051B6000/README.md) (SDK 4.4.0): 24,098 shaders (11,644 vertex, 11,734 pixel, 720 geometry, which run as compute kernels): 3,460 first seen in `MaterialArchive.arc` (its 3,468 stages), the rest in the 795 `MTRL` resources of the packs, 8 in the startup archive and 294 GLSLC outputs in `main`; about 8 s.

### MGFX effects in MonoGame XNB files

- **How it is stored:** the 13 XNB files in `Content/Effects` (XNB platform `S`, uncompressed, read by `EffectReader`) each hold one MGFX effect, and `main` holds SpriteBatch's built-in one; no other file and no GLSLC output. A compressed XNB (MonoGame's LZX flag 0x80 or LZ4 flag 0x40, as in Stardew Valley) is decompressed first (`xnb_decompress`).
- **Layout:** MGFX version 8 is MonoGame's own layout: `MGFX`, u8 version, u8 profile, u32 effect key, the constant buffers, then each shader as u8 vertex (1) / pixel (0), u32 length, the bytecode (u32 control length, the control section, u32 code length, the code area: GLSLC's output without its header), the samplers, the constant buffer indices and an attribute list (as MonoGame's OpenGL backend writes it), then the parameters and the techniques with their passes (`mgfx_effect`).
- **Reader:** the reader parses every effect to its end, which checks the layout; in `main`, four bytes `MGFX` count only when version 8 and a control section follow.
- **Pairing:** a pass names its vertex and pixel shader by index, 255 for none, and 10 of the 13 effects have no vertex shader: SpriteBatch binds its own and the effect's pass only replaces the pixel shader. Vertex and pixel shaders therefore combine across effects, and they are paired as single-stage programs.
- **Found on:** [Celeste](../../dm/games/01002B30028F6000/README.md) (MonoGame for the Switch, its C# compiled to C++ by Sickhead's BRUTE): 26 shaders (20 pixel, 6 vertex), all with Mario's binding layout; every binding decodes. [Stardew Valley](../../dm/games/0100E65002BB8000/README.md) (compressed XNB).

### Unity 2020.3 Shader objects in UnityFS bundles

- **How it is stored:** Unity 2020.3 keeps each shader's compiled programs in its `Shader` object (class 48), in UnityFS bundles (`StreamingAssets/aa`) and in the player's own serialized files (`data.unity3d`, `Resources/unity default resources`).
- **Reader:** `dm/gfx/tools/unityfs.py` reads them: UnityFS blocks (LZ4 / LZMA), the serialized file, the object through its type tree, then `compressedBlob` per platform (LZ4 segments; each segment starts with a u32 count and 12-byte `{offset, length, segment}` entries).
- **Type trees:** the player's files have no type trees; they borrow the bundles' `Shader` tree with the same type hash (same Unity build, same layout). Type-tree strings with the high bit set index Unity's common string table, which `unityfs.common_strings` reads from the game's own `main` (`AABB\0AnimationClip\0…`).
- **Program code:** on Switch every subprogram is one whole NVN program. Its code is a 0x60-byte header of four `i32 ×6` arrays indexed by NVN stage (0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess evaluation, 5 compute): an unused / scratch size, the block offset from +0x60 (−1 = no stage), the control size and the code size. Each block is the control section followed by the code area. The vertex and fragment stages of one program form a pair.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md): 209 `Shader` objects in 188 bundles plus the engine files, 9,269 stage shaders (4,634 vertex / pixel pairs and one geometry shader), no compute shaders.

### Unity 2022 Shader objects with parameter blobs

- **How it is stored:** the program code keeps the 0x60-byte stage header ([Unity 2020.3 Shader objects in UnityFS bundles](#unity-20203-shader-objects-in-unityfs-bundles)), but two things changed. From Unity 2022 the blob also holds each program's parameter tables as entries of their own (the parsed form's `m_ParameterBlobIndices`; their bytes start `PGlobals`), and the player's programs are listed in `m_PlayerSubPrograms[tier][].m_BlobIndex` (`m_SubPrograms` is empty).
- **Reader:** `unity_code_blobs` keeps only those player programs. UnityPy's type trees for 2020.3 (Engage) and 2021.3 have neither field, so those read every entry as before. Subprogram version 202012090 (from Unity 2021.2) also has no local keyword list, so `unityfs.subprogram` reads no second keyword array (with one it read the code length as a keyword count).
- **Type trees:** the player's serialized files have no type trees and the Addressables bundles hold no `Shader`, except the `unitybuiltinshaders` bundle, whose tree has the same hash and is borrowed in the second pass.
- **Found on:** [Ace Attorney Investigations Collection](../../dm/games/010005501E68C000/README.md) (Unity 2022.3.17f1): 4,951 stage shaders: 2,475 vertex / pixel pairs (4,294 in the player's files, 517 built-ins, 140 in bundles) and one geometry shader, which Metal cannot express (its build fails).
- **Note:** armyja's issue #17 proposed UnityPy and a reader of its own; neither was needed.

### Unity Shader layouts written out for players without bundles

- **Symptom:** the split stopped with `no bundle has a Shader type tree of the same hash to borrow`.
- **Cause:** a Unity player with no bundle to borrow a tree from: its serialized files have no type trees, and no bundle carries a `Shader` tree of the same hash.
- **Fix:** `unityfs.py` carries the layouts written out (`KNOWN_TREES`; `tree_from_layout` builds the tree a file with type trees would have), and the split's second pass gives one to the stripped files when no bundle lent a tree of that hash. Only there: taken in the first pass, Ace Attorney Investigations Collection's player files came before its `unitybuiltinshaders` bundle and the built-ins' source column and package indices moved, while with the second pass its tables come out byte-identical. The structs both the 2020.3 and 2022.3 layouts share are written once (`_SHADER_COMMON`), and the 2022.3 layout comes out byte-identical.
- **Found on:**
  - [Brain Memory Training Academy](../../dm/games/0100F5D01DA3A000/README.md) (Unity 2022.3.5f1: only `Data/*.assets`, `level*` and the two engine files in `Data/Resources`, none with type trees). The `Shader` type hash there (`ea424a5f…`) is the one in Ace Attorney Investigations Collection's bundles (2022.3.17f1): the same layout, about 30 structs. Checked against Ace Attorney's own tree: the same 2,324 nodes with the same alignment flags, and its 40 bundle `Shader` objects decode identically with either. 496 stage shaders: 439 engine built-ins, 46 of the game's, 11 of the player's in `main`, and the built-in VR blit's geometry shader, which does not build.
  - [Patrick's Parabox](../../dm/games/0100E44016C22000/README.md) (Unity 2020.3.30f1, built-in shaders only, no bundles) stopped there with hash `a30e3dcf…`, which one 18 KB shader bundle of Fire Emblem Engage (2020.3.18f1) carries; its tree is now in the table too (`SHADER_2020_3`: per subprogram global and local keyword indices, every subprogram in `m_SubPrograms`, no parameter blobs, keyword names or stage counts), equal to Engage's node for node (2,179 nodes, the alignment flags included), and each of the game's 48 `Shader` objects reads to its exact end.
  - [Raging Loop](../../dm/games/010020400E1C2000/README.md) (Unity 2018.3.10f1, no bundles: 48 `Shader` objects in `Data/*.assets` and `Data/Resources`) stopped there with hash `8db76007…`, and no game of that Unity version with a bundle was at hand, so `SHADER_2018_3` is written from AssetRipper's TypeTreeDumps (`StructsDump/release/2018.3.10f1.dump`), checked node for node against the dump (1,325 nodes, the alignment flags included). It is a layout of its own, older than the common structs: no conservative raster state, `BufferBinding` without `m_ArraySize`, `ConstantBuffer` without `m_IsPartialCB`, the parameters in each subprogram and no `m_CommonParameters`, one segment per platform (the pre-2019.3 form Cuphead's trees gave `shader_programs`). Each of the 48 objects reads to its exact end: 450 subprograms of version 201802150 (older than the local keyword list, which starts at 201806140).
  - [Donkey Kong Country Returns HD](../../dm/games/01009D901BC56000/README.md) (Unity 6000.0.28f1, no bundles): hash `2ec7223d…` matches BALL x PIT's Unity 6000.0.62f1 bundles. `SHADER_6000_0` derives from the 2022.3 layout by removing each pass's `m_EditorDataHash` and `m_Platforms` arrays and appending the shader's `m_AssetGUID`. The generated tree equals the bundle tree in types, field order and alignment flags; the game note records the object census and build results.
- **Note:** a Unity version whose `Shader` hash is not in the table still stops with the same message; read its tree from any bundle of that Unity version and add it. To take one file out of an NSP without extracting the RomFS, filter `nx_extract`'s `Copy` by path.

### Unity 6 Shader objects

- Read with no change: Hollow Knight: Silksong's 2,068 Addressables bundles carry their type trees (they report the engine version `0.0.0`, Unity's stripped version string), so no layout had to be written out.
- BALL x PIT keeps most of its shaders in the player's loose serialized files (`resources.assets`, `sharedassets1.assets`, the built-in resources), which have no type trees; they borrow the `Shader` tree its 10 shader bundles carry (`shader_split.py` defers a file that raises `NoTree` to a second pass), so it needed no layout either.
- **Found on:** [Hollow Knight: Silksong](../../dm/games/010013C00E930000/README.md) (Unity 6000.0.50f1): 2,450 stage shaders: 1,189 vertex, 1,193 pixel, 67 compute and one geometry shader, which does not build; 11 of them are the player's own in `main`. [BALL x PIT](../../dm/games/010086A022444000/README.md) (6000.0.62f1): 38,099 shaders, most of them in the loose files.

### Unity ComputeShader objects

- **Symptom:** the 3D battle scene was black behind a correct UI, with `[RED] shader 854a8afc9d07f88d is not in the shader map` and `the dispatched program has no compute shader`.
- **Cause:** `DM_NVN_SHDUMP` saved the program; its code bytes were in `Data/sharedassets2.assets`, uncompressed, inside a `ComputeShader` (class 72): the post-processing stack's colour grading LUT baker (`Lut3DBaker#KGenLut3D_AcesTonemap`), so the LUT the scene is graded through stayed black. The split read only `Shader` objects. A `ComputeShader` holds per platform variant a list of kernels, each with its unique variants; a variant's `code` is the program code of a `Shader` subprogram (the 0x60-byte stage header, only the compute block present) and its `threadGroupSize` matches control +0x748.
- **Fix:** `unity_compute` in `shader_split.py` reads them beside the `Shader` objects of every serialized file. The game has no bundle with a class 72 tree, so `unityfs.py` carries Unity 2022.3.54f1's layout (`COMPUTE_2022_3`, written from AssetRipper's TypeTreeDumps for that version; all 12 objects decode, `resourcesResolved` true at each end). 85 compute shaders came in (86 with the player's own).
- **Found on:** [Monster Train 2](../../dm/games/010051701FB46000/README.md) (Unity 2022.3.54f1).
- **Older layouts:** Unity 2019.4 stores each kernel's code directly, without `uniqueVariants` or `variantMap`. `unity_compute` accepts that form too, and `COMPUTE_2019_4` supplies the stripped-file layout. Found on [Pokémon Brilliant Diamond](../../dm/games/0100000011D90000/README.md) (2019.4.27f1); its missing runtime shader is the built-in GPU skinning kernel. The layout matches the release dump's 116 nodes and alignment flags, and both built-in compute objects decode to their exact ends.
- **Note:** a file whose `ComputeShader` has no tree still goes to the second pass; if it has none there either, the split prints a `[RED]` line and leaves its kernels out instead of stopping, so profiles whose compute shaders were never read keep their tables: [Cuphead](../../dm/games/0100A5C00D162000/README.md) (2) and [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md) (4) have `ComputeShader` objects only in files without type trees, of hashes no layout covers. So does [Raging Loop](../../dm/games/010020400E1C2000/README.md) (Unity 2018.3: the two built-ins in `unity default resources`, whose 2018 layout lists kernels with no unique variants), which dispatches no compute.

### GLSLC outputs in a Unity TextAsset

- **Symptom:** black, jagged holes in the ground of a map (whole tiles of the terrain mesh missing while others draw), with about 25 distinct `[RED] shader … is not in the shader map` keys and `program has no vertex shader`, on a Unity game whose `Shader` objects all split cleanly.
- **Cause:** the game draws its maps and effects with programs of its own, not Unity's: `DM_NVN_SHDUMP` saved 24 of them, and none was in any `Shader` subprogram of the 1,606 `Shader` objects in the RomFS (all of them decoded); their code areas were in `Data/resources.assets`, uncompressed, inside the `TextAsset` (class 49) `shader.switch.vfxsb` (11 MB, first word 0x20160817): 1,160 whole GLSLC outputs of a vertex and a pixel stage each. Their control sections carry GLSLC's full header (control +0x20 = 0xa10 and more), unlike the 0x850-byte sections Unity 2018 stores in its subprograms. The split read only `Shader` and `ComputeShader` objects, and the fallback for loose GLSLC outputs skips Unity files.
- **Fix:** `unity_text_glslc` in `shader_split.py` reads the `TextAsset`s of a serialized file that holds the GLSLC magic (`unityfs.text_assets`: name and bytes, the two leading fields every Unity version shares, so no type tree is needed) through `glslc_shaders`, which keeps only outputs that pass its checks. A file without the magic is not decoded, so other games' tables do not change. This one TextAsset added 1,207 distinct shaders (the game has 31,635 in all), with 1,156 vertex / pixel pairs; they covered all 57 keys the runs had reported missing.
- **Found on:** FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition (0100CE4010AAC000, Unity 2018.4.2f1; provisional profile, no note yet): the intro's overworld shot.

### Unreal Engine 5 shader code archives (Oodle)

- **How it is stored:** `ShaderArchive-Global-SF_GLSL_SWITCH_MOBILE-SP_GLSL_SWITCH_MOBILE.ushaderbytecode` (1,219 shaders) and `ShaderArchive-Octopath_Traveler0_Chunk0-…` (9,377), both stored in the pak ([01](../agents/01-extract.md)). Version 2 archives of shader map hashes, shader hashes, shader map entries, shader entries `{u64 offset, u32 size, u32 raw size, u8 frequency}` (17 bytes packed; frequency 0 vertex, 3 pixel, 5 compute), preload entries and shader indices, then the code.
- **Compression:** every shader is Oodle-compressed (Mermaid, block header `8C 0A`). There is no Oodle decoder in this repository, so the game's own `OodleLZ_Decompress` (statically linked into `main`; the host finds it as the one function that takes the address of the library's own name string "OodleLZ_Decompress", its profiler scope: Octopath Traveler 0 `main+0x4f77e10`, Pokemon Legends: Z-A `main+0x1a9c9e0`) decodes them in the Mac host: `DM_OODLE_JOB` (`dm/host/hle_oodle.c`), the NSP script's `decode_ue_shaders` for any game with Unreal paks.
- **Record:** a decoded shader is an `LSLG` record: `LSLG`, a stage tag (`SV`, `SP`, `SC`), a header with the source name (`/Engine/Private/…usf:MainPS`), then `u32` code length, the code area, `u32` control length (0x880), the control section and a 24- or 29-byte tail, all at unaligned offsets (`uepak.lslg`).
- **Check:** the host keys came out exactly: 37 of 37 shaders a first run reported missing were in the two archives.
- **Found on:** [Octopath Traveler 0](../../dm/games/01005270232F2000/README.md): 10,324 unique shaders (1,457 vertex, 6,346 pixel, 2,521 compute); vertex and pixel shaders combine per draw, so every vertex shader is paired with the single-stage cover. Bindings: separate textures and samplers, as Engage's (`0x168` / `0x568`); 61 vertex shaders pick a texture slot at run time (profile key `tex_dyn_slots`, [Texture slots picked at run time](#texture-slots-picked-at-run-time)), 2,084 shaders do atomics on buffer images ([Image atomics](#image-atomics)).
- **Note:** 16 compute shaders do atomics on a 3D `r32ui` image, all Unreal's paged global distance field (`GlobalDistanceFieldCompositeObjects.usf:CompositeObjectsIntoPagesCS`), which the mobile renderer does not dispatch (none in any run). They build since image atomics are native ([Image atomics](#image-atomics)).

### Unreal Engine 4.27 shader code archives (LZ4)

- **How it is stored:** the desktop renderer's `ShaderArchive-Global-SF_GLSL_SWITCH.ushaderbytecode` (1,764 shaders) and `ShaderArchive-DOLL_Chunk0-SF_GLSL_SWITCH.ushaderbytecode` (63,185), stored in the pak: the same version 2 archives and the same `LSLG` records as [Unreal Engine 5's](#unreal-engine-5-shader-code-archives-oodle), but every shader is an LZ4 block (the first bytes are LZ4 tokens followed by `LSLG`; LZ4 is Unreal's default shader compression since 4.26).
- **Reader:** `uepak.py` decodes them itself (`lz4_shaders`): every compressed shader must decode to its raw size as an `LSLG` record, else the archive goes to the Oodle job as before.
- **Found on:** [DRAGON QUEST VII Reimagined](../../dm/games/0100A9D01C446000/README.md) (Unreal Engine 4.27): 62,901 unique shaders (10,114 vertex, 50,756 pixel, 1,968 compute, 63 geometry).
- **Note:** its fragment shaders carry no output types either: one global shader writes integers bit-cast into a float output to an `R8Uint` target, listed in the profile's `shader_facts_manual.tsv` ([Integer fragment outputs declared by hand](#integer-fragment-outputs-declared-by-hand)).

### Unreal Engine 5.6 IoStore shader libraries

- **How it is stored:** the desktop renderer (`SF_GLSL_SWITCH`) with the same `LSLG` records, but no archive in the pak: the two libraries are IoStore shader libraries in `FFRS-Switch.ucas`, whose shader groups are Oodle-compressed as a whole ([01](../agents/01-extract.md)). Each shader entry packs its frequency (bits 0–3), group (4–33) and offset in the group's decoded bytes (from bit 34), and a shader runs to the next one's offset.
- **Reader:** `uepak.py` writes them out as version 2 archives, so the split reads them as any other.
- **Found on:** [FINAL FANTASY RESONANCE](../../dm/games/0100606026F12000/README.md) (Unreal Engine 5.6.1): 73,496 unique shaders (9,253 vertex, 61,305 pixel, 2,934 compute, 4 geometry).

### Unreal Engine 4.18 shaders inline in cooked packages

- **How it is stored:** legacy version 4 paks with LZ5-compressed blocks ([01](../agents/01-extract.md)) and no shader code archive. Each material's shader map is serialized in its `.uexp` (38 to 87 shaders in the ones looked at) and the engine's global shaders in `Engine/GlobalShaderCache-GLSL_SWITCH.bin` (945: 597 pixel, 104 vertex, 241 compute, 3 geometry). Each shader's code is a byte array (`i32` length, then the bytes) holding one zlib stream that inflates to the same `LSLG` record as Unreal Engine 5's.
- **Reader:** `shader_split.py` (`ue_legacy_jobs`, `ue_inline_shaders`) decodes every file of the newest copy of each path with the game's own LZ5 and takes each zlib stream that inflates whole from exactly the length in front of it and starts with `LSLG` (every such stream in the files looked at). Records stored uncompressed are taken too, but none were seen.
- **Found on:** [DRAGON QUEST XI S](../../dm/games/01006C300E9F0000/README.md): 38,892 unique shaders (4,654 vertex, 33,981 pixel, 234 compute, 23 geometry) from 1,740 files; vertex and pixel shaders are paired as single-stage programs, as for Octopath Traveler 0.

### Unreal Engine 4.27 shaders inline in cooked packages (LZ4)

- **Symptom:** `the RomFS has Unreal paks, but none … holds a shader code archive`, then `the shader split found no shaders in main or the RomFS`.
- **How it is stored:** a version 11 pak (Zlib blocks, index not encrypted) with no `.ushaderbytecode`: the project turned `bShareMaterialShaderCode` off, so each material keeps its shaders in its own `.uexp` and the engine's global shaders are in `Engine/GlobalShaderCache-SF_GLSL_SWITCH.bin`, as in [Unreal Engine 4.18](#unreal-engine-418-shaders-inline-in-cooked-packages). Unreal 4.27 serializes each one as an entry of `FShaderMapResourceCode`: the code (`i32` length, then an LZ4 block, the engine's shader compression since 4.26), the raw size (`i32`) and the frequency (`u8`).
- **Reader:** `uepak.py` `inline_shaders` tells such a pak from one with archives by its directory index (cooked packages and no `.ushaderbytecode`; an encrypted index is taken for one with archives), and `shader_split.py` `ue_legacy_jobs` reads it file by file, Zlib and LZ4 blocks decoded in Python (`Pak.read`), so no host runs. `ue_inline_shaders` finds each LZ4 block by its first literals, the record's `LSLG`, with the token and literal length bytes in front of it, and takes it when it decodes to exactly the raw size after the block.
- **Found on:** [OCTOPATH TRAVELER II](../../dm/games/0100A3501946E000/README.md): 13,980 unique shaders (1,910 vertex, 11,346 pixel, 694 compute, 30 geometry) from the 3.3 GB pak in 13 s.

## Control sections and code

### Code area and control section layout

- **Code area:** graphics = 0x30-byte header (magic `0x12345678`) + 0x50 SPH + instructions (at +0x80); compute = 0x30-byte header (magic `0x12345679`) + **zero padding, instructions at +0x100** (all 71 of Mario's).
- **Control section:** magic `0x98761234`; +0x700 = c1 offset in the code area, +0x704 = code size, +0x748 = compute thread-group size (u32 ×3), +0x754 = compute shared memory bytes.
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md).

### Control sections shorter than 0x880 bytes

- **Cause:** the control section stores its own length at +0x20. Mario's, Engage's, Dread's and 13 Sentinels' are all at least 0x880 bytes; 3D World has 2,888 of 0x850 bytes (its ShaderData bfsha and six layouts) and 0x848 in BAHS version 9, each followed directly by other data.
- **Fix:** the key hashes the first `min(0x880, control +0x20)` bytes (`ctl_len` in `shader_split.py`, `keys_hash` in `hle_nvn.c`); the other games' tables came out byte-identical.
- **Found on:** [Super Mario 3D World + Bowser's Fury](../../dm/games/010028600EBDA000/README.md) (SDK 10).

### Program keys computed inside `nvnProgramSetShaders`

- **Symptom:** shaders reported missing at random, and their dumped control sections are garbage (Unity).
- **Cause:** NVN lets the caller free a program's control section once `nvnProgramSetShaders` returns; Unity does, and the host used to hash it at the first draw, so keys computed later hashed freed memory.
- **Fix:** the keys are now computed inside `SetShaders` (`hle_nvn.c` `prog_key`). Mario calls `SetShaders` for some programs with a code address that is not mapped yet, so a stage whose code is not in a registered memory pool at that moment is hashed at its first draw, as before.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md) and Unity games in general (the source names no other); [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (the unmapped code addresses).

### Compute code with and without a header

- **Cause:** compute code areas have a header in Mario (`0x12345679`), not in D2R. 3D World's particle compute shaders (SDK 10) start their instructions at code +0 (like D2R), not after a `0x12345679` header.
- **Fix:** `shader_split.py` accepts both; `dmsh_build.py` strips only a header that is there.
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (header); [Super Mario 3D World + Bowser's Fury](../../dm/games/010028600EBDA000/README.md) (no header, SDK 10).

### Exact program start for the decoder

- **Symptom:** 12 compute shaders (particles, post effects) translated into an endless loop that hung the GPU at the first gameplay scene; SPIRV-Cross failed on 7 shaders instead of 2 ([SPIRV-Cross parse failures](#spirv-cross-parse-failures)).
- **Cause:** Ryujinx's decoder treats every 32-byte boundary *relative to the start* as a scheduling word. Mario's compute code was first fed from +0x30: the groups were misaligned by 0x10, and instructions were dropped or misread.
- **Fix:** the program start must be exact (compute instructions at +0x100). `dmsh_build.py` now refuses compute code whose padding is not zero.
- **Check:** for a new title, the first non-zero word after the header must be a control word, and the trailing `EXIT` (`e30000000007000f`) / `BRA`-to-self (`e2400fffff87000f`) must sit in instruction slots of 32-byte groups counted from your chosen start.
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md).

### GLSLC outputs read through their section headers

- **Symptom:** GLSLC outputs fail the reader's check and are dropped (44 of DARK SOULS: REMASTERED's `main`'s 72 outputs and most of its archives' pixel shaders).
- **Cause:** SDK 4's GLSLC outputs (version 0x20.4): pixel shaders' control sections store at +0x20 a value 12 bytes short of their length, so the code area is not right after it.
- **Fix:** an output's own section headers say where everything is: the output's +0x4c is the section count, section i's header is at +0x90 + 0x90 i (`{size, data offset from the output, type}`, type 0 = GPU code), and a GPU code header adds +0x30 control offset, +0x34 code offset (both from the section's data), +0x38 code length, +0x3c control length. `glslc_section` reads them for outputs the old rule rejects.
- **Found on:** [DARK SOULS: REMASTERED](../../dm/games/01004AB00A260000/README.md) (SDK 4.5.0).
- **Check:** on every output the old rule keeps (1,073 in 13 Sentinels, Three Houses, FINAL FANTASY TACTICS, Engage and Age of Calamity) both give the same key; the new reading also takes 8 outputs of [Fire Emblem: Three Houses](../../dm/games/010055D009F78000/README.md) and 3 of [Hyrule Warriors: Age of Calamity](../../dm/games/01002B00111A2000/README.md) (GLSLC version 0x1d.0, whose control +0x20 holds odd values) that used to be dropped, so those two profiles' tables gained them.

### VertexA and VertexB parts merged into one vertex shader

- **Symptom:** `shader_split.py` finds no stage for vertex shaders and stops at the first captured output (`no GLSLC output`). Translated, every post pass and the window draw one texel's colour (a black screen with sound); every varying at (0, 0, 0, 1).
- **Cause:** shader type 0 in every vertex shader's SPH (word 0 = `0x0061`; every other game's vertex shaders have `0x0461`). Shader type 0 is Maxwell's first vertex slot (VERTEX_CULL_BEFORE_FETCH), and this GLSLC (GPU code version 1.15) writes every vertex shader as two programs in one code area: part A at code +0 (the type-0 SPH) loads the position attribute and writes only the position; part B, at control +0x700 with a 0x30-byte header of zeros and an SPH of type 1, reads the other attributes and writes the other outputs. Control +0x748 lists both as {SPH offset, length from the SPH, ?}. The hardware runs A, then B. Translated alone, A left every varying at (0, 0, 0, 1), so the full-screen passes sampled their sources at (0, 0) and the 2D draws their atlases' first texel.
- **Fix:** Ryujinx's `ShaderHeader` reads a graphics header's type 0 as a vertex shader; `sph_stage` does too. `dmsh_build.py` (`vertex_ab`) merges the two parts as Ryujinx's `TranslatorContext.Translate(VertexA)` does: A's instructions with each unconditional `EXIT` turned into a `BRA` to B's first instruction, then B's, under B's SPH with A's flags and input / output maps ORed in and the larger local memory sizes. c1 is what follows part B. A type-0 shader with no part B listed is translated as it is.
- **Found on:** [Catherine: Full Body](../../dm/games/0100BF00112C0000/README.md): in all 1,244 vertex shaders A writes no generic output, A ends in an `EXIT` predicated on CC FCSM_TR (which Ryujinx never takes; left as it is), an unconditional `EXIT` and a `BRA` to itself, and neither part has an absolute or table jump, so moving B keeps every branch; c1 (what follows part B) is zero padding in all of them. A census of the other games' packages and of every `main` on hand found no other type-0 vertex shader.

## Translation

### SPIRV-Cross parse failures

- **Symptom:** SPIRV-Cross can't parse the SPIR-V (`nullptr` exception).
- **Cause:** 2 agl post-effect compute shaders still produce such SPIR-V. Before the compute start-offset fix ([Exact program start for the decoder](#exact-program-start-for-the-decoder)) it was 7: misdecoded code produced the bad SPIR-V.
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md).

### Pixel shader inputs the vertex shader does not write

- **Symptom:** `Fragment input(s) user(locnN) mismatching vertex shader output type(s) or not written by vertex shader` and the draw is skipped; the host reports `[RED] failed to create render pipeline (vertex … fragment …)`. Offline packing fails with `pso_offline: metal-tt failed` for the same reason.
- **Cause:** every pixel shader input must be a vertex shader output. Maxwell attributes are untyped slots: a pixel shader that reads a slot the vertex shader never wrote gets a default, but Metal refuses to build the pipeline. Translating one shader at a time cannot know what the partner reads. The offline archive exposed a second omission: prebuild reads both `shader_pairs.tsv` and BAHS program variations in `pso_pairs.tsv`, but padding read only the first table.
- **Fix:** `dmsh_build.py` reads both pair tables and gives each vertex shader the outputs its partners read but it doesn't write, as `float4` (0, 0, 0, 1) (`vs_pad`). A profile without `pso_pairs.tsv` takes the split's copy beside the package, as the archive builder does. The union controls pixel-before-vertex build order and shader cache dependencies ([Shader cache key covering the pairs](#shader-cache-key-covering-the-pairs)); the profile input stamp includes its `pso_pairs.tsv`, and the NSP script's shader reuse key includes the generated fallback. Padding precedes the depth, shadow-input, pulling and geometry-capture variants. Rebuild shaders and the host for the new chain. The host caches failed pipelines and reports each once with its vertex / fragment keys; before that, every draw retried the failing compile.
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md): 331 of 9,043 pairs failed; the opening course used none of them, later levels did (about 11,500 failed builds in one 11-minute session). The missing BAHS table was reproduced on [Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md#files-and-shaders) ([#283](https://github.com/seedds/nxnative/issues/283)) and [Super Mario Odyssey](../../dm/games/0100000000010000/README.md#files-and-shaders) ([#276](https://github.com/seedds/nxnative/issues/276)); their notes keep the pair counts and validation.
- **Check:** `dm/gfx/tests/test_dmsh_pairs.py` covers both table locations, union/deduplication, defaults and cache dependencies; Metal compiles the generated sources for Mac and iOS and builds an archive with regular and pulling vertex entries in both depth modes. `test_nsp_mac.py` checks fallback invalidation and progress ordering; `test_nsp_incremental.py` regenerates a missing fallback. Check all pairs offline after a game build (recipe in [05](../agents/05-debugging.md)).

### Single-stage programs paired with a cover

- **Cause:** a game that builds one NVN program per GLSLC output and combines vertex and pixel programs per draw with `BindProgram` stage bits has its real pairs only at draw time, so the offline build cannot know which pixel shaders a vertex shader must feed.
- **Fix:** the composite-program path ([Programs bound per stage](rendering.md#programs-bound-per-stage)) does all the pairing at replay. When every pixel shader input is a plain `float4` at fixed locations, `shader_split.py` pairs every vertex shader with a *cover*: the fewest pixel shaders whose inputs together are every input any pixel shader reads. `vs_pad` then gives each vertex shader all of them, and any vertex / pixel combination links.
- **Found on:** [13 Sentinels: Aegis Rim](../../dm/games/01003FC01670C000/README.md): 838 `ProgramSetShaders` at startup; every pixel shader input is a plain `float4` at locations 1–9 (the SPH input map, words 6–13, matches the translated MSL inputs for all 375 pixel shaders), so the cover is one shader.

### Unwritten components of a written output start as (0, 0, 0, 1)

- **Symptom:** outdoors dim and nothing casts a sun shadow (every shadow-map lookup fails).
- **Cause:** a vertex shader may write only part of an output (Link's Awakening writes a world position's x, y and z) while the pixel shader reads the rest (it divides by w). On the hardware the unwritten components read as (0, 0, 0, 1); Ryujinx gets the same by initializing the components the next stage reads when it links stages. The translator's MSL starts every vertex shader with `main0_out out = {};`, all zeros, so w was 0, and a fragment shader dividing by it got infinite compare values.
- **Fix:** general: `dmsh_build.py` (`vs_init`) sets every `float4` output to (0, 0, 0, 1) right after it, and the translated body overwrites what it writes.
- **Found on:** [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md) (Mac and iPhone): 12,281 of 18,410 vertex outputs never write w. The sun's shadow map (2048×2048, depth 32-bit float) was correct; the surface shaders compute the lookup from a world position the vertex shader passes in `locn24`, divided by its w. Found with `DM_NVN_TEXRAW` on the shadow map, the light matrices from `DM_NVN_CBDUMP`, and a hand-edited copy of one surface shader that wrote the compare value and the stored depth as its colour. Compared with forcing the compare to pass (`compareFunction` always): the same brightness, plus shadows from the fences, the house and the trees.

### Position invariance across shaders

- **Symptom:** the ground kept stale 32×32 blocks where the G-buffer draw failed the depth test: blocks of a stale image where a depth pre-pass and a later pass with other vertex shaders meet (Odyssey: 32×32 blocks on every fourth frame).
- **Cause:** a game may draw the same geometry with two programs and rely on identical depth, for example a depth pre-pass and then a less-or-equal G-buffer pass. Maxwell evaluates identical position math identically; Metal only does for outputs marked `[[invariant]]` (the translation marks `gl_Position`) when compiled with `-fpreserve-invariance`. Without it, Metal compiled two vertex shaders' identical position math to slightly different depths, so fragments of the second pass failed a less-or-equal depth test against the pre-pass and those pixels kept the previous content.
- **Fix:** `dmsh_build.py` (Mac) and `ios_metal.py` (iOS) pass `-fpreserve-invariance` to every compile.
- **Found on:** [Super Mario Odyssey](../../dm/games/0100000000010000/README.md) (Mac): the Cap Kingdom ground. Presents 4k were brighter and blocky, 4k+1 and 4k+2 clean, 4k+3 partly blocked, repeating to the decimal across runs. The blocks were already in the HDR target when the G-buffer pass ended, and only with the ground's material (`b6384a85afd3ef94`) drawn. In some frames the game first draws a depth pre-pass (`alRenderDepthOnly`) for objects its occlusion queries report visible (the queries rotate through three slots, hence the cycle), then draws the G-buffer with other vertex shaders and depth test less-or-equal, without clearing the colour targets.
- **Note:** ruled out on the way: the TBDR optimizations, same-pass feedback, the present path. Evaluating the game's conditional rendering on the CPU from earlier frames' results made it worse (the condition belongs to the same frame on the GPU), so the host still ignores the condition there and draws.

### iOS math mode (relaxed)

- **Cause:** the translated shaders reproduce Maxwell's NaN behaviour with `isnan` selects (8,337 of 12,809 use them), and the default fast mode allows the GPU compiler to assume there is no NaN.
- **Fix:** the iOS libraries are compiled with `-fmetal-math-mode=relaxed` (`dm/ios/tools/ios_metal.py`; the Vulkan and GL libraries in `vk_shaders.py` and `gl_shaders.py` too). Relaxed keeps fast math's reassociation but not that assumption.
- **Found on:** listed among the mechanisms first met in [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md); the source gives the 8,337 of 12,809 count without naming the game.

### Front end at -O1: Apple's control-flow lowering

- **Symptom:** making a pipeline archive prints `Max Unstruct ID 758 in function agc.main in shader 13941 exceeds supported limit`, and one pipeline takes many minutes, in `metal-tt` offline or in the host's prebuild on the device (`Preparing pipelines` holding one count for minutes). The line is a note, not an error: `metal-tt` goes on, exits 0 and writes that pipeline into the archive.
- **Cause:** at the front end's default `-O2`, `CorrelatedValuePropagation` and the `SimplifyCFG` after it fold a translated shader's repeated tests of one value into switches whose cases jump past their own join, so nested `if`s become an unstructured graph. Apple's GPU backend lowers unstructured control flow with per-function IDs; past its budget (255 on `applegpu_g13s`, the check in `libapplegpu-nt.dylib` of AIR-TT 32023.921 picks 255 or 65,535 by a capability not yet characterized), it prints the note and lowers the function another way that is far slower and larger. At `-O1` those two passes do not run; the backend still optimizes the AIR itself, so ordinary shaders come out the same.
- **Fix:** `dmsh_build.py` (Mac) and `ios_metal.py` (iOS) compile with `-O1`.
- **Found on:** [Super Mario Odyssey](../../dm/games/0100000000010000/README.md#files-and-shaders), [issue #276](https://github.com/seedds/nxnative/issues/276): the float body of `alRenderMaterialUber` (fragment `007d7e411ef14ebd`, 16,227 `if`s in one function) took 24.6 minutes and 17.3 MB of `g13s` code at `-O2`, 31 s and 1.3 MB at `-O1`. Its `-O1` graph still goes past the budget (642), at that small cost.
- **Check:** random samples of archive bases from seven games (Odyssey, Tears of the Kingdom, Breath of the Wild, Kirby, Luigi's Mansion 3, Wonder, Pikmin 4; 22 to 400 each) compile at `-O1` to vertex and fragment code within a few percent of `-O2` (Odyssey 400 bases: −0.9% / +0.01%; TotK: +0.1% / −1.0%; Kirby: −3.1% / +0.07%), with no `Max Unstruct ID` note at either level, and the front end is faster. Odyssey's full `g13s` archive of 32,864 bases builds in 15.6 minutes (exit 0); its Cap Kingdom frames 4,200 to 6,600 match `-O2` except for scattered differences of at most 3 of 255 in three of nine frames, and their GPU time per frame (`DM_NVN_GPUPROF=every:150`, three runs each on an M-series `g18g`) has medians of 4.76 to 5.05 ms at `-O1` and 4.75 to 4.83 ms at `-O2`, within run-to-run noise.

### min, max and clamp with a NaN operand

- **Symptom:** the half of Mario facing the camera's left was white (GitHub issue 72, on an iPhone).
- **Cause:** spirv-cross writes `FMin` / `FMax` / `FClamp` as `fast::min` / `max` / `clamp`, which leave a NaN operand's result to the GPU. Maxwell's `FMNMX` returns the other operand and its saturation gives 0, as Metal's `precise::` functions (IEEE `fmin` / `fmax`) do. Plain `max` / `min` / `clamp` compile to the `fast_` intrinsics in every Metal math mode, relaxed included ([iOS math mode (relaxed)](#ios-math-mode-relaxed)). Galaxy's GX lighting normalizes light directions the game leaves at (0, 0, 0) (the key and fill lights of its characters) and takes `max(0, dot(ldir, NaN))` as the spot term: 0 on the M4 Pro with either function, so the attenuation is 1. The fill light sits at the camera's left in view space, so its terminator runs down the middle of a character facing the camera.
- **Fix:** `dmsh_build.py` (`precise`), `vk_shaders.py` and `gl_shaders.py` write the `precise::` functions instead (every game).
- **Found on:** [Super Mario Galaxy 2](../../dm/games/0100FD8022DAA000/README.md) (GitHub issue 72). The NVN chain had about 124,000 `fast::max`, 98,000 `fast::clamp` and 41,000 `fast::min` in [Paper Mario: The Thousand-Year Door](../../dm/games/0100ECD018EBE000/README.md), none guarded by its `isnan` selects.

### Integer fragment outputs declared by hand

- **Symptom:** a pipeline red line for a float output meeting an integer attachment (the red line prints the row to add); a BC encoder's pipeline fails.
- **Cause:** with no PC version the translator has no output types and declares every output float, and Metal refuses a pipeline whose float output meets an integer attachment. A scan for outputs built from integer operations was not trusted: the translated code keeps float bits in integer variables (register reuse), so such a scan cannot tell an integer output from raw float bits stored into a float target.
- **Fix:** the profile's `shader_facts_manual.tsv` declares such outputs `uint`. When another one fails, the pipeline red line prints the row to add. Shaders that write an output from integer bits now also get integer entry points ([Integer entry points for integer colour outputs](#integer-entry-points-for-integer-colour-outputs)).
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md) (agl's BC4 encoder `8e2cc2671cae6020` and its twin for a 2D source `93aa5e22a8202409`, whose failure showed up in an iPhone run); [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) (the BC4 encoder `0544282c162305db` and the Ultrahand mask `e3fb0bb58a691e10`, R8Uint, found by PR #11; the Mac route does not reach it); [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md) (two system pixel shaders write an ID into an `R8_UI` target on the character creation screen); [DRAGON QUEST VII Reimagined](../../dm/games/0100A9D01C446000/README.md) (one global shader writing an `R8Uint` target, [Unreal Engine 4.27 shader code archives](#unreal-engine-427-shader-code-archives-lz4)).
- **Note:** the compressed render target side is in [Compressed textures as render targets through an uncompressed shadow](rendering.md#compressed-textures-as-render-targets-through-an-uncompressed-shadow).

### Integer entry points for integer colour outputs

- **Symptom:** `failed to create render pipeline … output of type float4 is not compatible with a MTLPixelFormatR32Uint color attachment` (or `MTLPixelFormatRGBA16Uint`); every draw into the target skipped.
- **Cause:** with no PC version the translator types every fragment output float, and the shaders write bit-packed integers through `as_type<float>`. A hand-written row per shader (`shader_facts_manual.tsv`, Breath of the Wild's way, [Integer fragment outputs declared by hand](#integer-fragment-outputs-declared-by-hand)) does not scale.
- **Fix:** `dmsh_build.py` `fs_uint` gives a pixel shader a second entry point, `main0_u`, with every colour output the unsigned integer vector of the same bits, when it writes an output from integer bits or nothing but constants (a clear drawn into an R32Uint target); the hardware writes the output's bits to an integer target whatever the shader computed, so the copy is exact. The host takes it for a pipeline whose written colour attachments are all unsigned integer formats (`pso_all_uint`). A shader whose outputs are of both kinds also gets a third entry point, `main0_m`, that returns only the outputs written from integer bits as unsigned integers, and records their colour indices (`uint_mask_m` in its reflection); the host takes it for a pipeline whose integer attachments are exactly those (`pso_mixed_uint`). Otherwise a pipeline with a float target as well keeps `main0` and the red line.
- **Found on:** [Luigi's Mansion 3](../../dm/games/0100DCA0064A6000/README.md): `main0_u`; about 1,400 pixel shaders writing its G-buffer of two integer targets (R32Uint, RGBA8Uint), so every scene draw was skipped. [Pokémon Sword](../../dm/games/0100ABF008968000/README.md): `main0_m`; 10 pipelines as the opening's stadium loads; the scene shaders write colour 0 and 2 as floats and colour 1 from integer bits into an RGBA16Uint target (colour 1 in all 336 of Sword's).

### Integer textures and images through float declarations keep their bits

- Integer textures and images read and written through float declarations (an `RGBA8_UINT` texture read by `texture3d<float>` and bit-cast, `R16_UINT` / `R32_UINT` buffers through `texture_buffer<float>`, integer stores into a float-declared `texture3d` image) were checked on an Apple GPU: all keep the integer bits, so they need no facts.
- **Found on:** [DRAGON QUEST VII Reimagined](../../dm/games/0100A9D01C446000/README.md) (the volumetric lightmap's indirection texture, the light grid buffers).

### Integer-typed vertex inputs fed normalized formats

- **Symptom:** `failed to create render pipeline … Cannot convert attribute …`; draws missing (a pipeline that fails to build).
- **Cause:** the translation declares a vertex input `uint4` when the shader only moves its bits (x bit-cast to an integer, y copied as a float; [Integer vertex inputs](rendering.md#integer-vertex-inputs)), and a game that feeds it a 16-bit normalized format gets a pipeline Metal will not build (no integer input pairs with a normalized format).
- **Fix:** the profile's `shader_facts_manual.tsv` declares such inputs float (the bits Maxwell would see), which changes only that profile's chain version. The guess is not wrong in general, so overrides are added only for shaders seen failing with float formats. The general replay-side answers are the CPU conversion and the shadow vertex inputs ([CPU conversion of vertex attributes the input type cannot take](rendering.md#cpu-conversion-of-vertex-attributes-the-input-type-cannot-take), [Shadow vertex inputs](rendering.md#shadow-vertex-inputs)).
- **Found on:** [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md): missing palm tree and flowers (the pipeline `147c829e79fb322f` + `0ebba5ff6a44aa52`, `shadowmap_pbr.bfsha` variant 641, vertex inputs 1–5): the foliage's shadow-map and depth pre-pass draws; the main pass shades with depth test EQUAL, so without the pre-pass the plants never appeared. Four outdoor shaders with the same loop (`03c43da3e3f46da8`, `079700f3577e311f`, `15c02683a10f8e9d`, `2344874045b1e758`) failed the same way on the iPhone in Mabe Village (Half2, RG8 / RG16 normalized) and are listed too. 1,437 of the game's vertex shaders have integer-typed inputs, many of them bone indices the shader multiplies as integers.

### Depth mode near = 0 needs the per-mode entry points

- **Symptom:** outdoors the whole screen blurred (depth of field out of focus everywhere).
- **Cause:** a game that sets depth mode near = 0 had every vertex shader translated for GL, so all depths sat in 0.5..1.
- **Fix:** the per-mode entry points of [04, section 5](../agents/04-graphics.md#5-depth-convention).
- **Found on:** [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md) (Mac and iPhone): the depth-of-field composite (`52fcbb4ca2a78907`, `postprocess.bfsha`) blends the sharp frame with two blurred levels by a blur amount computed from the half-size depth copy: `pow(clamp((c5[2].y / (depth + c5[2].x) − c5[3].x) · c5[3].y), c5[3].z)` above a screen line, by screen position below it. Found with a save file from the iPhone (Mabe Village outside the house), `DM_NVN_PASSDUMP` / `DM_NVN_TEXDUMP` of that frame, and LASRecomp, which rewrites z only for the GL mode.

### Open: the window Y-direction register `SR_Y_DIRECTION`

- The register is −1 under a lower-left origin. The Ryujinx recompiler answers a constant +1 (a TODO in `InstEmitMove.cs`; LASRecomp, a static recompilation of Link's Awakening, answers −1). Answering it per profile means a translator change, i.e. every game's chain.
- **Found on:** [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md): 2,487 of its pixel shaders read it; in the room shader the value only scales a depth slope under `abs()`; other shaders may use its sign (`dFdy`-based effects).

### Subgroup operations rewritten for a one-lane subgroup

- **Symptom:** `SPIRV-Cross threw an exception: Subgroup builtins are not available in this type of function`.
- **Cause:** Metal has no SIMD-group functions in vertex functions, and SPIRV-Cross's own subgroup emulation covers only `Elect`. RE Engine scalarizes uniform loads: ballot the active lanes, shuffle the first lane's value to all and take a fast path when all agree.
- **Fix:** with a one-lane subgroup that is exact, so `dmsh_build.py` rewrites the vertex shaders' SPIR-V that way before SPIRV-Cross (`spv_single_lane`: the ballot is the lane's own bit, shuffles and broadcasts return its own value, all / any are the predicate, all-equal and elect are true, lane ID 0, size 1). A mask component read through `OpAccessChain` / `OpInBoundsAccessChain` becomes an extraction from the constant mask, preserving the index.
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) (about 4 % of its vertex shaders); [Xenoblade Chronicles 3](../../dm/games/010074F013262000/README.md) (vertex `4b98320a0f512efe`, a prefix ballot that reads `SubgroupLtMask` through a component pointer).
- **Note:** the rewrite covers vertex shaders only: two of Bayonetta 3's domain shaders still fail this way ([Tessellation stages are not drawn](#tessellation-stages-are-not-drawn)).

### Geometry shaders that only pick the layer

- **Symptom:** the tone mapper read an empty LUT and the screen came out a flat blue.
- **Cause:** Unreal Engine 4.18 builds its 32×32×32 colour-grading LUT with `WriteToSliceMainVS` + `FWriteToSliceGS`, one slice per instance. Metal has no geometry stage.
- **Fix:** `dmsh_build.py` recognises such a shader in its SPIR-V (`gs_slice`: triangles in, at most 3 vertices out, positions and other inputs copied, `gl_Layer` taking the bits of one input component). It builds no library for it and records the component in its reflection. Each vertex shader that writes integer bits from the instance index into an output component gets two more entry points (`vs_layer_entry`: `main0_l` / `main0_lz` write that component as `[[render_target_array_index]]`). At bind time `prog_fold_gs` (`nvn_mtl.m`) swaps the vertex shader for its layer variant and drops the geometry stage when the components match. The pass and pipeline side of layer-writing vertex shaders: [Layered passes for vertex shaders that write the layer](rendering.md#layered-passes-for-vertex-shaders-that-write-the-layer), [Layer-writing vertex shaders get the triangle class](#layer-writing-vertex-shaders-get-the-triangle-class).
- **Note:** Unreal binds each stage from a program of its own, so a vertex shader is often paired for a moment with the previous draw's geometry shader. A stub that does not fit stays in the program, and only a draw that uses it prints a red line (`draw_gs_check`). The other 22 geometry shaders in DQ11 (one-pass cube shadows that emit transformed triangles) still do not build ([Geometry shaders with no Metal stage](#geometry-shaders-with-no-metal-stage)).
- **Found on:** [DRAGON QUEST XI S](../../dm/games/01006C300E9F0000/README.md).

### Geometry shaders with no Metal stage

- **Symptom:** a geometry shader fails to compile with `unknown main0_out`: SPIRV-Cross writes `unknown main0_out main0()` for the geometry stage, and the shader does not build.
- **Cause:** Metal has no geometry stage, so a geometry shader that does more than pick the layer has nothing to translate to. A geometry shader that only picks the layer is folded into the vertex shader instead ([Geometry shaders that only pick the layer](#geometry-shaders-that-only-pick-the-layer): `gs_slice`, `prog_fold_gs`; DRAGON QUEST XI S).
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md) (one geometry shader, in `unity_builtin_extra`; nothing in the opening scenes uses it); [Super Mario 3D World + Bowser's Fury](../../dm/games/010028600EBDA000/README.md) (fur fins, `RenderFinFur`, its one failing shader); [Captain Toad: Treasure Tracker](../../dm/games/01009BF0072D4000/README.md) (the fin fur geometry shader, as in Super Mario 3D World, is the one shader of its 7,258 that does not build); [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) (53 geometry shaders); [FINAL FANTASY TACTICS - The Ivalice Chronicles](../../dm/games/010038B015560000/README.md) (one, `bahamuteffect.tec`); [Big Brain Academy: Brain vs. Brain](../../dm/games/0100620012D6E000/README.md) (Bezel's line renderer, below); [Luigi's Mansion 3](../../dm/games/0100DCA0064A6000/README.md) (10 geometry shaders, one expands a box from two corners; no draw used one from the title through the opening cutscene to the lobby). The Unity games' built-in geometry shaders do not build either (the container entries above).
- **Note:** a program whose geometry shader has no library used to draw silently without that stage, with the vertex shader's positions taken as clip coordinates; `draw with geometry shader … which has no library` now names it (Luigi's Mansion 3).
- **Note:** Big Brain Academy's 12 geometry shaders (`_BezelSystemResources/line/shader/line.bfsha`) do not build (`unknown main0_out`) and their draws are skipped (`[RED] primitive type 11 not implemented`). They take a line strip with adjacency (primitive type 11) and emit up to 12 vertices per segment as triangle strips: thick lines with joins and caps (28 emit sites). From the main menu on, every frame draws one such line (width 10 in its uniform block) into the top-left 512x512 of the frame and copies that region into a texture, so the menu's chart loses its outlines. Not done.
- **Note:** the vendored Ryujinx translator has its geometry-to-compute transforms (`VertexToCompute`, `GeometryToCompute`), which the chain does not use yet.

### Geometry reader viewport masks

- **Symptom:** the geometry kernel compiles, but its `_bgv.metal` vertex reader fails with `use of undeclared identifier 'gl_BuiltIn_5253'` and `member reference base type 'int' is not a structure or union`.
- **Cause:** `ResourceReservations` packs `ViewportMask` into one output word; `GenerateVertexPassthroughForCompute` stores that word as a scalar although SPIR-V `IoMap` declares builtin 5253 (`ViewportMaskNV`) as `int[1]`. SPIRV-Cross emits an invalid array-copy expression for it, and Metal has no viewport-mask output. The mask still matters: zero discards a primitive, while bit zero selects viewport zero ([NV_viewport_array2](https://registry.khronos.org/OpenGL/extensions/NV/NV_viewport_array2.txt)).
- **Fix:** `dmsh_geometry.reader` recognizes that generated scalar-array store, retains the mask's integer bits and moves vertices beyond the X clip plane when bit zero is clear. This runs before `vs_depth` makes both depth entries. Using the X plane preserves culling with depth clamp too; moving vertices beyond the far depth plane allowed masked points to render when depth clamp was enabled. Unknown mask expressions fail the build instead of silently losing culling. No pinned translator change is required.
- **Found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): its four geometry shaders' readers.
- **Check:** `dm/gfx/tests/test_dmsh_geometry.py` uses synthetic generated-reader MSL. Both Mac and iOS compiler checks reproduce the original error before the fix. Mac GPU readback covers six masks, points and triangles, depth clip and clamp, and both production depth entries (48 cases); zero-mask points under depth clamp fail with the earlier depth-plane cull. All four original readers also pass both platform syntax checks.
- **Limits:** replay binds viewport zero only. Multi-viewport fanout is not implemented, and per-vertex-disagreeing masks are not validated; the tested primitives use one mask for every vertex. These tests establish compilation and viewport-zero culling, not the appearance of those effects in gameplay.

### Passthrough geometry output bounds follow input topology

- **Symptom:** a passthrough geometry kernel writes beyond its vertex and raw-index scratch regions, and compaction reads the wrong groups; corruption can reach the following primitive-map buffers without a red diagnostic.
- **Cause:** the builder used the SPH's raw maximum output count for every topology. Ryujinx's `ShaderDefinitions` overrides that count for `GpPassthrough` (SPH word 0 bit 24) with the input primitive's vertex count excluding adjacency. A header can declare one vertex while the triangle variant writes three vertices and six raw index words per group. Allocating two index words per group then overlaps later scratch regions.
- **Fix:** `dmsh_geometry.output_layout` mirrors the translator's override for each input variant: points, lines, lines with adjacency, triangles, triangles with adjacency use vertex bounds `1, 2, 2, 3, 3` and index strides `2, 4, 4, 6, 6`. Passthrough also inherits the input primitive: native GLSL omits the SPH's output primitive declaration, so `ShaderDefinitions` and metadata use point, line strip or triangle strip accordingly. `geometry_finish` writes those bounds and topology into each variant's metadata, which replay uses for allocation and compaction. Ordinary geometry shaders retain their declared bound and topology. Rebuild shaders and the host for the new chain.
- **Found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): its passthrough index-building geometry shaders; the triangle kernel's six-word stores disagreed with its two-word metadata while rendering flight. The game note tracks the visual result after correction.
- **Check:** `test_dmsh_geometry.py` covers all five variants, ordinary bounds and malformed headers. With `DM_TEST_GEOMETRY` pointing at `t_geometry`, it passes production-generated layouts to a native emitter with independent topology bounds. GPU canaries protect the nominal vertex and raw-index allocations, and compaction must preserve the emitted count and order. The old rule produced 34 failures; the corrected metadata passes all 170 native checks with Metal validation.

### Passthrough geometry kernels lose raster vertices

- **Symptom:** a geometry kernel compiles and runs its storage-buffer side effects, but contains no `geometry_vb_output` binding and fills its index stream with restart markers; its following raster pass draws no geometry.
- **Cause:** the offline accessor inherited native geometry-passthrough support. Native NV passthrough forwards input vertices without an explicit emit, but Metal's compute stage must emit them. Disabling native support alone still loses generic attributes: independently compiled stages have no linked next-stage usage to populate `PassthroughAttributes`.
- **Fix:** fixed-input compute jobs report no native geometry-passthrough support. `EmitterContext` explicitly emits input positions and every captured generic attribute the GS did not replace, removing adjacency vertices as the input topology requires. This uses the complete Maxwell capture map; ordinary linked translation retains its existing usage mask. Copying all 128 generic components also exposes a Metal backend compiler failure at normal optimization (`Internal compiler error` when creating the pipeline, despite successful offline compilation). `dmsh_geometry.capture_input` keeps capture reads volatile, preserving their values and normal kernel optimization while preventing that failure. No game IDs or shader keys participate.
- **Found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): its passthrough index-building shaders run before depth-writing raster passes during flight. The game note tracks rendering after rebuilding.
- **Check:** `PassthroughCompiler` translates a synthetic EXIT-only Maxwell passthrough shader for all five input topologies, deliberately declaring triangle output in every raw header. Both Mac and iOS compile checks run. With `DM_TEST_GEOMETRY`, production reflection and the argument encoder dispatch the actual kernels; readback checks positions, all 32 generic attributes, adjacency selection, two instances, raw indices, restart padding, primitive compaction and allocation canaries (9,066 native checks). The original pinned translator fails all five variants because raster output is absent. Removing volatile capture reads makes the 128-component synthetic pipeline fail to compile on the M4 Pro; reducing to 96 components or compiling at `-O0` independently removes that failure.

### Geometry capture waits for every indexed draw

- **Symptom:** geometry-heavy frames have a large submission wall/CPU gap despite short GPU execution; stack samples spend most of the main thread in `geo_draw` → `mdi_wait` → shared-event wait, with index sorting another substantial cost.
- **Cause:** every indexed geometry draw unconditionally committed and drained the GPU before assembling its primitives on the CPU. It also waited for vertex streams consumed only by native GPU capture. The capture list sorted every index reference to remove duplicates, even though the scratch bound already limits the vertex span.
- **Fix:** `nvn_geometry.inc` checks each actual CPU-read range against pending copies/updates/fills, current shader writes and previous live submission writes. Unknown or overflowing write sets retain the wait. Indirect records are checked before decoding; their selected index ranges are checked afterwards. CPU-converted vertex bindings retain the same hazard checks, while native capture follows GPU encoder ordering. A successful drain covers the remaining reads of that draw. Capture uniqueness uses a bounded vertex-span bitset and ascending set-bit enumeration; allocation failure or a wider span retains an in-place sort fallback. Non-indexed captures need no unique index list. The primitive map, restart handling, base vertex and instance semantics are unchanged.
- **Found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): its flight geometry path. The game note records matched frame-time and visual validation.
- **Check:** `t_geometry` first reproduced five unnecessary drains for clean static and indirect inputs. Its 226 native checks cover clean versus pending/current/prior GPU-written ranges, unknown write sets, GPU-only and converted streams, indirect arguments selecting a different dirty index range, 8/16/32-bit indices, restart, base vertex and instances. Actual copy/compute writes and raster readback validate synchronization; converted data must see the new GPU-written value. Sparse duplicates, ascending output, short destinations, fallback and allocation canaries cover the capture list. These checks establish behavior; throughput is measured separately in gameplay.

### Geometry compaction serial prefix scan

- **Symptom:** geometry-heavy frames remain GPU-bound after CPU readback waits are removed; encoder timestamps attribute most compaction time to the offset scan rather than the geometry shader or index writes.
- **Cause:** count and write dispatches ran in parallel, but one GPU thread walked all group counts to compute ordered offsets. Geometry replay also created a separate compute encoder for each of the geometry, count, scan and write dispatches, with full queue barriers at every boundary.
- **Fix:** `geo_pack_scan` divides the groups into contiguous chunks across at most 256 threads. Each thread sums its chunk; a threadgroup prefix scan gives its starting offset; a second chunk pass writes the ordered offsets. The group count does not need to fit in the threadgroup. Totals use 64 bits; unrepresentable 32-bit offsets saturate, output writes use subtraction-based capacity checks, and indirect counts retain complete-primitive truncation. `geo_pack_encode` keeps the geometry and three compaction dispatches in one encoder, with explicit dispatch-to-dispatch device-memory barriers. Full barriers remain at its boundaries. The primitive order and large-draw algorithm are preserved; no shader-library chain change is required.
- **Found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): its flight geometry passes. The game note records the baseline encoder timings and matched result.
- **Check:** `t_geometry` passes 1,066 checks with Metal validation. A GPU producer and the production compaction stages share one encoder; readback compares every offset and ordered index with an independent CPU result, including 129,032 groups. Cases cover points, lines and triangle strips, empty groups, zero/odd/thread-boundary group counts, 1/32/192/256-thread scans, short capacities, integer overflow and allocation canaries. The actual translated passthrough integration also passes all five input topologies through the same encoder path. Performance is checked separately with matched gameplay.

### Geometry draws above the capture scratch bound

- **Symptom:** `[RED] geometry draw needs 261 MiB of scratch, above the 256 MiB bound` (or `exceeds the capture or invocation bound`); the draw is skipped.
- **Cause:** the geometry path captures the vertex shader's outputs for every vertex between the draw's lowest and highest index, for every instance, and the geometry kernel's outputs take room per primitive (`nvn_geometry.inc` `geo_draw`). Unreal's one-pass point light shadow draws a whole level mesh through its geometry shader.
- **Fix:** such a draw is drawn in runs in the hardware's order (`geo_split`): one instance at a time, then runs of at most 4,096 primitives of a list topology whose index range fits 64 MB of capture. The index list is read once and the runs skip the GPU wait the outer draw took (a first version halved the draw recursively, each part reading the indices again and waiting: that frame took 805 ms). The index compaction after the geometry kernel was one thread walking every group; it is a count, an offset scan and a write dispatch now (`nvn_geometry.h`, `t_geometry`). Scratch arenas above 32 MB are kept while such draws go on and released after 600 idle submissions (`nvn_mtl.m` `arena_begin`), and `arena_new` takes the smallest free arena that fits.
- **Found on:** [OCTOPATH TRAVELER II](../../dm/games/0100A3501946E000/README.md): 129,032 triangles of one mesh (vertex `9214254d203a4e45`, geometry `cb17a9453d4789d0`, both from the default material) at the start of the first battle and in the scene after it; with the one-thread compaction each such frame took about 300 ms, now only the first is long (395 ms on the Mac, with two new pipelines and the scratch allocation).
- **Note:** strips, fans and primitive restart are not split. The primitive numbers restart at 0 in each run; this geometry shader reads only its index map by them, but one that read its primitive ID would see other numbers than on the Switch.

### Tessellation stages are not drawn

- **Symptom:** `primitive type 14 not implemented`; patch draws skipped.
- **Cause:** SPIRV-Cross already writes the hull shaders as kernels and the domain shaders as post-tessellation vertex functions, but Metal 4's render encoder and pipelines have no tessellation (no `drawPatches`, no factor buffer). The hull kernels also write quad factors (`MTLQuadTessellationFactorsHalf`) for triangle patches.
- **Fix:** not done. The draws would need a tessellator in a compute kernel and domain shaders rewritten as plain vertex functions.
- **Found on:** [Luigi's Mansion 3](../../dm/games/0100DCA0064A6000/README.md): 15 hull and 18 domain shaders. Two tessellated programs (vertex `28a39709d0adfcfa` / hull `51d2d156bb12c260` / domain `0b95742e863bec50`, and `d4d89931b5a9a514` / `8aa6feeb7d33e9f6` / `f254b4dce2d6c012`) draw twice a frame for about 30 frames, in the title scene behind the file select as it slides out before a new game. [Bayonetta 3](../../dm/games/01004A4010FEA000/README.md): two domain shaders do not build (`DS_Vanish05_*.dso`: `SPIRV-Cross threw an exception: Subgroup builtins are not available in this type of function`); the one-lane rewrite (`spv_single_lane`, [Subgroup operations rewritten for a one-lane subgroup](#subgroup-operations-rewritten-for-a-one-lane-subgroup)) covers vertex shaders only, and tessellated draws are skipped anyway.
- **Note:** the vertex shaders that feed a hull or geometry shader can be found from the shader headers (SPH): the generic attribute map at +0x18 of the consumer's header (inputs) equals the one at +0x36 of the vertex shader's (outputs). In Luigi's Mansion 3, 40 vertex shaders match a hull shader and 49 a geometry shader.

## Bindings and handles

### Driver-segment reads outside fragment shaders

- **Symptom:** a vertex or compute shader reads zeros from NVN's driver segment `c0`.
- **Cause:** `dmsh_build.py`'s `drv_reads` scan matches only fragment-shader names (`fp_c0` / `fp_c2`), so vertex and compute shaders that read `c0` were never marked, and the host passed them an all-zero `c0`; these reads are not listed in the JSON.
- **Fix:** the host treats a declared driver-segment binding as read (`nvn_refl.m`). The words the host fills are in the entries below.
- **Found on:** [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md), [Super Mario Odyssey](../../dm/games/0100000000010000/README.md), [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md).

### Compute `c0`: dispatch size and uniform buffer table

- **Symptom:** flickering hard-edged white tiles (tiled lighting with every tile row getting row 0's or other rows' lights).
- **Cause:** a compute shader that computes its tile index as workgroup y × `c0[0x200]` + workgroup x reads the dispatch's workgroup count from the driver segment, which the host left zero.
- **Fix:** for compute, `c0` carries the dispatch size at 0x200 (x, y, z) and the uniform buffer table at 0x210 + 16 r (address, size) in front of the storage buffer table at 0x310 (`encode_args_`).
- **Found on:** [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md) (Mac and iPhone): the step of Marin and Tarin's doorway. The room shaders use tiled forward lighting: the fragment position picks a 64×64-pixel tile (`trunc(y / 64) × ceil(width / 64) + trunc(x / 64)`), whose light count and light indices come from two storage buffers that a compute shader (`light_culling_compute.bfsha`, one thread per workgroup, one workgroup per tile) fills every frame. Every row's workgroups wrote row 0's entries, the last writer won (a different one each frame), and the bottom tile row, where the step is, got the lights of other rows; a dump of the tile lists showed only the first of 17 rows filled. After the fix every tile row has its lights and the doorway shows the game's soft light from outside.
- **Check:** Breath of the Wild (101 compute and 4 vertex shaders read `c0`) and Super Mario 3D World (11 and 1) render the same as before on the Mac.

### Geometry-stage storage buffer table

- **Symptom:** `[RED] shader ... reflection conversion failed: B binding gp_s0 has no resolved slot number`; a geometry shader reading driver segment `c0` reports that its storage-buffer table base was never measured.
- **Cause:** geometry shaders access their storage buffers through `c0 + 0x410 + 0x10 r`, but the builder's `SSBO` map and replay's `c0_ssbo_base` covered only vertex, pixel and compute. Running the lowered geometry shader as a Metal kernel does not change its logical NVN stage or driver table.
- **Fix:** geometry uses base `0x410` in both reflection and driver-buffer population. `encode_args` keeps NVN stage 2 for these kernels, so storage slots and driver-table rows come from the geometry bindings. Hull and domain bases remain unmeasured and unresolved.
- **Found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): the original shader's constant-buffer operands and the pinned translator's metadata agree on addresses in rows 0 and 2.
- **Check:** `dm/gfx/tests/test_dmsh_reflect.py` resolves synthetic geometry descriptors to slots 0 and 2 and rejects unknown or unaligned table addresses. `t_geometry` passes synthetic metadata through native reflection and the production argument encoder; GPU readback checks both rows' guest addresses and sizes with different compute-stage bindings, plus a write through the geometry storage pointer.

### Storage-buffer write flag in translation metadata

- **Symptom:** a storage buffer the shader stores to or uses atomically has `rw: false` in its generated reflection; replay does not record its GPU writes for dependent buffer reads.
- **Cause:** Ryujinx's `BufferUsageFlags.Write` is bit 0 (`1`), and `Program.cs` serializes that enum unchanged. The builder tested bit 1 (`2`), incorrectly marking every such descriptor read-only. Native reflection honors the explicit `rw` field, so its older fallback to the MSL pointer qualifier could not correct it.
- **Fix:** `reflect` tests bit 0 for both driver-table storage buffers and addresses carried in guest uniform buffers. Replay then uses its existing write-access and pending-write tracking paths. Rebuild shaders to replace the old metadata.
- **Found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): its geometry counter atomic and output stream both emitted translator flag `1` but reached reflection as read-only.
- **Check:** `test_dmsh_reflect.py` exercises writable and read-only translator descriptors, including an address in a guest uniform. `t_geometry` verifies that native reflection preserves explicit true and false flags and that the writable geometry output enters the GPU-write tracker before its dispatch.

### Graphics `c0 + 0x610`: the largest point size

- **Symptom:** point sprites of size 0 (Odyssey's Cap Kingdom fog never parted around Mario).
- **Cause:** a vertex shader that writes `gl_PointSize = min(size, c0[0x610])` read 0, because the host had nothing at that offset of the driver segment.
- **Fix:** `c0 + 0x610` holds the largest point size, and the host writes 2047 there for the graphics stages. Writing the `SetPointSize` value instead (always 1 in Odyssey) left one-pixel splats whose disc weight came out 0 (the fields held `-0.0`).
- **Found on:** [Super Mario Odyssey](../../dm/games/0100000000010000/README.md) (reported on an iPhone; fix checked on the Mac): on the Switch the ground fog opens around Mario as he walks through it. The fog's shading reads velocity fields that a small fluid simulation updates every frame: two 256×256 and two 128×128 RG16F targets, with a two-pass blur and a blended composite. Its only input is a point-sprite splat per mover, drawn with `alPointSpriteShader`: one vertex per mover, its size in the vertex data (Mario's main splat asks for about 18 px, others 3–5 px) and a soft disc in the fragment shader (`clamp(1 - 4r², 0, 1)` × velocity). The simulation fields stayed exactly zero (raw dumps with `DM_NVN_TEXDUMP` + `DM_NVN_SNAPRAW`; the 8-bit dump shows velocities as black either way). After the fix the fields hold velocities up to ±44, and the fog clears around Mario.

### Graphics `c0 + 0x644`: the draw's base instance

- **Symptom:** occlusion queries that always return 0 samples (Z-A: characters' legs folded back from the hips once they walked); every instanced draw's transform taken for instance 0.
- **Cause:** a shader that picks its transform with (instance index − base instance) + `c0[0x644]`: Maxwell's instance ID leaves the base instance out, and the NVN driver keeps it in its constant segment, where GLSLC reads it for `gl_InstanceID + gl_BaseInstance`. The host left `0x644` at 0.
- **Fix:** `encode_args` writes the draw's base instance there for graphics stages and does not reuse a stage's argument block across draws with different base instances. The word before it, `0x640`, is added to the vertex ID by 58 of Z-A's shaders and stays 0 until what the driver puts there is known.
- **Found on:** [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md): from the first step on, the player's legs stuck straight out behind her at hip height, and the rival's forward in a later cutscene; standing still beforehand looked right. The bone palette the game computes on the CPU was already bent. The cause was six occlusion queries a frame, one 36-vertex box each (`a793f436dcabd350`, base instance 61, 62, …), drawn against mip level 2 of the depth pyramid: every box was box 0, every query came back with 0 samples, and the game took its characters for hidden and posed their legs that way. Forcing every result to visible fixed the legs, and depth testing with ALWAYS did not. 793 of the game's vertex shaders read the word.
- **Note:** how it was found: the raycasts were logged with scratch observation gates (all correct), then `DM_HLECOUNT` showed nothing walking-specific, then the counter reports.

### Texture and sampler pools with overlapping IDs

- **Symptom:** a menu's labels drawn as plain bars, other menus empty except for the cursor (one pool's textures overwritten by another's); a black screen when IDs past a fixed table read as empty slots.
- **Cause:** a game that binds two texture pools and two sampler pools whose IDs overlap; the replay had one ID table.
- **Fix:** each pool has a base in the table, in creation order; `RegisterTexture` / `RegisterSampler` add it, and binds recorded on a command buffer add the base of the pool that command buffer last set (`bind_handles` in `hle_nvn.c`, the base kept in `cmd_t`, [next entry](#bases-of-bound-handles-kept-in-the-command-buffer); log line `pool #2 (…): numbered from N`). Handles read from uniform buffers take the base of the pool bound at replay (DARK SOULS: REMASTERED, `ubo_tex_id` / `ubo_samp_id`, [Handles from uniform buffers take the bound pools' bases](#handles-from-uniform-buffers-take-the-bound-pools-bases)). Deferred binds are not remapped (red line; [Deferred texture binds read at execution](#deferred-texture-binds-read-at-execution)). The table's capacity is the sum of the pools.
- **Found on:** [Fire Emblem: Three Houses](../../dm/games/010055D009F78000/README.md): the scene binds a 12,544-entry texture pool and a 3,856-entry sampler pool, the 2D UI's command buffer binds 4,096-entry ones, and both fill IDs from 256. The scene's default 8×8 white texture overwrote the UI's textures (the title menu's labels drew as plain bars). The scene registers each draw's textures into fresh IDs of a ring (8,729 and up a few seconds in), which used to read as empty slots and left the screen black.

### Bases of bound handles kept in the command buffer

- **Symptom:** red lines from the list of command buffers that bound a non-first pool (6,734 in one DARK SOULS: REMASTERED run).
- **Cause:** that list had 64 entries and was never cleared, and the game binds a non-first pool in more than 64.
- **Fix:** the bases of bound handles live in the command buffer (`cmd_t` `tex_base` / `samp_base`).
- **Found on:** [DARK SOULS: REMASTERED](../../dm/games/01004AB00A260000/README.md).

### Handles from uniform buffers take the bound pools' bases

- **Symptom:** distant walls and floors smeared sideways (DARK SOULS: REMASTERED): tiling textures took a clamp-to-edge sampler and repeated their edge column.
- **Cause:** the map materials read their combined texture / sampler handles from a uniform buffer (`fp_t_cb13_*`, resolved at replay by `ubo_handle`), and the game keeps several sampler pools, each with its own samplers under the same IDs, one per command buffer that records the scene. The replay resolved uniform-buffer handles in the first pool.
- **Fix:** the replay keeps the texture and sampler pools bound (`OP_SET_TEX_POOL` / `OP_SET_SAMP_POOL`, which it used to skip; a command buffer that binds none keeps the queue's, as on the GPU) and adds their bases to handles read from uniform buffers (`ubo_tex_id` / `ubo_samp_id` in `nvn_mtl.m`).
- **Found on:** [DARK SOULS: REMASTERED](../../dm/games/01004AB00A260000/README.md): the map materials `FRPG_Phn_*Lit*`; 12 sampler pools of 2,304 entries (ID 261 is clamp-to-edge with LOD 0..0 in one pool, repeat with mips in others); the walls' tiling base textures (u ≈ −3.45).
- **Check:** found by highlighting the six most common fragment shaders of the scene pass (`DM_NVN_HILITE`), then the next six, and reading the wall draw's binding lines (`DM_NVN_PASSDUMP_ALL`).

### Deferred texture binds read at execution

- **Symptom:** the 3D scene black, only particles and the HUD drawn; pass dump `B` lines all `tid=0 dummy`.
- **Cause:** `nvnCommandBufferBindTexturesDeferred` passes a pointer to the handle, and "deferred" means the GPU reads the handle from that address *when the command executes*. A game that records its command blocks once, submits them every frame and writes the handles afterwards got the handles the host copied at recording, mostly zeros, so draws read the dummy texture 0.
- **Fix:** the bind record keeps the source address (`p_bind_handles.defer / src`) and every replay-time reader refreshes the handles first (`bh_fresh` in `nvn_mtl.m`).
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md): the engine binds almost every texture this way (`main+0xec2e64`, one handle, the pointer being the handle field inside the texture-sampler object); the deferred-lighting composite (one full-screen triangle per material class, blending into the HDR target) read texture 0 for all of its inputs.
- **Note:** how it was found: a frame data-flow table built from `DM_NVN_PASSDUMP_ALL` (per pass: attachments, shader keys, sampled serials) showed the half-resolution lighting outputs never being read, and the composite pass's `B` lines were all `tid=0 dummy`. Without `_ALL` the dump lists only the first draw of each pass, which hid the composite. Deferred binds are not remapped by pool base ([Texture and sampler pools with overlapping IDs](#texture-and-sampler-pools-with-overlapping-ids)).

### Handles in uniform buffers written earlier in the same submission

- **Symptom:** bindless handles read from a uniform buffer name the wrong texture when the game writes them earlier in the same submission.
- **Cause:** the replay reads handles on the CPU, and a write queued earlier in the submission has not landed yet.
- **Fix:** inline uniform updates keep where their data sits, so the handle is read from the newest one covering it (`pend_read`). The old limit of 256 writes per submission, after which every address counted as written, is gone: pending writes are now indexed by 4 KB page (8,192 per submission).
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) (it queues more than 256 writes per submission).

### Storage buffer addresses in the game's own uniform buffer

- **Symptom:** a storage buffer reported with `cb_slot` 3, which had no NVN slot.
- **Cause:** the shader reads a storage buffer's address from the game's own uniform buffer (c3 + 0x38), which Ryujinx reports with `cb_slot` 3.
- **Fix:** the build marks it `sbuf_ubo` (uniform slot `cb_slot - 3`, byte offset), and the replay reads the 64-bit address there at bind time, as it reads bindless handles.
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) (about 30 compute shaders).

### Bindless handles through a loop phi

- **Symptom:** `no resolved slot number`, the shader does not load; the translation has a 32-entry texture-pool array (`fp_t2d_cb0_40000000`, handle type Direct).
- **Cause:** the handle is `c2[0x1A0] | 0x10500000` (separate texture slot 7, constant sampler 0x105), kept in a loop variable. Ryujinx's bindless elimination compared only the phi's direct inputs, gave up, and turned the access into an index into a 32-entry texture-pool array, which the replay cannot bind.
- **Fix:** the vendored translator now follows the phis that feed a phi, and treats two `constant buffer | constant` handles as the same value ([third_party/ryujinx/README.md](../../dm/third_party/ryujinx/README.md)). Those accesses become ordinary type-3 handles.
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) (218 pixel shaders).

### Bindless reads whose handle phi also carries other values

- **Symptom:** none in the log: texture reads come back 0. Particle meshes collapse to a point (their scale reads 0), so effects are missing or show as dark patches. The translator's own message (`Failed to find handle source for bindless access of type "textureBuffer"`) used to be dropped, because `dmsh_xlate`'s accessor had no `Log`; its messages now go into the shader's notes, marked `translator: `, and the build summary counts them apart from unanswered queries.
- **Cause:** the handle reaches the read through a phi. Some inputs are c2 handle words, the others are not handles: undefined, or a value the compiler kept in the same register on a path where the predicated read does not run (float arithmetic, a texture result, the absolute value of a uniform word). Some phis merge two or more different c2 handles too (a float and a half-precision buffer). `TryConvertBindless` cannot trace one handle, and `GenerateBindlessAccess` refuses a phi with such an input, so bindless elimination deleted the read and wrote 0 to its results.
- **Fix:** only for a phi `GenerateBindlessAccess` refuses (`TryTableHandle` in the vendored `BindlessElimination.cs`, marked `nxnative:`), the read takes the c2 handles among the phi's inputs (a word, or a texture word ORed with a sampler word or a constant sampler). With one handle, it takes the phi's place. With several, one read is made per handle and the result is the read whose handle equals the phi's value at run time: the host fills c2 with the bound handles, or with slot indices for `tex_dyn_slots`. A candidate that other reads of the shader use as a texture of another shape is left out, so those reads are converted in a second sweep once every other read has registered its texture (`ResourceManager.GetTextureTypes`). An image store or atomic with several candidates is left alone, since one per candidate would write each of them. A phi made only of handle words keeps `GenerateBindlessAccess` (the dynamic arrays of [Texture slots picked at run time](#texture-slots-picked-at-run-time)), so no other shader changes: in FINAL FANTASY RESONANCE, the SPIR-V of the 69,449 other shaders is byte for byte what it was. A chain change.
- **Found on:** [FINAL FANTASY RESONANCE](../../dm/games/0100606026F12000/README.md): 58,122 reads removed (57,861 of them texture-buffer reads, mostly in Unreal's Niagara particle vertex factories), of which 58,096 in 3,197 shaders are now resolved: 21,380 with one handle, the rest choosing among 2 to 5 at run time. 26 reads in 14 shaders, whose handle comes from no c2 word, are still removed. Reported in GitHub issue #168.

### Texture slots picked at run time

- **Symptom:** the shaders' bindless arrays have no slot and used not to load; the translator falls back to a 32-entry array such as `array<texturecube<float>, 32> fp_tcube_cb0_40000000`.
- **Cause:** the shader reads one of several c2 texture-table words, chosen by branches. With separate textures (Octopath Traveler 0) it ORs the word with a sampler-table word; with combined ones (Paper Mario) the handle is read from the combined texture table (c2 `0x20 + 8 × slot`, constant offsets only). The translator cannot trace that to one slot and falls back to Ryujinx's bindless arrays, `array<texture_buffer<float>, 32> vp_tb_cb0_40000000` and `array<sampler, 32> vp_s_cb0_40000000`, indexed by the handle's texture ID (low 20 bits) and sampler ID (high 12), capped at 31; the build leaves them without a slot.
- **Fix:** with the profile key `tex_dyn_slots` the reflection reads them as `NVN_RB_TEXD` / `NVN_RB_SAMPD` arrays (`nvn_refl.m`), the replay writes slot indices into that shader's c2 tables (texture slot r → r, sampler slot r → r << 20; in the combined table `r | r << 20` in slot r) and binds slot k at element k, so the shader's own arithmetic lands on the binding: the handle names element r, which holds the texture bound in slot r. The array's other elements hold whatever the other slots do, mostly 2D textures where the array is of cubes; an array element bound to a 2D texture gets the zero cube without a type mismatch report (`tex_fit_elem`), since the shader does not pick it there. The host logs each such shader's first encode with the slots that held textures. Host only: the chain version does not change.
- **Check:** check first that the handles come only from c2's tables: in Octopath Traveler 0 every integer read of c2 in those shaders is a table slot (texture slots 0-10, samplers 0-4).
- **Found on:** [Octopath Traveler 0](../../dm/games/01005270232F2000/README.md): 61 Unreal vertex shaders, particles reading their data from buffer textures (separate textures and samplers). [Paper Mario: The Thousand-Year Door](../../dm/games/0100ECD018EBE000/README.md): 2,386 pixel shaders (5,476 cube samples, 487 2D), slots 0, 7, 8, 13 and 28 (combined table).

### A texture chosen at run time between two uniform-buffer handles

- **Symptom:** Ryujinx's fallback arrays (`fp_t2d_cb0_40000000`) in pixel shaders whose handle comes from the game's own uniform buffers; the shaders do not load and their draws are skipped.
- **Cause:** the handle is one of two words of the game's own uniform buffers, chosen by a flag, packed as texture ID | sampler ID << 20. `tex_dyn_slots` covers only handles from the driver's c2 tables ([Texture slots picked at run time](#texture-slots-picked-at-run-time)).
- **Fix:** not done. Resolving them needs the translator to report such a choice of two uniform-buffer handles, a chain change for every profile.
- **Found on:** [Big Brain Academy: Brain vs. Brain](../../dm/games/0100620012D6E000/README.md): ten pixel shaders (six in `03GamePerception.bnbshpk`, two PBR, two in `main`); in `e10f24835bbaa5f6` the handle is c3 +0x130 or c12 +0x1d0, chosen by a flag in c12. None of the activities checked uses them; a long run of activities met one once.

### An array of handles in a uniform buffer

- **Symptom:** the host stops at the shader's first load: `unexpected argument buffer layout: encodedLength=640 maxAid=48`.
- **Cause:** a loop reads one handle per light from the game's uniform buffer at a computed offset; Ryujinx's texture array from a buffer indexes it by the handle's byte offset ÷ 8 (an NVN handle is 64-bit, the shader computes `(word >> 2) << 1 | (word & 3) >> 1`). The build reads the name as one bindless texture (`tex_ubo`, offset 0), so the argument buffer was 32 entries longer than the reflection said.
- **Fix:** the host's reflection reader takes the array length from the MSL type, and the replay fills element k from the handle at byte 8k of that uniform slot (`NVN_RB_TEXU` / `SAMPU` with `n`); elements whose bytes are not a registered texture or sampler ID read the dummy, silently. No chain change: the build already wrote the array type.
- **Found on:** [Paper Mario: The Thousand-Year Door](../../dm/games/0100ECD018EBE000/README.md): the reflection shaders (`reflection.bfsha`: `array<texture2d<float>, 32> fp_t2d_cb14_0` and its samplers), c14 (uniform slot 11). The first run showed elements 0–3, 6 and 8–12 holding textures, among them the shader's fixed handles at +8, +16 and +24 and the lights' from +64.

### An array of handles from the driver table c2

- **Symptom:** `shaders with a binding whose slot was not decoded 588`; shadow cascades read through `array<texture2d_array<float>, 32> fp_t2das_cb2_0`.
- **Cause:** Ryujinx's texture array from a buffer, as Paper Mario's uniform-buffer arrays ([An array of handles in a uniform buffer](#an-array-of-handles-in-a-uniform-buffer)), but over the driver table c2, indexed by (0x80 + a cascade offset) ÷ 8.
- **Fix:** `dmsh_build.py` `c2_array` names it `tex_c2a` / `samp_c2a` with the c2 byte offset of element 0; the replay fills element k from the slot whose c2 word is at that offset + 8k (combined, else separate), and the dummy elsewhere (`NVN_RB_TEXA` / `NVN_RB_SAMPA`, `c2_handle`).
- **Found on:** [Luigi's Mansion 3](../../dm/games/0100DCA0064A6000/README.md) (586 pixel shaders, the shadow cascades).

### A texture slot that shares its c2 word with an image slot

- **Symptom:** characters and props glowing white, about 10⁴ too bright, in DRAGON QUEST VII Reimagined's opening cutscene only, while walls and gameplay look right.
- **Cause:** a size query (`GetDimensions` on a `RWTexture3D`) is a TXQ on the image's handle, c2 `0x120` (image slot 0): the build numbers it combined texture slot 32, the slot that shares that word, and with no type in the facts table the translator read it as 2D. The host bound the empty texture slot 32, and the 1×1 dummy's size passed only thread (0, 0, 0).
- **Fix:** two general fixes: a texture binding of slot 32–39 reads the image bound in slot 0–7 when there is one, as c2 holds it (`nvn_mtl.m` `tex_slot_h`; Hades II's texture slot 34 without an image still reads the texture), and a query on an image handle the facts do not type is translated again with the type of that image from the first translation (`dmsh_xlate` `ImageTypesOf`).
- **Found on:** [DRAGON QUEST VII Reimagined](../../dm/games/0100A9D01C446000/README.md): the per-pixel volumetric lightmap read brick (0, 0, 0) of the brick atlas for almost every cell. Unreal merges each streamed level's lightmap into one atlas on the GPU: brick copies (`0a7ff4171bd1a82a`), then a patch of the shared 16³ `RGBA8_UINT` indirection texture (`7b958a7b21e72801`) whose bounds come from `GetDimensions` on the very `RWTexture3D` it writes. Only cell (0, 0, 0) was written, every other cell kept size 0, and the shader fell back to the atlas origin: the first daytime level's bricks, baked about 2¹⁵ brighter (the same bricks in gameplay hold 0.5–1.4). [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) has four such size queries (`cp_t_tcb_48` / `_4A`).
- **Check:** how it was found: the raw scene colour (`texraw`) put the characters at 1,000–2,000 and the walls at 0.05; a debug build of one character shader that wrote its terms in alternating 8-pixel columns isolated the volumetric lightmap ambient term; dumps of the atlas (`texlist` / `texraw`), of the uploaded level bricks (the 3D upload dump) and of the indirection texture, with the cell computed from the primitive's origin and View `VolumetricLightmapWorldToUVScale / Add` (View 220 / 221), gave the rest.

### More than 64 bindings in one shader

- **Symptom:** `[RED] shader … reflection conversion failed: 66 bindings, over the limit 64`, then `failed to load`, and the draws with it are dropped (a black map).
- **Cause:** the reflection is converted into fixed arrays of `NVN_REFL_MAXB` bindings (`nvn_refl.h`), which was 64. NVN gives a stage 32 textures and 32 samplers beside its uniform and storage buffers and images, so a shader can have more.
- **Fix:** `NVN_REFL_MAXB` is 96; the argument buffer entries keep their own limit (`NVN_REFL_MAXARG`, 192).
- **Found on:** [OCTOPATH TRAVELER II](../../dm/games/0100A3501946E000/README.md): the 3D world map of the traveler selection (`UiMM_WorldMap3D`, pixel shader `f7c8373b7323687d`: 30 textures, 30 samplers, 3 uniform buffers and the c1 / support / driver segments).

### Image atomics

- **Symptom:** the replay put a texture resource ID where the shader wanted an address; with the old 64 KB dummy buffer, pointers in the game's objects and in the host's program objects got their upper 4 bytes replaced and the game or the replay crashed, or `kIOGPUCommandBufferCallbackErrorPageFault`, after which macOS ignores the process's command buffers (GitHub issue #101). Later, `[RED] image atomics on a block-linear texture … not followed, the atomics go to a scratch buffer`, and compute shaders with atomics on a 3D image did not build (`array subscript is not an integer`).
- **Cause:** below MSL 3.1, SPIRV-Cross emulates an image atomic with a raw device pointer beside the texture (`texture_buffer<uint> cp_i_cb2_4C_r32ui` and `device atomic_uint* cp_i_cb2_4C_r32ui_atomic`), indexed by the bound image's width (`spvImage2DAtomicCoord`), so the host had to bind memory in that packed layout. A buffer image's pool memory has it. A block-linear image's Metal texture is a deswizzled copy whose pool memory has another layout, so its atomics went to a scratch buffer that nothing read, and a pointer shorter than the image (or an invalid coordinate) wrote over other GPU memory. For a 3D image SPIRV-Cross indexes the pointer with the `int3` coordinate itself.
- **Fix:** image atomics are Metal's native texture atomics (MSL 3.1: `texture3d<uint, access::read_write>::atomic_fetch_max`). `dmsh_atomic.py` has SPIRV-Cross write shaders with image atomics (an `OpImageTexelPointer`) at MSL 3.1, and every other shader at MSL 2.3 as before, so their MSL is unchanged and their libraries come from the compile cache; a shader whose MSL still has the pointer form fails the build. The atomics act on the texture every other access of the image uses (a buffer image's view of its pool, a linear image's pool memory, a block-linear image's Metal texture), for buffer, 2D, 2D-array and 3D images. Metal runs texture atomics only on an R32Uint / R32Sint texture made with `MTLTextureUsageShaderAtomic`: NVN images of those formats take that usage when they are made (`nvn_mtl.m` `img_usage`; a view keeps working through its base), the build marks the slots a shader uses atomically (`"atomic"` in the reflection, `nvn_rbind.atom`), and the host binds an atomic-capable write sink, logged red, in place of any other texture there (`dummy_img_atomic`). The translator's coordinate guard stays ([Bounds of image atomics](#bounds-of-image-atomics)).
- **Found on:** [Octopath Traveler 0](../../dm/games/01005270232F2000/README.md): 2,084 shaders with atomics on buffer images (Unreal's GPU particles; three ran in the first 100 seconds) and 16 on a 3D image. [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md): an atomic max into a 480x270 r32ui block-linear 2D array (fragment shader `2bd5037003980716`, the scene after the story opening's first cutscene), which ran about 1.4 MB past the 64 KB dummy buffer into zero-copy pools over the game's heap and into unmapped addresses (GitHub issue #101), then went to a scratch buffer. [FINAL FANTASY RESONANCE](../../dm/games/0100606026F12000/README.md) (8 shaders) and [Ghost Trick](../../dm/games/010029B018432000/README.md) (5, one of them a compare-and-swap): atomics on 3D images.
- **Check:** `dm/tools/tests/test_image_atomic_bounds.py` with the rebuilt translator, `DMSH_TEST_METAL=1 DMSH_TEST_GPU=1`: synthetic Maxwell atomics on buffer, 2D, 2D-array and 3D images, compare-and-swaps among them, run on native atomic textures of a Mac GPU under the Metal validation layer (`t_atomic_return.m`). `t_refl.m` checks the atomic flag of a reflected image.

### Buffer image reflection keeps its texture type

- **Symptom:** `image atomics: unsupported or overflowing IMG extent` at the first dispatch, despite a valid four-byte counter buffer.
- **Cause:** `nvn_refl.m` classified `texture_buffer<uint>` as the default 2D texture. The atomic admission check of the pointer emulation then rejected the actual Metal buffer texture against that 2D descriptor.
- **Fix:** the shared reflection type decoder recognizes `texture_buffer` as `MTLTextureTypeTextureBuffer`, so a buffer image keeps its buffer-image shape. `t_refl.m` checks the reflection converter with a buffer image.
- **Found on:** [SHIN MEGAMI TENSEI V: Vengeance](../../dm/games/010069C01AB82000/README.md), compute shader `2d2093c1f1d3a7e0`, image slot 2 (one R32Uint texel).

### Bounds of image atomics

- **Symptom:** Metal shader validation reports invalid image-atomic stores even with an image-sized scratch buffer; without validation, GPU page faults can freeze rendering.
- **Cause:** the translator lost the surface instruction's `IGN` bounds mode before lowering an image atomic, so a coordinate outside the image reached memory. Neither a pointer nor a native texture atomic outside its image is defined on Metal.
- **Fix:** the translator (`TranslationFlags.MetalImageAtomics`, Metal only; other backends keep their behaviour) keeps the bounds mode and whether the result is used, queries the bound image's size and tests every original coordinate, the layer included, before the atomic. Negative coordinates fail an unsigned comparison; coordinates are not clamped or flattened. Invalid coordinates leave memory unchanged and return zero ([ARB_shader_image_load_store, issue 30](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_image_load_store.txt)): a returning `IGN` atomic joins the old texel from the valid branch and zero from the other with an SPIR-V phi. A compare-and-swap (`SUATOM.CAS`: compare with Rb, swap in Rb+1) is Metal's texture compare-exchange behind the same guard; SPIRV-Cross loops `atomic_compare_exchange_weak` until it succeeds or the texel differs. Predicate-returning, `TRAP` and unknown modes fail translation, as do 64-bit, floating-point and half2 sources (checked before the descriptor format can erase their size), descriptors that are not R32 integers, bindless handles that do not resolve statically, and indexed or multisample images. Source 1D and 1D-array surfaces keep the translator's one-row normalization to 2D.
- **Found on:** [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md#rendering) ([PR #178](https://github.com/seedds/nxnative/pull/178)); [SHIN MEGAMI TENSEI V: Vengeance](../../dm/games/010069C01AB82000/README.md), whose two buffer-image counter shaders dispatched at boot consume their returned values.
- **Check:** `dm/tools/tests/test_image_atomic_bounds.py`. Portable tests check the MSL version choice and the rejection of the pointer form. With `DMSH_TEST_XLATE` set to a rebuilt translator (and `spirv-val`, `spirv-cross` on `PATH`) the synthetic shaders are translated, the guard's dataflow is checked in the SPIR-V (the size query and comparison dominate the atomic, the phi carries zero on the invalid edge), and the MSL is produced; `DMSH_TEST_METAL=1` compiles and links it (`DMSH_TEST_METAL_SDKS=macosx,iphoneos` for both SDKs) and `DMSH_TEST_GPU=1` runs the returning fixtures (above). Changing this path changes the chain version: rebuild the translator and update the lock before building shaders.

## Shaders compiled at run time

### Built-in shaders compiled with the game's own GLSLC

- **Symptom:** shaders missing from the map that no game file holds.
- **Cause:** a game whose shaders come from its own GLSLC at boot has no file holding their output. KTGL compiles 9 built-in GLSL shaders at boot with the game's own GLSLC (`glslcCompile` from `main+0x9c89f8`; the sources are strings in `main`).
- **Fix:** they are captured once on the Mac (`DM_GLSLC_CAPTURE`): `DM_GLSLC_CAPTURE=<dir>` saves every compile's output (`hle_glslc.c`; the compile object's first word points at {status, output}), and `shader_split.py` reads `<version dir>/glslc_capture` (the NSP script makes it for any game whose `main` holds GLSL sources and imports `glslcCompile`). The outputs are identical on every run. `shader_split.py` asks for captured GLSLC outputs only when a module of the profile's defines `glslcCompile`, as `profile_guess.py` does.
- **Found on:**
  - [Fire Emblem: Three Houses](../../dm/games/010055D009F78000/README.md) (SDK 6.5.0): the 2D primitive, text and loading-feather shaders.
  - [Catherine: Full Body](../../dm/games/0100BF00112C0000/README.md) (SDK 8.2.1): its two built-in programs are captured on the Mac the same way; they compile only once the host reports the GLSLC's GPU code version ([next entry](#glslc-gpu-code-version-against-the-drivers-range)).
  - [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md): `subsdk0` is NVN's runtime GLSL compiler and `main` imports `glslcCompile*`, but up to the house, the beach and the menus every shader the game loaded was in the offline tables.
  - [Captain Toad: Treasure Tracker](../../dm/games/01009BF0072D4000/README.md) (the rule that a module must define `glslcCompile`): `main` imports `glslcCompile` and holds agl's GLSL sources, but no module defines it, so nothing is compiled at run time.

### GLSLC GPU code version against the driver's range

- **Symptom:** a null read at `main+0x3316dc` in `nnMain`, right after the window setup (the program object's interface pointers at +0xd0 / +0xd8 both null).
- **Cause:** the game compiles two built-in programs at boot with its own GLSLC (`subsdk0`, the same build as Link's Awakening's), and first checks that GLSLC's GPU code version against `nvnDeviceGetInteger` items 0x2E–0x31 (the driver's supported range, `main+0x330dcc`). The host reported Super Mario Odyssey's SDK 7 driver range, 1.5–1.14; this GLSLC writes 1.15, so nothing was compiled and the first uniform lookup read the empty program.
- **Fix:** every SDK's driver runs the code of the GLSLC in the same SDK, so the host now reports at least the version the game's own `glslcGetVersion` returns (`hle_glslc.c` `dm_glslc_gpu_code`, `hle_nvn.c`). The host also advertises the translator's supported range for precompiled shaders ([Newest GPU code version reported by the driver](nvn-objects.md#newest-gpu-code-version-reported-by-the-driver)).
- **Found on:** [Catherine: Full Body](../../dm/games/0100BF00112C0000/README.md) (SDK 8.2.1, Atlus's GFD engine, CRI middleware; one boot).

## Pipelines and caches

### Shader-pair tables in a Mac app

- **Symptom:** a self-contained Mac app (`--platform mac-app`) logs `pipeline prebuild: no .../Resources/dm/shader_pairs.tsv, nothing to build` although every shader library is there; it draws no hybrid stand-ins, and each pipeline seen for the first time compiles on the submit thread (frames of 100–550 ms).
- **Cause:** `dm/mac/pack_app.py` copied the shader map, the prewarm manifest, `c1_full.tsv` and `cb_extent.tsv` beside `DM_NVN_SHMAP`, but not `shader_pairs.tsv` and `pso_pairs.tsv`, which `build_ipa.sh` copies into the IPA and the host's pipeline prebuild reads there (`pso_prebuild`). Without them the app makes no pulling bases. For a profile without `pso_pairs.tsv` a Mac run (`mac_run`) passes the shader split's copy as `DM_PSO_PAIRS`; the app had neither. A full shader-library build does not build the pipeline bases by itself.
- **Fix:** the packer copies both tables when the profile has them, and the split's `pso_pairs.tsv` (`--pso-pairs`, which the NSP script passes) when the profile has none. They stay optional. An app packed before the fix needs repacking. An NVN game's app also carries its pipeline archive made offline for this Mac's GPU, so the first start builds no base: [A Mac app](../development.md#a-mac-app).
- **Found on:** [The Legend of Zelda: Echoes of Wisdom](../../dm/games/01008CF01BAAC000/README.md#rendering-and-shaders), update 1.1.0: a 308.4 ms frame, 283.5 ms of it pipeline acquisition and 5.9 ms GPU work; with the tables the offline prebuild built all 7,495 bases. [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md#pipelines-on-a-cold-start): frames of 100–546 ms before, none of its bases compiled on the first start after.
- **Check:** `test_nsp_mac_app`: `App.test_pipeline_pair_tables`, `App.test_split_pso_pairs` and `App.test_layout` fail without the tables in the packer; `Script.test_pack_command` checks the split's copy is passed only when the profile has no `pso_pairs.tsv`.

### Offline pipeline archive reuse and fallback

- **Symptom:** a truncated `.mtl4a` reaches `MTL4ArchiveImpl::loadFromURL`; the bundle reports coverage with `0 of 0 probed bases found`; rebuilt shader libraries reuse an archive of their old binaries; or packing fails with `pso_offline: no base to build` when every eligible pair has function constants.
- **Cause:** the archive guard checked only four magic bytes; coverage accepted `found == probed` even when every sampled pair was ineligible; the offline cache key included library paths and profile stamps but omitted library bytes; and a zero-base plan aborted before writing its device remainder list.
- **Fix:** `arch_file_ok` checks the GPU Mach-O load commands and payload ranges, universal slice bounds and the serializer's bundled Metal library ranges before opening the archive. It does not validate the compiler's private payload. `pso_bundle_covers` requires at least one actual lookup and no misses. `pso_offline.py` hashes every consumed library's bytes into the cache key; when all eligible pairs need device specialization, it removes old archives and writes `rest_pairs.tsv` without invoking `metal-tt`.
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md#found-on-this-game), during review of its offline archive path with synthetic libraries and pair lists; no new gameplay or iPhone run.
- **Check:** `dm/tools/tests/test_pso_offline.py` changes vertex and fragment library bytes without changing entries or stamps, and exercises the all-constant plan. `t_pso_bundle` rejects bare headers, truncated archives, malformed load commands and out-of-range universal slices, accepts real offline and serialized archives, and rejects zero-probe coverage.

### Metal's build folders left by pipeline archive flushes

- **Symptom:** the user cache folder (`getconf DARWIN_USER_CACHE_DIR`, then `com.apple.gpuarchiver/`) fills with `gpuarchiver-*` folders of 0.3–5 MB each, one per pipeline archive flush, and they stay after the game exits: 456 folders, 2.5 GB, on one Mac after two days of NVN game runs. On iOS they stay in the app's own cache folder.
- **Cause:** the Metal 4 serializer (`MTL4PipelineDataSetSerializer`) makes a build folder for the captures between two flushes, at the first compile it captures. No flush, release or exit removes the folder. Nothing reads it once its flush has written the archive file: a probe that removed each folder after its flush took every pipeline from the files again. The process keeps no file open in it, and on the Mac the folder is shared by all of the user's programs.
- **Fix:** `nvn_mtl.m` (`ga_capture`, `ga_flushed`, `ga_exit`): the first capture after a flush lists the folder before and after its compile and takes the one folder that appeared as its own; the flush removes it once its file is written; exit removes the folder of captures no flush wrote. When more than one folder appeared in that window (another program's capture), none is removed. On iOS the start removes the folders earlier runs left. Folders left before this fix stay; they are safe to delete while no game or Metal tool runs. Compaction now captures the original shader functions too ([serializer crash](#a-compaction-flush-that-crashes-then-crashes-every-start)); the earlier lookup-import path instead made its folder during the flush.
- **Found on:** reported by MacNativeGames (`1eff989`, `110e6c6`, Metal 3's `MTLBinaryArchive`, 25 GB overnight on Tears of the Kingdom); reproduced here with the Metal 4 serializer by a probe and on this repository's Mac, no single game identified.
- **Check:** `t_pso_archive` checks that the first capture finds the folder and the flush removes it; a run of the test leaves no new folder (it left two before the fix, one from compaction and one from captures no flush wrote).

### A compaction flush that crashes, then crashes every start

- **Symptom:** a previously working game exits with 139 on every start after pipeline preparation; `dispatch_retain(NULL)` is called by `-[_MTLBinaryArchive materializeAirNTFromOnlineForSlice:entryList:]` under `airntSerializeToURL` and `arch_flush` ([issue #342](https://github.com/seedds/nxnative/issues/342)).
- **Cause:** more than eight run archives enables compaction. The prewarm used `MTL4CompilerTaskOptions.lookupArchives` to recapture their bases into the serializer that also captures newly compiled bases. Metal can crash while materializing that dataset for serialization. It crashes before `arch_compact_finish` removes the old files, so every restart retries it. Framework analysis recorded in issue #342 identifies a nil `MTLBinaryEntry.bitcode`, read from the captured function's library by `bitcodeDataInternal` when the bytes at its bitcode offset are not an AIR wrapper. Which function lacks AIR and why remain **open**; simple synthetic recaptures and a replay with no new bases did not reproduce it. A mixed Tears of the Kingdom replay did, with the reported null call and backtrace, on an M4 Pro with macOS 27.0.1 (26A434).
- **Fix:** work around the framework defect by capturing each compacted base from its original shader functions, with no lookup archives. Metal's ordinary compiler cache still applies. The archive threshold, ordinary archive lookups, deferred flush during prewarm and removal of old files only after a successful write remain in place. A compaction start can do more compiler work; normal starts keep their archive hits. `ga_capture` no longer accepts task options, so captured bases cannot accidentally import archive entries. The recovery guard from the earlier workaround remains: `pso_archive/compacting` records the system build and the capture method before a compaction flush, and is removed when the flush returns. A marker for this build and method suppresses another attempt after a crash. Legacy OS-only markers from archive imports are cleared so the safer capture method can run; a later OS or capture-method change also allows another attempt.
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md#found-on-this-game), reported on an M5 Max and reproduced with the local shader libraries on an M4 Pro. This is shared NVN archive code, not a game profile rule.
- **Check:** the same 19-file snapshot, 1,490 archived bases and 44 new bases failed in two of three runs with lookup imports and passed all three when only that option was removed. The implemented fix passed three further compactions and their following restarts: each compacted file supplied all 1,534 bases with zero new base compilations on restart. These are library-only manifest replays using the local `e7245308050d` libraries and a matching diagnostic chain header, not a new gameplay run or an iOS test. `t_pso_archive` rejects lookup imports at the real compiler seam; `test_pso_archive.py` exercises disk-built libraries, concurrent recaptures mixed with a fresh function, function constants, compaction and restart in separate processes (`make -C dm/gfx/tests GAME=<id> check_t_pso_archive`). The marker tests retain the recovery guard and check migration from the old OS-only marker, including a fresh-process restart with that marker present.

### Pipeline keys omit unused colour state

- **Symptom:** identical render pipelines compile repeatedly under different keys, lengthening startup prewarm and adding pipeline misses during play.
- **Cause:** NVN blend and channel-mask state persists between draws, including state for colour targets absent from the current pass. `pso_desc` skips targets without a format and reads blend factors only when blending is enabled, but the old keys retained unused state. Deduplicating manifest lines alone also misses old rows whose unused state differs.
- **Fix:** `pso_key_norm` in `nvn_pso_skey.h` clears blend factors when blending is disabled and, for normal passes, the blend-enable bit and write-mask nibble of every target without a format. Merged passes retain their logical blend-enable and mask state because `lmap` maps physical attachments to logical targets; a missing physical format does not imply the same logical target is unused. `pso_key_fill` normalizes live keys, `pso_skey_parse` normalizes historical prewarm rows, and `warm_worker` deduplicates the parsed keys across all input lists before compiling. Depth and stencil formats remain in the key because the `MTLDevice` path needs them, even though Metal 4 bases omit them.
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md#files-and-shaders) (disabled blend factors), [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md#files-and-shaders) (absent-target state, reported by MacNativeGames).
- **Check:** `t_pso_skey` covers normalized round trips, historical rows differing only in unused state, and preservation of merged-pass logical masks and blend bits; `t_pso_archive` checks disabled blend factors with all targets present. `make -C dm/gfx/tests GAME=<id> check_t_pso_warm` loads two synthetic manifests through `warm_worker`, checks compilation counts and draw-time cache hits, and compares GPU pixels with the original descriptors through both MTLDevice and Metal 4. It includes a merged pass whose physical attachment 0 uses logical target 7's blend and mask state.

### Shader cache key covering the pairs

- **Symptom:** a vertex shader whose pairs changed was restored unpadded from `cas/`.
- **Cause:** `dmsh_build.py`'s shader cache was keyed without the pairs.
- **Fix:** the per-shader key covers the pairs, and a paired vertex shader's key also covers its pixel shaders' keys (`shader_key`, `pair_keyed`), so a change to a profile's pairs rebuilds what it affects ([Pixel shader inputs the vertex shader does not write](#pixel-shader-inputs-the-vertex-shader-does-not-write)).
- **Found on:** [13 Sentinels: Aegis Rim](../../dm/games/01003FC01670C000/README.md).

### Function constants specialized at load

- **Symptom:** `fragmentFunction main0 cannot be used to build a pipeline state` (a Metal validation assertion).
- **Cause:** SPIRV-Cross declares an optional function constant, `spvLinearTextureAlignmentOverride`, in shaders that read a texel buffer, and Metal refuses any function with function constants until it is specialized.
- **Fix:** `shader_load` specializes such functions with an empty set of values.
- **Found on:** [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md) (one imgui pixel shader).

### Layer-writing vertex shaders get the triangle class

- **Symptom:** a pipeline for a vertex shader that writes `render_target_array_index` cannot be built.
- **Cause:** Metal builds such a pipeline only with an input primitive class, and the translator writes a point size in every vertex shader, which needs the point class: no class fits both.
- **Fix:** `vs_layer` drops the point size from vertex shaders that write the layer, and the host gives their pipelines the triangle class once Metal names the reason (`layer_out` in `pso_get_` and in the background prewarm). Their passes are layered ([Layered passes for vertex shaders that write the layer](rendering.md#layered-passes-for-vertex-shaders-that-write-the-layer)).
- **Found on:** [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md): two full-screen vertex shaders (`57246f9a04af8529`, `b29f08619f379a83`: one triangle per instance, layer = instance + offset, 20 pixel shaders read the layer).

### The precompile look-ahead restores the scissor

- **Symptom:** the picture bright green and white from some scene on, in runs with pipelines compiled on demand (prewarm lists off or short) and in none with `DM_NVN_PREDICT=0`.
- **Cause:** after a pipeline miss the replay looks ahead through the rest of the submission, replaying state only, to compile the next pipelines in the background (`pso_predict_rest`). It restored `S`, but `state_op` also sets the scissor, which lives in `R`: the replay went on with the last scissor of the submission until its next `OP_SCISSOR`, so the draws in between were clipped to another pass's rectangle.
- **Fix:** the scissor is saved and restored with `S`; the fusion probe (`fz_probe_cov2`) already restored all of `R`. Every game.
- **Found on:** [OCTOPATH TRAVELER II](../../dm/games/0100A3501946E000/README.md): from Castti's opening flashback and the battlefield of Hikari's opening on.
- **Check:** found by bisecting with four runs started at once on copies of one save: `DM_NVN_PREDICT=0` fixed it, a look-ahead that compiled nothing did not, and of its parts only the state ops brought it back; four runs green before the fix, four right after it.

### GPU frame captures need pipelines built through the device

- **Symptom:** a GPU frame capture (`DM_GPU_CAPTURE`, or Xcode with `MTL_CAPTURE_ENABLED=1`) fails with `Capturing Render pipeline specialization is not supported`, or the capture layer crashes on the pipeline archive; a capture of the `MTLDevice` is refused.
- **Cause:** Metal cannot capture pipelines made from Metal 4 bases (`DM_PSO_MTL4`, the default on the Mac), and its Metal 4 to 3 conversion for the capture layer crashed on the pipeline archive. A device the replay drives through Metal 4 cannot be the capture object.
- **Fix:** a run that may capture (`DM_GPU_CAPTURE` or `MTL_CAPTURE_ENABLED` set) builds its render pipelines through the `MTLDevice`, without bases or archive (`pso4_on`), and `gpu_capture_frame` captures the Metal 4 queue (`g_q4`): work on the old queue (present, some uploads) is not in the trace. Such a run builds pipelines more slowly than a normal one, so its frame times are not the game's. Every game.
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md) (Mac and iPhone).

### A cold Metal cache looks like a long load

- **Symptom:** the first boot sat on the loading clock for about a minute, the CPU idle, with every file read done after 12 s.
- **Cause:** that run compiled its Metal libraries and pipelines on first use (cold cache). With a warm cache the title is up after about 15 s; what looked like a 40-second wait in the read log (`DM_FSLOG`) was the title screen itself, which reads nothing.
- **Check:** screenshots every few seconds (`ctl.sh snap` from a script) told the two apart.
- **Found on:** [Bayonetta 3](../../dm/games/01004A4010FEA000/README.md) (SDK 13.3.2).
