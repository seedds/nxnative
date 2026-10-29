# Deep Rock Galactic: Survivor (01003EC027CC8000)

Provisional profile, brought up 2026-10-29 and confirmed running on both targets.

- Base 1.0.2 + update v196608 (1.0.3). Unity game, SDK 21, NVN.
- Tested on: macOS 26.6 (M3, 60 FPS, 42 draws/frame, audio through CoreAudio with zero underruns)
  and iPhone 17 Pro Max / iOS 26.5 (13 500+ frames at 60 FPS, audio 46 000 callbacks, 0 underruns).
- Built with `scripts/build_ipa_from_nsp.py`, `--romfs separate` (the RomFS image is 896 MB).
- Shaders: 10 155 split from the Unity bundles, all translated; no run-time shader capture needed.
- Known: `nvnTextureViewCompare` is unimplemented in the NVN host and gets called ~160 times;
  the game carries on and renders correctly, so it is cosmetic here.
- Notes for bringing up a new build: the survey is enough; the only local change needed was the
  free-space gate (`--romfs separate` builds do not need the merged image on disk).
