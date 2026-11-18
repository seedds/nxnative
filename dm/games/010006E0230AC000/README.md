# Tales of Eternia Remastered (`010006E0230AC000`)

Update 2.0.0 (v65536) over the 1.0.0 base game. Unity 6000.0.66f2 IL2CPP with CRIWARE, SDK 21.4.4, NVN. Early gameplay was tested on a Mac and an iPhone. The base game alone and a full playthrough have not been tested.

## What it needs

The survey's profile, plus the depth mode observed during the Mac boot. No game-specific rewriter cuts or host changes were needed.

| Setting | Why |
|---|---|
| `modules main subsdk0 sdk` | `subsdk0` is NintendoSDK_movie, used for the opening movies. |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000` | Unity maps its own memory and does not request a heap. The first 15 seconds of `nnMain` reached alias +3.63 GB within the 4.375 GiB alias region. |
| `app_pool 0xCD500000`, `sys_resource 0x10000000` | The application pool comes from the survey. The NPDM declares 256 MiB of system resource. |
| `depth_mode zero` | The game calls `nvnDeviceSetDepthMode` with near = 0. Offline pipeline bases need that mode. |

See [`game.tsv`](game.tsv) for the module build IDs. The split found 15,983 shaders, all compiled for Mac and iOS in the original test. The split includes Unity shader objects from the RomFS and GLSLC outputs in `main`.

## Testing

- 2026-11-18, NX2Apple 1.5.0 (engine upstream base `94a0944755da1a443f42651bf7ee49ab24ed5ee5`, with the bundled fork patches), Xcode 27.2: Apple M1 Pro, macOS 27.0.1. The Mac app showed the opening, Simplified Chinese menus and character movement in the first playable scene.
- Same build inputs, iPhone 15 Pro Max (A17 Pro), iOS 27.2: the tester entered gameplay and played a battle in the NX2Apple shared app. The standalone iOS IPA was built with separate game data (about 557 MiB) but was not installed; the shared app framework was tested instead.
- 2026-11-18, upstream `2dacca8d003137dfeab5c3baebb914f32ed8e9de` with this profile, shader chain `b-63853cc4c4e3`: rebuilt the signed Mac app with all 15,983 shaders. On the same M1 Pro, rendered the 2.0.0 title screen and the Simplified Chinese first-run dialog. No new iOS build or gameplay test on this upstream revision.
- Reference build: `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app --package v65536`. NX2Apple's bundled `nxb` 0.9.3 reported an IVFC hash failure on the base NSP; the LibHac reference extraction passed without allowing damaged data. See [NX2Apple compatibility report](https://github.com/LNPANDY/NX2Apple/issues/1428). This profile does not change the extractors.
