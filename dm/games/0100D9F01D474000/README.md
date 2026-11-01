# Rhythm Heaven Groove (`0100D9F01D474000`)

Base game 1.0.0 (v0), SDK 21.4.0, the newest SDK so far. Nintendo's own 2D engine on NVN; audio is the SDK's AudioRenderer (`sound/sm/**/*.cwv`). Modules `main` and `sdk`. Plays on the iPhone, handheld with Boost.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `op_mode handheld` | [`game.tsv`](game.tsv) | docked means a TV to the game, which then runs a latency calibration (press A on every fourth beat) until one passes; a phone is its own screen. See [Input](#input) |
| `perf_mode boost` | `game.tsv` | Boost keeps the 1920x1080 draw size (handheld Normal draws 1280x720); the game takes it from the performance mode alone (`hle_oe.c`). Boost in handheld mode is a combination a Switch 1 never reports, so a profile choice |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | `main` sets a heap of at most the pool − 64 MB: a 3328 MB heap region. The game has no virtual address memory, so the NSP script's heap check does not run |
| `shader_groups` (`/mesg/`, `/graph/`, `/sound/`, `/wavmark/`, `/voicetext/`, `/shader/`) | `game.tsv` | the shader split's groups |

## Rendering

One BNSH, `shader/shader_NX.bnsh` (2 vertex and 33 pixel shaders, all 2D), with Mario's binding layout (every binding decodes). No packaged textures: each texture file (`graph/**/*.tex`, raw BC7 or linear R8 with its description in the `.spr` beside it) is read into memory and gets a memory pool the CPU cannot access.

- **Resolved: black screen, every sprite and glyph in place under `DM_NVN_HILITE`.** The sprite shader (`40adb3021c79b155`) computes texture × colour multiply + colour add and discards a fragment whose alpha is 0. The uniform buffer was right (a `CBDUMP`: multiply 1, add 0), so every texture read 0. The game makes a `CPU_NO_ACCESS | GPU_CACHED` pool (flags 0x21) on the memory it read each texture file into: 245 pools by the first screens, each holding one texture at offset 0, 17 block-linear BC7 (flag `IMAGE`, no packaged layout) and 228 linear R8 (`IMAGE | LINEAR`). NVN keeps what a pool's storage held at `nvnMemoryPoolInitialize` as its content, but the host uploaded from guest memory only packaged, `WriteTexels` or flushed textures, and made a linear texture on its pool's buffer only in a pool the CPU can see. Now a new texture in a `CPU_NO_ACCESS` pool whose storage holds data starts with that content, uploaded once (`tex_init_fill` in `nvn_mtl.m`); it stays a GPU texture that can be rendered into, with no re-upload on page writes. A linear one is made on the pool's buffer in any pool (`tex_linear`). Left out: storage of zeros, `COMPRESSIBLE` textures (render targets here, in their own 0xa1 pools; their memory means nothing without the compression tags), multisample, depth / stencil and sparse ones. Found by a census of every texture's pool flags, flags, format and whether its storage held data at creation (a temporary line under `DM_NVN_LOG`).
- **Resolved: the latency calibration's beat numbers torn and its background in blue steps.** Two BC7 textures without a packaged layout were read with the wrong block height. A dump of the guest bytes deswizzled in Python with 1 to 16 GOBs and decoded with `texture2ddecoder` shows the right one by eye; a smoothness score does not, because a block height too tall reads padding and looks smooth. Truncating height in blocks × 1.5 / 8, as MacNativeGames found, matches every size checked by eye: 672×160 and 1232×160 (40 block rows: 4 GOBs, where rounding up gives 8), and 112×112 (4), 288×208 (8) and 880×432 (16), where both roundings agree and `floor(rows / 8)`, the other rule that fits 40 rows, does not. `nvn_tex.c` now uses it for every game (`pick_tile(ah * 3 / 16)`).

## Audio

- **Resolved: no sound; red `audren: command 0 in the command list is malformed (first word 0x181901 size 0)`.** SDK 21's DSP command header has no magic: +0x00 enabled (bit 0), +0x01 command ID, +0x02 size (s16), +0x04 estimated time, +0x08 node ID (`CommandListProcessor::Process`, `sdk+0x24fd50`), and every command body moved 8 bytes forward. DeviceSink is still ID 0x14 but 0x140 bytes: input count +0x128, input indices +0x12c, upsample buffer +0x120 (written at `sdk+0x23a7b0`). `AudioRenderSystem` keeps SDK 20's offsets (update lock +0x2d0). `capture` (`hle_audren.c`) tries this layout when neither magic walk works, bounded by the generated list's length; its first lists hold 4 commands or fewer, so the layout is now chosen on the first non-empty list.

## Services

- **Resolved: abort in `nn::sm::Initialize` under `nn::pl::sharedresource::GetPlatformSharedResourceManager`**, first thing in `nnMain`. The game asks for every shared font at once with `pl::GetSharedFont(int *, SharedFontData *, int, LanguageCode)` (`sdk+0x40f270`), not by type. `SharedFontData` is 0x18 bytes {u32 type, address, size}; the SDK adds the pl service's offsets, which point past each font's 8-byte BFTTF header, to the shared memory base, and the game hands each address straight to `ScalableFontEngine::LoadFont` (`main+0x501ff0`). The hook returns the six types in order with the stand-in TTFs past the header, whatever the profile's `shared_font` (`hle_sdk16.c`).
- **Resolved: SIGSEGV in `vi::GetDefaultDisplayPanelIdentifier`** (`main+0xa9a4`, before the window). The SDK's version calls through the vi service object the host never made. The game reads a u32 kind and, from kind 2, 0x14 more bytes; anything else makes it use zeros. The host answers an all-zero identifier (`hle_vi.c`).

## Input

- **Not a host bug: the TV latency calibration ("press A when you hear the fourth beat") ignores A.** Docked, the first boot runs it (scene script `main+0x4c0820`). It starts listening only `0x3c0` frames (16 s) after the prompt, through the active controller (`main+0x22b0` id `0x3ed`, virtual button 4 = A), and passes when about ten presses keep a steady offset within −5 to +13 frames of the cue (evaluation `main+0x2948c0`; offset = cue ticks at `+0x34` / about 17 ticks per frame, about 480 per beat). Scripted presses at fixed seconds drift off the beat; use frame-timed `DM_KEYS` at the bar period (117.12 frames at its 123 BPM) and read each press's offset with `DM_PROBE=main:2951f0@34`. A passed calibration is saved per TV panel identity (the host's all-zero one included) and not asked again.
- **Current:** the profile plays it handheld with Boost, which skips the TV setup and keeps the 1080p draw size.
- The iOS app starts it in the 2D gamepad layout.

