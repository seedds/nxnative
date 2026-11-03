# Trails in the Sky 1st Chapter (`01002C9022770000`)

Updates 1.0.4 (v196608) and 1.0.7 (v393216), SDK 19.3.13, Falcom's engine on NVN. Modules `main`, `subsdk0` and `sdk`. The profile was recovered from an older 1.0.4 Mac app with about 25 hours of owner-reported gameplay. Update 1.0.7 was built on the current host and shader chain and played on an Apple M1; the owner confirms scene furniture, menu eyes, bestiary models, and glass and metal reflections render correctly after the fixes below. iOS has not been tested.

## What it needs

| Setting | Why |
|---|---|
| `mem_reserve 0x1100000000`, `mem_alias 0x1000000000` | The engine reserves a virtual address region about ten times its allocator size on the Mac. The observed physical heap is 2.02 GB; the 64 GB alias range is virtual. The smaller provisional layout failed its heap check. |
| `sys_resource 0x1000000` | The NPDM declares 16 MB of system resource. |
| `depth_mode zero` | At boot the game sets near depth to zero; prebuilt vertex pipelines must use `main0_z`. |
| `shader_groups` | Shader files are under `asset/nx/shader`; the other groups keep the split's package paths stable. |

The registered build IDs in [`game.tsv`](game.tsv) cover both updates. The default package and shader tables are from 1.0.7. The base game has not been tested.

## Files and shaders

- The RomFS marker is `movie_en/mp4/sora_1st_ed.mp4`. The app has a 15 GB bundle; game files and saves are outside this repository.
- The recovered shader map has 6,600 entries: 1,269 vertex, 5,294 pixel and 37 compute. A full Mac build from the preserved shader package reported 6,600 successful translations on its original chain. The full 1.0.7 Mac build translated 6,602 shaders successfully, including two additional vertex programs from its executable.
- [`pso_manifest.tsv`](pso_manifest.tsv) has 2,358 unique shader and pipeline entries collected in the owner's Mac gameplay. The app originally shipped with an empty seed list; a later start read 2,246 entries from the local `pso_user.tsv` and prepared them in 0.47 seconds with an existing pipeline archive. First-start preparation without an archive has not been timed.

## Mac observations

