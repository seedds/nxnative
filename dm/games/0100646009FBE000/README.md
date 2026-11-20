# Dead Cells (`0100646009FBE000`)

Update 1.25.0 (v3014656; the title shows version 35, "The End Is Near") over the base game, SDK 13.3.5. Motion Twin's game on Heaps and HashLink, its Haxe code compiled to C (HL/C) in `main`, with its own NVN driver (`nx.nvn.NvnDriver`) on NVN 55.15. Modules `main`, `subsdk0` (GLSLC) and `sdk`. The five DLC NSPs are licences only. It runs on the Mac at 60 fps with sound and saves. An unsigned IPA has not been built; iOS is untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Hardware pool in a 4 GB guest layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the NPDM declares no system resource, so `IsVirtualAddressMemoryEnabled` is false and the game sets a heap: 3.06 GB in a 3.25 GB heap region |
| `shader_groups /=romfs` | `game.tsv` | all its shaders are in one file of the RomFS |

No code of the game is cut, and the profile needs no frame-rate key: the game asks for present interval 1.

## Shaders

All 1,262 unique shaders are in `res1.pak`'s `shaders.cache.nx`, as [GLSLC outputs in base64 in Heaps' shader cache](../../../docs/reference/shaders.md#glslc-outputs-in-base64-in-heaps-shader-cache). The game logs `1291 shaders loaded` at boot and `Missing shader shader.Fallback`, a line of its own that had no visible effect. `main` imports `glslcCompile` and names `onMissingShader`, so it may compile a shader missing from the cache (read from its strings, not from its code), but a run with `DM_GLSLC_CAPTURE` from boot through the title and the first rooms compiled nothing, and no run asked for a shader outside the map, so nothing is captured.

## Tested on the Mac

Headless on the Apple M6, with `DM_KEYS`-style presses through `DM_CTL`:

- **Reached:** the title, the menus, the update pop-up, a new game, the first rooms of the Prisoners' Quarters, the first NPC's dialogue, picking up items, doors, double jumps and ledge grabs, attack swings, taking damage from a zombie and an archer, death, the return to the start and the prisoner-cell scene. A second boot offered `Continue (Normal Mode)` and loaded the saved run.
- **Numbers:** no `[RED]` line from the game or the host and no crash in four runs, the longest 28,200 frames (about 8 minutes); frames at 60 Hz (the median window's p99 is 18.6 ms, the worst 111 ms, at a load). A 326 s audio dump has sound in 317 of those seconds, and 101 of 103 reports of 2,000 callbacks show no underrun (the others 1 and 7).
- **Memory:** resident 2.2 GB at the first rooms, levelling at 2.6 to 2.7 GB, GPU allocated 1.6 to 1.7 GB. The longest idle stretch held it flat, so that part is not evidence about varied play.

## Open

- **A kill is not confirmed.** The attack animation plays and enemies hurt the player, but no run showed an enemy dying: between remote presses the character stands idle, and levels are generated per run, so a blind route does not repeat. No later biome or boss was reached. A person playing with a controller should check this first.
- **Yellow-green glow on distant mobs.** Mobs out of the fight draw as soft bright silhouettes (the first zombie in reach had normal colours). `main` names a `switchMobGlow` and the options' `outlineMobs` is off, so it reads as the game's own effect, but it has not been compared with a Switch.
- **iOS.** Nothing has run on an iPhone. 2.7 GB resident is high for one, so memory is the first thing to watch there.
- **Prewarm list.** [`pso_manifest.tsv`](pso_manifest.tsv) covers only the route above.

## Running it on the Mac

The work folder is `<NSP folder>/0100646009FBE000-work`, with its own `venv`. The NSP script with `--platform mac` builds everything and prints the run command. On the title press A, A (Play), A (the update pop-up, on a new save), A (Normal Mode). B jumps, B twice double jumps (leave about 0.1 s between the presses), Y attacks, R opens doors and picks up items, and holding B skips a dialogue. A tool call between presses leaves the character idle for about 10 s of game time, enough for an archer to kill it, so send a fight's presses as one burst.

## Found on this game

- [GLSLC outputs in base64 in Heaps' shader cache](../../../docs/reference/shaders.md#glslc-outputs-in-base64-in-heaps-shader-cache).
