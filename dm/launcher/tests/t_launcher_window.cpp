// The launcher program itself (nxlauncher, found beside this test) over the games folder fake_games.h writes, with SDL's dummy video driver: the window draws one frame and exits
// (--frames 1); "Show this window at launch" off and DM_LAUNCHER=0 start the last game at once (nxhost-run's stand-in is this test, NXL_FAKE_HOST=1), --window shows the window anyway;
// --command prints what the model composes (launcher_model.cpp compose, in this process, recording the profile as Play does); --list prints the games as TSV; with Alpha's profile
// holding a sampled function its packs were made without (launcher_model.h, the second pack) the frame says a better pack is possible, and --repack runs the packer (this test again) and waits. The launcher's own settings go to a folder of the test's
// (XDG_CONFIG_HOME, APPDATA). No screen, GPU or game needed: the dummy driver's window has a software framebuffer, which SDL's 2D renderer draws into.
// The window driven by --answer and --press (main.cpp), as a player would use it: Add game... with the packer's stand-in (fake_games.h packer, through NXPACKER and PYTHON) to
// its result (the game listed, its NSP folder remembered), a pack cancelled, the keys missing, Stop of a running game whose stand-in installs the host's own plat_on_terminate
// (it ends gracefully), the Saves tab (Back up saves, Restore... and its confirmation, Open saves folder), Browse...; each run keeps pictures of its frames in the test's folder.
#include "launcher_model.h"
#include "fake_games.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

extern "C" void plat_on_terminate(void (*fn)(const char *why));   // dm/host/plat/plat.h (C): the host's way in for Stop

using namespace nxl;
namespace fs = std::filesystem;

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); } } while (0)

static void graceful(const char *why) {   // the host's graceful exit, as hle_lifecycle.c logs it
    std::printf("[dm] received %s: graceful exit\n[dm] graceful exit done (0.001 s)\n", why);
    std::fflush(stdout);
    std::_Exit(0);
}

static int fake_host(int argc, char **argv) {   // nxhost-run's stand-in: its arguments and two of its variables, on its standard output (the game's log.txt)
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);   // "\n" as written, as on Linux: the launcher's log is read back byte for byte
#endif
    if (std::getenv("NXL_FAKE_WAIT")) {   // a game that runs until Stop: the host's own way in (SIGTERM; the named event on Windows)
        plat_on_terminate(graceful);
        std::printf("fake host: ready\n"); std::fflush(stdout);
        for (int i = 0; i < 600; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 3;
    }
    std::string args;
    for (int i = 1; i < argc; i++) args += std::string(i > 1 ? "|" : "") + argv[i];
    const char *op = std::getenv("DM_OPMODE"), *st = std::getenv("NXL_FAKE_STATUS");
    std::printf("fake host: argv %s\nfake host: DM_OPMODE=%s\n", args.c_str(), op ? op : "<unset>");
    std::fflush(stdout);
    return st ? std::atoi(st) : 0;
}

struct Run { int status; std::string out; };

static fs::path g_root, g_cfg;
static std::string g_nx;

static Run run(const std::vector<std::string> &args, const std::vector<KV> &extra, const char *name) {
    Command c;
    c.argv.push_back(g_nx);
    c.argv.insert(c.argv.end(), args.begin(), args.end());
    c.set = {{"SDL_VIDEODRIVER", "dummy"}, {"SDL_VIDEO_DRIVER", "dummy"}, {"SDL_AUDIO_DRIVER", "dummy"}, {"XDG_CONFIG_HOME", fake::str(g_cfg)}, {"APPDATA", fake::str(g_cfg)},
             {"NXL_FAKE_HOST", "1"}, {"NXL_FAKE_STATUS", "5"}, {"DM_LAUNCHER", "1"}};
    for (const auto &e : extra) {
        bool done = false;
        for (auto &s : c.set) if (s.key == e.key) { s.value = e.value; done = true; }
        if (!done) c.set.push_back(e);
    }
    c.cwd = fake::str(g_root); c.log = fake::str(g_root / (std::string(name) + ".log"));
    Child ch; std::string err;
    if (!spawn(c, &ch, &err)) { CHECK(false, "nxlauncher started: " + err); return {-1, ""}; }
    int st = wait(&ch);
    Run r{st, fake::read(c.log)};
    std::fprintf(stderr, "---- nxlauncher %s: status %d\n%s", name, st, r.out.c_str());
    return r;
}

