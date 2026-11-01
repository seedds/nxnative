# FINAL FANTASY TACTICS - The Ivalice Chronicles (`010038B015560000`)

Update 1.4.0 (v393216) over the base game, SDK 19.3.9 (its `sdk` is byte-identical to Hades II's, [0100A00019DE0000](../0100A00019DE0000/README.md)). Square Enix's engine on NVN: `main` imports only `nvnBootstrapLoader`, and the bundled OpenGL driver (`subsdk0`, NintendoSDK_OpenGL) is not loaded, as `main` takes nothing from it the host does not supply. Modules `main` and `sdk`. Files are loose in `resources/` and `resources_nx/` (textures `.tex`, models, CRI ADX audio, CRI USM movies decoded by the game's own CRI Mana on the CPU); CRI Mana movies and ADX audio run in the game's own code. Playable on an iPhone.

Status: brought up on the Mac: initial settings, the opening movie, the prologue and the Orbonne Monastery battle up to the player's first turn, at 60 fps with no red line (about 1.4 GB resident).

## What it needs

| Setting | Where | Why |
|---|---|---|
| Job pool wake cut: `main+0x12554` `ldr x0,[x0]` → `str x0,[x0,#8]`, `main+0x12558` `b SignalEvent` → `b 0x1254c` | [`patches.py`](patches.py), `main` `3cfd4578…` | a worker woken in another's place passes the wake on; the boot hung in 4 of 6 runs without it; see [Threads](#threads) |
| `tex_flush_upload on`, `tex_dirty_upload on` | [`game.tsv`](game.tsv) | the UI textures are written by the CPU into uncached pools after creation, with no flush; without both keys every texture read 0 and the screen stayed black; see [Rendering](#rendering) |
| `mem_used 0x4c00000` | `game.tsv` | the rewritten modules (76 MB with the bss) are larger than the default 64 MB of used memory the game takes off the pool for its heap |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the NPDM declares no system resource: the game uses `SetHeapSize`, 3.13 GB (pool − used memory) in the 3.25 GB heap region |
| `pad_layout full` | `game.tsv` | the D-pad moves the cursor and the right stick turns the camera (the game's controls screen) |

## Threads

- **Resolved: the main thread waited forever on a job's done event at boot while every worker slept, two of them on one event** (4 of 6 Mac runs, `main+0x1426c` waiting on `Job::m_doneEvent`). The engine's job pools wake one worker through the pool's single auto-clear event. A job goes into worker k's own queue (`main+0x154c0`), and if k's state reads asleep the caller signals the event (`main+0x12554`, from the per-worker dispatch loops `main+0x14000` / `main+0x14350` / `main+0x17670`, the single-job submits `main+0x14640` / `main+0x14790` / `main+0x14dc0` and the shutdown `main+0x13df0`). The kernel wakes the waiter that slept first, which is k only while the workers sleep in index order. A worker woken for another's job runs only its own queue (worker loop `main+0x15ce0`) and sleeps again, so with two workers in one pool (threads 0x1006 / 0x1007, later pinned to core 2) the signal is lost: at boot when worker 1 went to sleep before worker 0, and later as a job left in worker 0's queue for 12 s.
- The thread dump's `wait #` (the order condition waiters arrived in, which `SignalProcessWideKey` wakes them in at equal priority) showed the order in every hung run.
- **Current:** the game's own wait wrapper (`main+0x12500`, `{event, pending}`) signals the event once more when it takes a nonzero pending word after waking, and nothing in `main` set it. The build cut makes the signal set it first, so a worker woken in another's place passes the wake on (in a one-worker pool the extra signal costs one empty loop): 6 of 6 runs boot.
- An experimental host change that made `StartThread` return only once the new thread ran brought the hangs to 1 in 6 and was dropped. That timing change did not establish a kernel guarantee.
- **Confirmed, 2026-10-26:** this is a guest queue-wake assumption, not incorrect host condition-wake ordering. The workers are priority 46 versus the main thread's 44 and initially have mask 0x7. The separate [same-core start correction](../../../docs/reference/threads.md#starting-a-higher-priority-thread-on-the-callers-core) therefore does not apply to these starts. With that correction, an unpatched copy hung in 2 of 6 ten-second Mac boots: main at `main+0x1426c`, workers 0x1006 / 0x1007 asleep on the same condition, lock free. The registered cut reached frame 300 in all 6 matching boots. The instruction replay checks one worker and both two-worker arrival orders, targeting each queue: both wrong-worker originals fail and all patched cases pass. See [the replay command](../../../docs/agents/05-debugging.md#threading-cut-regressions). The kernel evidence and distinction between SDK and kernel events are in [the reference entry](../../../docs/reference/threads.md#one-auto-clear-event-shared-by-a-pools-workers).

## Rendering

Mario's binding layout (combined textures at `0x20 + 8 × slot`, images at `0x120`); every binding decodes.

- **Resolved: black screen; the UI drew in the right places under `HILITE`.** The UI reads its textures from `.tex` files the engine loads straight into a `CPU_UNCACHED | GPU_CACHED | COMPRESSIBLE` pool (flags 0xa2) after creating the textures: 231 textures by the title screen, no packaged layout, no `WriteTexels`, no copies. Writes to an uncached pool need no flush, so neither the flush path nor `tex_init_fill` (which only takes `CPU_NO_ACCESS` pools) saw them. With `tex_flush_upload` and `tex_dirty_upload` the registered textures' pages are write-protected, and a write after `nvnTextureInitialize` uploads them.
- **Resolved: translucent UI (the frosted text bands, button guides, dialog headers) yellow and white.** The last passes have no render target: fragment shaders composite each UI layer into the window texture with image stores, blending in software. The composite (`uirender.tec`, `4cf1975ed1e36590`) loads the window texture's texel as `r32ui`, unpacks the four bytes and stores the blend through a float view of the same image. The host bound the RGBA8Unorm texture to the `texture2d<uint>` argument as it was, whose reads are undefined (Metal's validation layer does not check textures in argument tables). An image slot the shader declares as uint / int now gets a view of the same texel size in that class (`img_fit` in `nvn_mtl.m`); a texture gets the format-view usage only when a shader needs such a view, upgraded before the next draw pass opens.
- **Resolved: dialog boxes with holes the size of their text, and white specks where text faded.** Each UI element is drawn into its own layers and then composited into the window image by one draw per layer, all in one attachment-less pass, each a read-modify-write of the image, with `nvnCommandBufferBarrier(ORDER_FRAGMENTS | INVALIDATE_TEXTURE)` (bits 0x12) between them. The host took a barrier only as an order for the next pass, and Metal orders nothing between draws of one render pass for shader writes: the text element's composite read the window before the parchment had landed and wrote the old pixels back. A barrier with `ORDER_FRAGMENTS` or `ORDER_FRAGMENTS_TILED` now ends the open pass when a draw in it wrote an image or a storage buffer (`swrite_note` in `nvn_mtl.m`); reflection marks an image binding written when its type has write access (`nvn_refl.m`).
- **Resolved: `texture clear case not implemented … mask 0x1`.** The game clears an R8 render target with channel mask 0x1; a mask that covers every channel the format has is a whole clear (`mtl_pf_chmask`).
- **New NVN entry points:** `nvnCommandBufferBindTextures` (the plural of `BindTexture`), `nvnSamplerPoolRegisterSamplerBuilder` (a sampler registered from its builder), `nvnTextureWriteTexelsStrided` (`WriteTexels` with row and image strides) and device item 0x67, which the game reads before every program it loads and uses as the alignment of its copies of the shader (answered 256, not measured).

## Files and shaders

- 306 shaders: 257 GLSLC outputs in the engine's technique files (`resources_nx/release/master/shader/*.tec`, magic `TEC\0`, each output whole after a table of parameter names) and 49 in `main`. `shader_split.py` reads GLSLC outputs in any file no other reader takes, and pairs each as a single-stage program. Groups `resources` and `resourcesnx` (`shader_groups`).
- **Unbuilt:** one geometry shader (`bahamuteffect.tec`), as Metal has no geometry stage.
- On the outputs the old GLSLC rule keeps, this game's among them, the section-header reading added for SDK 4 (`glslc_section`) gives the same key ([GLSLC outputs read through their section headers](../../../docs/reference/shaders.md#glslc-outputs-read-through-their-section-headers)).
- `romfs_marker`: `resources/release/master/system/ffto/fftpack.en.bin`.

## Mac routes

- A fresh save reaches the first battle turn at about frame 21,000 with `DM_KEYS="22:A*60/2,150:Plus,152:X~2.5,165:Plus,167:X~2.5,175:A*70/2"` (A through the settings and the birthday, then + and a held X skip each part of the opening).

## Found on this game

General host changes first needed here; each applies to every game:

- **GLSLC outputs in any RomFS file no other reader takes** (`shader_split.py`): [GLSLC outputs in `main` and in loose files](../../../docs/reference/shaders.md#glslc-outputs-in-main-and-in-loose-files).
- **CPU writes into uncached pools uploaded** (`tex_dirty_upload` with `tex_flush_upload`), **integer views of image slots** (`img_fit`), **fragment barriers that end a pass after shader writes** (`swrite_note`, `nvn_refl.m`), **whole clears by channel mask** (`mtl_pf_chmask`) and the **new NVN entry points**: [CPU writes with no flush, caught by page protection (`tex_dirty_upload`)](../../../docs/reference/nvn-objects.md#cpu-writes-with-no-flush-caught-by-page-protection-tex_dirty_upload), [Integer views for images a shader declares uint or int](../../../docs/reference/rendering.md#integer-views-for-images-a-shader-declares-uint-or-int), [Fragment-ordering barriers between draws that write images](../../../docs/reference/rendering.md#fragment-ordering-barriers-between-draws-that-write-images), [A channel mask that covers every channel is a whole clear](../../../docs/reference/rendering.md#a-channel-mask-that-covers-every-channel-is-a-whole-clear), [Plural texture binds, builder-registered samplers, strided texel writes, device item 0x67](../../../docs/reference/nvn-objects.md#plural-texture-binds-builder-registered-samplers-strided-texel-writes-device-item-0x67).
- **A job wake lost between two workers on one auto-clear event** (thread dump `wait #`): [One auto-clear event shared by a pool's workers](../../../docs/reference/threads.md#one-auto-clear-event-shared-by-a-pools-workers).
- **Priority handoff when starting a same-core child**, found during the audit but not the cause of this game's hang: [Starting a higher-priority thread on the caller's core](../../../docs/reference/threads.md#starting-a-higher-priority-thread-on-the-callers-core).

## Open

- No source gives an iPhone measurement (frame rate, footprint); development.md says only "playable on an iPhone".
