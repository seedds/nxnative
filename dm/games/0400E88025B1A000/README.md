# Star Fox (`0400E88025B1A000`)

A Switch 2 game: update v131072, SDK 21.4.4 built for the Switch 2 (`Ounce`), NVN2 with DLSS. A custom engine (its own prefixes: `vfs`, `vrender`, `vjob`, `vscript`, `vspirv`, `veffects`, `vphysics`; project name `eclipse`), with Wwise 2024.1 for audio, Bullet physics and NPLN for online play. Modules `main`, `sdk`, `subsdk0` (the SDK's movie module), `subsdk3` (`nn::camera`); `subsdk1` (the C library) and `subsdk2` (`NintendoSDK_DLSS`) are shipped but `main` imports nothing from them. **Plays on the Mac** (M4 Pro, 48 GB): the title, the prologue cinematic and the tutorial flight, with sound. The tutorial runs at 56 to 60 fps with a 4K screenshot every 4 s and at 60 without them; the cinematic at 30 fps, which looks like the game's own choice (not checked). Scripted input here reached the tutorial's first lesson and worked its options menus; flying under input has not been watched on this Mac (pull request #1259's author reports 7 minutes of it with a controller on an M5 Max). No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB. The alias region mapped up to +8.26 GB of its 16 GB, 6.78 GB at most at once, through the tutorial's start; 8.5 GB resident, 4 GB of it allocated on the GPU |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB, so virtual address memory is on; `main` never sets a heap |
| `modules main subsdk0 subsdk3 sdk` | `game.tsv` | `main` imports 17 movie functions from `subsdk0` and 19 `nn::camera` functions from `subsdk3`, nothing from `subsdk1` or `subsdk2` |
| `romfs_marker data.zip` | `game.tsv` | the RomFS has two files, and this is the game |
| `zip_store .zip:.metallib` | `game.tsv` | the archive's members are LZ4-compressed already |
| `shader_groups /data.zip=shader` | `game.tsv` | every shader is a member of the archive |

There is no game-specific patch, and only v131072's build row: the base game, v0, has not been run.

## Files

