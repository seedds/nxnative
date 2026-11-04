# Axiom Verge (`0100052004384000`)

Update 1.0.3 (v131072), SDK 4.4.0. MonoGame for the Switch: MonoGame's and the game's C# compiled to C++ by Sickhead's BRUTE into `main` (the build path `E:\Projects\AxiomVerge\Brute\Switch\bin\NX64\Release`). Sound goes through the SDK's audio renderer (`nn::audio`, no FMOD), on NVN. Modules `main` and `sdk` only. `sdk` is the same build as Mario + Rabbids Kingdom Battle's ([010067300059A000](../010067300059A000/README.md)). Brought up on the Mac with no host change. The iPhone and iPad needed two host changes that apply to every game ([iPhone and iPad](#iphone-and-ipad)).

On the Mac: the title, the load screen, a new game (Normal), the intro cutscene and its skip, and 7.5 minutes of controller play through Eribu's first rooms (the Axiom Disruptor, the first enemies), then a save load. All of it runs at 60 fps with music and sound and no red line, at 600–690 MB resident, flat over the session.

## What it needs

The provisional profile's guesses held: no key needed changing.

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x0` | [`game.tsv`](game.tsv) | The NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon: no virtual address memory |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | As Celeste's ([01002B30028F6000](../01002B30028F6000/README.md)): 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region |
| `shader_groups /Content/=content` | `game.tsv` | The MonoGame effects; see [Shaders](#shaders) |
| VammManager node area and large-region alignment cuts | `dm/tools/sdk_patches.py` SDK-wide rules, `sdk` build `9de92bad67c4…` | Mario + Rabbids' cuts, from the SDK-wide rules on this build |

## Versions

- Only the merged 1.0.3 copy is brought up. The update replaces the whole program, and its RomFS changes almost nothing.
- The base game v0 (1.0.0) is another program: SDK 0.12.17, older than any profile here, with five modules (`main` `25c24171344c071d6c89aceb026871ea`, `rtld`, `sdk` `cc774be5e129…`, `subsdk0` NintendoSDK_NVN, `subsdk1` nn::sf).
- The NSP script reads an update's version only from a `[v<N>]` tag in its file name. A dump named `Axiom Verge v131072 (0100052004384800) (UPD).nsp` is labelled `vNone` in the survey; `… [0100052004384800][v131072] (UPD).nsp` is packed as v131072.

## Shaders

- 54 in MonoGame effects (MGFX version 8), read by `shader_split.py` (`mgfx_effect`):
  - 50 in the 17 XNB files in `Content/ShadersNX`;
  - 4 in SpriteBatch's two built-in effects in `main`.
- `Content/ShadersNX` also ships 59 loose GLSLC outputs (41 `<Effect>.xnb.<technique>.fdebug` pixel shaders and 18 `.vdebug` vertex shaders), whose control sections differ from the effects'. **The game never opens them** (`DM_FS=1`: no open in a 7.5-minute session, while the 17 XNB effects are opened at start). They are debug leftovers; the split translates them too, which costs only seconds.
- `pso_manifest.tsv` (23 shaders, 25 render pipelines) was recorded with prewarm off, over boot, the title, a new game, the intro skip, 7.5 minutes in Eribu and a save load. Without it, a first start compiled six pipelines on the main thread in that route, at 19–55 ms each.

## Audio

- `update_lock` finds `nn::os::MutexType` at +0x1d0, as on Mario Kart 8 Deluxe's SDK 4.4.0; `AudioRenderSystem` sits at +0x60 in `AudioRendererImpl`. The SDK 4 DeviceSink is the 0x130-byte command (ID 0xf), with 2 inputs.
- The game mixes hot. 0.26 % of the samples of a 7.5-minute `DM_AUDIO_DUMP` sit at full scale (332 of 449 s, runs of up to 261 samples). The host only clamps the SDK's own int32 mix to 16 bits, with no downmix or gain. By ear it is loud, not distorted.

## Input

- The menus confirm with B, the bottom face button (Xbox-position mapping); A does nothing there.
- `DM_DETCLOCK` stops the game before frame 1: the main thread waits on a mutex owned by a thread whose last SVC is SleepThread, and guest time advances only with presents. `DM_REC` recordings therefore replay without a fixed clock, and only approximately.

## Mac routes

- A fresh save reaches Eribu with `DM_KEYS="10:B,13:B,15.5:B,20:PLUS,21.5:Left,22.5:B"`: the title, the load screen, slot 1, Normal, then the intro cutscene skipped (Plus, Left to Yes, B). Control of Trace comes after "Wake up, Trace" (B advances the text).

## iPhone and iPad

- **Runs at 60 fps** on two devices:
  - an iPhone 16 Pro Max (A18 Pro, iOS 26.0), unsigned IPA through LiveContainer, frame time p50 16.67 ms, 422 MB resident;
  - an iPad Pro 11-inch (M4, iPadOS 26.0), from Xcode and through LiveContainer, frame time p50 16.67 ms over 1200 frames.
  - The IPA is 153.5 MB. Checked on the devices: boot, the opening and the title. The Mac route was not replayed there.
- **Without the linear-texture fix, the opening's sky and mountains are magenta** and magenta frames flash. The game's four 8×8 RGBA8 linear textures have 32-byte rows. Both devices view a linear texture on a buffer only at 64-byte rows, while macOS asks for 16 on an M4 Pro. The iPhone showed the magenta. Both devices log `[RED] linear texture offset 0x3f8000 / row pitch 32 not aligned to 64 bytes`, then `[RED] deswizzle failed format 0x25 target 1`. The host fix uploads such textures from their storage: "Linear textures a device cannot view on their pool's buffer" in [nvn-objects.md](../../../docs/reference/nvn-objects.md).
- **The launcher's Metal 4 pipelines are off by default** (no `pso_mtl4`). Turned on, iOS 26.0 crashed before the first frame, in `objc_release` under `warm_task`'s autorelease pool: the driver returns the specialization descriptor autoreleased. The host fix is "A specialization descriptor iOS 26.0 returns autoreleased" in [rendering.md](../../../docs/reference/rendering.md). With it, the iPad runs at 60 fps with them on: 18 bases compiled and 25 specializations on a cold start, and the bases come from the archive on the next.
- An IPA meant to run with Metal 4 on needs `IOS_PULL=1`. Otherwise it leaves out the pulling vertex libraries, and each vertex shader logs `[RED] pulling library of … not usable`.

## Open

- Only Eribu was played; the other areas, bosses, the map screen and speedrun mode are not checked.
