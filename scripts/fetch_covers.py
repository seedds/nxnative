#!/usr/bin/env python3
import os
import re
import sys
import shutil
import subprocess
import urllib.parse
import urllib.request
import ssl
from concurrent.futures import ThreadPoolExecutor

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

HEADERS = {
    'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36'
}

# Verified Title ID -> GameTDB 5-letter IDs (checked against official switchtdb.txt)
VERIFIED_GAMETDB = {
    "0100000000010000": ["AAACA", "AAACB"], # Super Mario Odyssey
    "0100000011D90000": ["AX7QA"],          # Pokémon Brilliant Diamond
    "010005501E68C000": ["BG9VA"],          # Ace Attorney Investigations Collection
    "010013C00E930000": ["AUSUA"],          # Hollow Knight: Silksong
    "010014E00DB56000": ["ATPMB", "ATPMA"], # Digimon Story Cyber Sleuth: Complete Edition
    "010015100B514000": ["AQMXA"],          # Super Mario Bros. Wonder
    "0100152000022000": ["AABPA", "AABPB"], # Mario Kart 8 Deluxe
    "010020400E1C2000": ["AEK3B", "AEK3A"], # Raging Loop
    "010020D01AD24000": ["BCCHA"],          # ANIMAL WELL
    "010028600EBDA000": ["AUZPA"],          # Super Mario 3D World + Bowser's Fury
    "010029B018432000": ["A8VEA"],          # Ghost Trick: Phantom Detective
    "01002B00111A2000": ["AXEAA", "AXEAB"], # Hyrule Warriors: Age of Calamity
    "01002B30028F6000": ["ACF3A"],          # Celeste
    "0100300012F2A000": ["AZRMA"],          # Advance Wars 1+2: Re-Boot Camp
    "0100317013770000": ["A2G7A", "A2G7B"], # Mario + Rabbids Sparks of Hope
    "010032E00E6E2000": ["AULSA"],          # Return of the Obra Dinn
    "0100346017304000": ["A7G4A"],          # Taiko Risshiden V DX
    "010036E00FB20000": ["AV3JA", "AV3JB"], # The Great Ace Attorney Chronicles
    "010038B015560000": ["A43UA"],          # FINAL FANTASY TACTICS - The Ivalice Chronicles
    "01003FC01670C000": ["A3RVA", "A3RVB", "A3RVC"], # 13 Sentinels: Aegis Rim
    "010040801A4C8000": ["BBLYA"],          # Blasphemous 2
    "010045C014650000": ["A3RVB", "A3RVA", "A3RVC"], # 13 Sentinels: Aegis Rim, Japanese release
    "01004A4010FEA000": ["AXB7A"],          # Bayonetta 3
    "01004AB00A260000": ["AK63B", "AK63A"], # DARK SOULS: REMASTERED
    "01004D300C5AE000": ["ARZGA"],          # Kirby and the Forgotten Land
    "010051701FB46000": ["BKCRA"],          # Monster Train 2
    "01005270232F2000": ["BPRWA"],          # Octopath Traveler 0
    "0100535012974000": ["AY9RA", "AY9R1"], # Hades
    "010054B01AD92000": ["BACCC", "BACCA", "BACCB"], # Unicorn Overlord, Asian release
    "010055D009F78000": ["ANVYA"],          # Fire Emblem: Three Houses
    "010056B015FE8000": ["A532A", "A532B"], # NieR:Automata The End of YoRHa Edition
    "010057D006492000": ["AGY7A", "AGY7B", "AGY7E"], # OCTOPATH TRAVELER
    "01005CA01580E000": ["A42UC", "A42UA"], # Persona 5 Royal
    "01006000040C2000": ["AEA2A", "AEA2B"], # Yoshi's Crafted World
    "0100606026F12000": ["BUCVA"],          # FINAL FANTASY RESONANCE
    "0100620012D6E000": ["AZLSA"],          # Big Brain Academy: Brain vs. Brain
    "010069401ADB8000": ["BACCA", "BACCB", "BACCC"], # Unicorn Overlord
    "010069C01AB82000": ["BBXAA", "BBXAB", "BBXAC"], # SHIN MEGAMI TENSEI V: Vengeance
    "01006A800016E000": ["AAABA"],          # Super Smash Bros. Ultimate
    "01006BB00C6F0000": ["AR3NA"],          # The Legend of Zelda: Link's Awakening
    "01006C300E9F0000": ["ALC7C", "ALC7E", "ALC7A"], # DRAGON QUEST XI S
    "01006F8002326000": ["ACBAA"],          # Animal Crossing: New Horizons
    "0100713016CCC000": ["A4EFA", "A4EFB", "A4EFC"], # AI: THE SOMNIUM FILES - nirvanA Initiative
    "01007300020FA000": ["AB48A"],          # ASTRAL CHAIN
    "010074F013262000": ["AZ3HA"],          # Xenoblade Chronicles 3
    "010076F0049A2000": ["AE99A"],          # Bayonetta
    "01007960049A0000": ["AE98A"],          # Bayonetta 2
    "01007BB017812000": ["A7Z4A"],          # Portal Companion Collection
    "01007EF00011E000": ["AAAAA"],          # The Legend of Zelda: Breath of the Wild
    "010086A022444000": ["BNQEA"],          # BALL x PIT
    "010089B00D09C000": ["ASRPB", "ASRPA", "ASRPD"], # AI: THE SOMNIUM FILES
    "01008BC024FDE000": ["AMNPA", "AMNPB", "ASB4A"], # STEINS;GATE RE:BOOT
    "01008C0016544000": ["A6G7A"],          # Sea of Stars
    "01008CF01BAAC000": ["BDGEA"],          # The Legend of Zelda: Echoes of Wisdom
    "01008D7016438000": ["A3RVC", "A3RVA", "A3RVB"], # 13 Sentinels: Aegis Rim (Asian release)
    "01008DB008C2C000": ["ALZBA"],          # Pokémon Shield
    "010093801237C000": ["AYL8A"],          # Metroid Dread
    "010099C022B96000": ["BN84A"],          # Super Mario Galaxy
    "01009BF0072D4000": ["AJH9A"],          # Captain Toad: Treasure Tracker
    "01009D901BC56000": ["BDLWA"],          # Donkey Kong Country Returns HD
    "0100A00019DE0000": ["BA3FA"],          # Hades II
    "0100A3501946E000": ["A98RA"],          # OCTOPATH TRAVELER II
    "0100A3D008C5C000": ["ALZXA"],          # Pokémon Scarlet
    "0100A5C00D162000": ["ASVUA", "ASVUG"], # Cuphead
    "0100A6301214E000": ["AYFNA"],          # Fire Emblem Engage
    "0100A83022A22000": ["BN47A"],          # Absolum
    "0100A9D01C446000": ["BD9VA", "BD9VB"], # DRAGON QUEST VII Reimagined
    "0100ABF008968000": ["ALZAA"],          # Pokémon Sword
    "0100B04011742000": ["AXSEA", "AXSEB"], # Monster Hunter Rise
    "0100B7C00933A000": ["AMPYA"],          # Pikmin 4
    "0100B99019412000": ["A97PA"],          # Mario vs. Donkey Kong
    "0100BF00112C0000": ["AJ2AC"],          # Catherine: Full Body
    "0100C1F0051B6000": ["AFWTA"],          # Donkey Kong Country: Tropical Freeze
    "0100C2A01A730000": ["BBTCA", "BBTCC", "BBTCB"], # ROMANCE OF THE THREE KINGDOMS 8 REMAKE
    "0100CD801CE5E000": ["BEYKA"],          # Balatro
    "0100CE4010AAC000": ["AUYRA", "AUYRB", "AUYRC"], # FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition
    "0100D9F01D474000": ["BFLTA"],          # Rhythm Heaven Groove
    "0100DCA0064A6000": ["AG3JA"],          # Luigi's Mansion 3
    "0100E44016C22000": ["A6YSA"],          # Patrick's Parabox
    "0100E65002BB8000": ["ACPGA", "ACPGB"], # Stardew Valley
    "0100E95004038000": ["ADENB", "ADENA"], # Xenoblade Chronicles 2
    "0100ECD018EBE000": ["A9QDA"],          # Paper Mario: The Thousand-Year Door
    "0100F2C0115B6000": ["AXN7A"],          # The Legend of Zelda: Tears of the Kingdom
    "0100F43008C44000": ["ALZLA"],          # Pokémon Legends: Z-A
    "0100F5D01DA3A000": ["BF7QA"],          # Brain Memory Training Academy
    "0100FD8022DAA000": ["BPDLA"],          # Super Mario Galaxy 2
    "0100FF500E34A000": ["AUBQA"]           # Xenoblade Chronicles: Definitive Edition
}