- A NUP container of 30 GB: a base package (v0) and an update package (v131072), each with its own ExeFS and RomFS. Neither package carries a NACP or an icon, so the name comes from the profile, the display version is the placeholder `v131072`, and the app has no icon.
- The RomFS is two files: `data.zip` (14.5 GB) and `veclipse_build.cvar` (a JSON line: `system.is_shipping_build: true`). `main` opens `rom:/data.zip` through its own virtual file system (`vfs.zip.c`): [LZ4-chained zip members behind a VZIP preamble](../../../docs/reference/shaders.md#lz4-chained-zip-members-behind-a-vzip-preamble-and-shader-binaries-of-one-stage). Its 114,154 members are named by a 16-digit hash with no folders or extensions; 92,069 are chained, 22,085 (the Wwise `RIFF` sound files and `BKHD` banks) stored plain.
- `main` is 192 MB of code with 1,036 imports, of which the host and the other modules supply all but two (gRPC thread-local initialisers, `_ZTH…`). No shader is in any code module.

## Shaders

2,856 in [`shader_map.tsv`](shader_map.tsv): 1,483 vertex, 1,305 pixel, 68 compute; 2,828 one per archive member, no task or mesh shaders, and the rest from the two NDSR upscaler networks embedded in `VIPR` containers. All of them translate. A build takes about 4 minutes in all on an M4 Pro. Each member is the engine's shader binary (`_vrender_shader_binary_t`), read as a single-stage program and paired at draw time (`separable_pairs`: 1,480 cover pairs).

## The picture

Docked, every 3D target was 768x432 from the first frame of the title to the tutorial: MetalFX stands in for DLSS at 768x432 to 768x432, the game's NDSR compute networks take that to 1536x864, and the result reaches the 3840x2160 swap chain, whose top-left 1920x1080 holds the picture. The game's own defaults give the reason for the figure, not the cause: `render.dynres_min_scale_x` is 0.5 and `max` 1.0, so 768x432 is the minimum of a 1536x864 base, at 6 to 11 ms of GPU time a frame on the M4 Pro. Segment timestamps (the profile key `gpu_timestamps segment` then, the host's default since) changed nothing, with the GPU's idle gaps still counted. Resolved, the cause of the docked size: the host answered graphics scale tier 1 docked, and the game renders 0.8 times smaller below tier 0 (byte +0xa of its mode event, `main+0xba69970`): 768x432 for its 1920x1080 window where tier 0 gives 960x540, its handheld size. The host now answers tier 0 in both modes ([The graphics scale does not follow the operation mode](../../../docs/reference/services.md#the-graphics-scale-does-not-follow-the-operation-mode)). Peppy's close-up 65 s into the intro has a mean luminance gradient (x1000 at 1080p) of 5.8 in console footage, 5.3 with tier 0 and 4.6 with tier 1. The sizes and gradients were measured on a build of the PR author's fork, not on `main`.

The shipped Switch 2 configuration sets `render.resolution_scale_console=0.5` and selects NDSR (`render.main_upscaler_console=3`). At a 1920x1080 window and graphics scale tier 0, the game renders its 3D scene at 960x540 and upscales it to 1080p. [Issue #2895](https://github.com/seedds/nxnative/issues/2895#finding-2-why-the-3d-is-960x540-the-games-own-settings)

### Opt-in 4K on the Mac

Create a folder outside the repository with this `veclipse_build.cvar` file in its root. The game reads it after its platform settings. A scale of 1.0 uses the full rendering resolution, and preset 3 requests 3840x2160 for presentation.

```json
{
  "system.is_shipping_build": true,
  "render.resolution_scale_console": 1.0,
  "render.present_resolution_override": 3
}
```

On a display running at 3840x2160 or higher, add `DM_OPMODE=1 DM_ROMFS_OVERLAY="/absolute/path/to/overlay"` to the Mac run command. The overlay leaves the original RomFS files intact. The reporter measured 60 fps in the title and early gameplay on an M4 Max, with the game's 30 fps cinematic segments. Later levels at native 4K remain untested. [Issue #2895](https://github.com/seedds/nxnative/issues/2895)

## Driving it on the Mac

`python3 scripts/build_ipa_from_nsp.py <folder> --platform mac` builds the Mac shaders and the host and prints the run command. Headless, through `DM_CTL`: the title is up about 40 s after boot and `A` starts the game. The prologue cinematic follows, about 4 minutes on every run (a save holds no progress yet, and skipping it was not tried), then the tutorial's first question, about 300 s after boot: `LDown`, `A` answers no. Before the VI fix, shots of the 3840x2160 swapchain showed the 1920x1080 picture in the top-left quarter. A `shot` takes about 165 ms: a hitch every few seconds in a run that takes them is the shot's.

DLSS alpha control (2026-11-17, M6, macOS 27.2, Xcode 27.2): a fresh save, `DM_KEYS=30:A`, 75 seconds, screenshots at 60 and 68 seconds. The title only creates and resizes the network on this build; the prologue evaluates MetalFX at 960x540 to 960x540, sRGB RGBA8 colour into RGBA16F with an R8 reactive mask. This exposed the [alpha pass taking the original builder's size](../../../docs/reference/rendering.md#metalfxs-dlss-stand-in-preserves-alpha). With its extent matched to RGB, the candidate reached frame 3042 with no new red-line kinds and both screenshots within the harness thresholds against main `233e68b8`. The harness reports PASS with a known baseline issue: profile depth `gl` disagrees with observed `zero`, so this is not clean archive/profile validation. Both runs were marked busy; their frame-time numbers are not a performance comparison. The temporary regression route was not retained.

## Found on this game

- [SDK 21 socket initialization and overloads](../../../docs/reference/services.md#sdk-21-socket-initialization-and-overloads) and [Socket `Poll` and `Select` with nothing to wait on](../../../docs/reference/services.md#socket-poll-and-select-with-nothing-to-wait-on): the game's HTTP thread aborted on a failed `poll`.
- [SDK 21's OutputStream on a virtual output device](../../../docs/reference/audio.md#sdk-21s-outputstream-on-a-virtual-output-device) and [SDK 21's hardware Opus decoder manager](../../../docs/reference/audio.md#sdk-21s-hardware-opus-decoder-manager): its voice and music.
- [GameChat queries from the title screen (SDK 21)](../../../docs/reference/services.md#gamechat-queries-from-the-title-screen-sdk-21): a null read in `nn::chat::GetChattingUid`.
- [The camera commands of SDK 21's own camera module](../../../docs/reference/services.md#the-camera-commands-of-sdk-21s-own-camera-module): the game's own camera module, a console with no camera plugged in.
- [sRGB textures loaded and stored raw through an image](../../../docs/reference/rendering.md#srgb-textures-loaded-and-stored-raw-through-an-image): the pale picture with grey blacks (the upscaler network's encoded store was encoded again); measured on a build of our fork, not on `main`.
- [A GPU copy into a texture uploaded from its storage uploaded all of it again](../../../docs/reference/nvn-objects.md#a-gpu-copy-into-a-texture-uploaded-from-its-storage-uploaded-all-of-it-again): 7.6 s and 10 s frames at the prologue's scene changes; measured on a build of our fork, not on `main`.
- [The graphics scale does not follow the operation mode](../../../docs/reference/services.md#the-graphics-scale-does-not-follow-the-operation-mode): the 768x432 scene docked; measured on a build of the PR author's fork, not on `main`.
- [The display's largest layer from `vi::ListDisplays`](../../../docs/reference/services.md#the-displays-largest-layer-from-vilistdisplays): resolved. With the same overlay and a simulated 4K display query on an M4 Pro, the crop changed from 1920x1080 to 3840x2160 and held through 600 frames.
- [The DLSS network resized](../../../docs/reference/rendering.md#the-dlss-network-resized): `nvnDlssNetworkResize`.
- [NVN2's DLSS on MetalFX](../../../docs/reference/rendering.md#nvn2s-dlss-on-metalfx): its DLSS bias mask, which only a red line reported, reaches MetalFX as its reactive mask. Resolved; no visible change in the prologue. DLSS here is 960x540 to 960x540, and the mask (R8, 0 or 255) marks a band at the top and the bottom of the cinematic's frame and the near cockpit edges that cross it, never the characters: the fur trails pull request #1904 put down to the mask were not seen with or without it.
- [TXD through a uniform register kept no handle](../../../docs/reference/shaders.md#txd-through-a-uniform-register-kept-no-handle): 281 pixel shaders, the fur shells among them.
- [A handle pair ORed sampler word first](../../../docs/reference/shaders.md#a-handle-pair-ored-sampler-word-first): 62 pixel shaders.
- [NVN2 compute's separate handles in c\[0\]](../../../docs/reference/shaders.md#nvn2-computes-separate-handles-in-c0): `1002069a331ceded`, dispatched in the prologue.
- [Ampere BMSK and the read of a covered pixel](../../../docs/reference/shaders.md#ampere-bmsk-and-the-read-of-a-covered-pixel): the last 3 shaders, `58046f286de779e1`, `46040940ce0fd49b` and `826168691af2e211`, none asked for in play so far.
- [Compute pipelines compiled at first use](../../../docs/reference/shaders.md#compute-pipelines-compiled-at-first-use): 233 compute pipelines in the app's archive, none built at the title.
- [Compute pipelines outside the bundled archive built at every start](../../../docs/reference/shaders.md#compute-pipelines-outside-the-bundled-archive-built-at-every-start): the second headless start took its 8 from the device's archive.

## Open

- The game's wake-up eventfd fails (the host refuses `nn::socket::EventFd`), so its HTTP thread polls without a wake-up; harmless offline.
- A `pso_manifest.tsv` has not been recorded; the later levels and combat, and iOS, not tested.
- Mario Kart World 1.8.0 has not been run with the camera answers changed for this game.
