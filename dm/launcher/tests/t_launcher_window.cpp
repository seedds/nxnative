// The launcher program itself (nxlauncher, found beside this test) over the games folder fake_games.h writes, with SDL's dummy video driver: the window draws one frame and exits
// (--frames 1); "Show this window at launch" off and DM_LAUNCHER=0 start the last game at once (nxhost-run's stand-in is this test, NXL_FAKE_HOST=1), --window shows the window anyway;
// --command prints what the model composes (launcher_model.cpp compose, in this process, recording the profile as Play does); --list prints the games as TSV; with Alpha's profile
// holding a sampled offset its packs were made without (launcher_model.h, the second pack) the frame says a better pack is possible, and --repack runs the packer (this test again) and waits. The launcher's own settings go to a folder of the test's
// (XDG_CONFIG_HOME, APPDATA). No screen, GPU or game needed: the dummy driver's window has a software framebuffer, which SDL's 2D renderer draws into.
#include "launcher_model.h"
#include "fake_games.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace nxl;
namespace fs = std::filesystem;

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); } } while (0)

static int fake_host(int argc, char **argv) {   // nxhost-run's stand-in: its arguments and two of its variables, on its standard output (the game's log.txt)
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

int main(int argc, char **argv) {
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
    CHECK(has(game_log, "fake host: argv " + a_aot + "\n") && has(game_log, "fake host: DM_OPMODE=0\n"), "nxhost-run with the pack folder and the saved choice: " + game_log);

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
    std::printf("t_launcher_window: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
