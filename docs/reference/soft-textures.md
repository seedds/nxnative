# Soft textures (the kartnative / plumber-racer-2 family, ported to this tree's anchors)

`tex_soft` (default off; `DM_NVN_TEX_SOFT`) is a packaged texture with a real layout whose texture keeps its finest level out of the host texture and out of memory: `tex_desc` builds it from level 1, every upload reads the source one level down (level shift `sk`), `ReadTexels` sees one lower level, and the pages only level 0 holds go back to the system (`nvn_dirty_release`, `soft_sweep` after a 300-frame grace) unless the game copies from the texture (`soft_harden`). Soft textures upload on the CPU while their pool has no zero-copy buffer (`mtl_pool_wrapped`): iOS wires a buffer's whole memory pool, for a pool the GPU may otherwise never address. A second switch, `DM_NVN_TEX_RELEASE`, arms the release path; both off by default, so no game sees this until its profile names `tex_soft` (Mario Kart World's `game.tsv` carries that row in the community lines this port comes from, `dm/games/<id>/README.md` in each).

Data: on the iPhone Duo (iOS 27.3), a full Mario Kart World free-roam drive at 3.7–3.9 GB footprint (4,457–5,262 without; the same anchors' GPU-only uploads at 3.7→851 MB wrapped pools in the kartnative line), and the long-frame record without the file-backed page-in spikes this path had carried (p95 81 vs 215 ms). The 60-fps sustained state under driving is the same open problem this tree records elsewhere and did not change here.

| From | What |
|---|---|
| Symptom | a game whose large BC textures from its files cost whole-pool wiring and a finest level the GPU never samples enough to justify |
| Cause | the host texture of a large block-compressed texture is the game's own size, its finest mip and all, and the game's memory pool behind it wires identically |
| Fix | the soft texture, its release path and the CPU upload while unwired, above |
| Found on | [Mario Kart World](../../dm/games/0400C3F00006E000/README.md) (and the kartnative/plumber-racer-2 family of forks it ports, MIT) |
