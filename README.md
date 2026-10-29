<div align="center">

<a href="https://github.com/rennan101/nxnative_Launcher">
  <img src="docs/images/banner.png" width="100%" alt="NxNative Launcher Banner">
</a>

# NxNative Launcher

### Open-Source nxnative Graphical User Interface Tool for Apple Silicon

[![Platform](https://img.shields.io/badge/platform-macOS%2013%2B-blue.svg)](https://apple.com/macos)
[![Architecture](https://img.shields.io/badge/arch-Apple%20Silicon%20(M1%2B%20%26%20A17%2B)-orange.svg)](https://apple.com)
[![Swift](https://img.shields.io/badge/Swift-5.9%2B-FA7343.svg)](https://swift.org)
[![Version](https://img.shields.io/badge/version-v1.8.1-purple.svg)](https://github.com/rennan101/nxnative_Launcher/releases)
[![Controls](https://img.shields.io/badge/Controls-3D%20Mouse%20Look-blueviolet.svg)](#)
[![Appearance](https://img.shields.io/badge/UI-Liquid%20Glass-00E5FF.svg)](#)
[![Multi-Language](https://img.shields.io/badge/Languages-13%20Locales-34D399.svg)](#)
[![Verified Games](https://img.shields.io/badge/Verified%20Games-111%2B%20Titles-success.svg)](#5-compatibility-matrix-ram-filters--live-github-sync)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Core Engine](https://img.shields.io/badge/core%20engine-seedds%2Fnxnative-informational.svg)](https://github.com/seedds/nxnative)
[![Report Issues](https://img.shields.io/badge/Issues-Support%20%26%20Bugs-red.svg)](https://github.com/rennan101/nxnative_Launcher/issues)
[![Sync Upstream](https://github.com/rennan101/nxnative_Launcher/actions/workflows/sync-upstream.yml/badge.svg)](https://github.com/rennan101/nxnative_Launcher/actions/workflows/sync-upstream.yml)

<p align="center">
  <b>Port, configure, and play your owned Switch games natively on Apple Silicon Macs and iOS devices with 60 FPS, Apple MetalFX, Controller Remapping, 3D Mouse Look Camera Control, Cross Save Sync, and zero command-line friction.</b>
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

* [🎬 Live Demo (The Hook)](#-live-demo)
* [⚖️ Clean-Room & Legal Policy](#-zero-copyrighted-assets-distributed-strict-clean-room-policy)
* [💡 Why NxNative Launcher?](#-why-nxnative-launcher)
* [✨ Key Features](#-key-features)
  * [1. Console Game Library (Switch Dark Mode)](#1-console-style-game-library)
  * [2. Custom Covers & Atmosphere Mods](#2-custom-covers-atmosphere-mods--layeredfs)
  * [3. 3-Step Guided Porting Wizard](#3-3-step-guided-porting-wizard)
  * [4. Controller Remapping & Universal Gamepad](#4-controller-remapping--universal-gamepad-navigation)
  * [5. Compatibility Matrix & RAM Filters](#5-compatibility-matrix-ram-filters--live-github-sync)
  * [6. Personalization & 8 Neon Themes](#6-personalization--8-neon-highlight-themes)
  * [7. Engine Compatibility Patches & Upstream Sync](#7-engine-compatibility-patches--upstream-synchronization)
* [⚡ Quick Start Guide](#-quick-start-guide)
  * [For Gamers (Play in 60 Seconds)](#-for-gamers-play-in-60-seconds)
  * [For Developers (Build from Source)](#-for-developers-build-from-source)
* [🛠️ Environment Setup & Prerequisites](#-environment-setup--prerequisites)
* [🚀 Step-by-Step Porting Tutorial](#-step-by-step-porting-tutorial)
* [🕹️ Library Configuration & Launching](#-how-to-configure-and-play-your-game)
* [🎮 Gamepad Navigation Reference](#-gamepad-navigation-reference)
* [🗺️ Interactive Roadmap](#-interactive-roadmap)
* [❓ Frequently Asked Questions (FAQ)](#-frequently-asked-questions-faq)
* [🏛️ Upstream Architecture & Credits](#-upstream-architecture--community-lineage)
* [🇧🇷 Guia em Português (PT-BR)](#-guia-em-português-pt-br)
  * [Instalação de Dependências](#1-configuração-do-ambiente-e-dependências)
  * [Passo a Passo de Portabilidade](#2-passo-a-passo-como-portar-jogos)
  * [Configuração na Biblioteca, Saves e Mods](#3-configuração-na-biblioteca-saves-e-mods)
  * [Remapeamento de Controles e Como Jogar](#4-remapeamento-de-controles-e-como-jogar)
  * [Suporte e Relato de Erros](#5-suporte-e-relato-de-erros)

---

## 🎬 Live Demo

> Experience a console-grade desktop application with fluid cover carousels, responsive neon accents, and instantaneous 60 FPS performance toggles in full Retina clarity:

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

| Feature | Raw Command-Line Porting | 🎮 NxNative Launcher (v1.7.0) |
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

### 1. Console-Style Game Library
- **16:9 Hero Section & Box Art Shelf**: High-resolution panoramic art paired with authentic vertical box art (1 : 1.60 ratio).
- **Two-Tier Category Shelves**:
  - **Mac (macOS Native)**: Standalone compiled `.app` bundles running directly on Apple Silicon Metal.
  - **iPhone / iPad (iOS Port)**: Sideloadable `.ipa` packages ready for direct deployment via `xcrun devicectl` or AirDrop.
- **Cross Save & iCloud Sync**: Instantly resolves `~/Library/Application Support/nxnative/<TID>/save` with a one-click **"Saves"** button to reveal in Finder and synchronize with iOS paired devices.
- **Installed DLC & Update Detection**: Scans `<Game.app>/Contents/Resources/aoc` and displays installed expansions and bundle storage size.

<p align="center">
  <img src="docs/images/screenshots/library_hero.png" width="95%" alt="Library Hero View & Mac Native Category Shelf">
</p>
<p align="center">
  <img src="docs/images/screenshots/library_ios_airdrop.png" width="95%" alt="iPhone / iPad iOS Port Shelf with Direct AirDrop Sideloading">
</p>
<p align="center">
  <img src="docs/images/screenshots/library_game_details.png" width="95%" alt="Game Performance Options & 60 FPS Target">
</p>

---

### 2. Custom Covers, Atmosphere Mods & LayeredFS
- **Custom Cover Art Picker**: Customize game artwork with any local `.jpg`, `.png`, or `.webp` file. Persisted automatically to user cache and embedded into the application bundle.
- **Atmosphere LayeredFS Mods**: Drop game mods directly into `<Game.app>/Contents/Resources/atmosphere/contents/<TitleID>/` for instant HD texture packs, translations, or physics tweaks.
- **Save File Quick Backups & Device Sync**: One-click direct access to save states and single-button sync with paired iPhones/iPads.

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
<p align="center">
  <img src="docs/images/screenshots/port_step1_select_game.png" width="90%" alt="Step 1: Automated Game Discovery">
</p>

---

### 4. Controller Remapping, Universal Gamepad & 3D Mouse Look
- **3D Mouse Look / Right Stick Camera Emulation (macOS)**:
  - **PC-Style First/Third Person Camera**: Direct mouse look camera control for 3D exploration and action titles (*The Legend of Zelda: Tears of the Kingdom*, *Breath of the Wild*, *Super Mario Odyssey*, *NieR:Automata*, etc.).
  - **Deadzone Compensation (+6,000 Units)**: Overcomes in-engine analog stick deadzones instantly upon movement, ensuring immediate camera response without initial lag or dead intervals.
  - **Multi-Stage Decay Filter**: Simulates physical thumbstick return-to-center physics for natural decelerating camera motion.
  - **Sensitivity & Y-Axis Inversion**: Configurable multiplier with vertical Y-inversion switch and hardware cursor locking (`ESC` releases cursor anytime).
- **Gamepad Button Remapping**: Rebind any controller button, trigger, or stick click with dedicated profile management (Switch Pro, Xbox, PS5 DualSense, and Mac Keyboard & Mouse).
- **Real-Time Stick Drift Calibration**: Visual live stick monitor displays current X/Y deflections and deadzone boundaries to eliminate involuntary movement.
- **Plug & Play Support**: Native compatibility with **Nintendo Switch** (Joy-Cons & Pro Controller), **Xbox** (Series S/X, One, Bluetooth), **PlayStation** (DualShock 4 & DualSense PS5), **8BitDo**, and MFi gamepads via Apple's native `GameController` framework.
- **3-Second Couch Exit Shortcut**: Hold the **Xbox Guide / PS / Home** button for **3 seconds** during gameplay to close the game and immediately reactivate the Launcher.

<p align="center">
  <img src="docs/images/screenshots/controls_mapping.png" width="48%" alt="Controller Remapping & Real-Time Drift Calibration">
  <img src="docs/images/screenshots/controls_mouse_look.png" width="48%" alt="Mac Keyboard Mapping & 3D Mouse Look Camera Settings">
</p>

---

### 5. Compatibility Matrix (RAM Filters & Live GitHub Sync)
- **Live Upstream Sync**: Browse 111+ tested playable games synced directly with upstream compatibility reports.
- **RAM Requirement Filters**: Filter verified games instantly by Unified Memory: **All (111)**, **4GB (24)**, **6GB (76)**, **8GB (92)**, and **16GB (111)**.
- **iPhone Chip Tiering**: Filter games by minimum required iOS silicon: **A15**, **A16**, **A17 Pro**, **A18**, and **A18 Pro**.
- **Hardware Requirement Badges**: Displays minimum Mac Unified Memory, iOS RAM, and required SSD storage space.

<p align="center">
  <img src="docs/images/screenshots/compatibility_matrix.png" width="90%" alt="Compatibility Matrix & Live RAM Filters">
</p>

<!-- gallery: written by dm/tools/docs_gallery.py from docs/images/gallery.tsv; change the table, then run it -->
<table>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/mario-kart-8-deluxe.jpg" width="256" alt="Mario Kart 8 Deluxe"><br><small><b><a href="dm/games/0100152000022000/README.md">Mario Kart 8 Deluxe</a></b><br>no Mii heads</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/mario-vs-donkey-kong.jpg" width="256" alt="Mario vs. Donkey Kong"><br><small><b><a href="dm/games/0100B99019412000/README.md">Mario vs. Donkey Kong</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/paper-mario-the-thousand-year-door.jpg" width="256" alt="Paper Mario: The Thousand-Year Door"><br><small><b><a href="dm/games/0100ECD018EBE000/README.md">Paper Mario: The Thousand-Year Door</a></b><br>60 fps with 1.0.1</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-3d-world.jpg" width="256" alt="Super Mario 3D World + Bowser's Fury"><br><small><b><a href="dm/games/010028600EBDA000/README.md">Super Mario 3D World + Bowser's Fury</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-bros-wonder.jpg" width="256" alt="Super Mario Bros. Wonder"><br><small><b><a href="dm/games/010015100B514000/README.md">Super Mario Bros. Wonder</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-galaxy.jpg" width="256" alt="Super Mario Galaxy"><br><small><b><a href="dm/games/010099C022B96000/README.md">Super Mario Galaxy</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-galaxy-2.jpg" width="256" alt="Super Mario Galaxy 2"><br><small><b><a href="dm/games/0100FD8022DAA000/README.md">Super Mario Galaxy 2</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-odyssey.jpg" width="256" alt="Super Mario Odyssey"><br><small><b><a href="dm/games/0100000000010000/README.md">Super Mario Odyssey</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/zelda-breath-of-the-wild.jpg" width="256" alt="The Legend of Zelda: Breath of the Wild"><br><small><b><a href="dm/games/01007EF00011E000/README.md">The Legend of Zelda: Breath of the Wild</a></b><br>60 fps with 1.9.0</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/zelda-links-awakening.jpg" width="256" alt="The Legend of Zelda: Link's Awakening"><br><small><b><a href="dm/games/01006BB00C6F0000/README.md">The Legend of Zelda: Link's Awakening</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/zelda-tears-of-the-kingdom.jpg" width="256" alt="The Legend of Zelda: Tears of the Kingdom"><br><small><b><a href="dm/games/0100F2C0115B6000/README.md">The Legend of Zelda: Tears of the Kingdom</a></b><br>60 fps with 1.4.3</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/13-sentinels.jpg" width="256" alt="13 Sentinels: Aegis Rim"><br><small><b><a href="dm/games/01003FC01670C000/README.md">13 Sentinels: Aegis Rim</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/absolum.jpg" width="256" alt="Absolum"><br><small><b><a href="dm/games/0100A83022A22000/README.md">Absolum</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/ace-attorney-investigations.jpg" width="256" alt="Ace Attorney Investigations Collection"><br><small><b><a href="dm/games/010005501E68C000/README.md">Ace Attorney Investigations Collection</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/advance-wars-1-2-re-boot-camp.jpg" width="256" alt="Advance Wars 1+2: Re-Boot Camp"><br><small><b><a href="dm/games/0100300012F2A000/README.md">Advance Wars 1+2: Re-Boot Camp</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/ai-the-somnium-files.jpg" width="256" alt="AI: THE SOMNIUM FILES"><br><small><b><a href="dm/games/010089B00D09C000/README.md">AI: THE SOMNIUM FILES</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/ai-the-somnium-files-nirvana-initiative.jpg" width="256" alt="AI: THE SOMNIUM FILES - nirvanA Initiative"><br><small><b><a href="dm/games/0100713016CCC000/README.md">AI: THE SOMNIUM FILES - nirvanA Initiative</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/animal-crossing-new-horizons.jpg" width="256" alt="Animal Crossing: New Horizons"><br><small><b><a href="dm/games/01006F8002326000/README.md">Animal Crossing: New Horizons</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/animal-well.png" width="256" alt="ANIMAL WELL"><br><small><b><a href="dm/games/010020D01AD24000/README.md">ANIMAL WELL</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/astral-chain.png" width="256" alt="ASTRAL CHAIN"><br><small><b><a href="dm/games/01007300020FA000/README.md">ASTRAL CHAIN</a></b><br>60 fps target with 1.0.1</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/balatro.jpg" width="256" alt="Balatro"><br><small><b><a href="dm/games/0100CD801CE5E000/README.md">Balatro</a></b><br>played by touch</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/ball-x-pit.jpg" width="256" alt="BALL x PIT"><br><small><b><a href="dm/games/010086A022444000/README.md">BALL x PIT</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/bayonetta.jpg" width="256" alt="Bayonetta"><br><small><b><a href="dm/games/010076F0049A2000/README.md">Bayonetta</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/bayonetta-2.jpg" width="256" alt="Bayonetta 2"><br><small><b><a href="dm/games/01007960049A0000/README.md">Bayonetta 2</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/bayonetta-3.jpg" width="256" alt="Bayonetta 3"><br><small><b><a href="dm/games/01004A4010FEA000/README.md">Bayonetta 3</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/big-brain-academy-brain-vs-brain.jpg" width="256" alt="Big Brain Academy: Brain vs. Brain"><br><small><b><a href="dm/games/0100620012D6E000/README.md">Big Brain Academy: Brain vs. Brain</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/brain-memory-training-academy.jpg" width="256" alt="Brain Memory Training Academy"><br><small><b><a href="dm/games/0100F5D01DA3A000/README.md">Brain Memory Training Academy</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/captain-toad-treasure-tracker.jpg" width="256" alt="Captain Toad: Treasure Tracker"><br><small><b><a href="dm/games/01009BF0072D4000/README.md">Captain Toad: Treasure Tracker</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/catherine-full-body.jpg" width="256" alt="Catherine: Full Body"><br><small><b><a href="dm/games/0100BF00112C0000/README.md">Catherine: Full Body</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/celeste.jpg" width="256" alt="Celeste"><br><small><b><a href="dm/games/01002B30028F6000/README.md">Celeste</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/cuphead.jpg" width="256" alt="Cuphead"><br><small><b><a href="dm/games/0100A5C00D162000/README.md">Cuphead</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/dark-souls-remastered-creation.jpg" width="256" alt="DARK SOULS: REMASTERED"><br><small><b><a href="dm/games/01004AB00A260000/README.md">DARK SOULS: REMASTERED</a></b><br>60 fps with 1.0.3</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/digimon-story-cyber-sleuth.jpg" width="256" alt="Digimon Story Cyber Sleuth: Complete Edition"><br><small><b><a href="dm/games/010014E00DB56000/README.md">Digimon Story Cyber Sleuth: Complete Edition</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/donkey-kong-country-tropical-freeze.jpg" width="256" alt="Donkey Kong Country: Tropical Freeze"><br><small><b><a href="dm/games/0100C1F0051B6000/README.md">Donkey Kong Country: Tropical Freeze</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/dragon-quest-vii-reimagined.jpg" width="256" alt="DRAGON QUEST VII Reimagined"><br><small><b><a href="dm/games/0100A9D01C446000/README.md">DRAGON QUEST VII Reimagined</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/dragon-quest-xi-s.jpg" width="256" alt="DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition"><br><small><b><a href="dm/games/01006C300E9F0000/README.md">DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition</a></b><br>60 fps with 1.0.4</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/final-fantasy-resonance.jpg" width="256" alt="FINAL FANTASY RESONANCE"><br><small><b><a href="dm/games/0100606026F12000/README.md">FINAL FANTASY RESONANCE</a></b><br>60 fps with 1.0.1</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/final-fantasy-tactics.jpg" width="256" alt="FINAL FANTASY TACTICS - The Ivalice Chronicles"><br><small><b><a href="dm/games/010038B015560000/README.md">FINAL FANTASY TACTICS - The Ivalice Chronicles</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/fire-emblem-engage.jpg" width="256" alt="Fire Emblem Engage"><br><small><b><a href="dm/games/0100A6301214E000/README.md">Fire Emblem Engage</a></b><br>60 fps with 2.0.0</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/fire-emblem-three-houses.jpg" width="256" alt="Fire Emblem: Three Houses"><br><small><b><a href="dm/games/010055D009F78000/README.md">Fire Emblem: Three Houses</a></b><br>60 fps with 1.2.0</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/ghost-trick-phantom-detective.jpg" width="256" alt="Ghost Trick: Phantom Detective"><br><small><b><a href="dm/games/010029B018432000/README.md">Ghost Trick: Phantom Detective</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/the-great-ace-attorney-chronicles.jpg" width="256" alt="The Great Ace Attorney Chronicles"><br><small><b><a href="dm/games/010036E00FB20000/README.md">The Great Ace Attorney Chronicles</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/hades.jpg" width="256" alt="Hades"><br><small><b><a href="dm/games/0100535012974000/README.md">Hades</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/hades-ii.jpg" width="256" alt="Hades II"><br><small><b><a href="dm/games/0100A00019DE0000/README.md">Hades II</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/hollow-knight-silksong.jpg" width="256" alt="Hollow Knight: Silksong"><br><small><b><a href="dm/games/010013C00E930000/README.md">Hollow Knight: Silksong</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/hyrule-warriors-age-of-calamity.jpg" width="256" alt="Hyrule Warriors: Age of Calamity"><br><small><b><a href="dm/games/01002B00111A2000/README.md">Hyrule Warriors: Age of Calamity</a></b><br>60 fps with 1.3.0</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/kirby-and-the-forgotten-land.jpg" width="256" alt="Kirby and the Forgotten Land"><br><small><b><a href="dm/games/01004D300C5AE000/README.md">Kirby and the Forgotten Land</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/luigis-mansion-3.jpg" width="256" alt="Luigi's Mansion 3"><br><small><b><a href="dm/games/0100DCA0064A6000/README.md">Luigi's Mansion 3</a></b><br>60 fps with 1.4.0</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/metroid-dread.jpg" width="256" alt="Metroid Dread"><br><small><b><a href="dm/games/010093801237C000/README.md">Metroid Dread</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/monster-hunter-rise.jpg" width="256" alt="Monster Hunter Rise"><br><small><b><a href="dm/games/0100B04011742000/README.md">Monster Hunter Rise</a></b><br>60 fps with 16.0.2</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/monster-train-2.jpg" width="256" alt="Monster Train 2"><br><small><b><a href="dm/games/010051701FB46000/README.md">Monster Train 2</a></b><br>60 fps with 1.1.1</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/octopath-traveler.jpg" width="256" alt="OCTOPATH TRAVELER"><br><small><b><a href="dm/games/010057D006492000/README.md">OCTOPATH TRAVELER</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/octopath-traveler-0.jpg" width="256" alt="Octopath Traveler 0"><br><small><b><a href="dm/games/01005270232F2000/README.md">Octopath Traveler 0</a></b><br>60 fps with 1.0.5</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/octopath-traveler-ii.jpg" width="256" alt="Octopath Traveler II"><br><small><b><a href="dm/games/0100A3501946E000/README.md">Octopath Traveler II</a></b><br>60 fps with 1.1.1</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/patricks-parabox.jpg" width="256" alt="Patrick's Parabox"><br><small><b><a href="dm/games/0100E44016C22000/README.md">Patrick's Parabox</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/persona-5-royal.jpg" width="256" alt="Persona 5 Royal"><br><small><b><a href="dm/games/01005CA01580E000/README.md">Persona 5 Royal</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pikmin-4.jpg" width="256" alt="Pikmin 4"><br><small><b><a href="dm/games/0100B7C00933A000/README.md">Pikmin 4</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-brilliant-diamond.jpg" width="256" alt="Pokémon Brilliant Diamond"><br><small><b><a href="dm/games/0100000011D90000/README.md">Pokémon Brilliant Diamond</a></b><br>60 fps with 1.3.0</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-legends-z-a.jpg" width="256" alt="Pokémon Legends: Z-A"><br><small><b><a href="dm/games/0100F43008C44000/README.md">Pokémon Legends: Z-A</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-scarlet.png" width="256" alt="Pokémon Scarlet"><br><small><b><a href="dm/games/0100A3D008C5C000/README.md">Pokémon Scarlet</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-sword.jpg" width="256" alt="Pokémon Sword"><br><small><b><a href="dm/games/0100ABF008968000/README.md">Pokémon Sword</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-shield.jpg" width="256" alt="Pokémon Shield"><br><small><b><a href="dm/games/01008DB008C2C000/README.md">Pokémon Shield</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/portal.jpg" width="256" alt="Portal"><br><small><b><a href="dm/games/01007BB017812000/README.md">Portal</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/raging-loop.jpg" width="256" alt="Raging Loop"><br><small><b><a href="dm/games/010020400E1C2000/README.md">Raging Loop</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/rhythm-heaven-groove.jpg" width="256" alt="Rhythm Heaven Groove"><br><small><b><a href="dm/games/0100D9F01D474000/README.md">Rhythm Heaven Groove</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/romance-of-the-three-kingdoms-8-remake.jpg" width="256" alt="ROMANCE OF THE THREE KINGDOMS 8 REMAKE"><br><small><b><a href="dm/games/0100C2A01A730000/README.md">ROMANCE OF THE THREE KINGDOMS 8 REMAKE</a></b><br>controller only</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/sea-of-stars.jpg" width="256" alt="Sea of Stars"><br><small><b><a href="dm/games/01008C0016544000/README.md">Sea of Stars</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/shin-megami-tensei-v-vengeance.jpg" width="256" alt="SHIN MEGAMI TENSEI V: Vengeance"><br><small><b><a href="dm/games/010069C01AB82000/README.md">SHIN MEGAMI TENSEI V: Vengeance</a></b><br>60 fps with 1.0.3</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/stardew-valley.jpg" width="256" alt="Stardew Valley"><br><small><b><a href="dm/games/0100E65002BB8000/README.md">Stardew Valley</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/steins-gate-reboot.jpg" width="256" alt="STEINS;GATE RE:BOOT"><br><small><b><a href="dm/games/01008BC024FDE000/README.md">STEINS;GATE RE:BOOT</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/super-smash-bros-ultimate.jpg" width="256" alt="Super Smash Bros. Ultimate"><br><small><b><a href="dm/games/01006A800016E000/README.md">Super Smash Bros. Ultimate</a></b><br>offline</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/taiko-risshiden-v-dx.jpg" width="256" alt="Taiko Risshiden V DX"><br><small><b><a href="dm/games/0100346017304000/README.md">Taiko Risshiden V DX</a></b><br>Japanese · played by touch</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/unicorn-overlord.jpg" width="256" alt="Unicorn Overlord"><br><small><b><a href="dm/games/010069401ADB8000/README.md">Unicorn Overlord</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/yoshis-crafted-world.jpg" width="256" alt="Yoshi's Crafted World"><br><small><b><a href="dm/games/01006000040C2000/README.md">Yoshi's Crafted World</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/nier-automata.png" width="256" alt="NieR:Automata The End of YoRHa Edition"><br><small><b><a href="dm/games/010056B015FE8000/README.md">NieR:Automata The End of YoRHa Edition</a></b><br>60 fps with 1.0.3</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/zelda-echoes-of-wisdom.jpg" width="256" alt="The Legend of Zelda: Echoes of Wisdom"><br><small><b><a href="dm/games/01008CF01BAAC000/README.md">The Legend of Zelda: Echoes of Wisdom</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/donkey-kong-country-returns-hd.png" width="256" alt="Donkey Kong Country Returns HD"><br><small><b><a href="dm/games/01009D901BC56000/README.md">Donkey Kong Country Returns HD</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/mario-rabbids-sparks-of-hope.jpg" width="256" alt="Mario + Rabbids Sparks of Hope"><br><small><b><a href="dm/games/0100317013770000/README.md">Mario + Rabbids Sparks of Hope</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/xenoblade-chronicles-2.jpg" width="256" alt="Xenoblade Chronicles 2"><br><small><b><a href="dm/games/0100E95004038000/README.md">Xenoblade Chronicles 2</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/xenoblade-chronicles-3.jpg" width="256" alt="Xenoblade Chronicles 3"><br><small><b><a href="dm/games/010074F013262000/README.md">Xenoblade Chronicles 3</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/final-fantasy-crystal-chronicles.jpg" width="256" alt="FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition"><br><small><b><a href="dm/games/0100CE4010AAC000/README.md">FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/return-of-the-obra-dinn.png" width="256" alt="Return of the Obra Dinn"><br><small><b><a href="dm/games/010032E00E6E2000/README.md">Return of the Obra Dinn</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/blasphemous-2.png" width="256" alt="Blasphemous 2"><br><small><b><a href="dm/games/010040801A4C8000/README.md">Blasphemous 2</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/silver-pines.jpg" width="256" alt="Silver Pines"><br><small><b><a href="dm/games/0100D5B026844000/README.md">Silver Pines</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/blasphemous.jpg" width="256" alt="Blasphemous"><br><small><b><a href="dm/games/0100698009C6E000/README.md">Blasphemous</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/final-fantasy-x-x2-hd-remaster.jpg" width="256" alt="FINAL FANTASY X/X-2 HD Remaster"><br><small><b><a href="dm/games/0100BC300CB48000/README.md">FINAL FANTASY X/X-2 HD Remaster</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/mario-rabbids-kingdom-battle.jpg" width="256" alt="Mario + Rabbids Kingdom Battle"><br><small><b><a href="dm/games/010067300059A000/README.md">Mario + Rabbids Kingdom Battle</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/persona-4-golden.jpg" width="256" alt="Persona 4 Golden"><br><small><b><a href="dm/games/010062B01525C000/README.md">Persona 4 Golden</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/xenoblade-chronicles-definitive-edition.jpg" width="256" alt="Xenoblade Chronicles: Definitive Edition"><br><small><b><a href="dm/games/0100FF500E34A000/README.md">Xenoblade Chronicles: Definitive Edition</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/pokemon-legends-arceus.jpg" width="256" alt="Pokémon Legends: Arceus"><br><small><b><a href="dm/games/01001F5010DFA000/README.md">Pokémon Legends: Arceus</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/super-mario-party-jamboree.jpg" width="256" alt="Super Mario Party Jamboree"><br><small><b><a href="dm/games/0100965017338000/README.md">Super Mario Party Jamboree</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/cult-of-the-lamb.jpg" width="256" alt="Cult of the Lamb"><br><small><b><a href="dm/games/01002E7016C46000/README.md">Cult of the Lamb</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/paper-mario-the-origami-king.jpg" width="256" alt="Paper Mario: The Origami King"><br><small><b><a href="dm/games/0100A3900C3E2000/README.md">Paper Mario: The Origami King</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/batman-arkham-knight.jpg" width="256" alt="Batman: Arkham Knight"><br><small><b><a href="dm/games/0100ACD0163D0000/README.md">Batman: Arkham Knight</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/bastion.jpg" width="256" alt="Bastion"><br><small><b><a href="dm/games/010038600B27E000/README.md">Bastion</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/baba-is-you.jpg" width="256" alt="Baba Is You"><br><small><b><a href="dm/games/01002CD00A51C000/README.md">Baba Is You</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/steamworld-heist-ii.jpg" width="256" alt="SteamWorld Heist II"><br><small><b><a href="dm/games/010089C01E9CA000/README.md">SteamWorld Heist II</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/steamworld-heist.jpg" width="256" alt="SteamWorld Heist: Ultimate Edition"><br><small><b><a href="dm/games/010016100A186000/README.md">SteamWorld Heist: Ultimate Edition</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/subnautica-below-zero.jpg" width="256" alt="Subnautica: Below Zero"><br><small><b><a href="dm/games/010014C011146000/README.md">Subnautica: Below Zero</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/steamworld-dig-2.jpg" width="256" alt="SteamWorld Dig 2"><br><small><b><a href="dm/games/0100CA9002322000/README.md">SteamWorld Dig 2</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/streets-of-rage-4.jpg" width="256" alt="Streets of Rage 4"><br><small><b><a href="dm/games/0100EC9010258000/README.md">Streets of Rage 4</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/mario-party-superstars.jpg" width="256" alt="Mario Party Superstars"><br><small><b><a href="dm/games/01006FE013472000/README.md">Mario Party Superstars</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/subnautica.jpg" width="256" alt="Subnautica"><br><small><b><a href="dm/games/0100429011144000/README.md">Subnautica</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/steamworld-dig.jpg" width="256" alt="SteamWorld Dig"><br><small><b><a href="dm/games/01009320084A4000/README.md">SteamWorld Dig</a></b></small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/team-sonic-racing.jpg" width="256" alt="Team Sonic Racing"><br><small><b><a href="dm/games/010092B0091D0000/README.md">Team Sonic Racing</a></b></small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/well-dweller.jpg" width="256" alt="Well Dweller"><br><small><b><a href="dm/games/0100217023F6C000/README.md">Well Dweller</a></b></small></td>
<td></td>
<td></td>
</tr>
</table>
<!-- end of gallery -->

---

### 6. Personalization & 8 Neon Highlight Themes
- **8 Dynamic Accent Themes**: Switch Neon Red, Switch Neon Blue, Electric Purple, Sunset Orange, Emerald Green, Cyber Rose, Hyrule Gold, and Deep Cobalt.
- **Liquid Glass Materials**: Choose between ultra-smooth Liquid Acrylic or Frosted Translucent styling with dynamic vibrancy.
- **13 Multi-Language Locales**: Full native localization in English, Portuguese (BR), Spanish, French, German, Italian, Polish, Russian, Swedish, Japanese, Korean, Simplified Chinese, and Traditional Chinese.
- **Custom Application Icons**: Choose between Light, Dark, or Minimal Monochrome dock icons.

<p align="center">
  <img src="docs/images/gifs/demo_features_showcase.gif" width="95%" alt="Features, Controller Remapping & Neon Themes Demo">
</p>
<p align="center">
  <img src="docs/images/screenshots/personalization_themes.png" width="90%" alt="Personalization, Neon Themes, Liquid Glass & 13 Locales">
</p>

---

### 7. Engine Compatibility Patches & Toolchain Diagnostics
- **One-Click Engine Patches**: Apply title-specific memory allocator hooks, Unreal Engine allocator gates, and 60 FPS patches (e.g. Dragon Quest VII v0 metadata, Tears of the Kingdom frame step, and dynamic shader fallback).
- **Automated Toolchain Health & Repair**: Live status checks and single-click installation for Xcode CLT, Homebrew, Python, .NET SDK, XcodeGen, zstd, spirv-cross, and glslang.
- **Git Commit Diffing**: Live upstream tracking comparing your local build with the newest commits of `seedds/nxnative`.

<p align="center">
  <img src="docs/images/screenshots/settings_engine_patches.png" width="90%" alt="System Settings & Compiler Toolchain Diagnostics">
</p>

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
* **Hardware**: Apple Silicon (M1+ & A17+).
* **OS**: macOS 13.0 (Ventura), macOS 14.0 (Sonoma), macOS 15.0 (Sequoia) or newer.
* **Unified Memory**: 8 GB minimum (16 GB recommended for open-world titles like Zelda TotK).
* **Disk Space**: 20–50 GB free SSD space for RomFS extraction and shader caches.
* **Toolchain**: [Xcode](https://apps.apple.com/app/xcode/id497799835) 27.1, [Homebrew](https://brew.sh) and Python 3.12 or newer.

### Manual Dependency Installation (Terminal)
If you prefer configuring dependencies manually:
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install Homebrew dependencies (Python 3.12, 3.13, or 3.14 recommended)
brew install python@3.13 cmake ninja molten-vk libzip lz4 zstd spirv-cross xcodegen dotnet

# Install required Python packages for asset processing and shader translation
python3.13 -m pip install lz4 capstone fonttools zbic

# (Optional) For compressed .nsz / .xcz files, install nsz (requires Python <= 3.14)
python3.13 -m pip install nsz
```

> [!NOTE]
> `libhac` is a .NET C# library managed via NuGet and compiled automatically by `dotnet` for `nx_extract`. It is **not** a Python package.

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
  <img src="docs/images/screenshots/library_game_details.png" width="90%" alt="Step 2: Platform Configuration & Performance Tuning">
</p>

### Step 3: Library Launch & Device Sideloading
Compiled titles appear immediately on your Mac Native shelf, while iOS `.ipa` ports can be sent directly to your iPhone or iPad via AirDrop with a single click:

<p align="center">
  <img src="docs/images/screenshots/library_ios_airdrop.png" width="90%" alt="Step 3: Direct AirDrop iOS Port Sideloading">
</p>

---

## 🕹️ How to Configure and Play Your Game

### How to play

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
- [x] **v1.6.0, v1.6.1 & v1.6.2 — Controller Remapping, Save Sync & Robust Environment Setup**:
  - **Full Controller Remapping**: Custom button mapping for Switch Pro, Joy-Cons, Xbox Series/One, and PS5 DualSense controllers with profile duplication and reset.
  - **Save Synchronization Service (`SaveSyncService`)**: Bidirectional save state synchronization and backup between Mac and paired iOS devices.
  - **Custom Cover Art**: User-customizable box art picker (`.jpg`, `.png`, `.webp`) with instant persistence and fallback reset.
  - **Automated Environment Diagnostics & Python 3.12+ / Pip Fix**: One-click sequential Homebrew and build dependency installer in Settings tab, with dynamic Python 3.12+ resolution and safe pip package installation.
  - **3-Second Couch Exit**: Global 3-second hold shortcut on Xbox Guide / PS / Home buttons to close running games and refocus the Launcher.
- [x] **v1.6.3 — In-App Multi-Language Localization (13 Languages)**:
  - **In-App Dynamic Language Switcher**: Switch the launcher UI language on the fly under the *Personalization* tab without restarting or altering macOS system preferences.
  - **13 Languages Supported**: English, Portuguese (Brazil), Spanish, French, German, Italian, Polish, Russian, Japanese, Korean, Simplified Chinese, Traditional Chinese, and Swedish (contributed by @yeager).
  - **Full UI Coverage**: Localized navigation, library cards, porting progress banners, action sheets, settings dialogs, controller remapping, and system tools.
- [x] **v1.7.0 (Current Version) — Smart iCloud Cross-Save & Single-Button Device Sync**:
  - **Automated Smart Cross-Save**: Timestamp-based bidirectional save synchronization between Mac, iCloud Drive (`com~apple~CloudDocs/nxnative_launcher`), and connected iOS devices.
  - **Single "Sync Save" Button**: Replaced manual "Push" and "Pull" buttons with a single 1-click automatic synchronization action localized in all 13 languages.
  - **Auto-Sync on Game Launch**: Automatically checks and synchronizes saves with connected iPhone/iPad devices before starting a game on Mac.
  - **Safety Backups & iCloud Cover Badging**: Generates instant safety backups before any file replacement, and renders custom high-res box art onto iCloud game folders in Finder.

---

### ⏳ In Active Development & Future Milestones

- [ ] **v1.8.0 — MetalFX Temporal Upscaler**: Temporal anti-aliasing and dynamic temporal scaling for demanding 3D games.
- [ ] **v1.8.0 — Haptic Feedback & Gamepad Vibration**: DualSense adaptive triggers & Switch Pro linear resonant actuator rumble.
- [ ] **v1.9.0 — Discord Rich Presence**: Real-time rich presence showing active title, in-game playtime, and FPS counters.

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
NxNative Launcher uses <code>SaveSyncService</code> to detect paired iOS devices via <code>xcrun devicectl</code>. With v1.7.0, a single click on <b>Sync Save</b> automatically compares timestamps and synchronizes the newest save between your Mac, iCloud Drive, and iPhone 15 Pro / iPad. Saves are also verified and synchronized automatically whenever you launch a game with a connected device!
</details>

<details>
<summary><b>5. Does this run via Rosetta 2 or an emulator?</b></summary>
No! Nintendo Switch games use ARM64 binaries (AArch64). Apple Silicon processors (M1+ & A17+) run ARM64 instructions natively. NxNative recompiles and translates Horizon OS syscalls and Maxwell graphics into native macOS Mach-O binaries and Apple Metal Shading Language (`.metal`), resulting in near-native performance.
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

Bem-vindo ao guia oficial em português do **NxNative Launcher (v1.7.0)**! Esta seção contém tudo o que você precisa para configurar o ambiente, portar seus jogos, personalizar sua biblioteca, sincronizar saves e jogar com controles no Apple Silicon.

### 📑 Índice do Guia em Português
- [1. Configuração do Ambiente e Dependências](#1-configuração-do-ambiente-e-dependências)
- [2. Passo a Passo: Como Portar Jogos](#2-passo-a-passo-como-portar-jogos)
- [3. Configuração na Biblioteca, Saves e Mods](#3-configuração-na-biblioteca-saves-e-mods)
- [4. Remapeamento de Controles e Como Jogar](#4-remapeamento-de-controles-e-como-jogar)
- [5. Suporte e Relato de Erros](#5-suporte-e-relato-de-erros)

---

### 1. Configuração do Ambiente e Dependências

#### A. Requisitos de Sistema
- Dispositivo com processador **Apple Silicon** (M1+ ou A17+).
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

# 2. Instalar dependências via Homebrew (Python 3.12, 3.13 ou 3.14 recomendados)
brew install python@3.13 cmake ninja molten-vk libzip lz4 zstd spirv-cross xcodegen dotnet

# 3. Instalar pacotes Python para extração de assets e tradução de shaders
python3.13 -m pip install lz4 capstone fonttools

# (Opcional) Para arquivos comprimidos .nsz / .xcz, instale o nsz (requer Python <= 3.14)
python3.13 -m pip install nsz
```

> [!NOTE]
> `libhac` é uma biblioteca .NET C# gerenciada via NuGet e compilada automaticamente pelo `dotnet` para o `nx_extract`. **Não** é um pacote Python.

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
   - Clique no botão **Saves** no launcher para abrir a pasta diretamente no Finder ou sincronizar com aparelhos iOS emparelhados via `SaveSyncService`. Com a versão v1.7.0, um clique no botão **Sincronizar Save** compara datas e sincroniza automaticamente a versão mais recente entre seu Mac, iCloud Drive e dispositivos iPhone/iPad! Além disso, a sincronização ocorre automaticamente ao iniciar o jogo no Mac.
4. **Mods de Jogo (LayeredFS / Atmosphere)**:
   - No painel de opções, acesse a pasta de mods: `<Game.app>/Contents/Resources/atmosphere/contents/<TitleID>/`.
   - Cole suas texturas HD, mods de física ou traduções em PT-BR.

---

### 4. Remapeamento de Controles e Como Jogar

- **Mouse Look 3D & Emulação de Câmera (macOS)**:
  - **Mira e Câmera Estilo PC**: Controle a câmera de jogos 3D (*The Legend of Zelda: Tears of the Kingdom*, *Breath of the Wild*, *Super Mario Odyssey*, *NieR:Automata*, etc.) diretamente com o mouse do Mac.
  - **Compensação Automática de Deadzone (+6.000 unidades)**: Ultrapassa a zona morta do motor de jogo instantaneamente no primeiro movimento, garantindo resposta imediata sem atrasos ou zonas mortas.
  - **Filtro de Desaceleração Natural**: O sistema simula a física de retorno do analógico físico ao centro via decaimento suave em estágios de 16ms.
  - **Trava de Cursor e Atalho de Liberação**: Clique na janela do jogo para capturar o cursor em 360°; pressione **`ESC`** a qualquer momento para liberar o cursor imediatamente para o macOS.
  - **Ajustes Personalizados**: Configure multiplicador de sensibilidade e inversão do eixo Y vertical na aba Controls.
- **Remapeamento Completo de Botões**:
  - Na aba **Controls**, personalize cada botão do seu Switch Pro Controller, Joy-Cons, Xbox ou PS5 DualSense, além de perfis dedicados para Teclado e Mouse.
- **Calibração de Drift em Tempo Real**:
  - O monitor visual exibe os eixos analógicos X/Y e a zona morta em tempo real, eliminando movimentos involuntários no personagem.
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