- **Resolved: update 1.0.7 missing scene objects on M1 (2026-11-02).** Shader `43c2b838b11bc19c` initially failed pipeline creation because of its 44 KB shared allocation. The general [invocation-private shared array lowering](../../../docs/reference/shaders.md#invocation-private-shared-arrays-exceed-metals-threadgroup-limit) reproduces the working 1.0.4 correction exactly. That alone eliminates the error but leaves furniture absent. A second local change listed in the old app's metadata is also needed: [fixed group sizes guide register allocation](../../../docs/reference/shaders.md#fixed-compute-group-sizes-guide-register-allocation). The shader's default Metal pipeline supports only 768 threads; it supports its required 1,024 with the explicit descriptor. The owner confirms that furniture renders again with both changes. The material-output fix below also resolves the model colours.
- The older 1.0.4 log records a session through frame 669,900, with save transactions still completing. In the last 300-frame interval, frame time was p50 16.66 ms and p95 16.83 ms, GPU p95 7.24 ms and the reported footprint 3,057 MiB on an Apple M1. These numbers describe that interval, not every scene.
- It logs two calls to unimplemented NVN functions: `nvnCommandBufferSetZCullZF32CompressionEnable` near boot and `nvnCommandBufferSetControlMemoryCallbackEnabled` later. No corresponding crash appears in the session log. Their effects are not established.
- **Resolved: incorrect iris colours (owner validation).** Reproduced in both 1.0.4 and the rebuilt 1.0.7, in Costume using a copy of the 25-hour save: Joshua's model has a red-looking iris beside an amber one, while his portrait shows yellow irises. There are two `chr_eye` draws: vertex `cfc9057b69fa8e4e` / pixel `b503751171dcdeff` draws the opaque iris; vertex `e001d2cbd46e29f5` / pixel `f2e6f8b3abe2214f` adds the blended highlight later. The 512×512 BC7 colour texture and its first seven mip levels decode to the expected amber iris; the two eyes' corresponding mesh UVs match. Merely changing the highlight shader's colour does not remove the asymmetry. Offline frame replay isolates the tint to SSAO combine pixel shader `a58cf7e9ad7ff74a`. The opaque iris writes packed material bits `0x7bde` as a constant to integer MRT 3; the generated mixed entry incorrectly has mask 6 rather than 14. Correcting that output type restores both gold irises in the captured frame. The general [constant mixed-output correction](../../../docs/reference/shaders.md#constant-outputs-in-a-mixed-integer-g-buffer) was built into the Mac app (chain `41af1cd4e607`). The owner confirms that both the menu-model eyes and bestiary models render correctly with this app.

- **Improved in owner playtesting: periodic microstutter.** After furniture was restored, visuals and monster detail improved, but the owner observed brief drops from 60 to about 55 fps every few seconds and reported 100% GPU utilization. After the cube image-store correction below, the owner reports that the periodic FPS jumps also seem to have disappeared. No same-scene timing comparison establishes the size or cause of that improvement; GPU saturation as the earlier explanation remains an owner observation.

- **Resolved: intermittent glass and metal reflection artifacts (owner validation).** The owner first reported incorrect reflections on building windows and a character's glasses in 1.0.7, absent in the older 1.0.4 app. An initial playtest after the material-output correction did not reproduce them, but later playtesting of that app (chain `41af1cd4e607`) reproduced an oily appearance on metal too: Estelle's blue shoulder armour, Joshua's shoulder armour and bracers, and a metal plaque behind them. The diffuse clothing and faces appeared unaffected. The cause is separate from the iris material-output correction: [cube image stores need face-array views](../../../docs/reference/rendering.md#cube-image-stores-use-face-array-views). The owner confirms that the same glass and armour render correctly after the host correction, using the existing shaders.
  - Frame 15,444 captures the defect outside Lavantar Casino & Bar, on the windows and the characters' armour. The capture is complete (no omitted textures or addresses outside its pools; SHA-256 `8a1eade9f6b0b94ec2b03c914123b0d90205c1fe4aceaf24e55d25b86a814bb1`). Three fresh diagnostic replays produce the same image, differing from the live capture in 109 pixels with a maximum per-channel difference of 5. The affected passes share environment texture serial 4,638,785 (512×512 RGBA8, six faces, ten mip levels; deferred shader `567fcd0c97a29418`, texture slot 145). Its captured Metal texels and its deswizzled pool storage match exactly in all 60 face/level pairs and already contain the coloured pattern. Zeroing only that captured texture in an offline diagnostic removes the artifacts from both glass and armour; this disables the reflection contribution and is not the fix. Neither cube producer runs in this captured frame.
  - A subsequent trace from boot reproduces the defect outside Hotel Blanche. At frame 885, `cubemap_copy` (`770559397e473ebb`) writes serial 87,182, then `cubemap_blend` (`ea9d6c8d8dd336d3`) writes serial 87,171, using cube views for each of the ten mip levels. Sampled aliases 87,181 and 87,170 share their respective backing, so an allocation mismatch does not explain this run. Both compute shaders declare float `texture2d_array` outputs. The host bound cube views to those outputs; converting the image bindings to face-array views restores their writes. The owner confirms the correction on the same affected materials in the rebuilt host.

## Found on this game

- [Cube image stores use face-array views](../../../docs/reference/rendering.md#cube-image-stores-use-face-array-views): environment updates must write through array views of their cube descriptors.
- [Constant outputs in a mixed integer G-buffer](../../../docs/reference/shaders.md#constant-outputs-in-a-mixed-integer-g-buffer): folded material constants need integer output alternatives.

- [Invocation-private shared arrays exceed Metal's threadgroup limit](../../../docs/reference/shaders.md#invocation-private-shared-arrays-exceed-metals-threadgroup-limit): the scene-object compute shader needs only 11 private words per invocation.
- [Fixed compute group sizes guide register allocation](../../../docs/reference/shaders.md#fixed-compute-group-sizes-guide-register-allocation): its 1,024-thread shader otherwise compiles for at most 768 threads on M1.
- [GLSLC compute code after control constants](../../../docs/reference/shaders.md#glslc-compute-code-after-control-constants): the split must use the output's explicit code offset to recover one shader from `main`.
- [Branches within an exclusive window](../../../docs/reference/rewriter.md#branches-within-an-exclusive-window): update 1.0.7's `main` has an internal branch to its store-exclusive and an unconditional branch past it; both must be copied with the window.

## DLC

DLC is validated by the owner: the app contains 98 add-on RomFS images, the host reports all 98 indices, and the packages appear in ITEM → DLC. Claim them there before looking for their costumes in Costume.

## Open

- Measure frame times in the same scene if further performance work is requested; the latest owner playtest reports improved smoothness after the reflection correction, without a quantitative comparison.
- Build and test on iPhone and iPad. The large virtual address reservation may need a different layout there.
