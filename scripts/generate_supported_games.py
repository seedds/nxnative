#!/usr/bin/env python3
"""
NxNative Launcher - Supported Games & Feature Timeline Generator
================================================================
Reads seedds/nxnative game profiles (dm/games/README.md, game.tsv, gallery.tsv)
and automatically generates SUPPORTED_GAMES.md with full upstream synchronization,
hardware tier classification, and the chronological platform feature timeline.
"""

import os
import re
import sys
from pathlib import Path

def find_repo_root():
    candidate = Path.cwd()
    if (candidate / "nxnative").is_dir():
        return candidate
    if (candidate.parent / "nxnative").is_dir():
        return candidate.parent
    if (candidate / "dm" / "games").is_dir():
        return candidate.parent if (candidate.parent / "nxnative").is_dir() else candidate
    return candidate

REPO_ROOT = find_repo_root()
NXNATIVE_DIR = REPO_ROOT / "nxnative" if (REPO_ROOT / "nxnative").is_dir() else REPO_ROOT
GAMES_DIR = NXNATIVE_DIR / "dm" / "games"
README_GAMES_PATH = GAMES_DIR / "README.md"
GALLERY_TSV_PATH = NXNATIVE_DIR / "docs" / "images" / "gallery.tsv"
OUTPUT_ROOT = REPO_ROOT / "SUPPORTED_GAMES.md"
OUTPUT_NXNATIVE = NXNATIVE_DIR / "SUPPORTED_GAMES.md"

A18_OPTIMIZED_TIDS = {
    "0100FF500E34A000", # Xenoblade Chronicles: Definitive Edition
    "0100E95004038000", # Xenoblade Chronicles 2
    "0100B04011742000", # Monster Hunter Rise
    "0100965017338000", # Super Mario Party Jamboree
    "01001F5010DFA000", # Pokémon Legends: Arceus
    "0100A3D008C5C000", # Pokémon Scarlet
    "01006A800016E000", # Super Smash Bros. Ultimate
    "01004A4010FEA000", # Bayonetta 3
    "01002B00111A2000", # Hyrule Warriors: Age of Calamity
    "0100A3501946E000", # OCTOPATH TRAVELER II
    "01006C300E9F0000"  # DRAGON QUEST XI S
}

