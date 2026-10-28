<div align="center">

<a href="https://github.com/rennan101/nxnative_Launcher">
  <img src="docs/images/banner.png" width="100%" alt="NxNative Launcher Banner">
</a>

# NxNative Launcher

### nxnative Graphical user interface Tool for Apple Silicon

[![Platform](https://img.shields.io/badge/platform-macOS%2013%2B-blue.svg)](https://apple.com/macos)
[![Architecture](https://img.shields.io/badge/arch-Apple%20Silicon%20(M1%2FM2%2FM3%2FM4)-orange.svg)](https://apple.com)
[![Swift](https://img.shields.io/badge/Swift-5.9%2B-FA7343.svg)](https://swift.org)
[![Version](https://img.shields.io/badge/version-v1.6.2-purple.svg)](https://github.com/rennan101/nxnative_Launcher/releases)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Core Engine](https://img.shields.io/badge/core%20engine-seedds%2Fnxnative-informational.svg)](https://github.com/seedds/nxnative)
[![Report Issues](https://img.shields.io/badge/Issues-Support%20%26%20Bugs-red.svg)](https://github.com/rennan101/nxnative_Launcher/issues)
[![Sync Upstream](https://github.com/rennan101/nxnative_Launcher/actions/workflows/sync-upstream.yml/badge.svg)](https://github.com/rennan101/nxnative_Launcher/actions/workflows/sync-upstream.yml)

<p align="center">
  <b>Port, configure, and play your owned Switch games natively on Apple Silicon Macs and iOS devices with 60 FPS, Apple MetalFX, Controller Remapping, Cross Save Sync, and zero command-line friction.</b>
</p>
<p align="center">
<b>It is not an emulator: the game's own code runs directly on the Apple chip, with Switch graphics rebuilt on Metal. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.<b>
</p>

<p align="center">
  <a href="#-english-documentation"><b>🇺🇸 English Documentation</b></a> • 
  <a href="#-guia-em-português-pt-br"><b>🇧🇷 Guia em Português (PT-BR)</b></a>
</p>

</div>

---

## 📑 Table of Contents / Índice Rápido

* [🎬 Live Demo (The Hook)](#live-demo)
* [⚖️ Clean-Room & Legal Policy](#zero-copyrighted-assets-distributed-strict-clean-room-policy)
* [💡 Why NxNative Launcher?](#why-nxnative-launcher)
* [✨ Key Features](#key-features)
  * [1. Console Game Library (Switch Dark Mode)](#1-console-style-game-library-switch-dark-mode)
  * [2. Custom Covers & Atmosphere Mods](#2-custom-covers-atmosphere-mods-layeredfs)
  * [3. 3-Step Guided Porting Wizard](#3-3-step-guided-porting-wizard)
  * [4. Controller Remapping & Universal Gamepad](#4-controller-remapping-universal-gamepad-navigation)
  * [5. Compatibility Matrix & RAM Filters](#5-compatibility-matrix-ram-filters-live-github-sync)
  * [6. Personalization & 8 Neon Themes](#6-personalization-8-neon-highlight-themes)
  * [7. Engine Compatibility Patches & Upstream Sync](#7-engine-compatibility-patches-upstream-synchronization)
* [⚡ Quick Start Guide](#quick-start-guide)
  * [For Gamers (Play in 60 Seconds)](#for-gamers-play-in-60-seconds)
  * [For Developers (Build from Source)](#for-developers-build-from-source)
* [🛠️ Environment Setup & Prerequisites](#environment-setup-prerequisites)
* [🚀 Step-by-Step Porting Tutorial](#step-by-step-porting-tutorial)
* [🕹️ Library Configuration & Launching](#how-to-configure-and-play-your-game)
* [🎮 Gamepad Navigation Reference](#gamepad-navigation-reference)
* [🗺️ Interactive Roadmap](#interactive-roadmap)
* [❓ Frequently Asked Questions (FAQ)](#frequently-asked-questions-faq)
* [🏛️ Upstream Architecture & Credits](#upstream-architecture-community-lineage)
* [🇧🇷 Guia em Português (PT-BR)](#guia-em-português-pt-br)
  * [Instalação de Dependências](#1-configuração-do-ambiente-e-dependências)
  * [Passo a Passo de Portabilidade](#2-passo-a-passo-como-portar-jogos)
  * [Configuração na Biblioteca, Saves e Mods](#3-configuração-na-biblioteca-saves-e-mods)
  * [Remapeamento de Controles e Como Jogar](#4-remapeamento-de-controles-e-como-jogar)
  * [Suporte e Relato de Erros](#5-suporte-e-relato-de-erros)

---

## 🎬 Live Demo

> **Show, don't tell!** Experience a console-grade desktop application with fluid cover carousels, responsive neon accents, and instantaneous 60 FPS performance toggles in full Retina clarity:

<p align="center">
  <img src="docs/images/gifs/demo_library_carousel.gif" width="100%" alt="NxNative Launcher Console Library Demo">
</p>

---

## ⚖️ ZERO COPYRIGHTED ASSETS DISTRIBUTED (Strict Clean-Room Policy)

> [!IMPORTANT]
> - **No Proprietary Keys**: This repository and application **DO NOT** contain, host, link to, or distribute any Nintendo cryptographic keys (`prod.keys`, `title.keys`), key seeds, or certificates. Users must dump their own keys from their personally owned Nintendo Switch hardware.
> - **No Games or ROMs**: **NO** game binaries, NSPs, XCIs, DLC files, game updates, or copyrighted assets are provided, distributed, or mirrored. Users must provide their own legally obtained game dumps.
> - **Independent Interoperability**: NxNative Launcher is purely an open-source graphical orchestration interface and utility tool built for Apple Silicon execution interoperability.

---

## 💡 Why NxNative Launcher?

Running Nintendo Switch games on Apple Silicon with upstream tools historically required manual terminal scripting, manual LibHac extractions, custom SPIRV-Cross flags, and hand-edited environment variables. **NxNative Launcher** brings the entire ecosystem into a seamless, console-like desktop interface.

| Feature | Raw Command-Line Porting | 🎮 NxNative Launcher (v1.6.2) |
| :--- | :---: | :---: |
| **User Interface** | Terminal Commands only | **Switch Dark Mode Console Library** |
| **Controller Remapping**| Fixed hardcoded layouts | **Custom Profiles (Switch Pro, Xbox, PS5, Keyboard)** |
| **Save Synchronization**| Hidden Application Support paths | **Cross-Device Save Sync & 1-Click Finder Reveal** |
| **Game Bundling** | Manual multi-step extraction | **Automatic Base + Update + DLC Grouping** |
| **Performance Tuning** | Manual `env.txt` edits | **1-Click 60 FPS, MetalFX & Metal HUD** |
| **Game Mods (LayeredFS)**| Manual directory navigation | **Integrated Atmosphere Mod Manager** |
| **Target Deployment** | Mac only via terminal | **Dual Target: Mac Native (.app) & iOS (.ipa)** |
| **Exit Game from Couch**| Force quit or terminal kill | **Hold Guide/Home for 3s to return to Launcher** |

---

## ✨ Key Features

### 1. Console-Style Game Library (Switch Dark Mode)
- **16:9 Hero Section & Box Art Shelf**: High-resolution panoramic art paired with authentic vertical box art (1 : 1.60 ratio).
- **Two-Tier Category Shelves**:
  - **Mac (macOS Native)**: Standalone compiled `.app` bundles running directly on Apple Silicon Metal.
  - **iPhone / iPad (iOS Port)**: Sideloadable `.ipa` packages ready for direct deployment via `xcrun devicectl` or AirDrop.
- **Cross Save & Backup**: Instantly resolves `~/Library/Application Support/nxnative/<TID>/save` with a one-click **"Saves"** button to reveal in Finder and synchronize with iOS paired devices.
- **Installed DLC & Update Detection**: Scans `<Game.app>/Contents/Resources/aoc` and displays installed expansions and bundle storage size.

<p align="center">
  <img src="docs/images/screenshots/library_game_details.png" width="90%" alt="Library Game Details & 60 FPS Target">
</p>

---

### 2. Custom Covers, Atmosphere Mods & LayeredFS
- **Custom Cover Art Picker**: Customize game artwork with any local `.jpg`, `.png`, or `.webp` file. Persisted automatically to user cache and embedded into the application bundle.
- **Atmosphere LayeredFS Mods**: Drop game mods directly into `<Game.app>/Contents/Resources/atmosphere/contents/<TitleID>/` for instant HD texture packs, translations, or physics tweaks.
- **Save File Quick Backups**: One-click direct access to save states for easy backup and synchronization.

<p align="center">
  <img src="docs/images/screenshots/game_options_mods.png" width="90%" alt="Game Options, Custom Covers & LayeredFS Mods">
</p>

---

### 3. 3-Step Guided Porting Wizard
- **Automated Game Discovery**: Drag and drop any folder containing `.nsp`, `.nsz`, `.xci`, or `.zip` archives. The wizard automatically groups base games with their corresponding updates and DLC packages.
- **Target Platform Selection**: Output double-clickable macOS applications (`.app`) or iOS packages (`.ipa`) tailored for iPhone 15 Pro and Apple Silicon iPads.
- **Live Pipeline Execution**: Real-time progress monitoring from ExeFS extraction to SPIR-V compilation and Mach-O host linking (`dmhost`).

<p align="center">
  <img src="docs/images/gifs/demo_porting_wizard.gif" width="95%" alt="3-Step Guided Porting Wizard Demo">
</p>

---

### 4. Controller Remapping & Universal Gamepad Navigation
- **Custom Button Remapping**: Rebind any controller button, trigger, or stick click with dedicated profile management (Gamepad Controller and Mac Keyboard & Mouse profiles).
- **Plug & Play Support**: Native compatibility with **Nintendo Switch** (Joy-Cons & Pro Controller), **Xbox** (Series S/X, One, Bluetooth), **PlayStation** (DualShock 4 & DualSense PS5), **8BitDo**, and MFi gamepads via Apple's native `GameController` framework.
- **Adaptive Glyphs & Haptic Audio**: On-screen prompts automatically match your connected controller (A/B/X/Y or ✕/○/□/△) with tactile mechanical audio feedback (`NSSound`).
- **3-Second Couch Exit Shortcut**: Hold the **Xbox Guide / PS / Home** button for **3 seconds** during gameplay to close the game and immediately reactivate the Launcher.

---

### 5. Compatibility Matrix (RAM Filters & Live GitHub Sync)
- **Live Upstream Sync**: Browse tested playable games synced directly with upstream compatibility reports.
- **RAM Requirement Filters**: Filter verified games instantly by Unified Memory: **All**, **4GB**, **6GB**, **8GB**, and **16GB**.
- **Hardware Requirement Badges**: Displays minimum Mac Unified Memory, iOS RAM, and required SSD storage space.

---

### 6. Personalization & 8 Neon Highlight Themes
- **8 Dynamic Accent Themes**: Switch Neon Red, Switch Neon Blue, Electric Purple, Sunset Orange, Emerald Green, Cyber Rose, Hyrule Gold, and Deep Cobalt.
- **Appearance Modes**: Light, Dark, Monochrome, and Auto (System).
- **Sound Effects Toggle**: Low-latency tactile feedback for navigation, tab changes, and game launching.

<p align="center">
  <img src="docs/images/gifs/demo_features_showcase.gif" width="95%" alt="Features, Controller Remapping & Neon Themes Demo">
</p>

---

### 7. Engine Compatibility Patches & Upstream Synchronization
- **One-Click Engine Patches**: Apply title-specific memory allocator hooks, Unreal Engine allocator gates, and 60 FPS patches (e.g. Dragon Quest VII v0 metadata and dynamic shader fallback).
- **Git Commit Diffing**: Live upstream tracking comparing your local build with the newest commits of `seedds/nxnative`.

---

## ⚡ Quick Start Guide

### 🎮 For Gamers (Play in 60 Seconds)

1. **Download the Launcher**:
   - Grab the latest **`NxNative_Launcher.dmg`** from the [GitHub Releases](https://github.com/rennan101/nxnative_Launcher/releases).
   - Open the `.dmg` and drag **NxNative_Launcher.app** into your `/Applications` folder.
2. **Provide your Decryption Keys**:
   - Place your legally dumped **`prod.keys`** in `~/.switch/prod.keys` (or select it in the **Settings** tab).
3. **Port & Play**:
   - Switch to the **Port Game** tab, drag and drop your game folder, select **Mac App (.app)**, and click **Port Game**.
   - Head to the **Library** tab, connect your gamepad, and hit **Play Game**!

---

### 🛠️ For Developers (Build from Source)

```bash
# 1. Clone this repository with submodules
git clone https://github.com/rennan101/nxnative_Launcher.git
cd nxnative_Launcher

# 2. Build the Swift GUI application
cd gui
swift build -c release

# 3. Assemble the standalone .app bundle
cd ..
bash scripts/build_gui.sh

# 4. (Optional) Generate the distributable .dmg installer
bash scripts/create_dmg.sh
```

---

## 🛠️ Environment Setup & Prerequisites

### System Requirements
* **Hardware**: Apple Silicon Mac (M1, M2, M3, M4 or later).
* **OS**: macOS 13.0 (Ventura), macOS 14.0 (Sonoma), macOS 15.0 (Sequoia) or newer.
* **Unified Memory**: 8 GB minimum (16 GB recommended for open-world titles like Zelda TotK).
* **Disk Space**: 20–50 GB free SSD space for RomFS extraction and shader caches.

### Manual Dependency Installation (Terminal)
If you prefer configuring dependencies manually:
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install Homebrew dependencies
brew install python@3.11 cmake ninja molten-vk libzip lz4 zstd spirv-cross

# Install required Python packages
pip3 install pycryptodome libhac
```

---

## 🚀 Step-by-Step Porting Tutorial

```mermaid
flowchart LR
    A["📁 Game Files<br/>(NSP / NSZ / XCI)"] --> B["⚡ Step 1: Select Game<br/>(Auto Base + Updates + DLC)"]
    B --> C["⚙️ Step 2: Target Config<br/>(Mac .app or iOS .ipa)"]
    C --> D["🔨 Step 3: Compile<br/>(LibHac, Metal MSL, dmhost)"]
    D --> E["🎮 Native Game Ready<br/>(Play in Library)"]
```

### Step 1: Drag & Drop Game Dumps
Drop your folder containing the base game, newest update, and DLCs. The launcher automatically matches Title IDs and verifies disk space:

<p align="center">
  <img src="docs/images/screenshots/port_step1_select_game.png" width="90%" alt="Step 1: Game Selection">
</p>

### Step 2: Configure Target & Enhancements
Select **Mac App (.app)** or **iOS IPA (.ipa)**, choose your destination folder, and enable **60 FPS Unlock** and **MetalFX Spatial Upscaling**:

<p align="center">
  <img src="docs/images/screenshots/port_step2_configure.png" width="90%" alt="Step 2: Platform Configuration">
</p>

### Step 3: Multi-Stage Build & macOS Notification
Watch real-time extraction, shader compilation, and host linking. A native macOS banner notification will chime when compilation finishes:

<p align="center">
  <img src="docs/images/screenshots/console_logs.png" width="90%" alt="Step 3: Live Compilation Logs">
</p>

---

## 🕹️ How to Configure and Play Your Game

1. **Launch from the Library**:
   - Highlight your game in the carousel using your gamepad or arrow keys, then press **`A` / `✕`** or **Enter**.
2. **Launch from Finder**:
   - The compiled game is a native macOS application bundle (`.app`). Simply double-click `<GameName>.app` in your destination folder!
3. **Per-Game Settings (`env.txt`)**:
   - Customize Framerate (30 vs 60 FPS Unlocked), Docked Mode (1080p) vs Handheld (720p), MetalFX Upscaling, and Metal HUD.
4. **Custom Box Art & Mods**:
   - Click **Options** to set custom covers or open the LayeredFS mods folder.

---

## 🎮 Gamepad Navigation Reference

| Action | Nintendo Switch | Xbox | PlayStation | Keyboard Shortcut |
| :--- | :---: | :---: | :---: | :---: |
| **Navigate Carousel / Lists** | D-Pad / Left Stick | D-Pad / Left Stick | D-Pad / Left Stick | Arrow Keys |
| **Play Game / Confirm** | **`A`** | **`A`** | **`✕`** | Return (Enter) |
| **Cancel / Return to Shelf** | **`B`** | **`B`** | **`○`** | Escape |
| **Open Options & Mods Deck** | **`X`** | **`X`** | **`□`** | `O` |
| **Open Save Files in Finder** | **`Y`** | **`Y`** | **`△`** | `S` |
| **Previous / Next Tab** | **`L` / `R`** | **`LB` / `RB`** | **`L1` / `R1`** | `[` / `]` |
| **Open Settings Menu** | **`+` (Plus)** | **Menu** | **Options** | `,` (Comma) |
| **Quick-Exit Game (Couch Mode)**| Hold **Home** (3s) | Hold **Guide** (3s) | Hold **PS** (3s) | `Cmd + Q` |

---

## 🗺️ Interactive Roadmap

Track our shipped milestones and upcoming features! Star the repository to follow development:

### ✅ Completed Milestones

- [x] **v1.0.0 — Foundation & Native Metal Execution**:
  - Direct Mach-O ARM64 rewriting and compilation on Apple Silicon.
  - Native Metal Shading Language (`.metal`) pipeline translation via SPIRV-Cross.
  - Horizon OS kernel service simulation & LibHac extraction.
  - Initial Swift graphical user interface.
- [x] **v1.4.0 — Switch Dark Mode UI & Authentic Cover Art**:
  - Full UI redesign inspired by the Nintendo Switch Dark Mode & CrossOver.
  - Panoramic 16:9 Hero section with dynamic specular neon rim lighting.
  - Official retail portrait box art (1 : 1.60 ratio) and dynamic game carousel.
  - Personalization tab with 8 dynamic neon highlight themes.
  - Compatibility Matrix with RAM requirement filters (4GB, 6GB, 8GB, 16GB).
- [x] **v1.5.0 — Dual-Tier Library & Pre-Flight Security**:
  - Two-tier library organization: Mac Native (`.app`) and iPhone / iPad (`.ipa`).
  - Automatic game bundling (Base + Update + DLCs) with disk space pre-flight calculation.
  - Cryptographic `KeyCheckService` validating `prod.keys` and NCA headers with AES-128-XTS.
  - Atmosphere LayeredFS mod integration (`<Game.app>/Contents/Resources/atmosphere/contents/<TID>/`).
- [x] **v1.5.1 — iOS Device Deployment & AirDrop**:
  - Real-time streaming progress feedback for iOS deployments via `xcrun devicectl`.
  - Native AirDrop sharing support for exported `.ipa` packages.
  - Deadlock prevention and background task safety.
- [x] **v1.6.0, v1.6.1 & v1.6.2 (Current Version) — Controller Remapping, Save Sync & Robust Environment Setup**:
  - **Full Controller Remapping**: Custom button mapping for Switch Pro, Joy-Cons, Xbox Series/One, and PS5 DualSense controllers with profile duplication and reset.
  - **Save Synchronization Service (`SaveSyncService`)**: Bidirectional save state synchronization and backup between Mac and paired iOS devices.
  - **Custom Cover Art**: User-customizable box art picker (`.jpg`, `.png`, `.webp`) with instant persistence and fallback reset.
  - **Automated Environment Diagnostics & Python 3.12+ / Pip Fix**: One-click sequential Homebrew and build dependency installer in Settings tab, with dynamic Python 3.12+ resolution and safe pip package installation.
  - **3-Second Couch Exit**: Global 3-second hold shortcut on Xbox Guide / PS / Home buttons to close running games and refocus the Launcher.

---

### ⏳ In Active Development & Future Milestones

- [ ] **v1.7.0 — Cross Save iCloud Sync**: Automated cloud save synchronization across macOS and iOS devices via iCloud Drive / CloudKit.
- [ ] **v1.7.0 — MetalFX Temporal Upscaler**: Temporal anti-aliasing and dynamic temporal scaling for demanding 3D games.
- [ ] **v1.8.0 — Haptic Feedback & Gamepad Vibration**: DualSense adaptive triggers & Switch Pro linear resonant actuator rumble.
- [ ] **v1.8.0 — Discord Rich Presence**: Real-time rich presence showing active title, in-game playtime, and FPS counters.

---

## ❓ Frequently Asked Questions (FAQ)

<details>
<summary><b>1. Why do I need <code>prod.keys</code> and where do I get them?</b></summary>
Nintendo Switch software binaries and RomFS containers are encrypted with cryptographic keys unique to the platform. To legally decrypt and run code you own, you must dump your keys from your personal Nintendo Switch console using tools like <a href="https://github.com/shchmue/Lockpick_RCM">Lockpick_RCM</a>. Place the file at <code>~/.switch/prod.keys</code>.
</details>

<details>
<summary><b>2. How does 60 FPS work without game speed-up bugs?</b></summary>
The launcher injects <code>DM_FPS_TARGET=60</code> via <code>env.txt</code>, interfacing with the engine's internal frame-stepping hooks (such as <code>fstep_tick</code> in Zelda titles) to uncap the framerate while maintaining normal game physics and audio timing.
</details>

<details>
<summary><b>3. Can I transfer my existing save files from a physical Switch or Ryujinx?</b></summary>
Yes! Saves are stored in plain unencrypted Horizon save formats at <code>~/Library/Application Support/nxnative/&lt;TitleID&gt;/save</code>. Simply click the <b>Saves</b> button in the launcher to reveal the directory in Finder and paste your save files.
</details>

<details>
<summary><b>4. How does Save Sync work between Mac and iOS devices?</b></summary>
NxNative Launcher uses <code>SaveSyncService</code> to detect paired iOS devices via <code>xcrun devicectl</code>. You can back up and sync save games between your Mac and iPhone 15 Pro / iPad with one click in the game options deck. Full automatic <b>Cross Save iCloud Sync</b> is currently on the roadmap for v1.7.0!
</details>

<details>
<summary><b>5. Does this run via Rosetta 2 or an emulator?</b></summary>
No! Nintendo Switch games use ARM64 binaries (AArch64). Apple Silicon processors (M1-M4) run ARM64 instructions natively. NxNative recompiles and translates Horizon OS syscalls and Maxwell graphics into native macOS Mach-O binaries and Apple Metal Shading Language (`.metal`), resulting in near-native performance.
</details>

---

## 🏛️ Upstream Architecture & Community Lineage

This launcher stands on the shoulders of brilliant software engineering across the open-source community:

| Project | Author / Team | License | Role in NxNative |
| :--- | :--- | :---: | :--- |
| **[nxnative](https://github.com/seedds/nxnative)** | **seedds** | MIT | Core engine, NVN/Vulkan/OpenGL-to-Metal host, ARM64 NSO rewriter, shader library prebuilds. |
| **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** | jhaizhou-ops | MIT | Original native execution approach for Switch code on Apple Silicon. |
| **[Ryujinx](https://github.com/Ryujinx/Ryujinx)** | Ryujinx Team | MIT | Maxwell shader translator (`Ryujinx.Graphics.Shader`) and kernel synchronization reference. |
| **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)** | SciresM & Team | GPLv2 | Mesosphère kernel synchronization semantics (address arbiter, condition variables). |
| **[LibHac](https://github.com/Thealexbarney/LibHac)** | Thealexbarney | MIT | NSP extraction and PFS/HFS layered filesystem handling (`dm/tools/nx_extract`). |
| **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** | Khronos Group | Apache-2.0 | SPIR-V to Metal Shading Language compilation. |
| **deko3d** | fincs | zlib | Low-level texture layout size calculation algorithm. |
| **yuzu** | yuzu Team | GPLv2 | Maxwell instruction encodings reference. |

---

# 🇧🇷 Guia em Português (PT-BR)

Bem-vindo ao guia oficial em português do **NxNative Launcher (v1.6.2)**! Esta seção contém tudo o que você precisa para configurar o ambiente, portar seus jogos, personalizar sua biblioteca, sincronizar saves e jogar com controles no Apple Silicon.

### 📑 Índice do Guia em Português
- [1. Configuração do Ambiente e Dependências](#1-configuração-do-ambiente-e-dependências)
- [2. Passo a Passo: Como Portar Jogos](#2-passo-a-passo-como-portar-jogos)
- [3. Configuração na Biblioteca, Saves e Mods](#3-configuração-na-biblioteca-saves-e-mods)
- [4. Remapeamento de Controles e Como Jogar](#4-remapeamento-de-controles-e-como-jogar)
- [5. Suporte e Relato de Erros](#5-suporte-e-relato-de-erros)

---

### 1. Configuração do Ambiente e Dependências

#### A. Requisitos de Sistema
- Mac com processador **Apple Silicon** (M1, M2, M3, M4 ou superior).
- macOS 13.0 (Ventura), 14.0 (Sonoma) ou 15.0 (Sequoia).
- Chaves de descriptografia **`prod.keys`** extraídas do seu próprio Nintendo Switch via Lockpick_RCM (salve em `~/.switch/prod.keys`).

#### B. Instalação Automática (Pelo Próprio Launcher)
1. Abra o **NxNative_Launcher.app**.
2. Vá até a aba **Settings** (pressione `6` ou use o botão `R` no controle).
3. Na seção de diagnóstico, clique em **"Install Missing"** ou **"Repair All"** para que o aplicativo instale os compiladores e ferramentas sequencialmente em segundo plano.

#### C. Instalação Manual via Terminal
Se preferir instalar manualmente pelo terminal:
```bash
# 1. Instalar as ferramentas de linha de comando do Xcode
xcode-select --install

# 2. Instalar dependências via Homebrew
brew install python@3.11 cmake ninja molten-vk libzip lz4 zstd spirv-cross

# 3. Instalar pacotes Python para extração de arquivos do Switch
pip3 install pycryptodome libhac
```

---

### 2. Passo a Passo: Como Portar Jogos

1. **Passo 1: Selecionar Arquivos do Jogo**:
   - Reúna os arquivos do seu jogo em uma pasta (Jogo Base `.nsp`/`.xci`, Updates e DLCs).
   - Na aba **Port Game**, arraste a pasta para a área indicada.
   - O launcher agrupa os arquivos automaticamente, calcula o espaço necessário em disco e valida a integridade. Clique no card do jogo para confirmar.
2. **Passo 2: Configurar Plataforma de Destino**:
   - Escolha **Mac App (.app)** para rodar no macOS ou **iOS / iPadOS (.ipa)** para iPhone 15 Pro / iPad.
   - Defina a pasta de saída (padrão: `Nx Ports/`).
   - Ative o **60 FPS Unlock** e o **Apple MetalFX Spatial Upscaling**.
   - Clique em **"Port Game"**.
3. **Passo 3: Compilação e Notificação**:
   - O pipeline extrai o RomFS/ExeFS, traduz os shaders Maxwell para SPIR-V e Metal Shading Language (`.metal`), e compila o binário Mach-O.
   - Uma **notificação nativa do macOS com som e banner** avisará assim que o jogo estiver pronto!

---

### 3. Configuração na Biblioteca, Saves e Mods

1. **Capas Customizadas**:
   - Na aba **Library**, selecione o jogo e clique no botão **Options** (ou tecla `O` / botão `X` no controle).
   - Clique em **"Choose Custom Cover..."** e selecione qualquer imagem do seu Mac (`.jpg`, `.png`, `.webp`).
2. **Desempenho e 60 FPS por Jogo**:
   - Configure individualmente cada jogo via `env.txt`: Framerate Target (30 FPS vs 60 FPS Unlocked), Docked Mode (1080p) vs Handheld (720p), MetalFX Upscaling e painel Metal Performance HUD.
3. **Sincronização de Saves (Cross Save)**:
   - Os arquivos de progresso ficam gravados em:
     `~/Library/Application Support/nxnative/<TitleID>/save/`
   - Clique no botão **Saves** no launcher para abrir a pasta diretamente no Finder ou sincronizar com aparelhos iOS emparelhados via `SaveSyncService`. O **Cross Save iCloud Sync** automático está em desenvolvimento para a v1.7.0!
4. **Mods de Jogo (LayeredFS / Atmosphere)**:
   - No painel de opções, acesse a pasta de mods: `<Game.app>/Contents/Resources/atmosphere/contents/<TitleID>/`.
   - Cole suas texturas HD, mods de física ou traduções em PT-BR.

---

### 4. Remapeamento de Controles e Como Jogar

- **Remapeamento Completo de Botões**:
  - Na aba **Controls**, você pode customizar cada botão do seu Switch Pro Controller, Joy-Cons, Xbox ou PS5 DualSense, além de configurar perfis para Teclado e Mouse.
- **Como Abrir o Jogo**:
  - Pelo Launcher: Aperte **`A` / `✕`** ou a tecla **Enter** sobre o jogo selecionado no carrossel.
  - Pelo Finder: Dê duplo clique no aplicativo autônomo `<NomeDoJogo>.app` na sua pasta `Nx Ports/`.
- **Atalho do Sofá para Sair**: Segure o botão central do seu controle (**Guide / PS / Home**) por **3 segundos** durante a gameplay para fechar o jogo com segurança e voltar ao Launcher!

---

### 5. Suporte e Relato de Erros

- 🚨 **Relatar Bug:** [Abrir Bug Report](https://github.com/rennan101/nxnative_Launcher/issues/new?template=bug_report.yml) ou participe do [Support Hub (Ticket #1)](https://github.com/rennan101/nxnative_Launcher/issues/1).
- 🎮 **Relatar Jogo Testado:** [Enviar Compatibilidade](https://github.com/rennan101/nxnative_Launcher/issues/new?template=game_compatibility.yml) ou consulte o [Compatibility Hub (Ticket #2)](https://github.com/rennan101/nxnative_Launcher/issues/2).
- 💡 **Sugerir Ideias:** [Sugerir Nova Feature](https://github.com/rennan101/nxnative_Launcher/issues/new?template=feature_request.yml).
- 🔄 **Sincronização Upstream:** O repositório possui uma GitHub Action que sincroniza automaticamente com as novidades de `seedds/nxnative`.

---

## ⚖️ Legal Disclaimer & Fair Use Notice

- **NxNative Launcher** is an open-source research and educational utility designed for interoperability and native software execution on Apple Silicon.
- **NO PROPRIETARY ASSETS DISTRIBUTED**: This software contains **no copyrighted code, binary assets, game data, or cryptographic keys** from Nintendo or any third-party game publishers.
- **BRING YOUR OWN KEYS & GAMES**: Users must supply their own legally obtained `prod.keys` dumped from their personal Nintendo Switch hardware, as well as their own game files dumped from cartridges or digital purchases they legally own.
- **TRADEMARKS**: Nintendo Switch is a registered trademark of Nintendo Co., Ltd. NxNative Launcher is an independent open-source project and is not affiliated with, endorsed by, or sponsored by Nintendo.
