# STAR WARS Battlefront Classic Collection (`010040701B948000`)

Update 1.0.4 (v262144) over the base game, SDK 16, on **Vulkan**. The collection is one launcher (`nro1`) and two games it loads as NROs with `nn::ro`: STAR WARS Battlefront (`nro0`) and STAR WARS Battlefront II (`nro2`). Its REI renderer compiles the shaders it draws from GLSL to SPIR-V **at run time**, so the RomFS holds no SPIR-V at all. Modules `main` and `sdk`; `subsdk0` and `subsdk1` are NVIDIA's OpenGL and Vulkan drivers, which the host's Vulkan layer stands in for and does not load. Both games run on the Mac. iPhone and iPad are untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | the Vulkan driver (`subsdk1`) and GLSLC are replaced by the host's Vulkan layer; the shaders are the game's own run-time output |
| `vk_capture.tsv` (1,359 keys) | profile | no SPIR-V in the RomFS: the keys are what the packer's capture stage waits for. See [Shaders](#shaders) |
| `shader_groups`, `shader_note` | [`game.tsv`](game.tsv) | no NVN shaders: the NVN tables stay empty and `vk_shaders.py` writes the Vulkan library and its table |
| `romfs_marker data1/_lvl_common/side/all.lvl` | [`game.tsv`](game.tsv) | the game's level archive lives in `data1` |
| 4 GB block: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x0` | [`game.tsv`](game.tsv) | the NPDM declares no system resource, so there is no virtual-address-memory heap. **Not measured:** the partition is the survey's guess (`dm/games/profile_guess.py`), and it is the one the shipped app was packed from |

## Shaders

- **No SPIR-V in the RomFS.** The renderer builds its modules at run time, so the profile ships the **key list** instead: 1,359 keys, one `sha256(SPIR-V)[:16]` each, in [`vk_capture.tsv`](vk_capture.tsv). The packer's *Vulkan shader capture* stage boots the game in the Mac host with `DM_VK_CAPTURE=<version dir>/vk_capture` until every listed key is there, then `vk_shaders.py` translates the capture together with the RomFS ([Vulkan shaders generated at run time](../../../docs/reference/rendering.md#vulkan-shaders-generated-at-run-time)).
- **The profile holds keys only:** no SPIR-V, no game file and nothing extracted from one. 1,359 keys = 1,359 captured modules at pack time = 1,359 `vk_shaders.tsv` rows = the host's `1359 shader table entries` at start.
- **The capture happens at pack time, on the machine that packs.** The key list is what lets the capture finish; it does not replace it. A session that played an Instant Action match on Tatooine created 507 modules and **all 507 were already listed** (0 new), which is the list's coverage measured rather than assumed.

## Status

| Program | Reached | Measured |
|---|---|---|
| Launcher (`nro1`) | the collection's menu; it starts `nro0` and `nro2` | not measured |
| Battlefront (`nro0`) | photosensitivity warning, attract screen (`PRESS + TO BEGIN`), main menu, then a match entered and played | at the frame 3,600 gate `draws 2100 (skipped 0), passes 1200, submits 1350`; 537 pipelines; 0 `[RED]` |
| Battlefront II (`nro2`) | launcher → singleplayer → map select → spawn → an Instant Action match on Tatooine (`DESERT EXTERMINATION`), with HUD, vehicles and weapon fire | `frame 25500: draws 200109 (skipped 0), passes 4800, submits 900`; 1,616 pipelines; 0 `[RED]` |
| Battlefront II, the shipped app, 8 m 55 s | ten pipeline-set rebuilds (scene loads), in-match draw counts throughout | frames 1–30,900, 31,642 display ticks; **8,445 pipelines built**; 106 of 106 draw reports `skipped 0`; **0** shader modules missing, **0** `[RED]`, **0** pipelines not built; 507 modules captured, 0 new |

Saves work: Battlefront writes `SaveGames1/PLAYER 1.p` and Battlefront II `SaveGames2/Player 1.p`, and both survive a restart. Audio, controllers, the HUD and 1920×1080 come up without per-game work. **Frame rate: not measured.** **Memory: not measured** (see the partition note above).

## Rendering

- **The command-post light bands are fixed.** A command post's light laid wide, straight-edged bands across the terrain, far beyond the light's radius. The pass is a second additive pass over the terrain that samples a 32×32×32 3D light volume the game stages as `align(bytes-per-row, 256) × rows` bytes per slice (`262144 = 256 × 32 × 32`); the copy took one padded layout only and excluded 3D images, so it read the volume at half its bytes, sheared. A copy whose buffer is exactly `pitch × rows` per slice is now read with that slice stride, for any layer count and image type (#3442). At the Tatooine Sandcrawler command post the control frames carry 6–8 wide green stripes across the upper plating and the fixed build's carry none. The same light volume path serves every map; the band cases on Cloud City and the other maps were not each re-photographed.
- **The font atlas needed `VK_EXT_4444_formats` and a faithful `A4R4G4B4` expansion** (#3443): without the extension listed the renderer passes `VK_FORMAT_UNDEFINED` and its font init faults with a NULL atlas; with a naive packed view the glyph cells come out opaque cyan (3.28 % of the version box, 0.00 % after the nibble expansion, white glyph coverage unchanged at 2.30 %). 12 images of that format are created and 0 formats go unmapped.
- **The entry's draw path needs `VK_EXT_extended_dynamic_state`** (#3447): the renderer loads `vkCmdBindVertexBuffers2EXT` by name and calls it unconditionally, so an unimplemented name is a jump to address 0 on Battlefront's first draw, and its light and billboard pipelines bind the vertex stride dynamically on every draw.
- **Saves need `nn::fs` to take an ApplicationId of 0 as the calling application** (#3445): a game of the collection is an NRO of the launcher's process and passes 0, which the host read as another program's save and refused; the mount also has to accept writes through the read-only mount the SDK names.

## Open

- **One hard-edged green parallelogram remains at the top of a beam column.** The beam column is drawn from a 32×32 BC2 page that holds one soft radial disc while the engine's vertex UVs cover a quarter of it; the rest of the column was corrected by a build-time vertex variant (`vk_<key>_uv4`, keyed on `3311a332947d770e`) that is **not part of this change**. The parallelogram is another draw of the same family, which the variant's gate declines (a sub-rect, or a page other than 32×32), and leaving the command-post light-volume pipeline out keeps it in place, so it is not that volume either. Which draw it is was not measured.
- **The light volume's addressing is the engine's:** its W axis is REPEAT, which carries the volume's alpha outward over the map. The fix corrects the bytes the shader samples, not the addressing the engine chose.
- **iPhone and iPad are untested.** No IPA was built for this title.

## Found on this game

General host changes first needed here; each is a change of its own, and this profile's pull request names them:

- **The staging row pitch proved from the buffer's own size, for every image type and layer count** (#3442): the sheared HUD icons and minimap (rows narrower than 256 B were copied tight) and the command-post light bands (3D light volumes).
- **`VK_EXT_4444_formats` and the packed 4444 expansion** (#3443): the font atlas.
- **`nn::fs` save mount: ApplicationId 0 is the calling application, and the mount writes** (#3445).
- **`VK_EXT_extended_dynamic_state`** (#3447): Battlefront's draw path.
- **A strip restarts at the maximum index in Metal, without the layer** (#3444): a question this title raised that closed as a test and a reference entry, not a host change.