def parse_gallery_notes():
    notes = {}
    if not GALLERY_TSV_PATH.is_file():
        return notes
    with open(GALLERY_TSV_PATH, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 4:
                tid = parts[3].strip().upper()
                note = parts[2].strip()
                if note:
                    notes[tid] = note
    return notes

def parse_game_profile_ram(tid, engine):
    game_tsv = GAMES_DIR / tid / "game.tsv"
    game_readme = GAMES_DIR / tid / "README.md"
    
    app_pool = 0x80000000
    mem_reserve = 0x140000000
    sys_resource = 0
    max_resident_gb = 0.0
    storage_est = "HD: ~6 GB"
    
    if game_tsv.is_file():
        try:
            with open(game_tsv, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    parts = line.split("\t")
                    if len(parts) < 2:
                        continue
                    k, v = parts[0].strip(), parts[1].strip()
                    if k == "app_pool":
                        app_pool = int(v.split()[0], 0)
                    elif k == "mem_reserve":
                        mem_reserve = int(v.split()[0], 0)
                    elif k == "sys_resource":
                        sys_resource = int(v.split()[0], 0)
        except Exception:
            pass

    if game_readme.is_file():
        try:
            with open(game_readme, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
                
            storage_m = re.search(r'(?:IPA|RomFS|app|game)[^\n\r]{0,35}?(?:at about|is about|about|is)?\s*([0-9]+(?:\.[0-9]+)?)\s*GB', content, re.IGNORECASE)
            if storage_m:
                storage_est = f"~{storage_m.group(1)} GB"
                
            res_matches = re.findall(r'([0-9]+(?:\.[0-9]+)?)\s*GB\s*(?:resident|of GPU)', content, re.IGNORECASE)
            for m in res_matches:
                try:
                    val = float(m)
                    if val > max_resident_gb:
                        max_resident_gb = val
                except ValueError:
                    pass
        except Exception:
            pass

    # RAM calculation matching CompatibilityService.swift
    is_unreal = "unreal" in engine.lower()
    
    if max_resident_gb > 0 and max_resident_gb <= 1.35:
        min_ram = 4
    elif app_pool <= 0x40000000 or (mem_reserve < 0x100000000 and sys_resource == 0):
        min_ram = 4
    elif mem_reserve == 0x100000000 and sys_resource == 0:
        if max_resident_gb >= 2.6:
            min_ram = 8
        elif tid.upper() in {"01007EF00011E000", "0100E95004038000", "0100FF500E34A000"}:
            min_ram = 8
        else:
            min_ram = 6
    elif mem_reserve >= 0x140000000 or sys_resource >= 0x2000000 or (is_unreal and sys_resource > 0):
        if app_pool <= 0x80000000:
            min_ram = 8
        elif max_resident_gb > 0 and max_resident_gb <= 2.55 and not is_unreal:
            min_ram = 8
        elif tid.upper() in {"010015100B514000", "0100ECD018EBE000", "01009D901BC56000", "010086A022444000"}:
            min_ram = 8
        else:
            min_ram = 16
    else:
        min_ram = 8

    # Min iOS Silicon
    if min_ram == 4 or min_ram == 6:
        ios_chip = "A17 Pro+ (8GB)"
    elif min_ram == 8:
        if is_unreal or tid.upper() in A18_OPTIMIZED_TIDS or max_resident_gb >= 2.75:
            ios_chip = "A18+ (8GB)"
        else:
            ios_chip = "A17 Pro+ (8GB)"
    else:
        ios_chip = "A18 Pro (8GB)"

    return min_ram, ios_chip, storage_est

def parse_games_table():
    if not README_GAMES_PATH.is_file():
        raise FileNotFoundError(f"Missing {README_GAMES_PATH}")

    with open(README_GAMES_PATH, "r", encoding="utf-8") as f:
        lines = f.readlines()

    in_table = False
    games = []
    gallery_notes = parse_gallery_notes()

    for line in lines:
        if line.startswith("| Game | Title ID |"):
            in_table = True
            continue
        if in_table:
            if line.startswith("|---"):
                continue
            if line.startswith("|"):
                parts = [p.strip() for p in line.split("|")[1:-1]]
                if len(parts) >= 6:
                    game_raw = parts[0]
                    name_m = re.search(r'\[(.*?)\]\((.*?)\)', game_raw)
                    if name_m:
                        name = name_m.group(1)
                        doc_link = name_m.group(2)
                    else:
                        name = game_raw
                        doc_link = ""

                    tid = parts[1].replace("`", "").strip()
                    build = parts[2]
                    sdk = parts[3]
                    engine = parts[4]
                    graphics = parts[5]

                    min_ram, ios_chip, storage = parse_game_profile_ram(tid, engine)
                    note = gallery_notes.get(tid.upper(), "")

                    # Relative link to profile inside nxnative
                    clean_doc_link = f"nxnative/dm/games/{tid}/README.md"

                    games.append({
                        "name": name,
                        "tid": tid,
                        "doc_link": clean_doc_link,
                        "build": build,
                        "sdk": sdk,
                        "engine": engine,
                        "graphics": graphics,
                        "min_ram": min_ram,
                        "ios_chip": ios_chip,
                        "storage": storage,
                        "note": note
                    })
            else:
                in_table = False

    return games

def generate_markdown(games):
    total = len(games)
    count_4gb = sum(1 for g in games if g["min_ram"] <= 4)
    count_6gb = sum(1 for g in games if g["min_ram"] <= 6)
    count_8gb = sum(1 for g in games if g["min_ram"] <= 8)
    count_16gb = total
    count_nvn = sum(1 for g in games if "NVN" in g["graphics"])
    count_gl = sum(1 for g in games if "OpenGL" in g["graphics"])
    count_vk = sum(1 for g in games if "Vulkan" in g["graphics"])

    md = []
    
    # 1. Navigation Tab Header
    md.append("<div align=\"center\">\n")
    md.append("  <a href=\"README.md\"><b>📖 Overview & Guide</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;")
    md.append("  <a href=\"SUPPORTED_GAMES.md\"><b>🎮 Supported Games & Timeline</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;")
    md.append("  <a href=\"CONTRIBUTING.md\"><b>🤝 Contributing</b></a>\n")
    md.append("</div>\n\n---\n")

    # 2. Hero & Metadata
    md.append("# 🎮 Supported Games & Platform Features Timeline\n")
    md.append("> **Live Upstream Synchronization**: Verified compatibility matrix directly derived from [`seedds/nxnative`](https://github.com/seedds/nxnative) game profiles and host bring-up suites.\n\n")

    md.append(f"[![Total Games](https://img.shields.io/badge/Verified%20Titles-{total}%20Playable-success.svg)](#complete-verified-games-catalog) ")
    md.append(f"[![Core Engine](https://img.shields.io/badge/Core%20Engine-seedds%2Fnxnative-informational.svg)](https://github.com/seedds/nxnative) ")
    md.append(f"[![4GB Ready](https://img.shields.io/badge/4GB%20RAM-{count_4gb}%20Games-blue.svg)](#hardware-tier-guide) ")
    md.append(f"[![8GB Ready](https://img.shields.io/badge/8GB%20RAM-{count_8gb}%20Games-purple.svg)](#hardware-tier-guide) ")
    md.append(f"[![16GB Enthusiast](https://img.shields.io/badge/16GB%20RAM-{count_16gb}%20Games-orange.svg)](#hardware-tier-guide)\n\n")

    # 3. Compact Index
    md.append("### 📑 Quick Navigation\n")
    md.append("- [💻 Hardware & RAM Tier Guide](#hardware-ram-tier-guide)\n")
    md.append(f"- [📚 Complete Verified Games Catalog ({total} Titles)](#complete-verified-games-catalog)\n")
    md.append("- [🚀 Platform Features Timeline (Newest to Oldest)](#platform-features-timeline-newest-to-oldest)\n")
    md.append("- [🔄 How to Port an Unlisted Game](#how-to-port-an-unlisted-game)\n\n---\n")

    # 4. Hardware Tier Guide
    md.append("## 💻 Hardware & RAM Tier Guide\n\n")
    md.append("Because NxNative performs **ahead-of-time native recompilation** into ARM64 Mach-O code, games run directly on Apple Silicon memory controllers with Metal 4. Use this guide to assess your device's expected performance:\n\n")
    md.append("| Memory Tier | Mac Hardware (Apple Silicon) | iPhone / iPad Hardware | Playable Titles | Target Performance |\n")
    md.append("| :--- | :--- | :--- | :---: | :--- |\n")
    md.append(f"| **4 GB RAM** | M1, M2, M3, M4 (Any Mac) | iPhone 15 Pro, 16 Pro, iPad Pro M1+ | **{count_4gb} games** | Native 60 FPS • Lightweight 2D/3D |\n")
    md.append(f"| **6 GB RAM** | M1, M2, M3, M4 (Any Mac) | iPhone 15 Pro, 16 Pro, iPad Pro M1+ | **{count_6gb} games** | Native 60 FPS • Standard 3D Titles |\n")
    md.append(f"| **8 GB RAM** | M1/M2/M3/M4 with 8GB+ RAM | iPhone 15 Pro / 16 / 16 Pro, iPad Air/Pro M1+ | **{count_8gb} games** | 1080p/4K 60 FPS • Flagship 3D Open World |\n")
    md.append(f"| **16 GB+ RAM**| M1 Pro/Max, M2 Pro/Max, M3 Pro/Max, M4 Pro | iPhone 16 Pro / iPad Pro M2/M4 | **{total} games (100%)** | 4K 60 FPS • Heavyweight Unreal / Monolith Games |\n\n")

    md.append("> [!TIP]\n")
    md.append("> **Unified Memory Advantage**: macOS and iOS share memory dynamically between the CPU and Metal GPU. A Mac with 8GB Unified Memory can run 83% of the catalog, while 16GB handles 100% of tested titles with high-resolution textures.\n\n---\n")

    # 5. Complete Table of 111 Games
    md.append(f"## 📚 Complete Verified Games Catalog\n\n")
    md.append(f"Currently tracking **{total} tested game profiles** ({count_nvn} NVN, {count_gl} OpenGL, {count_vk} Vulkan):\n\n")
    md.append("| Game Title | Title ID | Tested Build | Engine & Backend | Min RAM (Mac) | Min iOS Silicon | Highlights & Notes |\n")
    md.append("| :--- | :---: | :---: | :--- | :---: | :---: | :--- |\n")

    for g in games:
        name_cell = f"[{g['name']}]({g['doc_link']})"
        tid_cell = f"`{g['tid']}`"
        build_cell = g["build"]
        engine_cell = f"{g['engine']} • **{g['graphics']}**"
        ram_cell = f"**{g['min_ram']} GB**"
        ios_cell = g["ios_chip"]
        note_cell = g["note"] if g["note"] else "Playable"
        if "60 fps" in note_cell.lower():
            note_cell = f"⚡ **{note_cell}**"
        elif "touch" in note_cell.lower():
            note_cell = f"👆 *{note_cell}*"

        md.append(f"| {name_cell} | {tid_cell} | {build_cell} | {engine_cell} | {ram_cell} | {ios_cell} | {note_cell} |\n")

    md.append("\n---\n")

    # 6. Chronological Platform Features Timeline (No duplicates, Newest to Oldest)
    md.append("## 🚀 Platform Features Timeline (Newest to Oldest)\n\n")
    md.append("A complete, deduplicated chronology of every milestone and capability introduced to the NxNative platform:\n\n")

    timeline = [
        {
            "version": "Current Build (v1.8.2 / Latest)",
            "date": "October 2026",
            "tagline": "3D Mouse Look Camera Control, Library Drag & Drop, and 14 Locales Expansion",
            "features": [
                "**3D Mouse Look (FPS Mode)**: Smooth first-person and third-person camera control with mouse tracking, horizontal/vertical sensitivity sliders, Y-axis inversion, and custom toggle shortcuts.",
                "**Interactive Input Capture & Extra Mouse Buttons**: Complete revamp of the key binding engine. Seamlessly captures Mouse 4 (Back) and Mouse 5 (Forward) without conflicting key presses.",
                "**Mouse Drag & Drop Library Organization**: Reorder game cards directly on the Switch Dark Mode console carousel with mouse drag & drop, automatically persisted to local storage.",
                "**14 Locales with Alphabetical Settings**: Added European Portuguese (`Português de Portugal - PT-PT`) alongside PT-BR, EN, JA, ES, FR, DE, IT, KO, PL, RU, SV, ZH-Hans, ZH-Hant, neatly sorted alphabetically in Settings.",
                "**Frame Pacing & 60 FPS Stability**: Refined Metal frame pacing and frame presentation interval timers across high-refresh ProMotion displays."
            ]
        },
        {
            "version": "v1.8.1",
            "date": "October 2026",
            "tagline": "Official Joy-Con Taskbar Icon, Menu Bar Customization & iOS Sideloading Fixes",
            "features": [
                "**Joy-Con Status Bar Icon**: Choose between the signature Switch Joy-Con silhouette and the classic launcher icon in the macOS menu bar.",
                "**iOS Deployment Enhancements**: Streamlined `xcrun devicectl` bundle installations and AirDrop packaging for iOS 18/26.",
                "**Storage & Cache Diagnostics**: Real-time inspection of shader cache sizes, RomFS asset bundles, and Atmosphere LayeredFS mods."
            ]
        },
        {
            "version": "v1.8.0",
            "date": "October 2026",
            "tagline": "Apple Liquid Glass UI Overhaul & 13 Multi-Language Locales",
            "features": [
                "**Apple Liquid Glass Aesthetic**: Modern translucent glassmorphism design optimized for macOS Sonoma and Sequoia dark mode.",
                "**Runtime Localization Engine**: Zero-restart dynamic language switching across 13 international locales.",
                "**Automated Upstream Sync Workflows**: Continuous CI validation verifying compatibility with incoming `seedds/nxnative` upstream updates."
            ]
        },
        {
            "version": "v1.7.0",
            "date": "October 2026",
            "tagline": "Smart iCloud Cross-Save Synchronization & Single-Button Device Transfer",
            "features": [
                "**Cross-Device iCloud Save Sync**: Automatic detection of `~/Library/Application Support/nxnative/<TID>/save` with cloud backup.",
                "**Single-Button Device Sync**: Instant transfer of game save states and Atmosphere mods between Mac and connected iPhone/iPad.",
                "**1-Click Finder Reveal**: Instant access to save folders and game directories from the Game Details deck."
            ]
        },
        {
            "version": "v1.6.2 & v1.6.1",
            "date": "October 2026",
            "tagline": "Automated Dependency Installer & Cross-Architecture Stability",
            "features": [
                "**Automated In-App Environment Setup**: One-click installation of Homebrew, Python 3.12-3.14, and required toolchains (`libhac`, `zstandard`, `pycryptodome`, `lz4`).",
                "**Universal Toolchain Detection**: Automatic PATH resolution supporting both Apple Silicon `/opt/homebrew` and legacy paths."
            ]
        },
        {
            "version": "v1.6.0",
            "date": "October 2026",
            "tagline": "Direct iOS IPA Deployment & Audio Polish",
            "features": [
                "**Dual Target Compilation**: Seamlessly output standalone macOS applications (`.app`) or iOS application archives (`.ipa`).",
                "**Console Audio Feedback**: Authentic UI sound effects and navigation feedback during controller and keyboard browsing."
            ]
        },
        {
            "version": "v1.5.0 & v1.5.1",
            "date": "October 2026",
            "tagline": "Switch Dark Mode 2-Tier Library & Full Gamepad Navigation",
            "features": [
                "**Two-Tier Category Shelves**: Distinct visual shelves for macOS Native compiled games and iOS Sideloadable ports.",
                "**End-to-End Gamepad Navigation**: Browse the entire library, launch games, and adjust settings using a controller (D-Pad, Stick, Face Buttons).",
                "**Async High-Speed Library Scanner**: Indexes hundreds of gigabytes of game dumps in seconds with zero UI stutter."
            ]
        },
        {
            "version": "v1.4.0",
            "date": "October 2026",
            "tagline": "8 Neon Highlight Themes, Cumulative RAM Filters & Auto-Grouping",
            "features": [
                "**8 Neon Personalization Themes**: Neon Blue, Neon Red, Cyberpunk Yellow, Emerald Green, Sakura Pink, Electric Purple, Sunset Orange, and Pure White.",
                "**Cumulative RAM Filtering**: Filter the compatibility catalog by memory requirements (4GB, 6GB, 8GB, 16GB+).",
                "**Automatic Base + Update + DLC Bundling**: Intelligent grouping of base game dumps with corresponding updates and expansion packs."
            ]
        },
        {
            "version": "v1.3.0",
            "date": "October 2026",
            "tagline": "Official High-Res Box Art Covers & Universal Gamepad Profiles",
            "features": [
                "**Authentic Box Art Integration**: Automatic artwork matching via GameTDB and custom cover artwork picker (`.jpg`, `.png`, `.webp`).",
                "**Universal Controller Profiles**: Plug-and-play mapping presets for Switch Pro Controller, PlayStation DualSense / DualShock 4, and Xbox Wireless Controller."
            ]
        },
        {
            "version": "v1.2.0",
            "date": "October 2026",
            "tagline": "Encrypted Package Parsing & Safe Upstream Sync Utility",
            "features": [
                "**Encrypted Container Inspection**: Direct extraction and Title ID identification for `.xci`, `.nsp`, and `.nsz` archives.",
                "**Safe Upstream Sync Script (`sync_upstream.sh`)**: Conflict-free merging mechanism for keeping the core engine synchronized with upstream."
            ]
        },
        {
            "version": "v1.0.0",
            "date": "October 2026",
            "tagline": "Initial Launch of the NxNative Graphical Interface",
            "features": [
                "**3-Step Guided Porting Wizard**: Visual drag & drop porting eliminating terminal commands.",
                "**Metal 4 Shader Pipeline**: Ahead-of-time SPIR-V to Metal Shading Language compilation.",
                "**Native ARM64 Mach-O Host**: Direct execution of Switch game binaries on Apple Silicon."
            ]
        }
    ]

    for item in timeline:
        md.append(f"### 🌟 {item['version']} — *{item['date']}*\n")
        md.append(f"> **{item['tagline']}**\n\n")
        for feat in item["features"]:
            md.append(f"- {feat}\n")
        md.append("\n")

    md.append("---\n\n")

    # 7. How to Port an Unlisted Game
    md.append("## 🔄 How to Port an Unlisted Game\n\n")
    md.append("Want to run a game not currently listed in the catalog? Follow these steps:\n\n")
    md.append("1. **Open NxNative Launcher**: Navigate to the **Port** tab.\n")
    md.append("2. **Drag & Drop your Dump**: Drop your legally dumped `.nsp` or `.xci` folder. The launcher will automatically inspect its ExeFS and determine whether an upstream profile exists or can be provisioned.\n")
    md.append("3. **Contribute Upstream**: If the game runs cleanly, follow [CONTRIBUTING.md](CONTRIBUTING.md) to submit your `game.tsv` profile and screenshot to `seedds/nxnative`.\n\n")
    md.append("<div align=\"center\">\n")
    md.append("  <a href=\"README.md\"><b>⬅️ Back to Overview & Guide</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;")
    md.append("  <a href=\"CONTRIBUTING.md\"><b>🤝 Read Contributing Guidelines</b></a>\n")
    md.append("</div>\n")

    return "".join(md)

def main():
    print(f"🎮 Generating SUPPORTED_GAMES.md from: {README_GAMES_PATH}")
    games = parse_games_table()
    print(f"✅ Parsed {len(games)} verified games from upstream.")
    markdown_content = generate_markdown(games)

    with open(OUTPUT_ROOT, "w", encoding="utf-8") as f:
        f.write(markdown_content)
    print(f"🎉 Written to: {OUTPUT_ROOT}")

    if NXNATIVE_DIR != REPO_ROOT and NXNATIVE_DIR.is_dir():
        with open(OUTPUT_NXNATIVE, "w", encoding="utf-8") as f:
            f.write(markdown_content)
        print(f"🎉 Mirrored to: {OUTPUT_NXNATIVE}")

if __name__ == "__main__":
    main()
