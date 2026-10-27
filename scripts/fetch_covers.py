#!/usr/bin/env python3
import os
import re
import sys
import shutil
import urllib.parse
import urllib.request
import ssl

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

HEADERS = {
    'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36'
}

KNOWN_MAP = {
    "0100F2C0115B6000": "AXN7A",  # Zelda: Tears of the Kingdom
    "01007EF00011E000": "AAAAA",  # Zelda: Breath of the Wild
    "01006BB00C6F0000": "AR3NA",  # Zelda: Link's Awakening
    "01002B00111A2000": "AXEAA",  # Hyrule Warriors: Age of Calamity
    "01008CF01BAAC000": "BDZ3A",  # Zelda: Echoes of Wisdom
    "0100000000010000": "AAACA",  # Super Mario Odyssey
    "0100152000022000": "AABPA",  # Mario Kart 8 Deluxe
    "010028600EBDA000": "AUZPA",  # Super Mario 3D World + Bowser's Fury
    "010015100B514000": "AQMXA",  # Super Mario Bros. Wonder
    "01006A800016E000": "AAABA",  # Super Smash Bros. Ultimate
    "01006F8002326000": "ACBAA",  # Animal Crossing: New Horizons
    "010093801237C000": "AYL8A",  # Metroid Dread
    "0100CD801CE5E000": "BEYKA",  # Balatro
    "0100A9D01C446000": "BD9VA",  # DRAGON QUEST VII Reimagined
    "01006C300E9F0000": "ALC7A",  # Dragon Quest XI S
    "01004D300C5AE000": "ARZGA",  # Kirby and the Forgotten Land
    "0100B7C00933A000": "AMPYA",  # Pikmin 4
    "0100ECD018EBE000": "A9QWA",  # Paper Mario: The Thousand-Year Door
    "01005CA01580E000": "AXU7A",  # Persona 5 Royal
    "010055D009F78000": "ANVYA",  # Fire Emblem: Three Houses
    "0100A6301214E000": "AYFNA",  # Fire Emblem Engage
    "010069401ADB8000": "AQ36A",  # Unicorn Overlord
    "01003FC01670C000": "AU9HA",  # 13 Sentinels: Aegis Rim
    "01008D7016438000": "AU9HA",  # 13 Sentinels Asian
    "01004A4010FEA000": "AWGKA",  # Bayonetta 3
    "010076F0049A2000": "AEUCA",  # Bayonetta
    "01007960049A0000": "BKBPA",  # Bayonetta 2
    "0100535012974000": "AUY4A",  # Hades
    "01002B30028F6000": "AHGAA",  # Celeste
    "0100A5C00D162000": "AFNGA",  # Cuphead
    "0100E65002BB8000": "ACFWA",  # Stardew Valley
    "0100B04011742000": "AXSEA",  # Monster Hunter Rise
    "01007300020FA000": "AB48A",  # Astral Chain
    "010056B015FE8000": "A49WA",  # NieR:Automata
    "010057D006492000": "AGP8A",  # Octopath Traveler
    "0100A3501946E000": "A7W8A",  # Octopath Traveler II
    "0100C1F0051B6000": "AFWTA",  # Donkey Kong Country: Tropical Freeze
    "0100DCA0064A6000": "AG3JA",  # Luigi's Mansion 3
    "01009BF0072D4000": "AJVCA",  # Captain Toad: Treasure Tracker
    "01006000040C2000": "AEA2A",  # Yoshi's Crafted World
    "0100E95004038000": "ADENA",  # Xenoblade Chronicles 2
    "010074F013262000": "AZ3HA",  # Xenoblade Chronicles 3
    "0100FF500E34A000": "AUBQA",  # Xenoblade Chronicles: Definitive Edition
    "0100000011D90000": "AX7QA",  # Pokémon Brilliant Diamond
    "0100ABF008968000": "ALZAA",  # Pokémon Sword
    "01008DB008C2C000": "ALZBA",  # Pokémon Shield
    "0100A3D008C5C000": "ALZPA",  # Pokémon Scarlet
    "0100BF00112C0000": "ATQTA",  # Catherine: Full Body
    "01004AB00A260000": "AKFNA",  # Dark Souls Remastered
    "0100300012F2A000": "A27CA",  # Advance Wars 1+2: Re-Boot Camp
    "0100B99019412000": "A9SPA",  # Mario vs. Donkey Kong
    "01009D901BC56000": "BE4LA",  # Donkey Kong Country Returns HD
    "0100620012D6E000": "AXVPA",  # Big Brain Academy
    "0100317013770000": "APX3A",  # Mario + Rabbids Sparks of Hope
    "010069C01AB82000": "BFR3A",  # Shin Megami Tensei V Vengeance
    "01008C0016544000": "BCMHA",  # Sea of Stars
    "010029B018432000": "A93PA",  # Ghost Trick
    "01007BB017812000": "BDNFA",  # Portal Companion Collection
    "010014E00DB56000": "AY6HA",  # Digimon Story Cyber Sleuth
    "010005501E68C000": "BDZLA",  # Ace Attorney Investigations Collection
    "010036E00FB20000": "AWYLA",  # The Great Ace Attorney Chronicles
}