static void config(const std::string &text) { fake::write(g_cfg / "nxnative" / "launcher.txt", text); }
static bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

// ---- The window driven by --answer and --press ----
static bool shot_ok(const fs::path &p) { std::error_code ec; return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 100000; }   // a BMP of the 1100 x 720 window

static void driven(const fake::Folder &f, const std::string &games) {
    const fs::path shots = g_root / "shots";
    fs::create_directories(shots);
    const std::string shot = fake::str(shots);
    // Add game: the file chosen in the dialog, its NSP folder outside the games folder, the keys chosen before (a stand-in file), the packer's stand-in to its end
    const fs::path nsp = g_root / "elsewhere" / "Phi NSPs", file = nsp / "Phi Game [0100FFFF00000000][v0].nsp";
    fake::write(file, "not a game\n");
    fake::write(g_root / "stand-in.keys", "not keys\n");
    config("show_window=1\nlast=0100AAAA00000000\nkeys=" + fake::str(g_root / "stand-in.keys") + "\n");
    const std::vector<KV> packer = {{"NXPACKER", fake::str(g_root / "scripts" / "build_ipa_from_nsp.py")}, {"PYTHON", self_path()}, {"NXL_FAKE_PACKER", "1"}};
    Run r = run({"--frames", "1500", "--games", games, "--answer", fake::str(file), "--press", "Add game...", "--press", "shot:" + shot + "/add.bmp", "--press", "Pack",
                 "--press", "until:Added Phi Game", "--press", "shot:" + shot + "/added.bmp", "--exit-idle"}, packer, "add");
    CHECK(r.status == 0 && has(r.out, "nxlauncher: --answer: asked file Add a game") && has(r.out, "Add game: the packer exited with status 0: Added Phi Game."),
          "Add game: the dialog's file packed by the packer's stand-in, and the game added");
    CHECK(has(r.out, "is outside the games folder: remembered in") && has(fake::read(g_cfg / "nxnative" / "launcher.txt"), "nsp_folder=" + fake::str(nsp) + "\n"),
          "its NSP folder, outside the games folder, remembered in launcher.txt");
    CHECK(has(fake::read(nsp / "nxlauncher-pack.txt"), "fake packer: argv " + fake::str(g_root / "scripts" / "build_ipa_from_nsp.py") + "|" + fake::str(nsp) + "|--platform|" + kPlatform +
                                                        "|--progress|json|-k|" + fake::str(g_root / "stand-in.keys")), "the packer got the NSP folder, the platform, JSON progress and the keys");
    CHECK(shot_ok(shots / "add.bmp") && shot_ok(shots / "added.bmp"), "the pictures of Add game before and after the pack");
    r = run({"--list", games}, {}, "list-after-add");
    CHECK(!has(r.out, "Phi Game"), "--list <folder> lists that folder only");
    r = run({"--command", "0100FFFF00000000", "--games", games}, {}, "command-added");
    CHECK(r.status == 0 && has(r.out, "# Phi Game (0100FFFF00000000)"), "--command finds the added game through launcher.txt");
    // A pack cancelled: the stand-in packer runs until Cancel, which ends it as Ctrl+C would
    std::vector<KV> slow = packer; slow.push_back({"NXL_FAKE_PACK", "slow"});
    r = run({"--frames", "1500", "--games", games, "--answer", fake::str(file), "--press", "Add game...", "--press", "Pack", "--press", "until:still extracting 5",
             "--press", "shot:" + shot + "/packing.bmp", "--press", "Cancel", "--press", "until:Packing was cancelled", "--press", "shot:" + shot + "/cancelled.bmp", "--exit-idle"},
            slow, "cancel");
    CHECK(r.status == 0 && has(r.out, "Add game: the packer exited with status 130: Packing was cancelled."), "Cancel: the packer ended as Ctrl+C ends it, said in plain words");
    CHECK(shot_ok(shots / "packing.bmp"), "the picture of the pack in progress");
    // The keys missing: the packer's own message, in plain words
    std::vector<KV> nokeys = packer; nokeys.push_back({"NXL_FAKE_PACK", "keys"});
    r = run({"--frames", "1500", "--games", games, "--answer", fake::str(file), "--press", "Add game...", "--press", "Pack", "--press", "until:found no prod.keys",
             "--press", "shot:" + shot + "/nokeys.bmp", "--exit-idle"}, nokeys, "nokeys");
    CHECK(r.status == 0 && has(r.out, "The packer found no prod.keys at " + fake::str(g_root / "stand-in.keys")), "the packer's missing keys, in plain words");
    // Stop: a game that runs until it is asked to end
    r = run({"--frames", "1500", "--games", games, "--press", "Play", "--press", "until:fake host: ready", "--press", "shot:" + shot + "/running.bmp", "--press", "Stop",
             "--press", "until:It stopped gracefully", "--press", "shot:" + shot + "/stopped.bmp", "--exit-idle"}, {{"NXL_FAKE_WAIT", "1"}}, "stop");
    CHECK(r.status == 0 && has(r.out, "nxlauncher: Stop: asking Alpha Game") && has(r.out, "Alpha Game: It stopped gracefully (status 0"), "Stop: the game ended by the host's graceful exit");
#if defined(_WIN32)
    CHECK(has(fake::read(f.a_run / "log.txt"), "received stop request: graceful exit"), "through the named event");
#else
    CHECK(has(fake::read(f.a_run / "log.txt"), "received SIGTERM: graceful exit"), "through SIGTERM");
#endif
    CHECK(shot_ok(shots / "running.bmp") && shot_ok(shots / "stopped.bmp"), "the pictures of the running game with Stop and after it");
    // The saves: a backup made before (its name known), Back up saves, Restore... with its confirmation, Open saves folder
    fake::write(f.a_run / "save" / "game_data.sav", "first");
    Saves s = save_place(*find_game(find_games(games), "0100AAAA00000000", nullptr), ProcessEnv());
    Backup before; std::string err;
    CHECK(backup_saves(s, 1800000000, &before, &err), "a backup made beforehand: " + err);
    fake::write(f.a_run / "save" / "game_data.sav", "second");
    r = run({"--frames", "1500", "--games", games, "--press", "Saves", "--press", "Back up saves", "--press", "until:Backed up as", "--press", "Open saves folder",
             "--press", "Restore " + before.name, "--press", "shot:" + shot + "/restore.bmp", "--press", "Replace the saves", "--press", "until:Put back",
             "--press", "shot:" + shot + "/saves.bmp", "--exit-idle"}, {}, "saves");
    CHECK(r.status == 0 && has(r.out, "Alpha Game: Backed up as ") && has(r.out, "Alpha Game: Put back " + before.name + ". The saves it replaced are in the backup "),
          "Back up saves, then Restore... confirmed");
    CHECK(fake::read(f.a_run / "save" / "game_data.sav") == "first", "the save as it was in that backup");
    CHECK(list_backups(s).size() == 3, "three backups: the one before, Back up saves', and the saves Restore replaced");
    CHECK(has(r.out, "nxlauncher: --answer: shown in the file manager: " + s.folder), "Open saves folder: the save folder in the file manager");
    CHECK(shot_ok(shots / "restore.bmp") && shot_ok(shots / "saves.bmp"), "the pictures of the confirmation and of the Saves tab");
    fs::remove_all(f.a_run / "save"); fs::remove_all(fs::u8path(s.backups));
    // Browse...: the folder dialog's answer becomes the games folder
    fs::create_directories(g_root / "empty games");
    r = run({"--frames", "1500", "--games", fake::str(g_root / "empty games"), "--answer", games, "--press", "Browse...", "--press", "until:Games folder: " + games, "--press", "shot:" + shot + "/browse.bmp",
             "--exit-idle"}, {}, "browse");
    CHECK(r.status == 0 && has(r.out, "nxlauncher: --answer: asked folder Choose the games folder") && has(fake::read(g_cfg / "nxnative" / "launcher.txt"), "games=" + games + "\n"),
          "Browse...: the folder chosen is the games folder, remembered");
    // A step that never comes: status 3 and the step named
    r = run({"--frames", "5", "--games", games, "--press", "No such button"}, {}, "missing-step");
    CHECK(r.status == 3 && has(r.out, "[RED] nxlauncher: --press: 1 step not done, the first press:No such button"), "a step not done: status 3");
}


