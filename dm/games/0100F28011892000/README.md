# Overcooked! All You Can Eat (`0100F28011892000`)

Update 1.0.12 (v786432) over the base game, SDK 10.4.1. Unity
2019.4.11f1 IL2CPP with FMOD, rendering through NVN. Modules `main`,
`subsdk0` (Nintendo's movie library), and `sdk`. The original campaign's
opening kitchen plays on the Mac at 60 fps, using the existing Unity
frame-rate control and shared SDK rules. This profile adds no game patches
or host changes. iPhone and iPad runtime validation is open.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | The NPDM declares 16 MB of system resource: virtual address memory. |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | `nninitStartup` gives StandardAllocator pool minus used (3.14 GB), without setting a heap. The Mac address-region check reaches alias +3.37 GB in the first 15 seconds of `nnMain`, within the 4.38 GB alias region. |
| `depth_mode zero` | `game.tsv` | The game calls `nvnDeviceSetDepthMode` with near = 0, so the offline pipeline bases use the same depth convention. |
| `fps_target 60` | `game.tsv` | The shared Unity limiter changes `QualitySettings.vSyncCount` from 2 to 1 and the host presents at 60. The rewriter locates the engine settings through registered internal-call names, without a build gate. |
| Shared SDK function rules | [`sdk_patches.py`](../../tools/sdk_patches.py) | The node area is reduced to 62 MB and large regions are aligned to 2 MB. These are existing SDK rules; this profile adds no rewriter patches. |

## Startup and input

- From an empty save, the save warning, Team17 logo, and animated title render.
  Press L+R together to continue. A confirms the screen bounds. Rejecting the
  optional agreement permits offline play; A dismisses the update notice.
- Campaign expands to Overcooked!, Overcooked! 2, and Extra Trimmings.
  Press Down to enter the first campaign, then A.
- Mac tests use the window with the Metal HUD, `DM_CTL` input, and
  `DM_ALIAS_SIZE=0x118000000` to exercise the profile's address-space limit.
  Button holds of 0.2–0.4 seconds work in menus.
- After creating slot 1, restarting and choosing Campaign → Overcooked! →
  Continue reloads Intro Apocalypse. A advances its opening dialogue.
- In the kitchen, the left stick moves, A picks up and puts down ingredients,
  and R switches between the two chefs. The dividing counter lets them pass
  an ingredient from the crates on the right to the boards on the left.
- X chops an ingredient placed on a board. Verified: lettuce picked up,
  passed across the divider, placed on a board and chopped; the game advances
  to "Chopped Lettuce must now be placed on a plate!" Holding A skips the
  tutorial's instruction card.

## Frame rate and memory

- The game's original limit is 30 fps (present interval 2). `fps_target 60`
  uses [Unity's shared frame-rate control](../../../docs/reference/frame-rate.md#unitys-vsynccount-and-targetframerate-found-through-the-internal-call-table):
  this Unity 2019.4 build registers the internal calls from code. The host
  changes the current quality level's `vSyncCount` from 2 to 1 and applies
  the matching present cap. `Application.targetFrameRate` stays at -1.
  No `present_interval display` override or fixed-address patch is needed.
  The mechanism is engine-bound; the tested default of 60 is game policy.
- Measured on the Apple M6, 16 GB RAM, windowed on a 60 Hz display, in the
  opening kitchen, with pipelines created as requested (`DM_PSO_PREBUILD=0`).
  Paired `scripts/bench.py` runs use [`bench.tsv`](bench.tsv)'s `intro-walk`
  route and identical copies of the same save, measuring seconds 136–146:

  | Target | Frame time p50 | Frame time p95 | GPU p50 | Frames |
  |---|---|---|---|---|
  | Original 30 fps | 33.33 ms | 34.20 ms | 4.54 ms | 300 |
  | Profile default 60 fps | 16.65 ms | 17.48 ms | 4.78 ms | 600 |

  Neither timing window dropped frames or logged red diagnostics; both runs
  reported zero audio underruns. GPU time per frame is similar; twice as
  many frames increases the work per second. The 30 fps variant explicitly
  sets `DM_FPS_TARGET=30`; the 60 fps variant uses the profile without that
  variable. The benchmark's `over_budget` count uses the profile's 60 fps
  budget even for the deliberate 30 fps variant.
- Speed check: the route holds the stick down for 0.5 seconds twice. At
  1920×1080 the first, unobstructed walk ends with the blue marker's bounds
  within 2 horizontal and 4 vertical pixels between 30 and 60 fps; the
  second reaches the same bottom counter (the camera animation differs).
  This checks movement speed on the Mac; completed orders and later levels
  remain open. Use `DM_FOCUS_KEEP=1` for scripted runs: switching applications
  otherwise pauses this game and invalidates a timed input comparison.
- Shader inputs stay `04978e0b9d59`, so the existing full Mac library remains
  valid; no shader rebuild is required for the frame-rate setting.
- In the original 30 fps port test, resident memory reached 4,276 MB during
  loading and settled to 3,876 MB in the kitchen; GPU allocation stayed around
  1,279 MB. That five-minute full-library run had no red diagnostics or crashes.

## Files and shaders

- RomFS: 575 files, 12.42 GB. There are 557 UnityFS bundles (12.18 GB),
  including the Addressables bundles under `Data/StreamingAssets/aa/Switch`.
- The shader split finds 520,394 shaders: 287,522 vertex, 232,842 pixel,
  21 compute, and 9 geometry. Eleven are GLSLC outputs in `main`; the rest
  come from the Unity assets. The supplemental pair table covers the Unity
  programs.
- The full Mac shader build succeeded on 2026-11-15: 520,394 of 520,394 shaders,
  no failed side variants, no undecoded bindings, no unfillable driver reads,
  and nothing lost without failing. Chain `66eab9c9a6b4`, profile inputs
  `04978e0b9d59`. The shader stage took 1:01:39 on an Apple M6 with 16 GB RAM,
  including packing the 47 GB library and generating the runtime tables.
- The first playtests use a separate partial library, adding missing shaders
  from each run. Missing-library and unstamped-library diagnostics in those
  tests describe that temporary library, not missing shader extraction.
  `shader_seen.tsv` records the requested shaders so `--seen-shaders` can
  rebuild the covered route quickly after a chain change.
- DLC index 1 is a licence-only package with no content files. It is reported
  as owned by the existing add-on-content service.
- The optional bulk pipeline prebuild has 293,290 pairs. It was stopped
  deliberately after 61,000 pairs (no failures in that prefix); the gameplay
  tests use the existing on-demand pipeline path. A complete bulk prewarm
  was not required for those tests and remains unvalidated.

## Sound

- The host opens stereo AudioOut with 240-frame, 48 kHz buffers. The normal
  three-minute warm repeat and the subsequent chopping run reported no
  underruns.
- A separate instrumented run produced non-silent 16-bit stereo PCM
  (295 seconds, normalized RMS 0.0726, no clipped samples). That run reported
  intermittent underruns with `DM_AUDIO_DUMP` enabled; whether dumping or
  first-use work caused them is not isolated. Playback was not listened to.

## Open

- A completed cooking order, the end of the tutorial, the second campaign,
  and later kitchens.
- Full bulk pipeline prewarm; the audio-dump run's intermittent underruns.
- Local multiplayer and online services; no online account has been connected.
- iPhone and iPad runtime validation. This port is requested as a Mac-tested
  profile only; no IPA has been built.
- The base game alone is not registered or tested; it takes a derived profile.
