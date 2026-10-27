# Restricted software MIN reduction on iOS 16

The audited NVN header defines AVERAGE as 0, MIN as 1 and MAX as 2. The captured
depth-pyramid sampler has reduction 1, min filter 3 (linear, nearest mip), mag
filter 1 (linear), wrap 7/7/7 (clamp to edge), LOD 0..1000, bias 0, anisotropy 1
and comparison disabled. An averaged pyramid cannot substitute for that MIN
reduction. A device readback demonstrated a mip-7 parent greater than a child in
mip 6, including a zero child: this violates the minimum reduction needed here.

`export_min_reduction.py` exports two companions from exact audited originals in
chain `43806fc88b5e`. It changes only each source's unique ordinary slot-17
explicit-LOD sample expression and inserts a self-contained helper after the
Metal namespace declaration. Entry `main0`, resource bindings and all other
source bytes are preserved. The original sources, reflection and compiled
libraries are never replaced. Output contains game-derived MSL and must remain
with the user's game inputs, outside source control.

The FS (`151607c7a92386cc_b`) obtains its explicit LOD from an integer. The CS
(`ec5ebd6d9902a9a1_b`) uses `max(0, ceil(log2(extent + epsilon)) - 1)`, also an
integer. Runtime must check the exact sampler contract, a 2D depth view and the
source/library hashes before selecting either companion. A different sampler or
shader keeps the original function; source hashes are not permission to apply
this algorithm to unrelated bindings.

At the selected view-relative mip, the helper computes `p = uv * size - 0.5`,
then takes the floor and fraction. Signed neighboring texel coordinates are
clamped before conversion to unsigned integers. At most four integer reads are
reduced component by component. A neighboring row/column whose interpolation
weight is zero is excluded: unconditional four-tap MIN incorrectly expands the
footprint at exact texel centers. Normalized UV is clamped before conversion to
handle outside or infinite coordinates safely; existing NaN-to-zero behavior is
retained. Dimensions and mip count come from the texture view rather than fixed
1920x1080 constants, including odd and one-texel mip dimensions.

The standalone LOD prototype implements GL's nearest-mip half-tie convention,
`ceil(lod + 0.5) - 1`, after sampler clamps and before view-range clamps. The two
audited shaders have integer LODs, so fractional NVN tie behavior does not enter
this path. Fractional LOD and hardware interpolation-weight quantization need
their own NVN validation before broadening it. Nearest spatial filtering,
linear-between-mip filtering, anisotropy, implicit derivatives, repeat/border
addressing and comparison sampling are outside this companion contract. For
comparison sampling, each depth comparison must precede reduction; reducing raw
depth and then comparing is not generally equivalent.

Each `<key>.json` record binds schema, chain, kind, key, stage, entry, slot,
sample count, integer-explicit-LOD and 2D metadata to these hashes:

- `original_source_sha256`: pinned audited source bytes.
- `library_sha256`: the original bundled compiled library bytes.
- `source_sha256`: the complete self-contained rewritten companion bytes.
- `helper_sha256`: UTF-8 bytes strictly between the helper begin/end marker
  lines, excluding both markers. `helper_file` is `embedded`.

The original `.metal` need not be bundled when runtime checks its manifest SHA
against the audited constant. Runtime still checks actual original library and
rewritten source bytes, the metadata and actual bound sampler. A companion's
selected function must participate in both render and compute pipeline cache
identity; a pipeline made for the original function cannot be reused for it.

Export to a new directory, then package its four files in
`<app shader chain directory>/min-reduction-v1/`:

```sh
python3 dm/ios/tools/export_min_reduction.py \
  '<shader root>/cache/b-43806fc88b5e' \
  '<shader root>/ios/b-43806fc88b5e' \
  '<shader root>/min-reduction-v1/b-43806fc88b5e'
```

The exporter validates both originals before writing; it rejects changed source,
reflection, chain, an overlapping input/output directory, repository output or a
differing existing companion. An identical export may be rerun.

Synthetic checks, containing no game assets:

```sh
python3 -m unittest dm/tools/tests/test_ios_min_reduction_export.py -v
clang -O1 -Wall -Wextra -Werror -fsanitize=undefined,float-cast-overflow \
  dm/gfx/tests/t_reduction_footprint.c -o /tmp/t_reduction_footprint
/tmp/t_reduction_footprint
xcrun --sdk iphoneos metal -std=ios-metal2.4 -c \
  dm/gfx/tests/reduction_sample_fixture.metal -o /tmp/reduction_sample_fixture.air
```

CPU checks cover exact centers with smaller unused neighbors, one-axis zero
weights, MIN/MAX, four corners and the middle, outside/Inf/NaN coordinates, one
texel, odd dimensions, all 11 observed integer mip levels and clamp boundaries.
The synthetic Metal kernel supports GPU readback with the same queries. CPU and
compiler checks alone do not establish device GPU results; the complete
companion-path device verification is recorded below.

Primary references: NVIDIA's [EXT_texture_filter_minmax specification](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_filter_minmax.txt)
defines nonzero-weight participation and comparison order; Khronos's
[GLSL texture-query algorithm](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html)
defines the nearest-mip half-tie convention. These establish the portable model;
the audited local NVN enum and captured binding establish which model is needed.

## Device verification (2026-10-25)

On iPadOS 16.1 / M2, both companions compiled on the device and both actual
variant pipelines were created successfully. At the same stationary saved
alley camera, the large black silhouette became the textured bridge, wall and
plants. Pixel readback confirmed that the formerly black point gained albedo,
normal and material data in the main G-buffer; its final RGB changed from
approximately (0.004, 0.008, 0.016) to (0.443, 0.439, 0.424). Across twelve
verifiable 2x2 parent/child comparisons in the depth pyramid, violations fell
from four to zero. Indirect arguments remained identical to the captured raw
GPU results, with no forced visibility or conditional-render override.

This comparison changes both the reduction algorithm and the device compiler
path. The old precompiled fragment library also produced a depth-attachment
pipeline error, despite its iOS 16.1 target and a valid depth/stencil format.
A separate unchanged-source runtime compile has not isolated the contribution
of that binary compatibility issue. The demonstrated result applies to the
complete companion path, not to MIN emulation alone. Warm switching off does
not necessarily restore the defect in a stationary view: valid old pyramid
contents can remain when an ordinary mip pipeline fails. A cold launch is
needed for a meaningful ordinary-path baseline.

A subsequent short run with both Metal API and GPU validation enabled reached
1,500 total frames, including about 300 frames of the restored field scene,
without reported GPU memory errors. This establishes neither exhaustive scene
coverage nor long-term stability; later scenes and performance still require
testing. Validation was then disabled and the companion opt-in persisted in
the device's ordinary startup environment.