int main(int argc, char **argv) {
    if (std::getenv("NXL_FAKE_PACKER")) return fake::packer(argc, argv);
    if (std::getenv("NXL_FAKE_HOST")) return fake_host(argc, argv);
    g_root = fake::tmp_root("t_launcher_window"); g_cfg = g_root / "config";
    fake::Folder f = fake::make(g_root, self_path());
    fake::write(g_root / "scripts" / "build_ipa_from_nsp.py", "# the packer Alpha's launch file names\n");   // --repack runs the packer and nxaot rows' files when they are there
    fake::write(g_root / "nxaot", "nxaot");
    g_nx =fake::str(fs::u8path(self_path()).parent_path() / (std::string("nxlauncher") + kExeSuffix));
    if (!fs::is_regular_file(fs::u8path(g_nx))) { std::fprintf(stderr, "FAIL: no %s beside this test (it is built with it)\n", g_nx.c_str()); return 1; }
    std::string games = fake::str(f.games), a_aot = fake::str(f.a / "aot_v131072"), a_log = fake::str(f.a_run / "log.txt");
    // Alpha has a profile its balanced packs were made without (the second pack: launcher_model.h): the window says "a better pack is possible" and offers the repack
    const fs::path prof = f.a_run.parent_path() / "profile";
    fake::write(prof / "main.tsv", "# nxaot profile\n0x100 40\n");
    fake::write(prof / "main.misses.tsv", "# nxaot misses\n0x208 indirect 1\n");
    const auto t0 = fs::file_time_type::clock::now();
    for (const char *n : {"main", "nro0"}) fs::last_write_time(f.a / "aot_v131072" / (std::string(n) + kPackSuffix), t0 - std::chrono::hours(1));

    Run r = run({"--frames", "1", "--games", games}, {}, "window");
    CHECK(r.status == 0, "the window drew its frame and closed");
    CHECK(has(r.out, "nxlauncher: 1 frame drawn, 3 games listed"), "one frame, the three games");
    CHECK(!fs::exists(g_root / "imgui.ini"), "no imgui.ini written");
    CHECK(has(r.out, "nxlauncher: Alpha Game: Profile recorded: a better pack is possible (2 files in " + fake::str(prof) + "; Repack with profile)") &&
              !has(r.out, "Beta Game: Profile recorded"), "the frame with Alpha's profile its packs were made without: a better pack is possible, for Alpha only");

    // --repack: the packer without the window (the launch file's python row is this test, which acts as the packer), waited for, its status returned
    r = run({"--games", games, "--pack", "max", "--repack", "0100AAAA00000000"}, {}, "repack");
    std::string repack_log = fake::read(f.a_run / "repack.txt");
    CHECK(r.status == 5 && has(r.out, "the packer started for the max pack") && has(r.out, "nxlauncher: the packer exited with status 5") &&
              has(repack_log, "fake host: argv " + fake::str(g_root / "scripts" / "build_ipa_from_nsp.py") + "|" + games + "|--platform|" + kPlatform + "|--pack-mode|max|-k|"),
          "--repack runs the packer on the NSP folder with the mode and waits: " + repack_log);
    r = run({"--games", games, "--repack", "0100BBBB00000000"}, {}, "repack-none");
    CHECK(r.status == 1 && has(r.out, "[RED] nxlauncher: ") && has(r.out, "pack it once more"), "--repack of a pack without the repack's rows: a red line, status 1");

    config("show_window=0\nlast=0100AAAA00000000\n");
    r = run({"--frames", "2", "--games", games}, {}, "show-off");
    CHECK(r.status == 5, "\"Show this window at launch\" off: the last game started at once, and its exit status returned");
    CHECK(has(r.out, "no window (\"Show this window at launch\" is off): Alpha Game starts at once"), "it says why");
    std::string game_log = fake::read(f.a_run / "log.txt");
    CHECK(has(game_log, "fake host: argv --profile|" + fake::str(fake::profile_file(f.a)) + "|" + a_aot + "\n") && has(game_log, "fake host: DM_OPMODE=0\n"),
          "nxhost-run with the game profile's file, the pack folder and the saved choice: " + game_log);

    r = run({"--frames", "2", "--games", games, "--window"}, {}, "show-off-window");
    CHECK(r.status == 0 && has(r.out, "nxlauncher: 2 frames drawn"), "--window shows it anyway");

    config("show_window=1\nlast=0100AAAA00000000\n");
    r = run({"--frames", "1", "--games", games}, {{"DM_LAUNCHER", "0"}, {"DM_OPMODE", "1"}}, "dm-launcher");
    CHECK(r.status == 5 && has(r.out, "no window (DM_LAUNCHER=0)"), "DM_LAUNCHER=0: the last game at once");
    CHECK(has(fake::read(f.a_run / "log.txt"), "fake host: DM_OPMODE=1\n"), "what the environment sets wins over the saved choice");
    CHECK(has(fake::read(f.a_run / "log.prev.txt"), "fake host: DM_OPMODE=0\n"), "the previous run's log kept");

    r = run({"--games", games, "--pack", "max", "--command", "0100aaaa00000000"}, {}, "command");
    Command want = compose(*find_game(find_games(games), "0100AAAA00000000", nullptr), "max", ProcessEnv(), true);   // Play records by default
    std::string expect = "# Alpha Game (0100AAAA00000000), the max pack\n# cwd " + want.cwd + "\n# log " + want.log + "\n";
    for (const auto &e : want.set) expect += e.key + "=" + e.value + "\n";
    expect += "\n";
    for (const auto &a : want.argv) expect += a + "\n";
    CHECK(r.status == 0 && r.out.rfind(expect, 0) == 0, "--command prints what the model composes: the variables, a blank line, the argv\n" + expect);
    CHECK(has(r.out, "DM_PRESENT_UPSCALE=metalfx\n") && has(r.out, "\n" + fake::str(f.a / "aot_v131072" / "max") + "\n"), "env.txt's lines and the max pack");

    r = run({"--list", games}, {}, "list");
    std::istringstream lines(r.out); std::string line; std::vector<std::string> rows;
    while (std::getline(lines, line)) if (!line.empty() && line[0] != '#' && line.rfind("nxlauncher:", 0) != 0) rows.push_back(line);
    CHECK(r.status == 0 && rows.size() == 3, "--list: three games");
    if (rows.size() == 3) {
        CHECK(rows[0].rfind("0100AAAA00000000\tAlpha Game\t1.2.0\tbalanced,max\t", 0) == 0 && !has(rows[0], "\tnever\t"), "Alpha: both modes, played");
        CHECK(rows[1].rfind("0100BBBB00000000\tBeta Game\t1.2.0\tbalanced\tnever\t", 0) == 0, "Beta: balanced, never played");
        CHECK(rows[2].rfind("0100CCCC00000000\tGamma Game\t1.2.0\tnone\tnever\t", 0) == 0 && has(rows[2], "incomplete"), "Gamma: no complete pack, and why");
    }
    r = run({"--command", "0100EEEE00000000", "--games", games}, {}, "unknown");
    CHECK(r.status == 1 && has(r.out, "[RED] nxlauncher: no game 0100EEEE00000000"), "an unknown game: a red line, status 1");
    r = run({"--bogus"}, {}, "usage");
    CHECK(r.status == 2 && has(r.out, "usage: nxlauncher"), "an unknown option: the usage, status 2");
    driven(f, games);
    std::printf("t_launcher_window: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
