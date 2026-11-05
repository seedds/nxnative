# Decisions

Choices that apply to every game: a default, a platform setting, a policy. One entry each, newest first, written with the change that makes it ([charter](charter.md#changes-that-apply-to-every-game)). A later entry reverses an earlier one by linking it, with its own evidence; entries are not edited away.

Each entry: **Decision**, **Evidence** (devices, games, numbers, links), **Cost** (what it gives up, and what was not measured), **Switch** (how to get the old behaviour), **Would reverse it** (what evidence would).

## 2026-11-04: Mac apps opt out of Game Mode

- **Decision:** `dm/mac/pack_app.py` writes `LSSupportsGameMode` false in every Mac app (`cb59a672`, GitHub issue #349). It reverses `f3d8cf33` of the same day, which had declared it, as the iOS app does.
- **Evidence:** two reporters on M4 Pro Macs: Tears of the Kingdom in fullscreen fell from 60 to 30 fps after 45 s to 3 minutes, its GPU time 10.5 → 19.7 ms, with the GPU at its lowest clock (338 MHz) while 72–98 % busy. A 100-line Metal app with no host code did the same in Game Mode and not without it (one run each, M4 Pro on AC power). [macOS Game Mode holds the GPU at its lowest clock](reference/frame-rate.md#macos-game-mode-holds-the-gpu-at-its-lowest-clock).
- **Cost:** what else Game Mode does is lost for every game and Mac: doubled Bluetooth sampling for controllers and AirPods, lower priority for background tasks. Not measured. Not checked on an M1 or M2, nor on games limited by the CPU rather than the GPU.
- **Switch:** none. A player turns Game Mode on per game from the Game Overlay in full screen; macOS keeps that choice, and keeps Game Mode on for a bundle ID it has already labelled a game whatever the key says.
- **Would reverse it:** the clock drop not reproducing on other device classes, or a measured cost to input or audio latency with Game Mode off.
