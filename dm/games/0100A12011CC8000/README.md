# Fire Emblem: Shadow Dragon and the Blade of Light (`0100A12011CC8000`)

Base game 1.0.0 (v0, no update), SDK 10.4.0. An NES game: the RomFS is empty and `main` holds the Japanese Famicom ROM, its emulator and the English layer ([Inside main](#inside-main-the-nes-game-and-its-shell)). It renders with **OpenGL** through EGL, not NVN: NVIDIA's desktop GL, which the host implements on Metal (`dm/gfx/gl/hle_gl.m`). Modules `main` and `sdk`; `subsdk0` (NVIDIA's OpenGL driver) is never loaded. It needs the GL layer's separate vertex attribute format and binding, which is #1364. Brought up on the Mac only; nothing here was run on an iPhone or iPad.

On the Mac: the Nintendo logo, the intro text ("Our tale begins with the Shadow Dragon, the blade Falchion"), the title screen and an attract-mode unit card (Knight), with no red line and 2 draws per frame. Input, sound, saves and anything past the attract mode were not tried.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | `subsdk0` is NVIDIA's OpenGL driver, which programs the GPU through NvRm: the GL layer answers `main`'s EGL imports |
| `sys_resource 0x0`; `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | The provisional profile's guesses, unchanged: the NPDM declares no system resource, and `main` sets a heap of at most the pool − 64 MB (a 3328 MB heap region). The boot reaches `nnMain` and runs on them |
| Node area cut and 4 MB and larger → 2 MB region alignment | `dm/tools/sdk_patches.py` SDK-wide rules, `sdk` build `204eaef83fe5…` | The rewrite log: `AddressRegionManager node area 1 GB → 62 MB` (implementation 2 of 7) and `AllocateAddressRegion: regions of 4 MB and more aligned to 2 MB` (implementation 1 of 4) |
| The GL layer's separate vertex format and binding, the `GL_UNIFORM_BUFFER` target, `glMemoryBarrier` | `dm/gfx/gl/hle_gl.m` (#1364) | See [Rendering (OpenGL)](#rendering-opengl) |
| No NVN shader tables | `game.tsv` `shader_note` | no NVN shaders: `gl_shaders.py` writes the GL library and its table |

## Rendering (OpenGL)

- **Telling the API apart:** the survey finds EGL and `glShaderSource` in `main`. NVN appears only in `sdk` and in the unloaded `subsdk0`.
- **Which GL functions:** one loader function (`main+0x1f450` to `+0x30978`, called once from `main+0x1ef54`) runs 2,956 `eglGetProcAddress` stanzas, with no table-driven variant. The code calls 45 GL names at 80 sites, plus all 11 `egl*` imports. Two of them the boots never reach: `glDetachShader` (`main+0xd9e4`, `+0xd9f0`, on the teardown path), which #1364 adds, and `glDepthBoundsEXT` (`main+0x475c`), which stays out (below).
- **Resolved: a jump to address 0 from `main+0xd720`** (`lr=main+0xd724`): the game calls `glVertexAttribFormat(0, 2, GL_FLOAT, GL_FALSE, 0)` and the layer had no such function, so `eglGetProcAddress` returned a null pointer. With `DM_GL_STUBS=1` the run goes on and names the rest: `glVertexAttribBinding`, `glBindVertexBuffer` and `glBindBufferRange(GL_UNIFORM_BUFFER, …)` and `glMemoryBarrier(CLIENT_MAPPED_BUFFER_BARRIER_BIT)` every frame, and `glBindBuffer` / `glBufferStorage` on `GL_UNIFORM_BUFFER`; no draws. #1364 makes an attribute take its buffer, offset, stride and divisor from its binding, as GL 4.3 does, and adds the uniform buffer target and a `glMemoryBarrier` that does nothing. With it: 2 draws per frame, 1 pipeline.
- **`glDepthBoundsEXT` stays unimplemented:** its caller is behind a byte (`main+0xc0e370`) that the loader's extension loop sets when `glGetStringi(GL_EXTENSIONS, i)` lists `GL_EXT_depth_bounds_test` (entry 261 of its 588-name table). The layer does not list it, so the game never enables `GL_DEPTH_BOUNDS_TEST_EXT`.
- **What a frame does** (`main+0xda70` to `+0xdc28`, static): `glClearBufferfv` black, `glUseProgram` and `glBindVertexArray`, `glBindVertexBuffer(0, positions, 0, 8)`, `glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, 0, 4)` and `glMemoryBarrier`, alpha blending, then two uploads and draws into the **same** texture:
  - `glTexImage2D` 512×256 RGBA from a CPU image (the NES picture in the left 256 columns), `glDrawArrays(GL_TRIANGLES, 0, 6)`;
  - `glTexImage2D` 1920×720 when a second CPU image exists, `glDrawArrays(GL_TRIANGLES, 6, 6)` (fullscreen), unconditionally;
  - `glFinish` and `eglSwapBuffers` (`main+0x53e0` to `+0x59ac`).

  The positions are a vec2 stream at `main+0x45c124`. The first quad is ±0.7625 × ±1.0167, a 4:3 picture with a little overscan, computed from `nn::oe::GetDefaultDisplayResolution` (`main+0xd2b4` to `+0xd38c`). The texture coordinates are a second stream by `glVertexAttribPointer` with stride 0. The 4-byte uniform buffer (`MAP_WRITE_BIT | PERSISTENT | COHERENT`) is never written or mapped and neither shader reads a block. There is no framebuffer object, viewport, scissor or sRGB. With `DM_GL_DRAWLOG=4:8` draw 1 samples texture 1 at 512×256 and draw 2 the same texture at 1920×720: the redefinition between the draws keeps them apart. The surface is 1280×720.
- **Shaders:** the only two are the `glShaderSource` calls at `main+0xd464` and `+0xd4e0`; `gl_shaders.py` translates them from `main`.

## Inside main: the NES game and its shell

Static findings from the extracted `main`. Offsets are `main` module offsets (`main+X`, the addresses the host's backtraces print); in an ELF from `dm/tools/nso2elf.py` every segment sits at file offset X + 0x1000.

- The RomFS is empty and `main` has no iNES or FDS header.
- **The ROM is the Japanese Famicom one, unpatched.** PRG, 256 KB raw, at `main+0xd721c` to `+0x11721c`, behind a custom 16-byte header at `+0xd720c`. The last bank, at `+0x11321c`, has the vectors NMI `$C163`, RESET `$C075` and IRQ `$C075`; the RESET target at `+0x113291` decodes as `SEI; CLD; LDX #0; STX $2000; STX $2001`. CHR follows at `+0x11721c`. A debugger read of the cartridge descriptor at cart init gives 256 KB PRG and 128 KB CHR.
- **Emulator:** a 6502 interpreter dispatching through a 256-entry table of {length, cycles, handler} at `main+0x467518` (all 151 documented opcodes match the reference lengths and cycle counts), with a 38-entry mapper table (`MMC1` to `MMC6`, `VRC*`, `DiskSystem`, …) at `+0x469648`. Which mapper the game uses was not pinned down.
- **English:** the PRG is not patched. The shell reads the game's RAM and VRAM, looks up resources by ID (`DATA_TALK*`, `DATA_WEAPON_NAME*`, `SCRN_M_*`; the table at `main+0x45c998`), and draws the English text and screens into the 1920×720 RGBA image of draw 2, over the NES picture. A Japanese system language (`nn::oe::GetDesiredLanguage`, `main+0x47b4`) turns that layer off.
- **Saves:** `save:/FA5J_FA5J.sram` (the cartridge's 8 KB battery RAM), `save:/suspend.sram` and one save state `save:/marugoto.sss` (a 16-byte header with version 0, then named chunks `+CPU`, `+PPU`, `+SPU`, `-WRAM`, `-SRAM`, `-VRAM`, `-OAM`, …). Rewind keeps 20 snapshots of 256 KB. The menu opens with X or L+R. No checksum over the ROM or the saves.

## Mac routes

- `DM_NVN_SHOT=300,900,1500,2300:shot_%d.png` on a run with no input: the Nintendo logo (frame 300), the intro text over the sword (900), the title screen (1500) and the Knight card (2300).

## Open

- Input, sound, saves, the game itself past the attract mode and the iPhone and iPad are not checked.
- The mapper: cart init reads board id 5, which is also the header's first byte. Which mapper that id selects is not pinned down.
- `glDetachShader` has never been reached in a run.
