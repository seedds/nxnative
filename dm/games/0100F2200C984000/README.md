# Mortal Kombat 11 (`0100F2200C984000`)

Update 1.0.26 (v1703936) over the base game. NetherRealm's modified Unreal engine, Wwise and Havok; NVN, SDK 9.3.1. Tested on an Apple M2 Mac through NX Play. The owner reports the repaired build working on 2026-11-15. iPhone and iPad playback have not been tested.

Integration status: this draft promotes the metadata from the owner's working local build. It has not been validated against current upstream main. [Movie fixes (PR #2798)](https://github.com/seedds/nxnative/pull/2798) and the remaining shared shader/runtime work in [scope issue #2797](https://github.com/seedds/nxnative/issues/2797) are prerequisites. Keep this profile PR in draft until those dependencies and upstream gameplay are checked.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `main subsdk0 sdk` | [game.tsv](game.tsv) | `subsdk0` is NintendoSDK_movie, used for the MP4 extractor and movie player |
| 4 GB reservation, 256 MB alias region, 256 MB stack region | `game.tsv` | the NPDM declares no system resource; virtual address memory is disabled and the game uses a normal heap |
| `app_pool 0xCD500000`, `mem_used 0xcba4000` | `game.tsv` | the tested rewritten modules exceed the default 64 MB used-memory allowance; the game sizes its heap from the remaining application pool |
| `depth_mode zero` | `game.tsv` | observed `nvnDeviceSetDepthMode` uses near=0; offline pipeline archives must select `main0_z` |
| 585 observed fragment-output facts | [shader_facts_manual.tsv](shader_facts_manual.tsv) | preserve the tested material facts when splitting the shader archives again; shared typed-output entries cover attachment combinations beyond these observations |
| Recorded prewarm seeds | [pso_manifest.tsv](pso_manifest.tsv) | 3,901 shader entries, 198 compute pipelines and 3,192 render pipelines from Mac runs; every referenced shader key is in the map |

There are no game-specific instruction cuts, frame-step stubs or feature-disable switches. The repairs below are shared host or shader-tool fixes and must accompany this profile; adding the tables alone to an older host does not provide them.

## Files and shaders

- The update contains about 25.9 GB of RomFS data, principally `Asset/*.xxx`, `Movies/*.mp4` and `MKScript`. The local setup includes licence-only fighter, skin and Aftermath DLC; individual DLC coverage has not been exhaustively checked.
- The shader map contains 76,865 distinct keys: 11,875 vertex, 63,739 pixel, 972 compute, 152 domain and 127 hull shaders. Shader code remains in the owner's extracted game files; this profile contains hashes, translation facts, pairings and prewarm metadata only.
- UE3 package chunks use 64-bit tag/size fields. Some zlib streams use a small window (`Asset/Tweakvars.xxx`, header `28 15`); these are not Oodle streams. See [Unreal Engine 3 shader caches](../../../docs/reference/shaders.md#unreal-engine-3-shader-caches-oodle-packages).
- The generated `shader_facts.tsv` includes the manual material facts. `shader_pairs.tsv` supplies the observed vertex/pixel pairings; `pso_pairs.tsv` and `bf_want.tsv` currently contain no data rows.
- `c1_full.tsv` and `cb_extent.tsv` are deliberately not shipped in Git. A full shader build generates them against its own library stamp. Stale tables must be regenerated from that library's sources and reflection, rather than relabelled.

## Status on the Mac

- Local runs reached the main menus, fighter selection, matches and story movies. Screenshots verified clear character portraits and recognizable fighter materials; movie output was 1280×720 through native NVN textures.
- The owner subsequently rebuilt with NX Play and reported that everything seemed to work on 2026-11-15, after the story-transition allocator repair, shader metadata repair and movie-audio flush repair. This is user gameplay confirmation, not a recorded `regress.py` validation.
- The inspected audio trace had no device underruns. Focused stereo and six-channel AAC seek tests reproduce fresh PCM after Flush, including repeated Flush. No measured frame-rate, long-session or audio-drift benchmark is claimed.

## Mac routes

Build with `scripts/build_ipa_from_nsp.py <NSP folder> --platform mac`, supplying the owner's keys through the script's normal configuration. Use the generated Mac launcher or NX Play.

From the title screen, enter the main menu and fighter selection, then start a local match. Story mode exercises the H.264/AAC movie path and the transition from a movie into a fight. Earlier runs exercised Scorpion/Sub-Zero materials and Fujin/Joker selection and matches. There is no deterministic saved-input smoke route yet; prewarm seeds record the local runs, rather than guaranteeing coverage of every mode.

## Found on this game

- Updating Game Data wait: [Network-clock reads must advance offline](https://github.com/seedds/nxnative/issues/2797).
- Character-selection static: [Copies into an uncompressed alias of a compressed texture](../../../docs/reference/rendering.md#copies-into-an-uncompressed-alias-of-a-compressed-texture).
- Native movie buffers with a zero byte capacity: [Native texture buffers with zero capacity](https://github.com/seedds/nxnative/issues/2797).
- Material attachment classes: [Fragment output types selected from render attachments](https://github.com/seedds/nxnative/issues/2797).
- Oversized constant-buffer reads: [LDC's immediate is an unsigned byte offset](https://github.com/seedds/nxnative/issues/2797).
- 268 removed texture accesses in 105 shaders: [Computed bindless texture handles retain their sampled reads](https://github.com/seedds/nxnative/issues/2797).
- False uniform-handle write hazards: [Handles in uniform buffers written earlier in the same submission](../../../docs/reference/shaders.md#handles-in-uniform-buffers-written-earlier-in-the-same-submission).
- Story-transition null allocation: [Movie extractor allocations need the SDK graphics allocator](https://github.com/seedds/nxnative/issues/2797).
- Audio history at a seek/loop boundary: [Audio decoder history survives a movie seek](https://github.com/seedds/nxnative/issues/2797).
- Offline compile reuse: [Offline shader compilation reuses stage work and concurrent requests](https://github.com/seedds/nxnative/issues/2797).

## Open

- Tessellation/patch draws (primitive 14) remain unsupported by the shared runtime. The shader map includes hull/domain stages; the owner's successful gameplay report does not establish coverage of every effect that uses them.
- Online services, all fighters/stages/modes, full story completion, long sessions and iOS are unverified.
- A reproducible smoke route, cross-game regression report for the shared fixes and harness-generated `validated.tsv` remain to be supplied before claiming automated regression coverage. No validation stamp is fabricated from the user's gameplay report.
