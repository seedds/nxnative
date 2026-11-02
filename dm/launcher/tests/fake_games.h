// The games folder the launcher's tests write (t_launcher_model.cpp, t_launcher_window.cpp): three packs as scripts/build_ipa_from_nsp.py --platform linux / windows writes them
// (port_launch_file, port_settings, aot_packs), with fake packs and no game files.
//   Alpha Game (0100AAAA00000000): both pack modes (max in aot_v131072/max/), an NRO with its pack, a refused one, the settings list, a pack environment file's switch (DM_MOTION=0),
//     and the player's env.txt: a saved choice (Handheld), a value the pack does not offer (metalfx), a render scale, a diagnostic switch and a comment; the rows of a repack
//     (package: the games folder, its NSP folder; packer, keys, nxaot: names only; python: host, so that the test acts as the packer too) and no profile row (the folder beside run/)
//   Beta Game (0100BBBB00000000), one level down in its NSP folder: the balanced pack only, no settings list, a profile row of its own and no repack rows
//   Gamma Game (0100CCCC00000000): a max pack row whose folder lacks main's pack: no complete pack
// host: the program the launch files name as nxhost-run (the test itself, which acts as one with NXL_FAKE_HOST=1 in its environment)
#pragma once
#include "launcher_model.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fake {
namespace fs = std::filesystem;

inline std::string str(const fs::path &p) { return p.u8string(); }

inline void write(const fs::path &p, const std::string &text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

inline std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline fs::path tmp_root(const char *name) {   // under TMPDIR (CTest and the runners give the build folder's tmp/), else the system's
    const char *t = std::getenv("TMPDIR");
    if (!t || !*t) t = std::getenv("TEMP");
    fs::path root = (t && *t ? fs::u8path(t) : fs::temp_directory_path()) / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

struct Folder { fs::path games, a, b, c, a_run, b_run; };

inline void packs(const fs::path &dir, bool nro, bool main_pack = true) {
    write(dir / "main.aot.tsv", "# nxaot-manifest 1\tmain\tabababababababababababababababab\t" + std::string(64, '1') + "\n");
    write(dir / "main.image.bin", "image");
    if (main_pack) write(dir / (std::string("main") + nxl::kPackSuffix), "pack");
    if (nro) {
        write(dir / "nro.tsv", "# name\tsha256\tsize\tnrr\twhere\nnro0\t" + std::string(64, '2') + "\t128\tgame.nrr\tloose a.nro\nrefused\t" + std::string(64, '3') + "\t256\tgame.nrr\teos\n");
        write(dir / (std::string("nro0") + nxl::kPackSuffix), "pack");
    }
}

inline std::string launch(const std::string &title, const std::string &id, const fs::path &build, const fs::path &run, const std::string &host, bool settings,
                          const std::vector<std::pair<std::string, fs::path>> &modes, const std::string &extra_env = "") {
    std::string p = nxl::kPlatform;
    std::string t = "# nxnative launch 1\n# " + title + " (" + id + "), written by the launcher tests\n";
    t += "game\ttitle\t" + title + "\ngame\ttitle_id\t" + id + "\ngame\tversion\t1.2.0\ngame\tpackage\tv131072\ngame\tplatform\t" + p + "\ngame\tgraphics\tnvn\n";
    t += "path\thost\t" + host + "\npath\tcwd\t" + str(run) + "\npath\tchoices\t" + str(run / "env.txt") + "\npath\tlog\t" + str(run / "log.txt") + "\n";
    if (settings) t += "path\tsettings\t" + str(build / ("launcher-" + p + ".json")) + "\n";
    for (const auto &m : modes) t += "pack\t" + m.first + "\t" + str(m.second) + "\n";
    t += extra_env;
    t += "env\tDM_ROOT\t" + str(build) + "\nenv\tDM_ROMFS\t" + str(build / "extract/v131072/RomFS.img") + "\nenv\tDM_SAVE\t" + str(run / "save") + "\nenv\tDM_TEMP\t" + str(run / "temp") + "\n";
    t += "env\tDM_MSL_DIR\t" + str(build / "shader/cache") + "\nenv\tDM_LANG\ten-US\nenv\tDM_WINDOW\t1\nenv\tDM_AUDIO\t1\n";
    return t;
}

// settings.py's list for the port (launcher-<platform>.json), schema 1
inline const char *kSettings = R"({
 "schema": 1,
 "game": "0100AAAA00000000",
 "settings": [
  {"env": "DM_FPS_TARGET", "kind": "choice", "section": "Game", "label": "Frame rate", "note": "", "default": "60", "level": "basic", "options": [["30", "30 fps"], ["60", "60 fps"]]},
  {"env": "DM_RENDER_SCALE", "kind": "range", "section": "Game", "label": "Render scale", "note": "", "default": "1.0", "level": "basic", "min": 0.5, "max": 1.0, "step": 0.05,
   "base": {"docked": [1600, 900], "handheld": [1280, 720]}},
  {"env": "DM_OPMODE", "kind": "choice", "section": "Game", "label": "Console mode", "note": "", "default": "1", "level": "basic", "options": [["1", "Docked"], ["0", "Handheld"]]},
  {"env": "DM_PRESENT_UPSCALE", "kind": "choice", "section": "Game", "label": "Upscaling", "note": "FSR 1 uses more GPU time.", "default": "off",
   "options": [["off", "Standard"], ["fsr", "FSR 1"], ["integer", "Integer"]]},
  {"env": "DM_LANG", "kind": "choice", "section": "Game", "label": "Game language", "note": "", "default": "en-US", "level": "basic", "options": [["en-US", "American English"], ["ja", "Japanese"]]},
  {"env": "DM_RUMBLE", "kind": "toggle", "section": "Controls", "label": "Game rumble", "note": "", "default": "1", "level": "basic", "on": "1", "off": "0"},
  {"env": "DM_RUMBLE_GAIN", "kind": "range", "section": "Controls", "label": "Rumble strength", "note": "", "default": "1.0", "needs": "DM_RUMBLE=1", "min": 0, "max": 3, "step": 0.1},
  {"env": "DM_FPS_HOLD", "kind": "toggle", "section": "Performance", "label": "Hold frames when late", "note": "", "default": "on", "on": "on", "off": "off"},
  {"env": "DM_FUTURE", "kind": "slider", "section": "Game", "label": "A kind this reader does not know", "note": "", "default": "1"}
 ]
}
)";