## Files

- `romfs_marker` is `sound/sm/showtime/n_virus_very_hard.cwv`.

## Found on this game

General host changes first needed here; each applies to every game:

- **Initial content of `CPU_NO_ACCESS` pools** (`tex_init_fill`) and **linear textures in any pool** (`tex_linear`): [A pool's storage at creation is its textures' content](../../../docs/reference/nvn-objects.md#a-pools-storage-at-creation-is-its-textures-content), [Linear textures in CPU-visible pools](../../../docs/reference/nvn-objects.md#linear-textures-in-cpu-visible-pools).
- **Truncated automatic block height** (`pick_tile(ah * 3 / 16)` in `nvn_tex.c`): [Automatic block height truncates](../../../docs/reference/rendering.md#automatic-block-height-truncates).
- **SDK 21's DSP command list without magic** (`capture` in `hle_audren.c`), **`pl::GetSharedFont`** for every font at once (`hle_sdk16.c`) and **`vi::GetDefaultDisplayPanelIdentifier`** (`hle_vi.c`): [SDK 21 DSP command header without a magic](../../../docs/reference/audio.md#sdk-21-dsp-command-header-without-a-magic), [Shared fonts requested all at once](../../../docs/reference/services.md#shared-fonts-requested-all-at-once), [Default display panel identifier](../../../docs/reference/services.md#default-display-panel-identifier).
- **`perf_mode`** (Boost whatever the operation mode, `hle_oe.c`): [dm/games/README.md, Keys](../README.md#keys).

## Open

- `game.tsv` still describes the game as "In bring-up" (on the Mac it plays from boot through the setup and its TV latency calibration into the first rhythm game, Hoop Trundling, with sound), while AGENTS.md lists it as playable on the iPhone.
