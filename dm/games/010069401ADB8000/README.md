# Unicorn Overlord (`010069401ADB8000`)

Update 1.05 (v262144) over the base game, SDK 16.2.4. Vanillaware's engine, as in 13 Sentinels: Aegis Rim ([01003FC01670C000](../01003FC01670C000/README.md)), on NVN. Its ExeFS holds only `main` and `sdk` (no movie module); the `sdk` build is the one Brain Memory Training Academy ships ([0100F5D01DA3A000](../0100F5D01DA3A000/README.md)). Playable on the iPhone at 60 fps; on the Mac brought up through the logos, the title, a new game and the opening story scene with no red line.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `alloc_big_align on` | [`game.tsv`](game.tsv) | without it a thread's ring heap kept base 0 and the game wrote to `0x268` right after the title screen; see [Memory](#memory) |
| `mem_used 0x5c00000` | `game.tsv` | the rewritten modules are 92 MB, larger than the default 64 MB of used memory |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region, which holds the 3.12 GB heap |
| Node area cut 1 GB → 62 MB | `dm/tools/sdk_patches.py`, `sdk` build `12f16103…` (`sdk+0x134778` / `+0x13480c`) | registered for Brain Memory Training Academy, which has virtual address memory. This game has none and never allocates an address region, so it never needed the cut; it runs the same with it |

No `patches.py`, no `pad_layout` (the iPhone pad starts in the 2D layout).

## Memory

- The NPDM declares no system resource (META +0x14 = 0): the game uses `SetHeapSize` with pool − used, as 13 Sentinels does, a 3.12 GB heap. `main`'s bss is 30 MB; the rewritten modules are 92 MB (`mem_used`).
- Resident: about 2.9 GB in the opening scene on the Mac (development notes; `game.tsv` says about 3.0 GB in the opening story scene). On an iPhone (A20 Pro): a 2.88 GB footprint with at least 3.2 GB of headroom.
- **Resolved: a write to `0x268` right after the title screen** (`main+0x5d6708`, a constructor whose `this` is `0x200`). `operator new` (`main+0x5a55d4`) takes the thread's ring heap, a bump allocator whose cursor starts at its base. One ring heap kept base 0, as in 13 Sentinels: the engine cuts ring heaps out of a memory pool at 64 KB GPU alignment, and the host's pool storage from malloc is only 4 KB aligned. The first allocation returned 0, which `operator new`'s null check skipped, and the next returned `0x200`. With `alloc_big_align` (allocations of 64 KB or more aligned to 64 KB) the game plays from the title screen into its opening story scene.

## Frame rate

- 60 fps. The game sets present interval 1; on the iPhone the frame time is 16.7 ms at the median.

## Audio and services

Each found by one boot ([Hardware Opus decoders built on the SDK's software decoders](../../../docs/reference/audio.md#hardware-opus-decoders-built-on-the-sdks-software-decoders), [Cryptographically random bytes](../../../docs/reference/services.md#cryptographically-random-bytes)):

- **Resolved: abort in `sm::Initialize` under `nn::codec::GetHardwareOpusMultiStreamDecoderWorkBufferSize(int, int, int, int, int)`** (at `nnMain`, CRI's Opus streams). The host now has the C interface of the hardware multi-stream decoder, including the SDK 12+ overload with an option argument (Age of Calamity only needed the class and the four-argument size). Each call goes to the `sdk`'s software `OpusMultiStreamDecoder` C interface, with a host-held state per game object as for the single-stream decoder (`hle_codec.c`). `GetHardwareOpusMultiStreamDecoderProcessingTime` reports 0.
- **Resolved: abort in `sm::Initialize` under `nn::crypto::GenerateCryptographicallyRandomBytes`** (`std::random_device` → the `sdk`'s own `getentropy` → `InitializeCsrng`). The `sdk` defines `getentropy`, so the host's `getentropy` hook never sees the call, but the call into `nn::crypto` goes through the `sdk`'s PLT. An `HLE_FORCE` on it fills from the host's entropy stream (`hle_os.c`).

## Files and shaders

- All data is in 7 CRI CPK archives (`Unicorn.CPK`, the profile's `romfs_marker`, `UnicornPatch.CPK` and one per language). Audio is CRI ADX2 with Opus streams (`nn::codec`'s hardware multi-stream decoder, replaced by the SDK's software one).
- Both DLC NSPs (indices 1 and 2) are licences only: their RomFS holds just the SDK's placeholder file.
- Shaders: not in RomFS. 2,188 GLSLC compiler outputs (magic `0x19866891`) compiled into `main`'s read-only segment, 1,725 distinct (614 vertex, 1,111 pixel), read by `shader_split.py` `glslc_shaders`. Single-stage programs, combined per draw. Every binding decodes.

## Input

- The game reads a USB mouse and no touch screen, as OCTOPATH TRAVELER 0 and Tears of the Kingdom do; a rule that turns touches into a mouse would need testing on it ([ROMANCE OF THE THREE KINGDOMS 8 REMAKE](../0100C2A01A730000/README.md)).

- Rumble: `main` sends each player's vibration every frame (two `hid::SendVibrationValue` calls per player; Asian release `main` `9c3116f0…` at `+0x5ead8c`) from fields of its pad object that only the pad reset writes (amplitude 0, 160 / 320 Hz), so that path never vibrates. CRI's audio-driven vibration (`hid::VibrationConverter`, behind the sound renderer type 6 that CRI registers at startup, `+0x790b60`) never opened in the opening scene nor in a battle on the Mac. The game has no rumble: on the Switch it does not vibrate either. With a DualSense its (zero) values reach the controller since [Vibration handles before a controller is reported](../../../docs/reference/services.md#vibration-handles-before-a-controller-is-reported).

## Mac routes

- `DM_KEYS=f2400:A*20/300` goes from the title screen (about frame 2,500) through a new game into the opening story scene (about frame 4,900).

## Found on this game

General host changes first needed here; each applies to every game:

- `alloc_big_align` was first needed on 13 Sentinels ([its note](../01003FC01670C000/README.md#memory), [the profile keys](../README.md#keys)).
- **The hardware multi-stream Opus decoder's C interface** forwarded to the SDK's software decoder (`hle_codec.c`) and **`GenerateCryptographicallyRandomBytes` under the `sdk`'s own `getentropy`** (`HLE_FORCE`, `hle_os.c`): [Hardware Opus decoders built on the SDK's software decoders](../../../docs/reference/audio.md#hardware-opus-decoders-built-on-the-sdks-software-decoders), [Cryptographically random bytes](../../../docs/reference/services.md#cryptographically-random-bytes).
- **A 5 ms device buffer on the Mac** (`hle_audio.c`), maintained across output-device and sample-rate changes: [The Mac's default I/O buffer](../../../docs/reference/audio.md#the-macs-default-io-buffer) records the underrun measurements, host validation and remaining checks.
- **Vibration handles answered before a controller's haptics are routed** (`hle_hid.c`): the game asks for them once at startup, before GameController reports the pad, and every value went to handle 0 (seen in the log; the game has no rumble, so nothing changed in play) ([Vibration handles before a controller is reported](../../../docs/reference/services.md#vibration-handles-before-a-controller-is-reported)).

## Open

- Resident memory in the opening scene: about 2.9 GB in the development notes, about 3.0 GB in `game.tsv`.
- `game.tsv` still describes the game as "In bring-up" (the Mac into the opening story scene), while AGENTS.md lists it as playable on the iPhone.
- `game.tsv`: only the merged 1.05 copy is extracted; the base game's own build IDs are added when needed.