inline const char *kChoices = "# the player's choices\nDM_OPMODE=0\nDM_PRESENT_UPSCALE=metalfx\nDM_DEBUG_LOG=1\nDM_RENDER_SCALE=0.75\n";

inline Folder make(const fs::path &root, const std::string &host) {
    Folder f;
    f.games = root / "games";
    f.a = f.games / "0100AAAA00000000_build"; f.a_run = f.games / "0100AAAA00000000-work/run";
    f.b = f.games / "Beta NSPs/0100BBBB00000000_build"; f.b_run = f.games / "Beta NSPs/0100BBBB00000000-work/run";
    f.c = f.games / "0100CCCC00000000_build";
    std::string p = nxl::kPlatform;
    fs::path a_aot = f.a / "aot_v131072";
    packs(a_aot, true); packs(a_aot / "max", true);
    const std::string repack = "path\tpackage\t" + str(f.games) + "\npath\tpacker\t" + str(root / "scripts" / "build_ipa_from_nsp.py") + "\npath\tpython\t" + host +
                               "\npath\tkeys\t" + str(root / "keys" / "prod.keys") + "\npath\tnxaot\t" + str(root / "nxaot") + "\n";
    write(f.a / ("launch-" + p + ".tsv"), launch("Alpha Game", "0100AAAA00000000", f.a, f.a_run, host, true, {{"balanced", a_aot}, {"max", a_aot / "max"}}, "env\tDM_MOTION\t0\n" + repack));
    write(f.a / ("launcher-" + p + ".json"), kSettings);
    write(f.a_run / "env.txt", kChoices);
    packs(f.b / "aot_v0", false);
    write(f.b / ("launch-" + p + ".tsv"), launch("Beta Game", "0100BBBB00000000", f.b, f.b_run, host, false, {{"balanced", f.b / "aot_v0"}},
                                                 "path\tprofile\t" + str(f.games / "Beta NSPs" / "recorded") + "\n"));
    packs(f.c / "aot_v0/max", false, false);
    write(f.c / ("launch-" + p + ".tsv"), launch("Gamma Game", "0100CCCC00000000", f.c, f.games / "0100CCCC00000000-work/run", host, false, {{"max", f.c / "aot_v0/max"}}));
    write(f.games / "notes/readme.txt", "not a pack\n");   // a folder without packs is skipped
    write(f.games / ("0100DDDD00000000_build/launch-" + std::string(p == "linux" ? "windows" : "linux") + ".tsv"), "# nxnative launch 1\n");   // another system's pack only
    return f;
}

}   // namespace fake
