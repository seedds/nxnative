<div align="center">
  <a href="README.md"><b>📖 Overview & Guide</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;  <a href="SUPPORTED_GAMES.md"><b>🎮 Supported Games & Timeline</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;  <a href="CONTRIBUTING.md"><b>🤝 Contributing</b></a>
</div>

---
# 🎮 Supported Games & Platform Features Timeline
> **Live Upstream Synchronization**: Verified compatibility matrix directly derived from [`seedds/nxnative`](https://github.com/seedds/nxnative) game profiles and host bring-up suites.

[![Total Games](https://img.shields.io/badge/Verified%20Titles-111%20Playable-success.svg)](#complete-verified-games-catalog) [![Core Engine](https://img.shields.io/badge/Core%20Engine-seedds%2Fnxnative-informational.svg)](https://github.com/seedds/nxnative) [![4GB Ready](https://img.shields.io/badge/4GB%20RAM-27%20Games-blue.svg)](#hardware-tier-guide) [![8GB Ready](https://img.shields.io/badge/8GB%20RAM-91%20Games-purple.svg)](#hardware-tier-guide) [![16GB Enthusiast](https://img.shields.io/badge/16GB%20RAM-111%20Games-orange.svg)](#hardware-tier-guide)

### 📑 Quick Navigation
- [💻 Hardware & RAM Tier Guide](#hardware-ram-tier-guide)
- [📚 Complete Verified Games Catalog (111 Titles)](#complete-verified-games-catalog)
- [🚀 Platform Features Timeline (Newest to Oldest)](#platform-features-timeline-newest-to-oldest)
- [🔄 How to Port an Unlisted Game](#how-to-port-an-unlisted-game)

---
## 💻 Hardware & RAM Tier Guide

Because NxNative performs **ahead-of-time native recompilation** into ARM64 Mach-O code, games run directly on Apple Silicon memory controllers with Metal 4. Use this guide to assess your device's expected performance:

| Memory Tier | Mac Hardware (Apple Silicon) | iPhone / iPad Hardware | Playable Titles | Target Performance |
| :--- | :--- | :--- | :---: | :--- |
| **4 GB RAM** | M1, M2, M3, M4 (Any Mac) | iPhone 15 Pro, 16 Pro, iPad Pro M1+ | **27 games** | Native 60 FPS • Lightweight 2D/3D |
| **6 GB RAM** | M1, M2, M3, M4 (Any Mac) | iPhone 15 Pro, 16 Pro, iPad Pro M1+ | **75 games** | Native 60 FPS • Standard 3D Titles |
| **8 GB RAM** | M1/M2/M3/M4 with 8GB+ RAM | iPhone 15 Pro / 16 / 16 Pro, iPad Air/Pro M1+ | **91 games** | 1080p/4K 60 FPS • Flagship 3D Open World |
| **16 GB+ RAM**| M1 Pro/Max, M2 Pro/Max, M3 Pro/Max, M4 Pro | iPhone 16 Pro / iPad Pro M2/M4 | **111 games (100%)** | 4K 60 FPS • Heavyweight Unreal / Monolith Games |

> [!TIP]
> **Unified Memory Advantage**: macOS and iOS share memory dynamically between the CPU and Metal GPU. A Mac with 8GB Unified Memory can run 83% of the catalog, while 16GB handles 100% of tested titles with high-resolution textures.

---
## 📚 Complete Verified Games Catalog

Currently tracking **111 tested game profiles** (105 NVN, 3 OpenGL, 3 Vulkan):

| Game Title | Title ID | Tested Build | Engine & Backend | Min RAM (Mac) | Min iOS Silicon | Highlights & Notes |
| :--- | :---: | :---: | :--- | :---: | :---: | :--- |
| [13 Sentinels: Aegis Rim](nxnative/dm/games/01003FC01670C000/README.md) | `01003FC01670C000` | 1.10 | Vanillaware's engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [13 Sentinels: Aegis Rim, Asian release](nxnative/dm/games/01008D7016438000/README.md) | `01008D7016438000` | 1.00 (base) | Vanillaware's engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [13 Sentinels: Aegis Rim, Japanese release](nxnative/dm/games/010045C014650000/README.md) | `010045C014650000` | 1.00 (base) | Vanillaware's engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Absolum](nxnative/dm/games/0100A83022A22000/README.md) | `0100A83022A22000` | 1.1.2 | FNA / .NET NativeAOT (FNA3D's NVN driver) • **NVN** | **8 GB** | A17 Pro+ (8GB) | Playable |
| [Ace Attorney Investigations Collection](nxnative/dm/games/010005501E68C000/README.md) | `010005501E68C000` | 1.0.0.2 | Unity 2022.3.17f1 IL2CPP • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Advance Wars 1+2: Re-Boot Camp](nxnative/dm/games/0100300012F2A000/README.md) | `0100300012F2A000` | 1.0.0 (base) | Unity 2020.2.1f3 IL2CPP • **NVN** | **8 GB** | A17 Pro+ (8GB) | Playable |
| [AI: THE SOMNIUM FILES](nxnative/dm/games/010089B00D09C000/README.md) | `010089B00D09C000` | 1.0.1 | Unity 2017.4.17f1 IL2CPP • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [ASTRAL CHAIN](nxnative/dm/games/01007300020FA000/README.md) | `01007300020FA000` | 1.0.1 | PlatinumGames' engine • **NVN** | **4 GB** | A17 Pro+ (8GB) | ⚡ **60 fps target with 1.0.1** |
| [AI: THE SOMNIUM FILES - nirvanA Initiative](nxnative/dm/games/0100713016CCC000/README.md) | `0100713016CCC000` | 1.0.2 | Unity 2020.3.30f1 IL2CPP • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [ANIMAL WELL](nxnative/dm/games/010020D01AD24000/README.md) | `010020D01AD24000` | 1.0.10 | its own (not named) • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Animal Crossing: New Horizons](nxnative/dm/games/01006F8002326000/README.md) | `01006F8002326000` | 3.0.3 | Nintendo EPD (sead / agl / gsys) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Baba Is You](nxnative/dm/games/01002CD00A51C000/README.md) | `01002CD00A51C000` | 1.0.12 | Multimedia Fusion 2 game on MP2 Games' runtime (Lua) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [BALL x PIT](nxnative/dm/games/010086A022444000/README.md) | `010086A022444000` | 1.293 | Unity 6000.0.62f1 IL2CPP • **NVN** | **8 GB** | A17 Pro+ (8GB) | Playable |
| [Balatro](nxnative/dm/games/0100CD801CE5E000/README.md) | `0100CD801CE5E000` | 1.1.3 | LÖVE 11.4 (love-nx, LuaJIT, SDL) • **OpenGL** | **4 GB** | A17 Pro+ (8GB) | 👆 *played by touch* |
| [Bastion](nxnative/dm/games/010038600B27E000/README.md) | `010038600B27E000` | 1.0.0 (base) | Supergiant Games' engine on MonoGame 3.0 • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Batman: Arkham Knight](nxnative/dm/games/0100ACD0163D0000/README.md) | `0100ACD0163D0000` | 1.0.5 | Unreal Engine 3 (Rocksteady), Wwise • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Bayonetta](nxnative/dm/games/010076F0049A2000/README.md) | `010076F0049A2000` | 1.1 | PlatinumGames' engine (Wii U form) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Bayonetta 2](nxnative/dm/games/01007960049A0000/README.md) | `01007960049A0000` | 1.2 | PlatinumGames' engine (Wii U form) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Bayonetta 3](nxnative/dm/games/01004A4010FEA000/README.md) | `01004A4010FEA000` | 1.2.0 | PlatinumGames' engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Big Brain Academy: Brain vs. Brain](nxnative/dm/games/0100620012D6E000/README.md) | `0100620012D6E000` | 1.2.0 | NintendoWare Bezel Engine 1.56 • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Blasphemous](nxnative/dm/games/0100698009C6E000/README.md) | `0100698009C6E000` | 1.0.8 | Unity 2017.4.28f1 IL2CPP, FMOD • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Blasphemous 2](nxnative/dm/games/010040801A4C8000/README.md) | `010040801A4C8000` | 1.0.8 | Unity 2021.3.20f1 IL2CPP, FMOD • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Brain Memory Training Academy](nxnative/dm/games/0100F5D01DA3A000/README.md) | `0100F5D01DA3A000` | 1.0.0 (base) | Unity 2022.3.5f1 IL2CPP • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Captain Toad: Treasure Tracker](nxnative/dm/games/01009BF0072D4000/README.md) | `01009BF0072D4000` | 1.4.2 | Nintendo EPD Tokyo (`al` over sead / agl) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Catherine: Full Body](nxnative/dm/games/0100BF00112C0000/README.md) | `0100BF00112C0000` | 1.0.1a | Atlus GFD (CRI middleware) • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Celeste](nxnative/dm/games/01002B30028F6000/README.md) | `01002B30028F6000` | 1.4.1.1 | MonoGame (C# → C++ by BRUTE) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Cult of the Lamb](nxnative/dm/games/01002E7016C46000/README.md) | `01002E7016C46000` | 1.5.28 | Unity 2022.3.62f3 IL2CPP, FMOD • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Cuphead](nxnative/dm/games/0100A5C00D162000/README.md) | `0100A5C00D162000` | 1.3.7 | Unity 2017.4.9f1 IL2CPP • **NVN** | **8 GB** | A18+ (8GB) | Playable |
| [DARK SOULS: REMASTERED](nxnative/dm/games/01004AB00A260000/README.md) | `01004AB00A260000` | 1.0.3 | FromSoftware's engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | ⚡ **60 fps with 1.0.3** |
| [Digimon Story Cyber Sleuth: Complete Edition](nxnative/dm/games/010014E00DB56000/README.md) | `010014E00DB56000` | 1.0.3 | Media.Vision MVGL • **OpenGL** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Donkey Kong Country Returns HD](nxnative/dm/games/01009D901BC56000/README.md) | `01009D901BC56000` | 1.1.0 | Unity 6000.0.28f1 IL2CPP • **NVN** | **8 GB** | A17 Pro+ (8GB) | Playable |
| [Donkey Kong Country: Tropical Freeze](nxnative/dm/games/0100C1F0051B6000/README.md) | `0100C1F0051B6000` | 1.0.2 | Retro Studios' engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [DRAGON QUEST VII Reimagined](nxnative/dm/games/0100A9D01C446000/README.md) | `0100A9D01C446000` | 1.2.0 | Unreal Engine 4.27 • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition](nxnative/dm/games/01006C300E9F0000/README.md) | `01006C300E9F0000` | 1.0.4 | Unreal Engine 4.18 • **NVN** | **16 GB** | A18 Pro (8GB) | ⚡ **60 fps with 1.0.4** |
| [FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition](nxnative/dm/games/0100CE4010AAC000/README.md) | `0100CE4010AAC000` | 1.0.3 | Unity 2018.4.2f1 IL2CPP, FMOD • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [FINAL FANTASY RESONANCE](nxnative/dm/games/0100606026F12000/README.md) | `0100606026F12000` | 1.0.1 | Unreal Engine 5.6.1 • **NVN** | **16 GB** | A18 Pro (8GB) | ⚡ **60 fps with 1.0.1** |
| [FINAL FANTASY RESONANCE, Asian release](nxnative/dm/games/0100B8A026F14000/README.md) | `0100B8A026F14000` | 1.0.1 | Unreal Engine 5.6.1 • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [FINAL FANTASY TACTICS - The Ivalice Chronicles](nxnative/dm/games/010038B015560000/README.md) | `010038B015560000` | 1.4.0 | Square Enix's engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [FINAL FANTASY X/X-2 HD Remaster](nxnative/dm/games/0100BC300CB48000/README.md) | `0100BC300CB48000` | 1.0.0 (base) | PhyreEngine (Virtuos) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Fire Emblem Engage](nxnative/dm/games/0100A6301214E000/README.md) | `0100A6301214E000` | 2.0.0 | Unity 2020.3.18f1 IL2CPP, Wwise • **NVN** | **16 GB** | A18 Pro (8GB) | ⚡ **60 fps with 2.0.0** |
| [Fire Emblem: Three Houses](nxnative/dm/games/010055D009F78000/README.md) | `010055D009F78000` | 1.2.0 | Koei Tecmo's KTGL • **NVN** | **8 GB** | A18+ (8GB) | ⚡ **60 fps with 1.2.0** |
| [Ghost Trick: Phantom Detective](nxnative/dm/games/010029B018432000/README.md) | `010029B018432000` | 1.0.0 (base) | Capcom's RE Engine, Wwise • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Hades](nxnative/dm/games/0100535012974000/README.md) | `0100535012974000` | 1.0.38233 | Supergiant Games' engine (The Forge) • **Vulkan** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Hades II](nxnative/dm/games/0100A00019DE0000/README.md) | `0100A00019DE0000` | base game | Supergiant Games' engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Hollow Knight: Silksong](nxnative/dm/games/010013C00E930000/README.md) | `010013C00E930000` | 1.0.30000 | Unity 6000.0.50f1 IL2CPP • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Hyrule Warriors: Age of Calamity](nxnative/dm/games/01002B00111A2000/README.md) | `01002B00111A2000` | 1.3.0 | Koei Tecmo's KTGL • **NVN** | **6 GB** | A17 Pro+ (8GB) | ⚡ **60 fps with 1.3.0** |
| [Kirby and the Forgotten Land](nxnative/dm/games/01004D300C5AE000/README.md) | `01004D300C5AE000` | 1.1.0 | HAL Laboratory's engine (NintendoWare) • **NVN** | **8 GB** | A18+ (8GB) | Playable |
| [Luigi's Mansion 3](nxnative/dm/games/0100DCA0064A6000/README.md) | `0100DCA0064A6000` | 1.4.0 | Next Level Games' engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | ⚡ **60 fps with 1.4.0** |
| [Mario Kart 8 Deluxe](nxnative/dm/games/0100152000022000/README.md) | `0100152000022000` | 4.0.0, 4.0.1 | Nintendo (sead / agl / gsys, NintendoWare, Pia) • **NVN** | **6 GB** | A17 Pro+ (8GB) | no Mii heads |
| [Mario + Rabbids Kingdom Battle](nxnative/dm/games/010067300059A000/README.md) | `010067300059A000` | 1.9.589692 | Ubisoft Snowdrop • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Mario + Rabbids Sparks of Hope](nxnative/dm/games/0100317013770000/README.md) | `0100317013770000` | 1.6.2225577 | Ubisoft Snowdrop • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Mario Party Superstars](nxnative/dm/games/01006FE013472000/README.md) | `01006FE013472000` | 1.1.1 | NintendoWare Bezel Engine 1.55 • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Mario vs. Donkey Kong](nxnative/dm/games/0100B99019412000/README.md) | `0100B99019412000` | 1.0.1 (cartridge base) | Nintendo (sead / agl / gsys) • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Metroid Dread](nxnative/dm/games/010093801237C000/README.md) | `010093801237C000` | 2.1.0 | MercurySteam's engine • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Monster Hunter Rise](nxnative/dm/games/0100B04011742000/README.md) | `0100B04011742000` | 16.0.2 | Capcom's RE Engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | ⚡ **60 fps with 16.0.2** |
| [Monster Train 2](nxnative/dm/games/010051701FB46000/README.md) | `010051701FB46000` | 1.1.1 | Unity 2022.3.54f1 IL2CPP • **NVN** | **4 GB** | A17 Pro+ (8GB) | ⚡ **60 fps with 1.1.1** |
| [NieR:Automata The End of YoRHa Edition](nxnative/dm/games/010056B015FE8000/README.md) | `010056B015FE8000` | 1.0.3 | PlatinumGames' engine • **NVN** | **16 GB** | A18 Pro (8GB) | ⚡ **60 fps with 1.0.3** |
| [NieR:Automata The End of YoRHa Edition, Asian release](nxnative/dm/games/0100B8E016F76000/README.md) | `0100B8E016F76000` | 1.0.3 | PlatinumGames' engine • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [OCTOPATH TRAVELER](nxnative/dm/games/010057D006492000/README.md) | `010057D006492000` | 1.0.5 | Unreal Engine 4.18 • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Octopath Traveler 0](nxnative/dm/games/01005270232F2000/README.md) | `01005270232F2000` | 1.0.5 | Unreal Engine 5 • **NVN** | **4 GB** | A17 Pro+ (8GB) | ⚡ **60 fps with 1.0.5** |
| [OCTOPATH TRAVELER II](nxnative/dm/games/0100A3501946E000/README.md) | `0100A3501946E000` | 1.1.1 | Unreal Engine 4.27 • **NVN** | **16 GB** | A18 Pro (8GB) | ⚡ **60 fps with 1.1.1** |
| [Paper Mario: The Origami King](nxnative/dm/games/0100A3900C3E2000/README.md) | `0100A3900C3E2000` | 1.0.1 | Intelligent Systems' engine on NintendoWare • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Paper Mario: The Thousand-Year Door](nxnative/dm/games/0100ECD018EBE000/README.md) | `0100ECD018EBE000` | 1.0.1 | Intelligent Systems' engine on NintendoWare • **NVN** | **8 GB** | A17 Pro+ (8GB) | ⚡ **60 fps with 1.0.1** |
| [Patrick's Parabox](nxnative/dm/games/0100E44016C22000/README.md) | `0100E44016C22000` | 1.0.1 | Unity 2020.3.30f1 IL2CPP • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Persona 4 Golden](nxnative/dm/games/010062B01525C000/README.md) | `010062B01525C000` | 1.0.2 | Atlus in-house (CRI middleware) • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Persona 5 Royal](nxnative/dm/games/01005CA01580E000/README.md) | `01005CA01580E000` | 1.0.2 | Atlus GFD / Needle (CRI middleware) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Pikmin 4](nxnative/dm/games/0100B7C00933A000/README.md) | `0100B7C00933A000` | 1.1.1 | Unreal Engine 4.26.2 • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Pokémon Brilliant Diamond](nxnative/dm/games/0100000011D90000/README.md) | `0100000011D90000` | 1.3.0 | Unity 2019.4.27f1 IL2CPP • **NVN** | **8 GB** | A17 Pro+ (8GB) | ⚡ **60 fps with 1.3.0** |
| [Pokémon Legends: Arceus](nxnative/dm/games/01001F5010DFA000/README.md) | `01001F5010DFA000` | 1.1.1 | Game Freak's engine (between Sword's and Trinity) • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Pokémon Legends: Z-A](nxnative/dm/games/0100F43008C44000/README.md) | `0100F43008C44000` | 2.0.2 | Game Freak's Trinity engine • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Pokémon Scarlet](nxnative/dm/games/0100A3D008C5C000/README.md) | `0100A3D008C5C000` | 1.0.0 | Game Freak's Trinity engine • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Pokémon Shield](nxnative/dm/games/01008DB008C2C000/README.md) | `01008DB008C2C000` | 1.3.2 | Game Freak's engine (before Trinity) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Pokémon Sword](nxnative/dm/games/0100ABF008968000/README.md) | `0100ABF008968000` | 1.3.2 | Game Freak's engine (before Trinity) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Portal](nxnative/dm/games/01007BB017812000/README.md) | `01007BB017812000` | 1.0.3 | Valve's Source engine (28 NROs) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Raging Loop](nxnative/dm/games/010020400E1C2000/README.md) | `010020400E1C2000` | 1.0.0 (base) | Unity 2018.3.10f1 IL2CPP • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Return of the Obra Dinn](nxnative/dm/games/010032E00E6E2000/README.md) | `010032E00E6E2000` | 1.0.1 | Unity 2017.4.28f1 IL2CPP • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Rhythm Heaven Groove](nxnative/dm/games/0100D9F01D474000/README.md) | `0100D9F01D474000` | 1.0.0 (base) | Nintendo's own 2D engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [ROMANCE OF THE THREE KINGDOMS 8 REMAKE](nxnative/dm/games/0100C2A01A730000/README.md) | `0100C2A01A730000` | 1.1.3 | Koei Tecmo's KTGL • **NVN** | **6 GB** | A17 Pro+ (8GB) | controller only |
| [Sea of Stars](nxnative/dm/games/01008C0016544000/README.md) | `01008C0016544000` | 2.0.59001 | Unity 2021.3.25f1 IL2CPP • **NVN** | **8 GB** | A17 Pro+ (8GB) | Playable |
| [SHIN MEGAMI TENSEI V: Vengeance](nxnative/dm/games/010069C01AB82000/README.md) | `010069C01AB82000` | 1.0.3 | Unreal Engine 4.27 • **NVN** | **16 GB** | A18 Pro (8GB) | ⚡ **60 fps with 1.0.3** |
| [Silver Pines](nxnative/dm/games/0100D5B026844000/README.md) | `0100D5B026844000` | 1.0.0 (base) | Unity 6000.3.15f1 IL2CPP • **NVN** | **8 GB** | A17 Pro+ (8GB) | Playable |
| [STEINS;GATE RE:BOOT](nxnative/dm/games/01008BC024FDE000/README.md) | `01008BC024FDE000` | 1.0.3 | M2's ADV engine • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Stardew Valley](nxnative/dm/games/0100E65002BB8000/README.md) | `0100E65002BB8000` | 1.6.15.13 | MonoGame (C# → C++ by BRUTE) • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [SteamWorld Dig](nxnative/dm/games/01009320084A4000/README.md) | `01009320084A4000` | 1.12.0 | Image & Form's own engine (SteamWorld) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [SteamWorld Dig 2](nxnative/dm/games/0100CA9002322000/README.md) | `0100CA9002322000` | 4.1.1 | Image & Form's own engine (SteamWorld) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [SteamWorld Heist II](nxnative/dm/games/010089C01E9CA000/README.md) | `010089C01E9CA000` | 1.5.1 | Thunderful's own engine (SteamWorld) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [SteamWorld Heist: Ultimate Edition](nxnative/dm/games/010016100A186000/README.md) | `010016100A186000` | 2.2.1 | Image & Form's own engine (SteamWorld) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Streets of Rage 4](nxnative/dm/games/0100EC9010258000/README.md) | `0100EC9010258000` | 1.0.9 | MonoGame (C# → C++ by BRUTE) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Subnautica](nxnative/dm/games/0100429011144000/README.md) | `0100429011144000` | 1.23.83358 | Unity 2019.4.36f1 IL2CPP, FMOD • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Subnautica: Below Zero](nxnative/dm/games/010014C011146000/README.md) | `010014C011146000` | 1.23.55063 | Unity 2019.4.36f1 IL2CPP, FMOD • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Super Mario 3D World + Bowser's Fury](nxnative/dm/games/010028600EBDA000/README.md) | `010028600EBDA000` | 1.2.2 | Nintendo EPD (sead / agl, NintendoWare) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Super Mario Bros. Wonder](nxnative/dm/games/010015100B514000/README.md) | `010015100B514000` | 1.2.1 (and 1.0.0) | Nintendo EPD (sead / agl) • **NVN** | **8 GB** | A17 Pro+ (8GB) | Playable |
| [Super Mario Galaxy](nxnative/dm/games/010099C022B96000/README.md) | `010099C022B96000` | 1.3.1 | NERD's Hagi Wii layer (recompiled PowerPC) • **Vulkan** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Super Mario Galaxy 2](nxnative/dm/games/0100FD8022DAA000/README.md) | `0100FD8022DAA000` | 1.4.0 | NERD's Hagi Wii layer (recompiled PowerPC) • **Vulkan** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [Super Mario Odyssey](nxnative/dm/games/0100000000010000/README.md) | `0100000000010000` | 1.4.1 | Nintendo EPD (sead / agl, NintendoWare) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Super Mario Party Jamboree](nxnative/dm/games/0100965017338000/README.md) | `0100965017338000` | 2.3.0 | NintendoWare Bezel Engine 2.9 • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Super Smash Bros. Ultimate](nxnative/dm/games/01006A800016E000/README.md) | `01006A800016E000` | 13.0.5 | Bandai Namco / Sora's engine • **NVN** | **8 GB** | A18+ (8GB) | offline |
| [Taiko Risshiden V DX](nxnative/dm/games/0100346017304000/README.md) | `0100346017304000` | 1.1.1 | Koei Tecmo's KTGL • **NVN** | **4 GB** | A17 Pro+ (8GB) | 👆 *Japanese · played by touch* |
| [Team Sonic Racing](nxnative/dm/games/010092B0091D0000/README.md) | `010092B0091D0000` | 1.0.3 | Sumo Digital's engine, Wwise • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [The Great Ace Attorney Chronicles](nxnative/dm/games/010036E00FB20000/README.md) | `010036E00FB20000` | 1.0.0 (base) | Capcom's MT Framework • **NVN** | **4 GB** | A17 Pro+ (8GB) | Playable |
| [The Legend of Zelda: Breath of the Wild](nxnative/dm/games/01007EF00011E000/README.md) | `01007EF00011E000` | 1.9.0 | Nintendo EPD (sead / agl / gsys / ksys, Havok) • **NVN** | **8 GB** | A18+ (8GB) | ⚡ **60 fps with 1.9.0** |
| [The Legend of Zelda: Echoes of Wisdom](nxnative/dm/games/01008CF01BAAC000/README.md) | `01008CF01BAAC000` | 1.1.0 | NintendoWare (G3d / Ui2d / Vfx) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [The Legend of Zelda: Link's Awakening](nxnative/dm/games/01006BB00C6F0000/README.md) | `01006BB00C6F0000` | 1.1.0 | Grezzo's engine on NintendoWare • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [The Legend of Zelda: Tears of the Kingdom](nxnative/dm/games/0100F2C0115B6000/README.md) | `0100F2C0115B6000` | 1.4.3 | Nintendo EPD (sead / agl / gsys) • **NVN** | **16 GB** | A18 Pro (8GB) | ⚡ **60 fps with 1.4.3** |
| [Unicorn Overlord](nxnative/dm/games/010069401ADB8000/README.md) | `010069401ADB8000` | 1.05 | Vanillaware's engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Unicorn Overlord, Asian release](nxnative/dm/games/010054B01AD92000/README.md) | `010054B01AD92000` | 1.05 | Vanillaware's engine • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Well Dweller](nxnative/dm/games/0100217023F6C000/README.md) | `0100217023F6C000` | 1.0.0 | GameMaker Studio 2 (its runner) • **OpenGL** | **6 GB** | A17 Pro+ (8GB) | Playable |
| [Xenoblade Chronicles: Definitive Edition](nxnative/dm/games/0100FF500E34A000/README.md) | `0100FF500E34A000` | 1.1.2 | Monolith's engine • **NVN** | **8 GB** | A18+ (8GB) | Playable |
| [Xenoblade Chronicles 2](nxnative/dm/games/0100E95004038000/README.md) | `0100E95004038000` | 2.1.0 | Monolith's engine • **NVN** | **8 GB** | A18+ (8GB) | Playable |
| [Xenoblade Chronicles 3](nxnative/dm/games/010074F013262000/README.md) | `010074F013262000` | 2.2.1 | Monolith's engine • **NVN** | **16 GB** | A18 Pro (8GB) | Playable |
| [Yoshi's Crafted World](nxnative/dm/games/01006000040C2000/README.md) | `01006000040C2000` | 1.0.1 | Unreal Engine 4.18 era (Good-Feel) • **NVN** | **6 GB** | A17 Pro+ (8GB) | Playable |

---
## 🚀 Platform Features Timeline (Newest to Oldest)

A complete, deduplicated chronology of every milestone and capability introduced to the NxNative platform:

### 🌟 Current Build (v1.8.2 / Latest) — *October 2026*
> **3D Mouse Look Camera Control, Library Drag & Drop, and 14 Locales Expansion**

- **3D Mouse Look (FPS Mode)**: Smooth first-person and third-person camera control with mouse tracking, horizontal/vertical sensitivity sliders, Y-axis inversion, and custom toggle shortcuts.
- **Interactive Input Capture & Extra Mouse Buttons**: Complete revamp of the key binding engine. Seamlessly captures Mouse 4 (Back) and Mouse 5 (Forward) without conflicting key presses.
- **Mouse Drag & Drop Library Organization**: Reorder game cards directly on the Switch Dark Mode console carousel with mouse drag & drop, automatically persisted to local storage.
- **14 Locales with Alphabetical Settings**: Added European Portuguese (`Português de Portugal - PT-PT`) alongside PT-BR, EN, JA, ES, FR, DE, IT, KO, PL, RU, SV, ZH-Hans, ZH-Hant, neatly sorted alphabetically in Settings.
- **Frame Pacing & 60 FPS Stability**: Refined Metal frame pacing and frame presentation interval timers across high-refresh ProMotion displays.

### 🌟 v1.8.1 — *October 2026*
> **Official Joy-Con Taskbar Icon, Menu Bar Customization & iOS Sideloading Fixes**

- **Joy-Con Status Bar Icon**: Choose between the signature Switch Joy-Con silhouette and the classic launcher icon in the macOS menu bar.
- **iOS Deployment Enhancements**: Streamlined `xcrun devicectl` bundle installations and AirDrop packaging for iOS 18/26.
- **Storage & Cache Diagnostics**: Real-time inspection of shader cache sizes, RomFS asset bundles, and Atmosphere LayeredFS mods.

### 🌟 v1.8.0 — *October 2026*
> **Apple Liquid Glass UI Overhaul & 13 Multi-Language Locales**

- **Apple Liquid Glass Aesthetic**: Modern translucent glassmorphism design optimized for macOS Sonoma and Sequoia dark mode.
- **Runtime Localization Engine**: Zero-restart dynamic language switching across 13 international locales.
- **Automated Upstream Sync Workflows**: Continuous CI validation verifying compatibility with incoming `seedds/nxnative` upstream updates.

### 🌟 v1.7.0 — *October 2026*
> **Smart iCloud Cross-Save Synchronization & Single-Button Device Transfer**

- **Cross-Device iCloud Save Sync**: Automatic detection of `~/Library/Application Support/nxnative/<TID>/save` with cloud backup.
- **Single-Button Device Sync**: Instant transfer of game save states and Atmosphere mods between Mac and connected iPhone/iPad.
- **1-Click Finder Reveal**: Instant access to save folders and game directories from the Game Details deck.

### 🌟 v1.6.2 & v1.6.1 — *October 2026*
> **Automated Dependency Installer & Cross-Architecture Stability**

- **Automated In-App Environment Setup**: One-click installation of Homebrew, Python 3.12-3.14, and required toolchains (`libhac`, `zstandard`, `pycryptodome`, `lz4`).
- **Universal Toolchain Detection**: Automatic PATH resolution supporting both Apple Silicon `/opt/homebrew` and legacy paths.

### 🌟 v1.6.0 — *October 2026*
> **Direct iOS IPA Deployment & Audio Polish**

- **Dual Target Compilation**: Seamlessly output standalone macOS applications (`.app`) or iOS application archives (`.ipa`).
- **Console Audio Feedback**: Authentic UI sound effects and navigation feedback during controller and keyboard browsing.

### 🌟 v1.5.0 & v1.5.1 — *October 2026*
> **Switch Dark Mode 2-Tier Library & Full Gamepad Navigation**

- **Two-Tier Category Shelves**: Distinct visual shelves for macOS Native compiled games and iOS Sideloadable ports.
- **End-to-End Gamepad Navigation**: Browse the entire library, launch games, and adjust settings using a controller (D-Pad, Stick, Face Buttons).
- **Async High-Speed Library Scanner**: Indexes hundreds of gigabytes of game dumps in seconds with zero UI stutter.

### 🌟 v1.4.0 — *October 2026*
> **8 Neon Highlight Themes, Cumulative RAM Filters & Auto-Grouping**

- **8 Neon Personalization Themes**: Neon Blue, Neon Red, Cyberpunk Yellow, Emerald Green, Sakura Pink, Electric Purple, Sunset Orange, and Pure White.
- **Cumulative RAM Filtering**: Filter the compatibility catalog by memory requirements (4GB, 6GB, 8GB, 16GB+).
- **Automatic Base + Update + DLC Bundling**: Intelligent grouping of base game dumps with corresponding updates and expansion packs.

### 🌟 v1.3.0 — *October 2026*
> **Official High-Res Box Art Covers & Universal Gamepad Profiles**

- **Authentic Box Art Integration**: Automatic artwork matching via GameTDB and custom cover artwork picker (`.jpg`, `.png`, `.webp`).
- **Universal Controller Profiles**: Plug-and-play mapping presets for Switch Pro Controller, PlayStation DualSense / DualShock 4, and Xbox Wireless Controller.

### 🌟 v1.2.0 — *October 2026*
> **Encrypted Package Parsing & Safe Upstream Sync Utility**

- **Encrypted Container Inspection**: Direct extraction and Title ID identification for `.xci`, `.nsp`, and `.nsz` archives.
- **Safe Upstream Sync Script (`sync_upstream.sh`)**: Conflict-free merging mechanism for keeping the core engine synchronized with upstream.

### 🌟 v1.0.0 — *October 2026*
> **Initial Launch of the NxNative Graphical Interface**

- **3-Step Guided Porting Wizard**: Visual drag & drop porting eliminating terminal commands.
- **Metal 4 Shader Pipeline**: Ahead-of-time SPIR-V to Metal Shading Language compilation.
- **Native ARM64 Mach-O Host**: Direct execution of Switch game binaries on Apple Silicon.

---

## 🔄 How to Port an Unlisted Game

Want to run a game not currently listed in the catalog? Follow these steps:

1. **Open NxNative Launcher**: Navigate to the **Port** tab.
2. **Drag & Drop your Dump**: Drop your legally dumped `.nsp` or `.xci` folder. The launcher will automatically inspect its ExeFS and determine whether an upstream profile exists or can be provisioned.
3. **Contribute Upstream**: If the game runs cleanly, follow [CONTRIBUTING.md](CONTRIBUTING.md) to submit your `game.tsv` profile and screenshot to `seedds/nxnative`.

<div align="center">
  <a href="README.md"><b>⬅️ Back to Overview & Guide</b></a> &nbsp;&nbsp;|&nbsp;&nbsp;  <a href="CONTRIBUTING.md"><b>🤝 Read Contributing Guidelines</b></a>
</div>