# IDs that previously had wrong GameTDB codes and must be re-downloaded
CHANGED_IDS = {
    "010005501E68C000", "010014E00DB56000", "010029B018432000", "01002B30028F6000",
    "0100300012F2A000", "0100317013770000", "010036E00FB20000", "01003FC01670C000",
    "01004A4010FEA000", "01004AB00A260000", "0100535012974000", "010056B015FE8000",
    "010057D006492000", "01005CA01580E000", "0100620012D6E000", "010069401ADB8000",
    "010069C01AB82000", "01006C300E9F0000", "010076F0049A2000", "01007960049A0000",
    "01007BB017812000", "01008C0016544000", "01008CF01BAAC000", "01008D7016438000",
    "01009BF0072D4000", "01009D901BC56000", "0100A3501946E000", "0100A3D008C5C000",
    "0100A5C00D162000", "0100B99019412000", "0100BF00112C0000", "0100E65002BB8000",
    "0100E95004038000", "0100ECD018EBE000"
}

def get_image_dimensions(path):
    if not os.path.isfile(path):
        return 0, 0
    try:
        out = subprocess.check_output(["sips", "-g", "pixelWidth", "-g", "pixelHeight", path], stderr=subprocess.DEVNULL).decode()
        w = int(re.search(r"pixelWidth:\s*(\d+)", out).group(1))
        h = int(re.search(r"pixelHeight:\s*(\d+)", out).group(1))
        return w, h
    except Exception:
        return 0, 0

