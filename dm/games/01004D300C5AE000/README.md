# Kirby and the Forgotten Land (`01004D300C5AE000`)

Update 1.1.0 (v65536) over the base game, SDK 11.4.0. HAL Laboratory's own engine on NintendoWare (G3d, Font, Fxt) with `nn::atk2` sound and NEX online. Modules `main`, `subsdk0` (NintendoSDK_movie 11.4.0) and `sdk`; no other profile ships these `sdk` and `subsdk0` builds. Plays on the Mac at 30 fps, as on the Switch: the online and difficulty prompts, the 1080p opening movie, the beach and the walk through Point of Arrival with its guide movies, the reveal of the city, inhaling a Blade Knight for the Sword ability, fights with Awoofies and Bronto Burts, the title screen and continuing a file. An unsigned IPA is built; iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB block: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | no system resource; 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region. See [Memory](#memory) |
| `modules main subsdk0 sdk` | `game.tsv` | `subsdk0` is the movie library: the opening and ending movies and the guide movies |
| `shader_groups /gfx/=gfx /ptcl/=ptcl /lyt/=lyt /shader/=shader` | `game.tsv` | the shaders are BNSH archives in HAL's `.cmp` containers; see [Files and shaders](#files-and-shaders) |
| Node area cut and 2 MB alignment of large regions (SDK 20's) | `dm/tools/sdk_patches.py` SDK-wide rules, `sdk` build `b39add63…` | [Pokémon Legends: Arceus](../01001F5010DFA000/README.md)'s build, cut by the SDK-wide rules; this game has no virtual address memory and never reaches them |

No code of the game is cut, and no run-time key is set. The game asks for present interval 1 and paces itself at 30 fps.

## Memory

- The NPDM declares no system resource, so there is no virtual address memory: the game sets a heap of the pool less the used memory, 3.14 GB (`[mem] heap 3.14 GB (heap region 3.25 GB)`), which the 3.25 GB heap region holds.
- **Current:** 4.1 GB resident and 2.67 GB of Metal allocations on the Mac after 20 minutes in Point of Arrival (`footprint`: 2.6 GB untagged, the guest memory the game touches; 0.7 GB graphics; 0.6 GB large host mallocs). It grew with each new area (3.2 GB at the beach, 3.97 GB at the city) and then stayed nearly flat, 4.13 → 4.17 GB with Metal allocations steady at 2.67 GB, over a 15-minute soak in one area. The highest resident figure of the games so far; how much a phone allows is open.

## Files and shaders

- RomFS: 9,386 files, 5.72 GB. 7,201 `.cmp` and 430 `.cmpbin` containers (HAL's size-prefixed zstd and LZ4 frames), 52 `.bfstm` and 104 `.opus` streams, 36 WebM movies (`mov/EventMovie`, `mov/GuideMovie`).
- 27,297 shaders in the map (18,460 pixel, 8,755 vertex, 72 compute, 10 geometry): 18,476 in `gfx/`, 8,313 in `ptcl/` (70 of them compute), 387 in `lyt/`, 67 in the loose `shader/` files and 54 in BNSH archives embedded in `main`. Before the split opened the `.cmp` containers it found 121 ([HAL's `.cmp` containers](../../../docs/reference/shaders.md#hals-cmp-containers-a-size-prefixed-frame)). All of them built for the Mac without a failure, and no run met a shader missing from the map (`shader keys not in the shader map … 0`).

## Rendering

- **Current, harmless:** `[RED] texture type mismatch: shader … declares slot 1 / 2 as Metal type 2, texture ID … is type 7 (NVN target 2, format 0x38 …); bound as is`, once per shader, for eight UI layout pixel shaders so far (`lyt/TitleMenu`, `lyt/Resident/TitleLogo`, `lyt/Cmn/HeroPanel`, `lyt/Step/ScopeCine`, `lyt/Arena/Entry`). The texture is a 16×16×16 RGBA8 sRGB 3D texture, one of the colour-grading lookup tables (`texlist 2`), left in slots the layout material does not use: the layout shader samples slots 1 and 2 only when its uniform `c3[0].y` is non-zero.

- **Fixed on the Mac, iPhone retest open (2026-11-16): most of the scene missing on an iPhone** ([#2760](https://github.com/seedds/nxnative/issues/2760), iPhone19,4, 1.0.0 base): `failed to create render pipeline … output of type float4 is not compatible with a MTLPixelFormatR8Uint color attachment` for 76 fragment shaders in Waddle Dee Town (`dfcaf654d8910f1c`, Kirby's Cone model, and `9df52ef750e8bd77`, a town house, among them), each writing colour 5 into an R8Uint target beside float ones; black sky, no characters. With `DM_PSO_MTL4=1` the pipelines built from Metal 4 bases and the scene looked right for 20 minutes, but colour 5 took undefined values. None of the 76 had the integer entry point the build gave by its code; every pixel shader has the typed-output entry point now ([Typed outputs for every pixel shader](../../../docs/reference/shaders.md#typed-outputs-for-every-pixel-shader)). The Mac draws through Metal 4 bases by default, which hid it: Point of Arrival has such shaders too. Checked 2026-11-16 on an M4 Pro, 1.1.0: all 15,552 pixel shaders with a colour output have `main0_u` (the other 2,908 write depth only); `dfcaf654d8910f1c` and `9df52ef750e8bd77` with the reporter's pipeline (colour 5 R8Uint) through the `MTLDevice`: `main0` is refused with the reporter's message, `main0_u` builds; the beach and Point of Arrival with `DM_PSO_MTL4=0`, then with Metal 4 bases: 30 fps, the scene whole, over 128 shaders taking `main0_u` for an unsigned colour 5 (`typed outputs: side library … loaded for target classes 0x20`), no red line but the layout shaders' [texture type mismatches](#rendering). Waddle Dee Town is past the Mac's tested route; the iPhone is to be checked by the reporter.

## Movies

- The SDK's demuxer (libwebm in `subsdk0`) runs natively; the host decodes VP9 with libvpx and Vorbis with stb_vorbis into the game's CPU buffers (NV12, mode 1). The opening movie is 1920×1080 VP9 with a Vorbis track (2,297 frames); the guide movies that show a move in a corner of the screen are 480×270 VP9 without sound.
- **Resolved: the game froze at the first guide movie** ("A Jump", on the first walk from the beach, every time). A job worker (priority 46) returning an output buffer signalled the movie's events while the host decoder held its decode lock; the signal handed core 0 to the movie threads (priority 44), and the input thread among them was waiting for that lock in host code, which the handoff counted as running ([A host lock held across a same-core handoff](../../../docs/reference/threads.md#a-host-lock-held-across-a-same-core-handoff)).

## Frame rate

- **Current:** 30 fps on the Mac, as on the Switch (`power` lines: 28–30 fps; frame interval p99 33.6 ms in play). The game sets present interval 1 and paces its frames itself. CPU about one core and 5–5.6 W on an Apple M6 in Point of Arrival.

## Audio

- `nn::atk2` through an audio renderer (96 voices, 14 effects, one sink). 131,543 of the first 132,000 rendered frames were not silent.
- **Current, open: with the silent sink the game often crashes right after Continue.** On the Mac's default output (no `DM_AUDIO`, or `DM_AUDIO=0` as `regress.py` runs it), `A` on the title's Continue crashed 7 boots of 11 within 4 s, during the load: `CRASH sig=11 addr=0 pc=main+0x6da824 lr=main+0x6dabf8`, on a job worker (priority 46, backtrace `main+0x6dabf8 main+0x803cd4 main+0x6a69e4 main+0x6d9e80 main+0x78a5b8`), with Continue pressed at 20 s or 26 s, an empty or a full pipeline archive, this host or the one the game was brought up with. With `DM_AUDIO=1` (Core Audio output) 3 boots of 3 reached the beach. The cause is not known: what the null pointer is and why the silent sink's pacing exposes it (2026-10-27).

## Mac routes

- Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac`; its last line runs the game.
- A fresh save, through `DM_CTL`: the online prompt is up about 20 s after boot: `Left`, `A` (Do Not Use), `A`; the difficulty: `Right`, `A` (Spring-Breeze Mode). The game saves here, then plays the opening movie and the beach scene (about 75 s) and gives control on the beach.
- With that save, the title shows Continue about 25 s after boot; `A` puts Kirby on the beach. Forward (`LUp`) leads to the first guide movie in about 15 s; the path then turns right past a rock, crosses a ravine under a plank (float up on the right with `A`) and runs forward between trees to the city reveal, then right past the building's base to the first Blade Knight.
- A `DM_CTL` press replaces the one being held: send the next key only after the hold ends.
- `smoke.tsv` (for `scripts/regress.py`) starts from that save, kept as `saves/beach` in the NSP folder. Run it with `--env DM_AUDIO=1` until the [crash after Continue with the silent sink](#audio) is fixed.

## Testing

- 2026-10-26, Mac: boot, the online and difficulty prompts, the opening movie, the beach, Point of Arrival with three guide movies, the city reveal, inhaling a Blade Knight (Sword), fighting with the Sword, losing it to damage, the title screen and Continue, over five runs (the longest 30 minutes, with a 15-minute soak of walking, jumping, floating, inhaling and attacking, no crash). The only red lines were the layout shaders' [texture type mismatches](#rendering); a key dump of the title menu's shader (`DM_NVN_KEYDUMP=5de6e5dc52a56300`) shows `c3[0].y` = 0, so its reads of slots 1 and 2 do not run.

## Open

- The rest of the game is untested on the Mac: no stage was finished, so no Waddle Dee Town, boss or Mouthful Mode yet, and no save at a stage's end.
- iOS: the unsigned IPA (`KirbyAndTheForgottenLand.ipa`, 6.4 GB, 6,846,727,151 bytes, with the fps counter; 27,297 iOS shader libraries, no pipeline archive) is untested on a device; its memory (4.1 GB resident on the Mac) may be the first limit there, above the 3.4 GB iOS gives an app without the Increased Memory Limit capability.

## Found on this game

- [iOS drops a lone Joy-Con without an extended gamepad profile](../../../docs/reference/services.md#ios-drops-a-lone-joy-con-without-an-extended-gamepad-profile).
- [HAL's `.cmp` containers: a size-prefixed frame](../../../docs/reference/shaders.md#hals-cmp-containers-a-size-prefixed-frame).
- [A host lock held across a same-core handoff](../../../docs/reference/threads.md#a-host-lock-held-across-a-same-core-handoff).
- [Typed outputs for every pixel shader](../../../docs/reference/shaders.md#typed-outputs-for-every-pixel-shader): its 76 shaders writing an R8Uint colour 5 in Waddle Dee Town ([#2760](https://github.com/seedds/nxnative/issues/2760)).
- [Built-in fonts and the audio device notification of SDK 21](../../../docs/reference/services.md#built-in-fonts-and-the-audio-device-notification-of-sdk-21): its Nintendo Switch 2 Edition (program `04004D300C5AE001`) asks for its fonts with `GetBuiltInFontDataSet` at boot.
