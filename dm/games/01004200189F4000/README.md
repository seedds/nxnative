# Factorio (`01004200189F4000`)

Factorio 2.0.77 (build 84539), update v2097152, SDK 21.4.2. Wube's engine on SDL with the **NintendoSDK OpenGL driver**, which the host serves through its GL → Metal layer (`dm/gfx/gl/hle_gl.m`). The game is an unusual OpenGL port: it imports EGL and compiles GLSL at run time, so the shaders are translated by `dm/gfx/tools/gl_shaders.py` rather than the NVN shader chain. Modules `main` and `sdk`; `subsdk0` is the OpenGL driver and the host does not load it.

Brought up on an iPad Pro (M2). The main menu, a new world, the intro cutscene and gameplay render. An iPhone 15 (A16, 6 GB) runs it with the decode path for GPUs that sample no BC, which is a separate change.

**This patch needs a GPU that samples BC.** It carries no decoder for one that does not (A16 and older): the BC extensions are not advertised there, and a compressed upload is refused with a message instead of being decoded.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | `game.tsv` | `subsdk0` is NintendoSDK's OpenGL driver; the host's own GL layer replaces it |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | main's own pool (the log reports a ~3.1 GB guest heap) |
| `shader_groups /data/=data` | `game.tsv` | the GLSL sources under `rom:/data/core/graphics/shaders` |
| `zip_store .dat:.ogg:.png:.ttf:.zip:.metallib` | `game.tsv` | RomFS files kept stored in the IPA |
| `op_mode handheld` | `game.tsv` | Factorio's free cursor is moved by the right stick and jumped by tapping; touch is only fed in handheld mode. The launcher's Console mode overrides it |
| `romfs_marker data/core/fonts/NotoSansCJKsc-Bold.ttf` | `game.tsv` | the RomFS survey marker |

No `patches.py`: nothing in the game's files is cut or rewritten. What is game-specific is the profile itself (keyed by title ID), plus one host row keyed by this build's ID: the copy-image dispatch slot the host writes into `main` (see [Rendering](#rendering)).

## Rendering

- **GL coverage.** Factorio uses GL 4.5 desktop features: multiple render targets, uniform buffer objects, sampler objects, integer textures (`RGBA32UI`/`RG32UI`), `glCopyImageSubData`, compressed textures, texture readback and 3D textures. The GL layer implements these; 97 GLSL shaders translate and link.
- **MRT.** The 3D bitmaps render into two colour attachments; passes order attachments by `glDrawBuffers`, and per-attachment write masks are carried in the pipeline key. `glColorMaski` is per draw buffer.
- **UBOs/samplers.** Named uniform blocks get their own Metal buffer slots; `glGetUniformBlockIndex`/`glBindBufferBase`/`glUniformBlockBinding` and sampler objects are implemented.
- **Factorio's copy-image dispatch slot.** The game fills `main+0x2e37260` from a weak `glCopyImageSubData` import and never asks `eglGetProcAddress` for it; the host writes its function there at the first 16³ 3D bitmap (gated on this build ID).
- **Cutscene.** `glCopyTexSubImage2D/3D` copy the read framebuffer into the bound texture; the level intro uses the 3D form.
- **Texture readback (the save preview).** Factorio renders the preview into 75/76/79, composites it into texture 74 (`game.frag`, sampling `gameview` = 75) and reads 74 with `glGetTexImage`. The read used a command buffer of its own, committed at once, while the frame's draws were still uncommitted: the queue ran the read first, so the preview was a solid `0xFF00FF` rectangle, and even 75 (hundreds of `sprite_light` draws) read as one colour. A client-visible read now commits and waits for the frame's command buffer first, as `glFinish` does. The preview is the game's own 960×540 capture; there is no host fallback.
- **Buffer textures (biome and water edges).** `tiles.vert` and `tiles-lava-effect.vert` fetch each tile's mask coordinates from `maskTexCoordTable`, a `samplerBuffer`: the game fills a buffer object and attaches it with `glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, …)`. The layer implements `GL_TEXTURE_BUFFER` (and `glTexBufferRange`) as a Metal `texture_buffer` view over the GL buffer's own `MTLBuffer`, remade when the buffer object is reallocated. Before that `samplerBuffer` was not in the shader type table, the sampler was skipped at bind time and every `texelFetch` returned 0: tiles blended with the mask texel at 0 (hard biome edges) and the water surface never reached over the shore.
- **Compressed sub-images (the water surface).** Factorio allocates a mask or effect texture with `glTexImage2D(GL_COMPRESSED_RED_RGTC1, …, NULL)` and then streams the blocks through **`glCompressedTexSubImage2D`**, one block-aligned rect at a time. The sub-image path refused a compressed sub-image into a compressed texture, so the water noise texture (512×512 `RGTC1`) and the alpha masks stayed uninitialised (black): the water surface and the character's shadow mask drew nothing. A compressed texture takes the sub-rect's blocks as they are now. `CV_COPY` also converts per pixel when the source has more components than the texture (an `RGBA` source into `R8` kept each pixel's red channel; a row `memcpy` had been taking the first quarter of the row).

## Services and input

- **Ramdisk.** `nn::fs::MountRamDisk` is replaced by a host mount of `<temporary storage>/ramdisk`; `GetRamDiskBufferSize` reports 64 MB. Without it, `ramdisk:/currently-playing/blueprint.dat` was ENOENT when starting a new game.
- **Software keyboard.** The save-name field uses the **UTF-8** `InlineKeyboard` callbacks (`SetChangedStringCallbackUtf8` / `SetDecidedEnterCallbackUtf8`), which the host records and answers with a `ChangedStringArgUtf8`/`DecidedEnterArgUtf8`. The host shows the iOS input box.

## Open

- The host reports the Switch's 256 MB save quota; Factorio warns that save space is low (`268435456/268435456`) even though the phone has free space. Saving itself works, preview included.

## Building

```
scripts/build_ipa_from_nsp.py <folder with the base and update NSP> -k <keys> --allow-other-metal --env-file <env>
```

The GL shader library is generated for iOS by `dm/gfx/tools/gl_shaders.py --ios`; the profile's NVN shader tables are placeholders and unused.