def is_valid_portrait(path):
    w, h = get_image_dimensions(path)
    return w > 200 and h > int(w * 1.15)

def download_url_if_portrait(url, dest_path):
    req = urllib.request.Request(url, headers=HEADERS)
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=4.5) as res:
            if res.status == 200:
                data = res.read()
                if len(data) > 8000:
                    tmp_path = f"{dest_path}.{os.getpid()}.tmp"
                    with open(tmp_path, "wb") as f:
                        f.write(data)
                    w, h = get_image_dimensions(tmp_path)
                    if w > 180 and h > int(w * 1.12):
                        if url.lower().endswith(".png"):
                            subprocess.run(["sips", "-s", "format", "jpeg", tmp_path, "--out", dest_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                            os.remove(tmp_path)
                        else:
                            shutil.move(tmp_path, dest_path)
                        return True, url, os.path.getsize(dest_path)
                    if os.path.exists(tmp_path):
                        os.remove(tmp_path)
    except Exception:
        pass
    return False, None, 0

def fetch_cover_from_sources(tid, title, dest_path):
    gids = VERIFIED_GAMETDB.get(tid, [])
    subdirs = ["coverHQ", "coverM", "cover"]
    regions = ["US", "EN", "JA", "AU", "FR", "DE", "ES", "KO", "ZH"]
    for gid in gids:
        for sub in subdirs:
            for reg in regions:
                url = f"https://art.gametdb.com/switch/{sub}/{reg}/{gid}.jpg"
                ok, u, sz = download_url_if_portrait(url, dest_path)
                if ok:
                    return True, u, sz

    clean_variants = [
        title,
        title.replace(":", " -"),
        re.sub(r"\s*\(.*?\)", "", title).replace(":", " -"),
    ]
    regions_lr = [" (USA)", " (World)", " (USA, Europe)", " (Europe)", " (Japan)", ""]
    for base_t in dict.fromkeys(clean_variants):
        for r_sfx in regions_lr:
            encoded = urllib.parse.quote(f"{base_t}{r_sfx}.png")
            url = f"https://thumbnails.libretro.com/Nintendo%20-%20Switch/Named_Boxarts/{encoded}"
            ok, u, sz = download_url_if_portrait(url, dest_path)
            if ok:
                return True, u, sz

    return False, None, 0

def main():
    force = "--force" in sys.argv
    home = os.path.expanduser("~")
    cache_dir = os.path.join(home, "Library/Application Support/nxnative/covers")
    repo_covers_dir = "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images/covers"
    os.makedirs(cache_dir, exist_ok=True)
    os.makedirs(repo_covers_dir, exist_ok=True)
    
    base_games = "/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/dm/games"
    pattern = re.compile(r"^#\s+(.+?)(?:\s+\(`?([0-9A-Fa-f]{16})`?\))?$")
    
    profiles = []
    for item in sorted(os.listdir(base_games)):
        p = os.path.join(base_games, item, "README.md")
        if os.path.isfile(p):
            with open(p, "r", encoding="utf-8", errors="ignore") as f:
                first_line = f.readline().strip()
                m = pattern.match(first_line)
                title = m.group(1) if m else item
                profiles.append((item.upper(), title))
                
    print(f"Loaded {len(profiles)} game profiles.")

    def process_one(item):
        tid, title = item
        dest_cache = os.path.join(cache_dir, f"{tid}.jpg")
        dest_repo = os.path.join(repo_covers_dir, f"{tid}.jpg")
        
        needs_redownload = force or (tid in CHANGED_IDS) or not is_valid_portrait(dest_repo)
        if not needs_redownload:
            if not is_valid_portrait(dest_cache):
                shutil.copyfile(dest_repo, dest_cache)
            return ("OK", tid, title, "cached")
            
        ok, url, sz = fetch_cover_from_sources(tid, title, dest_cache)
        if ok:
            shutil.copyfile(dest_cache, dest_repo)
            return ("DL", tid, title, f"{url} ({sz}B)")
        else:
            # Remove stale landscape image if present so we know it needs fallback
            if os.path.isfile(dest_repo) and not is_valid_portrait(dest_repo):
                os.remove(dest_repo)
            if os.path.isfile(dest_cache) and not is_valid_portrait(dest_cache):
                os.remove(dest_cache)
            return ("MISS", tid, title, None)

    with ThreadPoolExecutor(max_workers=10) as ex:
        results = list(ex.map(process_one, profiles))

    ok_cnt = 0
    unresolved = []
    for status, tid, title, info in results:
        if status in ("OK", "DL"):
            ok_cnt += 1
            print(f"[{status}] {tid} | {title} -> {info}")
        else:
            unresolved.append((tid, title))
            print(f"[MISS] {tid} | {title}")

    print(f"\nPrimary pass complete: {ok_cnt}/{len(profiles)} portrait covers ready.")
    if unresolved:
        print("Unresolved titles:", unresolved)

if __name__ == "__main__":
    main()
