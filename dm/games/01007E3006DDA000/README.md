# Kirby Star Allies (`01007E3006DDA000`)

Update 4.0.0a (v393216) over the base game, SDK 4.4.1. HAL Laboratory's own engine on NintendoWare (G3d, Font, Ui2d, Vfx) with `nn::atk` sound. Modules `main`, `subsdk0` (NintendoSDK_movie 4.4.1) and `sdk`. Plays on the Mac: the opening movie, the title, the update notices, the movie after the first file start, the World of Peace map and the first part of Green Gardens, with a copy ability prompt and a friend made with a Friend Heart. iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB block: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | no system resource; 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk0` is the movie library: the game demuxes MP4 with it and the host decodes |
| `shader_groups /gfx/=gfx /ptcl/=ptcl /lyt/=lyt /shader/=shader` | `game.tsv` | the shaders are BNSH archives in HAL's `.cmp` containers |

No code of the game is cut, and no run-time key is set. The game asks for present interval 1.

## Memory

- The NPDM declares no system resource, so there is no virtual address memory: the game sets a heap of the pool less the used memory, 3.14 GB (`[mem] heap 3.14 GB (heap region 3.25 GB)`).
- **Current:** about 4.0 GB resident on the Mac in Green Gardens with the pipeline archive on the internal disk, 5.1 GB with it on an external drive, which reads its 1.13 GB into memory ([A pipeline archive on an external drive is read into memory](../../../docs/reference/memory.md#a-pipeline-archive-on-an-external-drive-is-read-into-memory)). Nearly flat over about 15 minutes on the external drive (5,070 → 5,125 MB). `footprint` with the archive on the internal disk: 3.15 GB untagged, the guest memory (all of the 3.14 GB heap, already at the title; Kirby and the Forgotten Land on the same host touches 2.6 GB of its heap, so this looks like the game's doing, not traced to an instruction); 0.66 GB graphics; 0.15 GB small mallocs. How much a phone allows is open.

## Files and shaders

- RomFS: 5,961 files, 3.76 GB. 3,056 `.cmp` containers, 258 `.bfstm` streams, 2 MP4 movies (`mov/Product/Opening.mp4`, `ImageMovie.mp4`).
- 31,984 shaders in the map (20,493 pixel, 11,420 vertex, 71 compute): 22,513 in `gfx/`, 9,058 in `ptcl/`, 277 in `lyt/`, 84 in the loose `shader/` files and 52 in `main`. All built for the Mac without a failure, and no run met a shader missing from the map.

## Rendering

- Deferred. The G-buffer is RGBA16F (normals), RGBA8 sRGB (albedo), RGBA8Uint (material bits) and RG11B10F; the lighting pass reads the material bits to choose how a pixel is lit.
- **Resolved: grass, ground and trees dark, and most of the World of Peace map black.** The integer target stayed zero for 687 of the G-buffer's pixel shaders ([Integer outputs told by the pipeline's targets](../../../docs/reference/shaders.md#integer-outputs-told-by-the-pipelines-targets)).
- **Current, harmless:** `[RED] stage 0 cb1 has no uniform buffer bound`, once per run, first seen as the movie after the first file start begins ([An unbound uniform slot reads the zero bank](../../../docs/reference/rendering.md#an-unbound-uniform-slot-reads-the-zero-bank)). Which shader it is was not looked at; nothing visibly wrong goes with it.

## Movies

- Both are 1920×1080 H.264 at 30 fps with AAC sound, BT.709 limited range. The player copies each picture's planes into two textures and converts them in a pixel shader.
- **Resolved: both movies played black.** The conversion matrix was all zeros, because the host's output format did not set `nv12-colorspace` ([`nv12-colorspace` picks the game's YUV matrix](../../../docs/reference/movies.md#nv12-colorspace-picks-the-games-yuv-matrix)). The player and its matrices are at `main+0x1529d8` and `main+0x3b99f0`.

## Frame rate

- **Current:** 30 fps in the stage (frame time p50 33.3 ms, p95 33.6 ms; `power` line: 30.0 fps, CPU 19% of a core and about 1 W), 60 fps on the title. Whether the stages' 30 fps is the game's own was not checked against a Switch; the host presents at interval 1 as asked, and GPU time is about 5 ms a frame on an Apple M6, so the Mac is not what holds it there.

## Audio

- `nn::atk` through an audio renderer (96 voices, 14 effects, one sink), with the SDK 4 command layout. 48,808 of the first 60,000 rendered frames were not silent. Heard output was not checked: the runs used `DM_AUDIO=0`.

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`; its last line runs the game.
- A fresh save: the opening movie plays first (46 s), then the title. `A`, `A` (File 1); the free update notices and the Dream Friends pages take about a minute of `A` presses, 1.5 s apart; then the second movie (84 s) and the map.
- With a save past that, the title is up about 15 s after boot. `A` (wait 7 s), `A` (wait 12 s), four `A` presses, `A` closes the world card, `LRight` for 1.5 s reaches Green Gardens, `A` enters it. In the stage `A` jumps and floats, `B` attacks and inhales, `X` throws a Friend Heart.
- The tutorial stops at a door (up to enter) and later waits for `X` before it goes on.

## Testing

- 2026-11-04, Mac: boot from an empty save and from a saved file, the two movies, the title, file select, the update notices, the map and the first rooms of Green Gardens: walking, jumping, floating, attacking, inhaling, the door, the Friend Heart, with a CPU partner following. A soak of about 6 minutes spent most of its time with Kirby held against a wall in Green Gardens, so it shows no crash and flat memory, not much more ground.

## Open

- No stage was finished, so no boss, no stage clear, no Dream Friend, no co-op, no sub-game and no save at a stage's end.
- iOS: no IPA was built or tested; memory may be the first limit there, at 3.15 GB of guest memory before any of the port's own ([Memory](#memory)).
- No `smoke.tsv` yet, so `scripts/regress.py` does not run this game.

## Found on this game

- [`nv12-colorspace` picks the game's YUV matrix](../../../docs/reference/movies.md#nv12-colorspace-picks-the-games-yuv-matrix).
- [H.264 packets are Annex B, not NAL lengths](../../../docs/reference/movies.md#h264-packets-are-annex-b-not-nal-lengths): its `ImageMovie.mp4` has 167 samples whose first NAL length begins `00 00 01`, and plays in full, so its packets come as Annex B.
- [Integer outputs told by the pipeline's targets](../../../docs/reference/shaders.md#integer-outputs-told-by-the-pipelines-targets).
- [A pipeline archive on an external drive is read into memory](../../../docs/reference/memory.md#a-pipeline-archive-on-an-external-drive-is-read-into-memory).
