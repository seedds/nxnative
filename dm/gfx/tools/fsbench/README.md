# fsbench: one translated pixel shader, timed alone

fsbench times a translated fragment shader and variants of its source, one at a time, on a single full-screen draw. It reports the time of each, checks each variant's output against the first, and prints Apple's compiler statistics. It runs offline, with no game: a scene file describes the draw.

Use it to find what a translation choice costs in one hot shader before changing the chain. Then confirm the result in game with `DM_NVN_SHADER_ALT` and `DM_AB` ([GPU measurement](../../../../docs/development.md#gpu-measurement)).

## Commands

```bash
python3 dm/gfx/tools/fsbench/fsbench.py tool [--bin <path>]                 # build the fsbench binary (default: beside this file; FSBENCH_BIN)
python3 dm/gfx/tools/fsbench/fsbench.py fetch <image> <key> <dir>          # <key>_b.metal / .json / .metallib from a build's shader image
python3 dm/gfx/tools/fsbench/fsbench.py scene --log game.log --frame F --pass P --key K --json K_b.json --msl K_b.metal --cb DIR [--draw N] [--fill rich|zero] [--lights N] --out scene.json
python3 dm/gfx/tools/fsbench/fsbench.py variants <base.metal> <dir> nobs nonan nobs+nonan …   # source variants (VARIANTS in fsbench.py), "+" combines
python3 dm/gfx/tools/fsbench/fsbench.py compile <in.metal> <out.metallib>  # the chain's Metal flags (dmsh_build.MAC_METAL)
fsbench stats <lib.metallib> [fn] [colour MTLPixelFormat]                  # registers, spill, instruction mix of the fragment stage
fsbench run <scene.json> <out dir> <lib.metallib>…                         # time of each library; output of each against the first
```

`run` prints one `output` line for the first library and a `diff` line for each of the others: bit-exact, or the share of pixels that differ and by how much. It ends with a `time` table: the median ms of a pass with its IQR, the net ms after subtracting a null shader, and the change against the first library. The output images and float dumps go to the out dir.

Environment:
- `FSBENCH_R` sets the rounds (default 40) and `FSBENCH_P` the passes in a command buffer (default 20).
- `FSBENCH_CAPTURE=<path>.gputrace` writes a capture of one draw per library, which Xcode's shader profiler can replay (needs `MTL_CAPTURE_ENABLED=1`).
- `FSBENCH_PERFSTATE=<n>` asks for a GPU performance state; this needs an entitlement that a tool usually lacks.
- `FSBENCH_REMARKS=1` prints the compiler's remarks with `stats`.

## Inputs from a run

A scene needs one dumped draw of the shader:
- `DM_NVN_PASSDUMP=<frame>` with `DM_NVN_PASSDUMP_ALL=1`, which records the draw's `K`, `B` and `d` lines and its pass's `P` lines.
- `DM_NVN_CBDUMP=<dir>`, which saves its uniform blocks. `DM_NVN_CBDUMP_KEY=<key>` limits the dump to that shader.

A block that was not dumped falls back to the same slot of another dumped draw of the pass, or failing that of the frame, and `scene` says which file it used. Game data (logs, dumps, scenes, shader sources) stays in the game's work folder, never in this repository.

## The scene file

`scene.json` is written by `fsbench.py scene`. It can also be written by hand.

| field | meaning |
|---|---|
| `key`, `frame`, `pass`, `draw` | where the draw came from (information only) |
| `w`, `h` | the render target size |
| `color` | the colour attachments' `MTLPixelFormat` values |
| `msl`, `fn` | the shader source and its fragment entry point (the base library's own) |
| `fill` | `rich` (textures and synthetic buffers filled so that optional paths run) or `zero` |
| `lights` | the light count a cluster cell gets in a synthetic light grid (`rich`) |
| `vary` | optional: how each interpolant varies over the screen (the generated vertex function makes them linear in the screen position) |
| `args` | one entry for each argument-buffer slot, by its `id` |

Each `args` entry has an `id` (the `[[id(n)]]` of `spvDescriptorSet0`) and a `kind`:

| kind | fields |
|---|---|
| `buffer` | `size`, and either `file` (raw bytes, such as a uniform dump) or `hex` |
| `tex` | `type` (`2d`, `2da`, `cube`, `3d`; taken from the shader's declaration), `fmt` (`MTLPixelFormat`), `w`, `h`, `d` (depth or layers), `mips`. The contents are random: finite values below 1 for float formats, random bits otherwise |
| `samp` | `min`, `mag` (linear or not), `mip` (0 none, 1 nearest, 2 linear), `wrap` (three of: 0 repeat, 1 mirrored repeat, 2 clamp to edge, 3 clamp to border, 4 mirror clamp to edge), `aniso`, `lodmin`, `lodmax`, `cmp` (optional: an `MTLCompareFunction` for `sample_compare` / `gather_compare`) |
| `texpool`, `samppool` | `n` entries of one `tex` or `samp`, laid out as the host's bindless pools (`{uint n; ulong seen; T e[n]}`) |

## Reading the numbers

- Time comes from the GPU's own clock, after warm-up rounds. Compare variants within one `run`, where they share the scene and the clock, rather than across runs.
- The ratio between variants carries over to the game better than the absolute time. A scene's random textures can send a shader down a cheaper path than the game's: one that discards or returns early on sky depth runs much faster on random depth.
- A variant that is not bit-exact changes behaviour. Read its `diff` line before taking its time.
