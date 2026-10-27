# dm/gfx/tex — texture format mapping, block-linear deswizzling, BC decompression

The part of the graphics layer that deals with texture data, called by the replay layer `../nvn/nvn_mtl.m`. It does three things:

1. converts NVN texture format numbers to Metal pixel formats;
2. unpacks the Switch's "block-linear" layout into an ordinary row-by-row layout;
3. decompresses BC textures on GPUs that do not support BC compressed textures.

Block linear is how the Tegra X1 GPU stores textures: the data is not one row after another, but interleaved in small blocks of 64 bytes × 8 rows (GOBs), and several GOBs are then stacked into larger blocks according to a "block height" and a "block depth". The game's packaged texture files are in exactly this layout, and it must be undone before uploading to Metal.

## Files

| File | Contents |
|---|---|
| `nvn_fmt_mtl.h`, `nvn_fmt_mtl.c` | `nvn_fmt_to_mtl()`: NVN format (0x01–0x9C) → Metal pixel format + component remapping. Returns 1 for an exact match, 2 for a substitute (precision, bit order or interpretation differs and a conversion is needed), 0 when Metal has no corresponding format. How each format is handled is written in the comments inside the table. Pure C, no dependency on Metal headers |
| `nvn_deswizzle.h`, `nvn_deswizzle.c` | `nvn_deswizzle()`: deswizzles one level and one layer from block linear to linear; `nvn_swizzle()`: the reverse, used for round trips in unit tests; `nvn_bl_offset()`: the block-linear address of a single point. Pure C |
| `nvn_bcdec.h`, `nvn_bcdec.c` | `nvn_bc_unpacked()` / `nvn_bc_decode()`: the decoded Metal format of a BC format, and a decoder for the blocks of one level and layer (BC1–BC7; decoding is `dm/third_party/bcdec`); `nvn_bc_cpu_wanted()`: whether this device needs it. Pure C |
| `nvn_deswizzle_gpu.h`, `nvn_deswizzle_gpu.m` | A Metal compute shader version of the same deswizzling algorithm, used by the replay layer when uploading packaged textures; when initialization fails or an unsupported format is encountered, the replay layer falls back to the CPU version |
| `tests/` | Unit tests |

The offsets of each level and layer within the storage, and the block height and block depth actually used by each level, are all taken from `../nvn/nvn_tex.c` (`nvn_tex_layout`, `nvn_tex_level_offset`, `nvn_tex_level_tiles`); this directory only does addressing and moving within a level. Packaged parameters override the automatic block dimensions: [Packaged texture block dimensions and linear storage rows](../../../docs/reference/nvn-objects.md#packaged-texture-block-dimensions-and-linear-storage-rows).

## BC textures

A device that can sample BC (`supportsBCTextureCompression` of `MTLDevice`: Apple silicon Macs and the latest iPhones) creates and uploads BC textures as BC, with the GPU deswizzle; this is the only path the supported devices take. A device that cannot (A15 / A16 and older iPhones and iPads) gets `nil` for every texture made in a BC pixel format, so the replay layer makes such a texture in the uncompressed format `nvn_bc_unpacked()` names (RGBA8, its sRGB form, R8 / RG8 for BC4 / BC5, RGBA16F for BC6H) and fills it with texels `nvn_bc_decode()` produced from the deswizzled blocks, on the CPU. The decoded texture takes 2 to 8 times the memory of the blocks (BC1 and BC6H 8 times), so a game with many BC textures may not fit; `DM_TEST_NO_BC=1` takes the path on any machine ([reference](../../../docs/reference/rendering.md#bc-textures-on-a-gpu-that-cannot-sample-them)).

A BC texture is never a render target and its raw blocks are not sampled through an uncompressed view, with or without BC support (Metal has no such views; Breath of the Wild encodes one 32×32 BC4 texture on the GPU, GitHub issue #4).

## Tests

```sh
make -C dm/gfx/tex/tests run       # run t_fmt_mtl, t_tex and t_bcdec
```

- **`t_fmt_mtl`**: takes the mapping for every NVN format in 0x01–0x9C and, for every one whose mapping is not 0, actually creates a texture on the local Metal device; all of them must be created successfully. It also checks that every NVN format with sRGB in its name lands on a Metal sRGB format. No game files needed.
- **`t_bcdec`**: the BC decoder and the decision to use it, no GPU: BC1 blocks against a decoder written without bcdec, hand-made BC2–BC7 blocks, every BC format's decoded format, partial edge blocks and mip levels down to 1×1, destination pitches, and `nvn_bc_cpu_wanted()` for a device with and without BC. `make -C dm/gfx/tests GAME=<id> check_t_bc_cpu` (Metal 4 needed, so local only) takes the path through the host: a CPU-written array texture with levels and a buffer-to-texture copy read back as the decoded texels, and the GPU path unchanged when the CPU path is off.
- **`t_tex`**: the deswizzling tests, in several groups:

  | Group | Criterion |
  |---|---|
  | A round trip | Writes random data into each level and layer and reads it back out; must be byte-for-byte equal. Covers 2D, 2D array and 3D, several bytes-per-block values and non-power-of-two sizes |
  | B address formula | The result of `nvn_bl_offset()` equals the address computed by hand from the public layout rules |
  | D differential copies | Both copy directions match a byte-address oracle for 1,029 deterministic layouts, including padded pitches, byte tails, sparse mip tails, packaged block dimensions, arrays and cubes; untouched storage and row padding are compared too |
  | E hand-made 3D | Places markers at hand-computed addresses, checks where they land after deswizzling, and checks block height, block depth and storage size |

  The A round trip can only prove "one-to-one and within bounds"; it cannot prove that the layout formula is right. What backs it up is the hand-computed addresses of groups B and E, and the games that render right.

## Limitations

- `nvn_deswizzle()` does not support multisampled textures, textures with the LINEAR flag (which are linear to begin with) or the BUFFER target; it returns -1 when it encounters them.
- Cubes and cube arrays are handled as layered textures.
- The component bit order of the 16-bit packed formats (565 / 5551 / 4444) is mapped according to the format names and has not been checked with real data; they are marked as "substitute" in the format table.
