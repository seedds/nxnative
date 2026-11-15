# Overcooked! All You Can Eat (`0100F28011892000`)

Update 1.0.12 (v786432) over the base game, SDK 10.4.1. Unity
2019.4.11f1 IL2CPP with FMOD, rendering through NVN. Modules `main`,
`subsdk0` (Nintendo's movie library), and `sdk`. The original campaign's
opening kitchen plays on the Mac at its original 30 fps limit, using the
existing host and shared SDK rules. This profile adds no game patches or
host changes. iPhone and iPad runtime validation is open.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | The NPDM declares 16 MB of system resource: virtual address memory. |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | `nninitStartup` gives StandardAllocator pool minus used (3.14 GB), without setting a heap. The Mac address-region check reaches alias +3.37 GB in the first 15 seconds of `nnMain`, within the 4.38 GB alias region. |
| `depth_mode zero` | `game.tsv` | The game calls `nvnDeviceSetDepthMode` with near = 0, so the offline pipeline bases use the same depth convention. |
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

- The game sets present interval 2; no frame-rate override is added.
- Measured on the Apple M6, 16 GB RAM, windowed on a 60 Hz display, in the
  opening kitchen: frame time p50 33.33 ms, p95 34.21 ms, GPU p50 4.57 ms
  (300-frame samples at frames 7800–8400). These are the game's 30 fps
  semantics, with pipelines created as requested, `DM_PSO_PREBUILD=0`.
- Resident memory reached 4,276 MB during loading and settled to 3,876 MB
  in the kitchen; GPU allocation stayed around 1,279 MB. The five-minute
  full-library run had no red diagnostics or crashes. Later levels are not
  measured.

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
