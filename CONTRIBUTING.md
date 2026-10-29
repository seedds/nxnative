<div align="center">
  <a href="README.md"><b>📖 Overview & Guide</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;
  <a href="SUPPORTED_GAMES.md"><b>🎮 Supported Games & Timeline</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;
  <a href="CONTRIBUTING.md"><b>🤝 Contributing</b></a>
</div>

---

# Contributing to NxNative & NxNative Launcher

Thank you for your interest in contributing to **NxNative Launcher** and the **nxnative** ecosystem! Issues and pull requests are welcome. This page outlines our development guidelines, clean-room legal policies, and the contribution workflow.

---

## ⚖️ Clean-Room Policy: Never Game Files or Keys

> [!CAUTION]
> **Zero Copyrighted Material Policy**:
> Never commit, attach, or distribute game ROMs, binaries, keys (`prod.keys`, `title.keys`), RomFS dumps, ExeFS modules, extracted assets, or decrypted shaders.
> 
> Game dumps must stay strictly on your local storage. Only logs, benchmark stats, screenshots, Title IDs, Build IDs, and code patches are permitted.

---

## 🛠️ Where to Start

- **Launcher UI & Frontend**: Built with Swift 5.9+ / SwiftUI for macOS. Core code lives under `gui/Sources/`.
- **Core Recompiler & Runtime**: Upstream repository is [`seedds/nxnative`](https://github.com/seedds/nxnative). Engine docs are in [docs/](docs/).
- **A new game profile**: Check [SUPPORTED_GAMES.md](SUPPORTED_GAMES.md) first. If your game is not listed, port it via the visual wizard or upstream scripts, test on Mac Apple Silicon, and contribute the profile `game.tsv` upstream.

---

## 🐞 Reporting Problems

When reporting an issue, please use the issue templates and provide:
1. **Device & OS**: Chip model (e.g., M1, M2 Pro, M3 Max, M4, iPhone 15 Pro) and macOS/iOS version.
2. **Game Information**: Title ID and Game Build Version.
3. **Logs**: Attach logs located at `~/Library/Logs/nxnative/<TitleID>.log`.
   - You can also run `python3 scripts/bug_report.py <TitleID>` to produce a sanitized diagnostic report zip.
4. **Description**: Clear description of the unexpected behavior (crash, visual glitch, frame pacing).

---

## 🔀 Submitting a Pull Request

1. **One change per PR** off `main`.
2. **Keep commits descriptive**: Explain the symptom, root cause, and how you verified the fix.
3. **Tests & Verification**:
   - For GUI changes: verify clean build with `./scripts/build_gui.sh`.
   - For supported games list: run `python3 scripts/generate_supported_games.py`.
   - For engine modifications: ensure host regression checks pass.

<div align="center">
  <a href="README.md"><b>⬅️ Back to Overview & Guide</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;
  <a href="SUPPORTED_GAMES.md"><b>🎮 View Supported Games</b></a>
</div>
