# The Legend of Zelda: Ocarina of Time (Switch 2) on the Mac

What it takes to play the Switch 2 Ocarina of Time (0400486024DEC000) on the Mac with the fixes from this branch, as of 2026-11-05. The game's profile comes with #1299; once that lands, this page belongs in the game's note, `dm/games/0400486024DEC000/README.md`.

## What you need

- **Your own dump of the game:** The Legend of Zelda: Ocarina of Time (Switch 2, title ID `0400486024DEC000`), base game plus update v131072, as one `.nup` file or the same files unpacked into a folder (`base/exefs`, `base/romfs`, `updates/131072/...`; [Switch 2 games](../../../README.md#switch-2-games)). No `prod.keys`: Switch 2 games are built from the NUP container or its files, and neither needs keys. Nothing from the game is in this repository or in these pull requests.
- **A Mac with Apple silicon** and the project's toolchain: macOS 26.6 or later, Xcode 27.2 (its Metal compiler builds the shaders), Homebrew and an arm64 Python 3.12 or newer ([README](../../../README.md)). Tested on an M5 Pro with 24 GB.
- **Memory:** the game's footprint is about 10.4-10.8 GB in gameplay (peaks ~11.1 GB), with up to ~10.5 GB of GPU allocation; 6-7 GB of it is wired at gameplay entry. **A 24 GB Mac is tight**: close other large apps and builds; the margin above memory-pressure level 2 was 0.3-0.7 GB and depended on what else ran. 32 GB or more is comfortable. The iPhone and iPad are not covered here.

## Pull requests it needs first

At the time of writing none of these are on `main`; the order is the order to apply them in.

| What | Pull request | Needed for |
|---|---|---|
| The game's profile (render_cond, memory layout) and note | #1299 (or #1387, another profile) | anything |
| SDK 22 calls the game makes (PlayReport and others) | #1434 (and the profile PR's own service fixes) | getting past the title |
| Ampere F2IP and a task shader's payload read | #1300 | untranslated shaders |
| Live shader inputs kept by dead-code removal (also fixes the NDVE network kernel: a two-result producer's sources were dropped) | #1343 | foliage jitter, movie network |
| Float add atomics kept | #1359 | outdoor scenes washed out |
| The raw global memory pool table grows with the pools | #1360 (or an 8,192-entry table) | GPU hangs in foliage task shaders |
| Indirect task / mesh draws (foliage) | the PR that adds DrawMeshTasksIndirect (not on `main`; check #1247, #1312, #1368) | grass and plants |
| LDSM fenced by subgroup barriers | #1376 | movie streaks (not needed with `DM_NDVE=metalfx`) |
| Shared raw-memory lookups ("raw-load quads") | #1405 | GPU cost; the fast lookup below builds on it |
| This branch: GPU-written ranges, texture release | `oot-host-perf-memory` | ground holes; memory |
| Follow-ups of this work: fast raw lookup, incremental pool table and hitch fixes, depth families and stale-ID skip, probe depth snapshots, foliage guards, MetalFX for NDSR / NDVE | to be opened | performance, memory, movies |

## Building

The NSP script does every step for a NUP or a folder (no keys):

```sh
python3 scripts/build_ipa_from_nsp.py <your .nup or folder> --platform mac-app
```

With the fast raw lookup (follow-up, on top of #1405) the shader chain must be generated and built with both options set, and the host built after it, so that its chain version matches the libraries:

```sh
DMSH_RAW_LOAD_QUADS=1 DMSH_RAW_FAST_LOOKUP=1 python3 dm/gfx/tools/dmsh_chain.py --header
DMSH_RAW_LOAD_QUADS=1 DMSH_RAW_FAST_LOOKUP=1 DM_GAME=0400486024DEC000 python3 dm/gfx/tools/dmsh_build.py <work>/shader/pkg <work>/build/cache/metal_shader_cache
make -C dm/host GAME=0400486024DEC000 dmhost
```

The steps by hand (rewriter, shader split, shader build, host) are in [Build by hand (Mac)](../../../docs/development.md#build-by-hand-mac). The full shader build is ~42,850 shaders and took ~40 minutes on the M5 Pro.

## Settings

Set these as environment switches (`--env-file`, a front end's per-game environment, or the shell). Ones marked *default* are on in the code once their pull request is in; the others should become defaults of this game's profile (see below).

| Switch | Value | What it does |
|---|---|---|
| `DM_TEX_RELEASE` | `2` | Gives back the guest copy of a texture's storage once the GPU has it (`CPU_NO_ACCESS` pools, and still-protected pages of CPU-visible uploads): ~2.3 GB less footprint. This branch. |
| `DM_TEX_REL_DONOR` | default `1` | A texture registered again over released storage is a GPU copy of the one released there (Link's hat embroidery was black without it). This branch. |
| `DM_NVN_GPUW_RANGE` | default `1` | GPU writes are tracked by storage range, so the CPU-written terrain heightmaps keep their heights (no holes at the pond). This branch. |
| `DM_TEX_LAZY` | `2` | A pool walk makes no host texture for IDs in `CPU_NO_ACCESS` pools (render targets are made when attached or copied). Not `1`: textures only sampled through the bindless pool then read the dummy (black rocks, flat fog and water). Follow-up. |
| `DM_GMEM_KEEP` | `new` | When the raw global memory pool table overflows, the partial tier keeps its newest pools (the current area's foliage data). Not needed once the table grows with the pools (#1360). Follow-up. |
| `DM_NVN_MESH_SKIP` | `none` | Draws every task / mesh draw (the profile in some trees sets `indirect` as a stopgap). Safe with the guards: |
| `DM_NVN_MESH_GUARD` | default `1` | Skips indirect task / mesh draws while the pool table leaves pools out, and for the rest of the run after a command buffer error (one GPU hang, not a series). Follow-up. |
| `DM_GMEM_PTRSCAN` | default `1` | Pools that storage-buffer pointers of raw-memory shaders point into get the table's first tier. Follow-up. |
| `DM_POOL_INCR` | default `1` | The bindless texture pool table is patched for the IDs that changed instead of rebuilt (0.47 → 0.056 ms a build, ~15 builds a frame). Follow-up. |
| `DM_ALIAS_NOTE`, `DM_ALIAS_BATCH`, `DM_POOL_RECYCLE` | default `1` | Storage take-overs no longer start a new texture-ID epoch; one pass over the serial table per take-over; pool table buffers reused. Follow-up. |
| `DM_VC_RETIRED` | default `12288` | Views of finalized textures the game still samples are cached (was ~3,000 new views a frame). Follow-up; needs finalized textures held by pool descriptors to stay in the serial table. |
| `DM_DEPTH_FAMILY`, `DM_POOL_SKIP_STALE` | default `1` | The dynamic-size shadow-map depth targets share one placement heap (Depth32 in residency 1.5 → 0.56 GB); stale retired IDs over storage newer textures took get the dummy (300-800 MB of uploads avoided). Follow-up. |
| `DM_NVN_FB_DPOOL`, `DM_NVN_FB_DSNAP` | default `2`, `1` | Light-probe faces read a depth snapshot instead of ending the pass at each depth write (passes 239 → 117 a frame on probe frames). Follow-up; overlaps #1430. |
| `DM_NVN_IXC_PAST` | default `1` | The indirect-draw conversion looks past uniform updates and copies, so the foliage no longer splits the scene pass ~58 times a frame. Follow-up. |
| `DM_NDSR` | default `metalfx` | The game's four-dispatch NDSR upscale (1536x864 → 2048x1152) is replaced by MetalFX's spatial scaler: no dotted streaks in the fog, ~0.8 ms less GPU. Follow-up. |
| `DM_NDVE` | default `metalfx` | Movies: the colour conversion runs, the game's NDVE enhancement network is skipped and its upscale is MetalFX: 26 → 60 fps. Follow-up. |
| `DM_PSO_PREBUILD` | `0` | No pipeline prebuild at start (long-frame classification found no pipeline-compile hitches that it would remove once the archive has learnt them). |

## Known issues

- **First-use pipeline compiles:** a shader pipeline first met in an area pauses 80 ms to 2.3 s; the pipeline archive learns it, and the next run does not pause there.
- **Area entry:** gameplay entry and new areas upload ~1 GB in a burst; ~10 frames of 40-120 ms follow.
- **Movies with `DM_NDVE=metalfx`:** the game's video enhancement network is skipped, so movies are slightly sharper with faint edge ringing and less denoising than on the console; `DM_NDVE=game` runs the network at ~26 fps.
- **No frame-time scaling:** the game's time step is fixed per frame, so below 60 fps it runs in slow motion. A patch that scales the time step by the real frame time is out of scope and not included.
- **Memory on 24 GB Macs:** see above; runs were stopped by a memory watchdog when the Mac had less than ~14 GB available before starting.
- **Not checked:** Lost Woods, Hyrule Field and later areas; the iPhone and iPad.

## Expected performance (M5 Pro, 24 GB, Kokiri Forest, 260 s walking runs)

| | Frame time p50 / p95 | GPU p50 / p95 | Notes |
|---|---|---|---|
| Before the view cache (house interior) | 38 / 51 ms | 14 ms | CPU-bound in texture-view creation |
| Kokiri Forest, before the probe snapshots and the fast lookup | 20.5-22.3 / 27.7 ms | 20.1-22.1 / 27.4 ms | |
| Kokiri Forest, all changes | 16.8-16.9 / 22.2 ms | 15.5-16.2 / 17.3-18.0 ms | 122-135 render passes a frame |
| Frames over 20 / 25 / 33 / 50 ms per ~4,500 frames | 665 / 105 / 37 / 20 → 111 / 5 / 2 / 1 | | hitch fixes (p99 29.4 → 21.3 ms) |
| Title movies | 37.6-38.4 ms (26 fps) → 16.66 ms (60 fps) | 38 → 3.5-4.1 ms | `DM_NDVE=metalfx` |

Memory in the same runs: footprint median 10.4-10.8 GB, wired growth ~6.6 GB, minimum available 3.3-3.75 GB on a Mac that started with ~14.5 GB available.

## Profile defaults to add

These are environment switches today because no profile key exists for them; for this game they should come from the profile (new keys in `game.tsv`, or rows in `dm/games/settings.tsv` with this game's defaults), so that a plain build plays as tested:

- `tex_release` = `2` (`DM_TEX_RELEASE`);
- `tex_lazy` = `cpu_no_access` (`DM_TEX_LAZY=2`);
- `gmem_keep` = `new` (`DM_GMEM_KEEP`), until #1360 or a larger table makes it unnecessary;
- the NDSR and NDVE shader keys that `DM_NDSR` / `DM_NDVE` recognise (today host arrays) as profile tables, like `DM_GAME_UPSCALER_KEYS`;
- `DM_NVN_MESH_SKIP=none` as the profile's state once the foliage guards and #1360 are in (drop any `indirect` stopgap);
- `DM_PSO_PREBUILD=0` only if the maintainer agrees (it is a player setting).