def clean_game_name(name):
    clean = re.sub(r'\(.*?\)', '', name)
    clean = re.sub(r'[:–—,\'’+&]', ' ', clean)
    clean = re.sub(r'\s+', ' ', clean).strip()
    return clean

def search_gametdb_id(title):
    q = clean_game_name(title)
    url = f"https://www.gametdb.com/Switch/Search?action=search&q=group=Switch&title_EN={urllib.parse.quote(q)}"
    req = urllib.request.Request(url, headers=HEADERS)
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=7) as res:
            html = res.read().decode('utf-8', errors='ignore')
            # Only match real search result table rows!
            m = re.findall(r"class='wikilink' href='https://www.gametdb.com/Switch/([A-Z0-9]{5})'", html)
            if m:
                return m[0]
    except Exception:
        pass
    return None

def download_cover(game_id, dest_path):
    subdirs = ["coverHQ", "coverfullHQ", "coverM", "cover"]
    regions = ["US", "EN", "JA", "CA", "DE", "FR", "ES", "AU"]
    
    for sub in subdirs:
        for reg in regions:
            url = f"https://art.gametdb.com/switch/{sub}/{reg}/{game_id}.jpg"
            req = urllib.request.Request(url, headers=HEADERS)
            try:
                with urllib.request.urlopen(req, context=ctx, timeout=6) as res:
                    if res.status == 200:
                        data = res.read()
                        if len(data) > 10000:
                            with open(dest_path, "wb") as f:
                                f.write(data)
                            return True, url, len(data)
            except Exception:
                continue
    return False, None, 0

def main():
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
                profiles.append((item, title))
                
    print(f"Loaded {len(profiles)} game profiles.")
    
    success_count = 0
    for tid, title in profiles:
        dest_cache = os.path.join(cache_dir, f"{tid.upper()}.jpg")
        dest_repo = os.path.join(repo_covers_dir, f"{tid.upper()}.jpg")
        
        # Check if high-res cover already exists (> 150KB)
        if os.path.isfile(dest_cache) and os.path.getsize(dest_cache) > 150000:
            if not os.path.isfile(dest_repo):
                shutil.copyfile(dest_cache, dest_repo)
            success_count += 1
            print(f"[OK] {title} ({tid})")
            continue
            
        gid = KNOWN_MAP.get(tid)
        if not gid:
            gid = search_gametdb_id(title)
            
        if gid:
            ok, url, sz = download_cover(gid, dest_cache)
            if ok:
                shutil.copyfile(dest_cache, dest_repo)
                success_count += 1
                print(f"[DOWNLOADED] {title} ({tid}) -> {gid} ({sz} bytes)")
                continue
                
        # Fallback to local docs/images if available
        slug = title.lower().replace("the legend of zelda", "zelda").replace("’", "").replace("'", "").replace(":", "")
        slug = re.sub(r'[^a-z0-9]+', '-', slug).strip('-')
        local_candidate = f"/Volumes/SSD FN501 PRO/Emulations/NativePort/nxnative/docs/images/{slug}.jpg"
        if os.path.isfile(local_candidate):
            shutil.copyfile(local_candidate, dest_cache)
            shutil.copyfile(local_candidate, dest_repo)
            success_count += 1
            print(f"[FALLBACK SLUG] {title} ({tid}) -> {slug}.jpg")
        else:
            print(f"[UNRESOLVED] {title} ({tid})")

    # In addition, ensure all installed apps have high-res covers
    installed = {
        "The Legend of Zelda Link’s Awakening.app": ["01006BB00C6F0000", "01002B00111A2000"],
        "DRAGON QUEST VII Reimagined.app": ["0100A9D01C446000"],
        "Legend of Zelda The Tears of the Kingdom.app": ["0100F2C0115B6000"]
    }
    ports_dir = "/Volumes/SSD FN501 PRO/Emulations/Nx Ports"
    for app_name, tids in installed.items():
        app_res = os.path.join(ports_dir, app_name, "Contents/Resources")
        if os.path.isdir(app_res):
            for tid in tids:
                cover_src = os.path.join(cache_dir, f"{tid}.jpg")
                if os.path.isfile(cover_src):
                    dest = os.path.join(app_res, "cover.jpg")
                    shutil.copyfile(cover_src, dest)
                    print(f"[INSTALLED UPDATED] {app_name} -> {cover_src} ({os.path.getsize(cover_src)} bytes)")
                    break

    print(f"\nDone! Successfully covered {success_count}/{len(profiles)} games.")

if __name__ == "__main__":
    main()
