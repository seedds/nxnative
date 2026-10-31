# dm/gfx/tex — texture format mapping, block-linear deswizzling, BC decompression

The part of the graphics layer that deals with texture data, called by the replay layer `../nvn/nvn_mtl.m`. It does three things:

1. converts NVN texture format numbers to Metal pixel formats;
2. unpacks the Switch's "block-linear" layout into an ordinary row-by-row layout;
3. decompresses BC textures on GPUs that do not support BC compressed textures.

For the Vulkan backend of the Windows and Linux port (`dm/gfx/hostvk`) it also holds that backend's format table and an ASTC decoder in plain C (below); the Apple builds compile the decoder with the rest of the directory and do not call it.

Block linear is how the Tegra X1 GPU stores textures: the data is not one row after another, but interleaved in small blocks of 64 bytes × 8 rows (GOBs), and several GOBs are then stacked into larger blocks according to a "block height" and a "block depth". The game's packaged texture files are in exactly this layout, and it must be undone before uploading to Metal.

## Files

| File | Contents |
|---|---|
| `nvn_fmt_mtl.h`, `nvn_fmt_mtl.c` | `nvn_fmt_to_mtl()`: NVN format (0x01–0x9C) → Metal pixel format + component remapping. Returns 1 for an exact match, 2 for a substitute (precision, bit order or interpretation differs and a conversion is needed), 0 when Metal has no corresponding format. How each format is handled is written in the comments inside the table. Pure C, no dependency on Metal headers |
| `nvn_deswizzle.h`, `nvn_deswizzle.c` | `nvn_deswizzle()`: deswizzles one level and one layer from block linear to linear; `nvn_swizzle()`: the reverse, used for round trips in unit tests; `nvn_bl_offset()`: the block-linear address of a single point. Pure C |
| `nvn_deswizzle_gpu.h`, `nvn_deswizzle_gpu.m` | A Metal compute shader version of the same deswizzling algorithm, used by the replay layer when uploading packaged textures; when initialization fails or an unsupported format is encountered, the replay layer falls back to the CPU version |
| `nvn_fmt_vk.h`, `nvn_fmt_vk.c` | The Vulkan backend's format table (`dm/gfx/hostvk`, [vulkan-backend.md](../../../docs/design/vulkan-backend.md) 5.1): NVN format → `VkFormat`, its substitute, the format an upload converts or decodes into, the component mapping, and the path a texture of it takes (direct, expanded to four components, converted, ASTC decoded, depth packing converted). It includes the Vulkan headers, so the Apple Makefiles leave it out |
| `astc_decode.h`, `astc_decode.c` | An ASTC LDR decoder in plain C (every 2D block size, the Khronos specification's procedure, the 8-bit decode): the reference the Vulkan backend's GPU decode (`dm/gfx/hostvk/shaders/astc_decode.comp`, the same steps under the same names) is checked against bit for bit, and its CPU fallback (decision D1). Apple decodes ASTC in hardware and does not use it |
| `tests/` | Unit tests |

The offsets of each level and layer within the storage, and the block height and block depth actually used by each level, are all taken from `../nvn/nvn_tex.c` (`nvn_tex_layout`, `nvn_tex_level_offset`, `nvn_tex_level_tiles`); this directory only does addressing and moving within a level. Packaged parameters override the automatic block dimensions: [Packaged texture block dimensions and linear storage rows](../../../docs/reference/nvn-objects.md#packaged-texture-block-dimensions-and-linear-storage-rows).

## BC textures

Only GPUs with BC compressed textures are supported (`supportsBCTextureCompression` of `MTLDevice`: Apple silicon Macs and the latest iPhones), so BC textures are created and uploaded as BC; there is no decompression path. A BC texture is never a render target and its raw blocks are not sampled through an uncompressed view (Metal has no such views; Breath of the Wild encodes one 32×32 BC4 texture on the GPU, GitHub issue #4).

## Tests

```sh
make -C dm/gfx/tex/tests run       # run t_fmt_mtl and t_tex
```

- **`t_fmt_mtl`**: takes the mapping for every NVN format in 0x01–0x9C and, for every one whose mapping is not 0, actually creates a texture on the local Metal device; all of them must be created successfully. It also checks that every NVN format with sRGB in its name lands on a Metal sRGB format. No game files needed.
- **`t_tex`**: the deswizzling tests, in several groups:

  | Group | Criterion |
  |---|---|
  | A round trip | Writes random data into each level and layer and reads it back out; must be byte-for-byte equal. Covers 2D, 2D array and 3D, several bytes-per-block values and non-power-of-two sizes |
  | B address formula | The result of `nvn_bl_offset()` equals the address computed by hand from the public layout rules |
  | D differential copies | Both copy directions match a byte-address oracle for 1,029 deterministic layouts, including padded pitches, byte tails, sparse mip tails, packaged block dimensions, arrays and cubes; untouched storage and row padding are compared too |
  | E hand-made 3D | Places markers at hand-computed addresses, checks where they land after deswizzling, and checks block height, block depth and storage size |

  The A round trip can only prove "one-to-one and within bounds"; it cannot prove that the layout formula is right. What backs it up is the hand-computed addresses of groups B and E, and the games that render right.

The CMake build ([dm/host/CMakeLists.txt](../../host/CMakeLists.txt), Linux and Windows) also runs:

- **`t_fmt_vk`**: the Vulkan format table, every row named, with a path (none without one since step P7), its sizes, class, flags and conversion format consistent with the format's name; it prints the rows by path.
- **`t_astc`**: the ASTC decoder over blocks the test writes itself (`tests/astc_synth.h`, which also writes the GPU test's blocks) and predicts: the unquantization tables against the specification's values, every trit and quint packing, the integer sequence encoding of every range and length, every block mode of every block size, void-extent and illegal blocks, each LDR colour endpoint mode (blue contraction and clamping included), dual planes, partitions, weight infill, an HDR partition in an LDR block, and 5,600 random blocks decoded texel by texel and block by block.
- **`t_astc_mesa`** (CTest, where EGL and OpenGL are installed: the `nxnative-build` image): the decoder against Mesa's own, through OpenGL on llvmpipe, on 256 synthetic blocks of every kind for each of the 28 formats: 426,496 texels, all equal. It exits 77 (skipped) without a GL context or the ASTC extension.

## Limitations

- `nvn_deswizzle()` does not support multisampled textures, textures with the LINEAR flag (which are linear to begin with) or the BUFFER target; it returns -1 when it encounters them.
- Cubes and cube arrays are handled as layered textures.
- The component bit order of the 16-bit packed formats (565 / 5551 / 4444) is mapped according to the format names and has not been checked with real data; they are marked as "substitute" in the format table.
