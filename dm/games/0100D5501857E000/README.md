# Sam & Max: The Devil's Playhouse (`0100D5501857E000`)

Update 1.8.0 (v524288) on the base game v0, SDK 17.5.6. Telltale Tool: the game is 41 `SM3_nx_*.ttarch2` archives (1.5 GB, deflate blocks, Lua scripts and `_resdesc_50_*.lua` sets beside them), FMOD sound through AudioOut, NVN. Modules `main` and `sdk` only; `main` imports `nn::ro`, but the RomFS has no `.nro`. The languages are en-US, en-GB, fr, de and ru.

On the Mac: it starts, reaches its frame loop (about 3,900 frames in the first run, window and sound, controller connected) and draws with its own shaders, with no red line, no crash and no audio underrun. What the picture showed was checked by eye only, on the first minute; no route, save or later episode has been played yet (see [Open](#open)).

## What it needs

The provisional profile's guesses held: no key needed changing.

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false, as on Horizon. The game sets a heap of at most the pool − 64 MB (3,328 MB) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | as the other 4 GB games |
| `shader_groups /=romfs` | `game.tsv` | every shader is in the RomFS |

No rewriter cut and no `patches.py`: the rewriter ran on `main` and `sdk` without a note and the game ran. No `pad_layout`.

## Shaders

- **Current:** 24,243 (21,914 pixel, 2,305 vertex, 24 compute) in the effect pack `SM3_nx_Shaders_mem.t3fxpack`, plus `SM3_nx_ShadersPatch.ttarch2`. No BNSH, BAHS or GLSLC output anywhere: the split found 0 until its Telltale reader was added ([Telltale effect packs](../../../docs/reference/shaders.md#telltale-effect-packs-in-ttarch2-archives)). The Mac libraries build for all of them (15 min on an M2 Pro).
- The pack holds bare records, not aligned: the code area, a `u32` with the control section's length, the control section. The 24 compute shaders have no code header. The patch archive (2,213 records) has the same layout; the split's total equals the base pack's alone, so every patch record is byte-identical to a base one (the key hashes control section and code area) and adds no shader. Which shader the game binds for a patched effect name is not read.
- **Open:** `shader_pairs.tsv` is empty and the pipeline manifest is not recorded (`DM_PSO_MANIFEST=off DM_PSO_RECORD`): every pipeline is compiled when first drawn, so the first minute has frames of 0.3–1 s (49 pipelines at once in one).

## Mac routes

- Run it with the command the NSP script prints (`--platform mac`); `DM_WINDOW=1` opens the window, `DM_AUDIO=1` plays sound. No scripted route yet.
- **Open:** without `DM_WINDOW=1` the first run of the final build presented 6 frames in 30 s (the main thread at 0.8 s of CPU, the present thread waiting, every guest job thread idle on a condition variable), against 1,356 frames in 26 s with the window. The cause is not looked into; run it with the window until it is.

## Found on this game

General host changes first needed here; each applies to every game:

- **Telltale `.ttarch2` archives:** the shader split read none of them and stopped at `found no shaders in main or the RomFS`: [Telltale effect packs in ttarch2 archives](../../../docs/reference/shaders.md#telltale-effect-packs-in-ttarch2-archives).

## Open

- Play it beyond the first minute: a new game, an episode, the save and a long session; look for wrong pictures, hitches and memory growth. Add a `smoke.tsv` route when it is played.
- The pipeline manifest and `shader_pairs.tsv` (above).
- The base game is the NSP `v0`; only the merged v524288 was extracted and run. The profile has no other build.
- Not tried on the iPhone.
