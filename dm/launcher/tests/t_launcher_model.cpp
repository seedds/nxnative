// nxlauncher's model (dm/launcher/launcher_model.cpp) over a games folder this test writes (fake_games.h): the games found and their pack modes, the settings rows and their effective
// values (saved choices, a value the pack does not offer, the pack's own defaults, what the environment fixes), what starts a game (the pack, the host, the environment in the order
// run.sh gives it, DM_SAVED_CHOICES=0, NXHOST), the saved-choice round trip through the player's env.txt, the second pack (the profile folder, "Record a profile while playing",
// whether the recorded profile, JIT misses and known targets make a better pack possible against what the launch file says the packs took, the repack's command), the game profile's
// file (--profile with the game_profile row's file; the row missing or its file gone: the problem), a moved bundle (the host bin/nxhost-run, packer
// and nxaot taken from the launcher's folder when the launch file's are gone; NXHOST over both; the problem when none is there), the launcher's own settings,
// DM_LAUNCHER and "Show this window at launch", a child process with its environment, working folder, log and exit status (this program, which acts as nxhost-run and as
// the packer with NXL_FAKE_HOST=1), and a pack the host refuses: nxhost-run's status 8 explained, and a pack whose PLT bindings name a symbol the host takes over
// (its --takeovers list; the stand-in prints NXL_FAKE_TAKEOVERS) named before the start, in the Problem column and Play's error, with the repack.
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
#include <string>
#include <vector>

#if !defined(_WIN32)
extern char **environ;
#endif

using namespace nxl;
namespace fs = std::filesystem;

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); } } while (0)
#define CHECK_EQ(a, b, msg) do { checks++; auto _a = (a); auto _b = (b); if (!(_a == _b)) { fails++; std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); } } while (0)

static int fake_host(int argc, char **argv) {   // nxhost-run's stand-in: what it was started with, on its standard output (the launcher's log)
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);   // "\n" as written, as on Linux: the launcher's log is read back byte for byte
#endif
    if (argc == 2 && !std::strcmp(argv[1], "--takeovers")) {   // nxhost-run --takeovers: its '#' line, then the names (NXL_FAKE_TAKEOVERS, separated by commas)
        std::printf("# nxhost-run take-overs: the launcher tests' stand-in\n");
        const char *t = std::getenv("NXL_FAKE_TAKEOVERS");
        std::string s = t ? t : "";
        for (size_t at = 0; at < s.size();) { size_t c = s.find(',', at); if (c == std::string::npos) c = s.size(); if (c > at) std::printf("%s\n", s.substr(at, c - at).c_str()); at = c + 1; }
        return 0;
    }
    std::error_code ec;
    std::printf("fake host: cwd %s\n", fs::current_path(ec).u8string().c_str());
    std::string args;
    for (int i = 1; i < argc; i++) args += std::string(i > 1 ? "|" : "") + argv[i];
    std::printf("fake host: argv %s\n", args.c_str());
#if defined(_WIN32)
    char **env = _environ;
#else
    char **env = environ;
#endif
    std::vector<std::string> vars;
    for (char **e = env; e && *e; e++) if (!std::strncmp(*e, "DM_", 3)) vars.push_back(*e);
    std::sort(vars.begin(), vars.end());
    for (const auto &v : vars) std::printf("fake host: env %s\n", v.c_str());
    std::fflush(stdout);
    const char *st = std::getenv("NXL_FAKE_STATUS");
    return st ? std::atoi(st) : 0;
}

static std::vector<std::string> keys(const std::vector<KV> &v) { std::vector<std::string> out; for (const auto &e : v) out.push_back(e.key); return out; }
static std::string value(const std::vector<KV> &v, const std::string &k) { for (const auto &e : v) if (e.key == k) return e.value; return "<unset>"; }
static const Setting *setting(const SettingsState &st, const std::string &env) { for (const auto &s : st.settings) if (s.env == env) return &s; return nullptr; }

static void env_file_rules(const fs::path &root) {
    fake::write(root / "env.txt", "# a comment\n  DM_A=1  \r\nDM_B = two words \n\tDM_C=\n=x\nbad name=1\nDM_D==x\n#DM_E=1\nDM_A=again\nno equals\n");
    auto kv = read_env_file(fake::str(root / "env.txt"));
    CHECK_EQ(keys(kv), (std::vector<std::string>{"DM_A", "DM_B", "DM_C", "DM_D", "DM_A"}), "env.txt lines as dm/mac/launcher.c's env_file reads them, names a shell can export");
    CHECK_EQ(kv[1].value, std::string(" two words"), "the value as it is after the =, the line's trailing blanks removed");
    CHECK_EQ(kv[2].value, std::string(""), "an empty value is a value");
    CHECK_EQ(kv[3].value, std::string("=x"), "the first = splits");
    CHECK(read_env_file(fake::str(root / "none.txt")).empty(), "no file: no lines");
    CHECK(valid_name("DM_X1") && valid_name("_a") && !valid_name("1A") && !valid_name("A-B") && !valid_name(""), "valid_name");
}

static void games(const fake::Folder &f) {
    auto g = find_games(fake::str(f.games));
    CHECK_EQ(g.size(), (size_t)3, "three packs of this platform (another system's pack and a folder without packs skipped)");
    if (g.size() != 3) return;
    CHECK_EQ(g[0].launch.title + "|" + g[1].launch.title + "|" + g[2].launch.title, std::string("Alpha Game|Beta Game|Gamma Game"), "sorted by title");
    CHECK_EQ(g[0].modes(), (std::vector<std::string>{"balanced", "max"}), "Alpha: both pack modes complete");
    CHECK_EQ(g[1].modes(), (std::vector<std::string>{"balanced"}), "Beta, one level down: balanced only");
    CHECK(g[2].modes().empty() && !g[2].problem.empty(), "Gamma: a pack folder without main's pack is no complete pack, and it says so");
    CHECK(g[0].problem.empty() && g[0].settings_error.empty() && g[1].settings.empty() && g[1].settings_error.empty(), "Alpha starts and shows settings; Beta has none to show");
    CHECK_EQ(g[0].settings.size(), (size_t)8, "the list's rows, without the kind this reader does not know");
    CHECK_EQ(g[0].launch.version + " " + g[0].launch.package + " " + g[0].launch.graphics, std::string("1.2.0 v131072 nvn"), "the game rows");
    std::string err;
    CHECK(find_game(g, "0100aaaa00000000", &err) == &g[0], "found by title ID in any case");
    CHECK(find_game(g, fake::str(f.b), &err) == &g[1], "found by its folder");
    CHECK(!find_game(g, "0100EEEE00000000", &err) && err.find("no game") != std::string::npos, "an unknown game is named");
    fake::write(f.a / "aot_v131072/max/extra.aot.tsv", "# a manifest without its pack\n");
    CHECK(!packs_complete(fake::str(f.a / "aot_v131072/max")), "a manifest without its pack: incomplete");
    fs::remove(f.a / "aot_v131072/max/extra.aot.tsv");
    fs::remove(f.a / (std::string("aot_v131072/max/nro0") + kPackSuffix));
    CHECK(!packs_complete(fake::str(f.a / "aot_v131072/max")), "an NRO nro.tsv lists without its pack: incomplete");
    fake::write(f.a / (std::string("aot_v131072/max/nro0") + kPackSuffix), "pack");
    CHECK(packs_complete(fake::str(f.a / "aot_v131072/max")), "complete again (the refused row needs no pack)");
}

static void settings(const fake::Folder &f) {
    auto g = find_games(fake::str(f.games));
    const Game &a = g[0];
    MapEnv none;
    SettingsState st = load_state(a, none);
    CHECK_EQ(st.values["DM_OPMODE"], std::string("0"), "a saved choice");
    CHECK_EQ(st.values["DM_RENDER_SCALE"], std::string("0.75"), "a saved range value");
    CHECK_EQ(st.values["DM_PRESENT_UPSCALE"], std::string("off"), "a saved value this pack does not offer shows the default");
    CHECK_EQ(st.refused["DM_PRESENT_UPSCALE"], std::string("metalfx"), "... and is named");
    CHECK_EQ(st.values["DM_FPS_TARGET"] + " " + st.values["DM_FPS_HOLD"] + " " + st.values["DM_LANG"], std::string("60 on en-US"), "the defaults");
    CHECK(st.fixed.empty() && st.changed(), "nothing fixed; the saved choices differ from the defaults");
    CHECK_EQ(st.sections(true), (std::vector<std::string>{"Game", "Controls"}), "the basic settings' sections");
    CHECK_EQ(st.sections(false), (std::vector<std::string>{"Game", "Controls", "Performance"}), "Advanced settings' sections");
    std::vector<std::string> basic_game;
    for (const Setting *s : st.rows("Game", true)) basic_game.push_back(s->env);
    CHECK_EQ(basic_game, (std::vector<std::string>{"DM_FPS_TARGET", "DM_RENDER_SCALE", "DM_OPMODE", "DM_LANG"}), "the basic rows in the list's order");
    const Setting *scale = setting(st, "DM_RENDER_SCALE"), *gain = setting(st, "DM_RUMBLE_GAIN");
    CHECK(scale && gain, "the rows are there");
    if (!scale || !gain) return;
    CHECK_EQ(st.range_label(*scale, 0.75), std::string("960 \xC3\x97 540 (75 %)"), "the render scale in pixels for Handheld");
    st.values["DM_OPMODE"] = "1";
    CHECK_EQ(st.range_label(*scale, 0.75), std::string("1200 \xC3\x97 675 (75 %)"), "... and Docked");
    CHECK_EQ(st.range_label(*gain, 1.5), std::string("150 %"), "a range as a percentage");
    CHECK(st.visible(*gain), "a sub-setting shows while its setting has the value it needs");
    st.values["DM_RUMBLE"] = "0";
    CHECK(!st.visible(*gain) && st.rows("Controls", false).empty(), "... and hides otherwise");
    CHECK(scale->accepts("0.5") && !scale->accepts("0.45") && !scale->accepts(" 0.75") && !scale->accepts("0x1") && !scale->accepts("nan"), "a range's saved values, as launcher.c takes them");
    CHECK(scale->same("1.00", "1.0") && !scale->same("0.95", "1.0"), "a range compares as numbers");
    st.reset();
    CHECK(!st.changed() && st.values["DM_OPMODE"] == "1" && st.values["DM_RENDER_SCALE"] == "1.0", "Reset to tested defaults");
    MapEnv fixed; fixed.vars = {{"DM_RUMBLE", "0"}, {"DM_VK_PSO_CACHE", "off"}};
    SettingsState sf = load_state(a, fixed);
    CHECK(sf.fixed.count("DM_RUMBLE") && sf.values["DM_RUMBLE"] == "0" && !sf.visible(*gain), "set at launch: fixed, with the environment's value");
    CHECK(sf.cache_fixed && !sf.cache_on(), "the cache switch set at launch");
    sf.reset();
    CHECK_EQ(sf.values["DM_RUMBLE"], std::string("0"), "Reset keeps what the environment fixed");
    MapEnv ignore; ignore.vars = {{"DM_SAVED_CHOICES", "0"}};
    SettingsState si = load_state(a, ignore);
    CHECK(si.ignore_saved && si.values["DM_OPMODE"] == "1" && si.refused.empty(), "DM_SAVED_CHOICES=0: the tested defaults");
    CHECK_EQ(cache_folder(a, load_state(a, none), none), fake::str(f.a_run / "vk_pipeline_cache"), "the pipeline cache beside the save (vk_cache.c folder())");
}

static void command(const fake::Folder &f) {
    auto g = find_games(fake::str(f.games));
    const Game &a = g[0], &b = g[1], &c = g[2];
    MapEnv none;
    Command cmd = compose(a, "", none);
    CHECK(cmd.error.empty() && cmd.mode == "balanced", "the first pack row by default");
    CHECK_EQ(cmd.argv, (std::vector<std::string>{a.launch.host, "--profile", fake::str(fake::profile_file(f.a)), fake::str(f.a / "aot_v131072")}),
             "nxhost-run, --profile with the game profile's file, and the pack folder");
    CHECK_EQ(cmd.cwd + "|" + cmd.log, fake::str(f.a_run) + "|" + fake::str(f.a_run / "log.txt"), "its working folder and log");
    CHECK_EQ(keys(cmd.set), (std::vector<std::string>{"DM_OPMODE", "DM_PRESENT_UPSCALE", "DM_DEBUG_LOG", "DM_RENDER_SCALE", "DM_MOTION", "DM_ROOT", "DM_ROMFS", "DM_SAVE", "DM_TEMP",
                                                      "DM_MSL_DIR", "DM_LANG", "DM_WINDOW", "DM_AUDIO"}), "env.txt's lines, then the env rows, as run.sh sets them");
    CHECK_EQ(value(cmd.set, "DM_PRESENT_UPSCALE"), std::string("metalfx"), "env.txt's lines go as they are, as run.sh passes them");
    CHECK_EQ(compose(a, "max", none).argv[3], fake::str(f.a / "aot_v131072" / "max"), "the max pack");
    MapEnv set; set.vars = {{"DM_LANG", "ja"}, {"DM_OPMODE", "1"}, {"NXHOST", "/opt/other/nxhost-run"}};
    Command cs = compose(a, "", set);
    CHECK(value(cs.set, "DM_LANG") == "<unset>" && value(cs.set, "DM_OPMODE") == "<unset>", "what the environment sets stays");
    CHECK_EQ(cs.argv[0], std::string("/opt/other/nxhost-run"), "NXHOST names another nxhost-run");
    // An empty value is unset on every platform (dm/host/plat/plat.h): set to nothing in the environment, or on a line of env.txt, it keeps nothing out
    MapEnv blank; blank.vars = {{"DM_LANG", ""}, {"NXHOST", ""}};
    CHECK(!blank.get("DM_LANG") && !ProcessEnv().get("T_LAUNCHER_SURELY_UNSET"), "an empty value counts as unset");
    Command cb = compose(a, "", blank);
    CHECK(value(cb.set, "DM_LANG") == "en-US" && cb.argv[0] == a.launch.host, "... so the env row's DM_LANG applies and the host row stands");
    fake::write(f.a_run / "env.txt", std::string(fake::kChoices) + "DM_LANG=\n");
    CHECK_EQ(value(compose(a, "", none).set, "DM_LANG"), std::string("en-US"), "an env.txt line with an empty value is as if absent: the env row applies");
    fake::write(f.a_run / "env.txt", fake::kChoices);
    MapEnv ignore; ignore.vars = {{"DM_SAVED_CHOICES", "0"}};
    CHECK_EQ(keys(compose(a, "", ignore).set).front(), std::string("DM_MOTION"), "DM_SAVED_CHOICES=0: env.txt is not read");
    CHECK(compose(b, "max", none).error.find("no max pack") != std::string::npos, "a mode that was not packed");
    CHECK(compose(c, "", none).error.find("incomplete") != std::string::npos, "an incomplete pack");
    Config cfg; cfg.set("pack.0100AAAA00000000", "max"); cfg.set("pack.0100BBBB00000000", "max");
    CHECK_EQ(chosen_mode(a, cfg) + " " + chosen_mode(b, cfg) + " " + chosen_mode(c, cfg), std::string("max balanced "), "the player's mode where it was packed, else the first");
}

// The game profile's file (launcher_model.h, "The programs a launch file names"): the path game_profile row read, passed with --profile whatever host runs (NXHOST too); a row
// whose file is gone and a launch file without the row are each the game's problem and Play's error, in the launcher's words, NXHOST or not; a missing host is said first
static void game_profile_rules(const fake::Folder &f, const fs::path &root) {
    MapEnv none, nxhost; nxhost.vars = {{"NXHOST", "/opt/other/nxhost-run"}};
    auto g = find_games(fake::str(f.games));
    const std::string bprof = fake::str(fake::profile_file(f.b));
    CHECK_EQ(g[0].launch.game_profile + "|" + g[1].launch.game_profile, fake::str(fake::profile_file(f.a)) + "|" + bprof, "the path game_profile row");
    Command cn = compose(g[1], "", nxhost);
    CHECK_EQ(cn.argv, (std::vector<std::string>{"/opt/other/nxhost-run", "--profile", bprof, fake::str(f.b / "aot_v0")}), "NXHOST's host gets the pack's --profile too");
    // the file gone
    fs::remove(fake::profile_file(f.b));
    const std::string gone = "the game profile " + bprof + " is missing: pack the game again";
    g = find_games(fake::str(f.games));
    CHECK_EQ(g[1].problem, gone, "the file gone: the game's problem says so");
    CHECK_EQ(compose(g[1], "", none).error, gone, "... and Play says so");
    CHECK(find_games(fake::str(f.games), kPlatform, bundle_dir(), nxhost)[1].problem == gone && compose(g[1], "", nxhost).error == gone, "... NXHOST or not");
    CHECK(g[0].problem.empty(), "another game's file is there: no problem");
    fake::profile(f.b, "0100BBBB00000000");
    CHECK(find_games(fake::str(f.games))[1].problem.empty(), "the file there again: no problem");
    // a launch file without the row (a pack made before the one host)
    const std::string id = "0100FFFF00000000";
    const fs::path games = root / "no profile row/games", build = games / (id + "_build"), run = games / (id + "-work/run");
    fake::packs(build / "aot_v0", false);
    std::string text = fake::launch("Phi Game", id, build, run, self_path(), false, {{"balanced", build / "aot_v0"}});
    const std::string row = "path\tgame_profile\t" + fake::str(fake::profile_file(build)) + "\n";
    CHECK(text.find(row) != std::string::npos, "the fake launch file has the row to take out");
    text.erase(text.find(row), row.size());
    fake::write(build / ("launch-" + std::string(kPlatform) + ".tsv"), text);
    const std::string none_row = "the launch file names no game profile (a pack made before one nxhost-run served every game): pack the game again";
    auto h = find_games(fake::str(games));
    CHECK(h.size() == 1 && h[0].launch.game_profile.empty() && h[0].problem == none_row, "no game_profile row: the game is listed with that problem: " + (h.empty() ? "" : h[0].problem));
    if (h.size() == 1) CHECK_EQ(compose(h[0], "", nxhost).error, none_row, "... and Play says so, NXHOST or not");
    // a missing host is said before a missing profile
    text = fake::launch("Phi Game", id, build, run, fake::str(root / "no profile row/no host"), false, {{"balanced", build / "aot_v0"}});
    text.erase(text.find(row), row.size());
    fake::write(build / ("launch-" + std::string(kPlatform) + ".tsv"), text);
    h = find_games(fake::str(games), kPlatform, fake::str(root / "no profile row/no bundle"), none);
    CHECK(h.size() == 1 && h[0].problem.rfind("the host ", 0) == 0, "neither the host nor the profile: the host's problem first");
}

static void round_trip(const fake::Folder &f) {
    auto g = find_games(fake::str(f.games));
    const Game &a = g[0];
    MapEnv none;
    SettingsState st = load_state(a, none);
    st.values["DM_FPS_TARGET"] = "30"; st.values["DM_OPMODE"] = "1"; st.cache_value = "off";
    std::string err;
    CHECK(save_choices(a, st, &err), "saved: " + err);
    CHECK_EQ(fake::read(f.a_run / "env.txt"), std::string("# the player's choices\nDM_DEBUG_LOG=1\nDM_RENDER_SCALE=0.75\nDM_FPS_TARGET=30\nDM_VK_PSO_CACHE=off\n"),
             "a choice equal to the default and the refused value go, the others stay in place, new ones at the end, other lines kept");
    SettingsState again = load_state(a, none);
    CHECK(again.values["DM_FPS_TARGET"] == "30" && again.values["DM_OPMODE"] == "1" && !again.cache_on() && again.refused.empty(), "read back as saved");
    CHECK_EQ(value(compose(a, "", none).set, "DM_VK_PSO_CACHE"), std::string("off"), "the cache switch reaches nxhost-run");
    again.reset();
    CHECK(save_choices(a, again, &err), "saved: " + err);
    CHECK_EQ(fake::read(f.a_run / "env.txt"), std::string("# the player's choices\nDM_DEBUG_LOG=1\n"), "Reset, then Play: every choice forgotten");
    fake::write(f.a_run / "env.txt", "DM_RUMBLE=0\n");
    MapEnv fixed; fixed.vars = {{"DM_RUMBLE", "1"}};
    SettingsState sf = load_state(a, fixed);
    sf.values["DM_OPMODE"] = "0";
    CHECK(save_choices(a, sf, &err), "saved: " + err);
    CHECK_EQ(fake::read(f.a_run / "env.txt"), std::string("DM_RUMBLE=0\nDM_OPMODE=0\n"), "a choice the environment hides for this start is kept");
    MapEnv ignore; ignore.vars = {{"DM_SAVED_CHOICES", "0"}};
    SettingsState si = load_state(a, ignore);
    si.values["DM_FPS_TARGET"] = "30";
    CHECK(save_choices(a, si, &err) && fake::read(f.a_run / "env.txt") == "DM_RUMBLE=0\nDM_OPMODE=0\n", "DM_SAVED_CHOICES=0: the saved choices stay as they are");
    const Game &b = g[1];
    SettingsState sb = load_state(b, none);
    CHECK(save_choices(b, sb, &err) && !fs::exists(f.b_run / "env.txt"), "nothing to save: no file made");
    fake::write(f.a_run / "env.txt", fake::kChoices);
}

// The host's files (dm/host/aot_profile.c dm_aot_profile_write and write_misses: their header lines, then one row per function or per address and reason)
static void profile_file(const fs::path &dir, const std::string &name, const std::string &sha, const std::vector<std::pair<uint64_t, uint64_t>> &rows) {
    uint64_t sum = 0;
    for (const auto &r : rows) sum += r.second;
    std::string t = "# nxaot profile: guest functions sampled by the x86-64 engine (dm/host/aot_profile.c, DM_AOT_PROFILE); nxaot build --profile <this file>\n";
    t += "# module " + name + ", build ID " + std::string(32, 'a') + ", image SHA-256 " + sha + "\n# pack /packs/" + name + kPackSuffix + "\n";
    t += "# " + std::to_string(sum) + " samples in " + std::to_string(rows.size()) + " of its " + std::to_string(rows.size() + 7) + " functions; 1000 Hz of each guest thread's CPU time, "
         "1 of 1 threads sampled; all modules: " + std::to_string(sum) + " samples, " + std::to_string(sum) + " in compiled code, 0 in host calls and SVCs of compiled functions, "
         "0 in the JIT fallback, 0 elsewhere in the host\n";
    t += "# <module offset of a function's entry> <samples>\n";
    for (const auto &r : rows) { char b[64]; std::snprintf(b, sizeof b, "%#llx %llu\n", (unsigned long long)r.first, (unsigned long long)r.second); t += b; }
    fake::write(dir / (name + ".tsv"), t);
}

struct Miss { uint64_t off; std::string reason; uint64_t n; };
static void misses_file(const fs::path &dir, const std::string &name, const std::string &sha, const std::vector<Miss> &rows) {
    uint64_t entries = 0; std::vector<uint64_t> addrs;
    for (const auto &r : rows) { entries += r.n; if (std::find(addrs.begin(), addrs.end(), r.off) == addrs.end()) addrs.push_back(r.off); }
    std::string t = "# nxaot misses: guest addresses the x86-64 engine's JIT fallback ran because the pack had no compiled code there (dm/host/aot_run.c, DM_AOT_PROFILE); "
                    "nxaot build --misses <this file>\n";
    if (!sha.empty()) t += "# module " + name + ", build ID " + std::string(32, 'a') + ", image SHA-256 " + sha + "\n";
    t += "# pack /packs/" + name + kPackSuffix + "\n";
    t += "# " + std::to_string(addrs.size()) + " addresses, " + std::to_string(entries) + " entries into the JIT fallback; " + std::to_string(addrs.size()) +
         " of the addresses this session, the rest from earlier sessions of this image\n";
    t += "# <module offset> <reason> <entries>: indirect, call, host, nocode (an entry of the next pack), nonlocal (a resume point), fpcr, refused, untranslated, other (no pack serves them)\n";
    for (const auto &r : rows) { char b[96]; std::snprintf(b, sizeof b, "0x%llx %s %llu\n", (unsigned long long)r.off, r.reason.c_str(), (unsigned long long)r.n); t += b; }
    fake::write(dir / (name + ".misses.tsv"), t);
}

// The host's known targets (dm/host/aot_targets.c): '#' lines naming the module and its image, then '0x<site> 0x<target> <binds>'
struct Target { uint64_t site, target, binds; };
static void targets_file(const fs::path &dir, const std::string &name, const std::string &sha, const std::vector<Target> &rows) {
    std::string t = "# nxaot targets: what the x86-64 engine's inline caches of indirect calls and branches were bound to (dm/host/aot_targets.c, DM_AOT_PROFILE); "
                    "nxaot build --profile <module>.tsv reads it beside the profile\n";
    if (!sha.empty()) t += "# module " + name + ", build ID " + std::string(32, 'a') + ", image SHA-256 " + sha + "\n";
    t += "# pack /packs/" + name + kPackSuffix + "\n# <site module offset> <target module offset> <binds>\n";
    for (const auto &r : rows) { char b[96]; std::snprintf(b, sizeof b, "0x%llx 0x%llx %llu\n", (unsigned long long)r.site, (unsigned long long)r.target, (unsigned long long)r.binds); t += b; }
    fake::write(dir / (name + ".targets.tsv"), t);
}
// The fixture dm/tools/tests/test_nsp_x86.py reads too (TARGET_ROWS): nxaot's rule (at least 90 % of a site's binds) gives 0x1a64 -> 0x1820 (its only target), 0x2000 -> 0x3000 (18
// of 20), 0x2300 -> 0x3300 (5 + 4 in two rows); 0x1984 (2, 1, 2), 0x2100 (17 of 19) and 0x2200 (0 binds) none. Its digest: FNV-1a 64 of '1a64:1820\n2000:3000\n2300:3300\n'
static const std::vector<Target> kTargetRows = {{0x1a64, 0x1820, 1}, {0x1984, 0x1690, 2}, {0x1984, 0x16d0, 1}, {0x1984, 0x1720, 2}, {0x2000, 0x3000, 18}, {0x2000, 0x3100, 2},
                                                {0x2100, 0x3000, 17}, {0x2100, 0x3100, 2}, {0x2200, 0x3200, 0}, {0x2300, 0x3300, 5}, {0x2300, 0x3300, 4}};
static std::vector<Target> single_targets(uint64_t sites) {   // sites call sites with one target each
    std::vector<Target> rows;
    for (uint64_t i = 0; i < sites; i++) rows.push_back({0x10000 + 4 * i, 0x20000, 1});
    return rows;
}

static std::vector<std::pair<uint64_t, uint64_t>> sampled(uint64_t offsets, uint64_t samples) {   // offsets functions sampled, samples in all
    std::vector<std::pair<uint64_t, uint64_t>> rows;
    for (uint64_t i = 0; i < offsets; i++) rows.push_back({0x1000 + 0x40 * i, samples / offsets + (i == 0 ? samples % offsets : 0)});
    return rows;
}

// The host's file since its function lines (aot_profile.c dm_aot_profile_write): per sampled function its line '# function 0x<entry> <samples> <rows>', then its rows, one per sampled
// guest instruction. functions(n, rows, per_row): n functions at sampled()'s entries, each with rows instructions of per_row samples and one sample more in a host call (counted in
// its line, at no row). with_lines false: the same rows without the function lines (the counting of a file without them)
static void functions_file(const fs::path &dir, const std::string &name, const std::string &sha, uint64_t n, uint64_t rows, uint64_t per_row, bool with_lines = true) {
    const uint64_t fn_samples = rows * per_row + 1, sum = n * fn_samples;
    std::string t = "# nxaot profile: guest instructions sampled by the x86-64 engine (dm/host/aot_profile.c, DM_AOT_PROFILE); nxaot build --profile <this file>\n";
    t += "# module " + name + ", build ID " + std::string(32, 'a') + ", image SHA-256 " + sha + "\n# pack /packs/" + name + kPackSuffix + " (its positions give each sample's instruction)\n";
    t += "# " + std::to_string(sum) + " samples in " + std::to_string(n) + " of its " + std::to_string(n + 7) + " functions: " + std::to_string(n * rows * per_row) + " at " +
         std::to_string(n * rows) + " guest instructions, 0 at the entries of the 0 functions with none at an instruction, " + std::to_string(n) + " not written (host calls, SVCs and "
         "code with no instruction of functions with samples at their instructions); 1000 Hz of each guest thread's CPU time\n";
    t += "# functions: each sampled function's rows follow its line `# function 0x<entry> <samples> <rows>` (its samples: all of them, at its instructions, in its host calls and SVCs "
         "and in its code with no instruction; its rows: the lines after it), most samples first; the packer and the launcher count the sampled functions by these lines, nxaot reads "
         "them as comments\n";
    t += "# <module offset> <samples>: a guest instruction's, or a function's at its entry when it has none at an instruction\n";
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t e = 0x1000 + 0x40 * i;
        char b[96];
        if (with_lines) { std::snprintf(b, sizeof b, "# function %#llx %llu %llu\n", (unsigned long long)e, (unsigned long long)fn_samples, (unsigned long long)rows); t += b; }
        for (uint64_t r = 0; r < rows; r++) { std::snprintf(b, sizeof b, "%#llx %llu\n", (unsigned long long)(e + 4 * r), (unsigned long long)per_row); t += b; }
    }
    fake::write(dir / (name + ".tsv"), t);
}

// The second pack (launcher_model.h): the profile folder, "Record a profile while playing" and DM_AOT_PROFILE in what starts the game, whether a better pack is possible (what the
// profile folder holds against the launch file's made rows, the files written in the host's exact form), and the packer's command for the repack, run (this program acts as the
// packer: the launch file's python row names it)
static void second_pack(const fake::Folder &f) {
    auto g = find_games(fake::str(f.games));
    const Game &a = g[0], &b = g[1];
    MapEnv none;
    const std::string a_prof = fake::str(f.a_run.parent_path() / "profile");
    CHECK_EQ(profile_folder(a), a_prof, "no profile row: <id>-work/profile beside run/, where the packer looks");
    CHECK_EQ(profile_folder(b), fake::str(f.games / "Beta NSPs" / "recorded"), "the profile row");
    Config c;
    CHECK(record_profile(c, a) && record_profile(c, b), "\"Record a profile while playing\" is on by default");
    c.set("record.0100AAAA00000000", "0");
    CHECK(!record_profile(c, a) && record_profile(c, b), "... and off per game");
    Command on = compose(a, "", none, true);
    CHECK(!on.set.empty() && on.set.back().key == "DM_AOT_PROFILE" && on.set.back().value == a_prof, "recording: DM_AOT_PROFILE, last, the profile folder");
    CHECK_EQ(keys(compose(a, "", none).set).size() + 1, on.set.size(), "... and nothing else changes");
    CHECK_EQ(value(compose(a, "", none, false).set, "DM_AOT_PROFILE"), std::string("<unset>"), "not recording: no DM_AOT_PROFILE");
    MapEnv set; set.vars = {{"DM_AOT_PROFILE", "/elsewhere"}};
    CHECK_EQ(value(compose(a, "", set, true).set, "DM_AOT_PROFILE"), std::string("<unset>"), "the environment's DM_AOT_PROFILE stays");
    fake::write(f.a_run / "env.txt", std::string(fake::kChoices) + "DM_AOT_PROFILE=/saved/profile\n");
    CHECK_EQ(value(compose(a, "", none, true).set, "DM_AOT_PROFILE"), std::string("/saved/profile"), "a DM_AOT_PROFILE line of env.txt wins over the launcher's");
    fake::write(f.a_run / "env.txt", fake::kChoices);
    // A better pack: what the profile folder holds against what the launch file says the packs took (its made rows), not the files' times
    ProfileState s0 = profile_state(a, "balanced");
    CHECK(s0.folder == a_prof && s0.files == 0 && s0.pack_files == 2 && !s0.better() && s0.reason().empty(), "no profile yet: nothing better (the balanced folder's two packs)");
    const fs::path prof = fs::u8path(a_prof);
    const std::string main_sha(64, '1'), nro_sha(64, '2'), other_sha(64, 'e');   // fake_games.h: main's manifest and nro0's row of nro.tsv
    // the packer's digests of these files (dm/tools/tests/test_nsp_x86.py DIGEST_OFFSETS, DIGEST_MISSES: build_ipa_from_nsp.py fnv64)
    const std::string d_offsets = "ce9643fb137f8dce", d_misses = "b3c6eacf4fcf4a0e";
    profile_file(prof, "main", main_sha, {{0x100, 40}, {0x200, 2}});
    misses_file(prof, "main", main_sha, {{0x100, "nonlocal", 2}, {0x208, "indirect", 1}});
    fake::write(prof / "main.tsv.tmp", "being written\n");   // the host's temporary name: not counted
    const auto t0 = fs::file_time_type::clock::now();
    for (const char *n : {"main", "nro0"}) {   // the balanced packs older than the files, the max packs newer: the rule does not look at times
        fs::last_write_time(f.a / "aot_v131072" / (std::string(n) + kPackSuffix), t0 - std::chrono::hours(1));
        fs::last_write_time(f.a / "aot_v131072" / "max" / (std::string(n) + kPackSuffix), t0 + std::chrono::hours(1));
    }
    ProfileState sb = profile_state(a, "balanced"), sm = profile_state(a, "max");
    CHECK(sb.files == 2 && !sb.known && sb.better() && sm.better(), "a launch file without made rows: packs made without a profile or misses, so both modes may be better, whatever the times");
    CHECK_EQ(sb.reason(), std::string("2 new JIT misses; 2 sampled functions the packs were made without; the packs were made before the launch file recorded what they took"), "... and why");
    CHECK(sb.samples["main"].count == 2 && sb.samples["main"].samples == 42 && sb.samples["main"].digest == d_offsets, "main.tsv as the packer reads it, with the packer's digest");
    CHECK(sb.misses["main"].count == 2 && sb.misses["main"].digest == d_misses, "main.misses.tsv as the packer reads it, with the packer's digest");

    // The launch file with made rows (build_ipa_from_nsp.py made_rows)
    const fs::path launch = f.a / ("launch-" + std::string(kPlatform) + ".tsv");
    const std::string launch0 = fake::read(launch);
    auto state = [&](const std::string &rows, const std::string &mode = "balanced") {
        fake::write(launch, launch0 + rows);
        auto gs = find_games(fake::str(f.games));
        return profile_state(gs[0], mode);
    };
    const std::string yes = "made\tprofile\tyes\nmade\tmisses\tyes\n", same = "samples\tmain\t2 42 " + d_offsets + "\nmisses\tmain\t2 " + d_misses + "\n";
    {
        fake::write(launch, launch0 + yes + same);
        auto gs = find_games(fake::str(f.games));
        const LaunchFile &l = gs[0].launch;
        CHECK(l.made_profile == "yes" && l.made_misses == "yes" && l.made_samples.at("main").count == 2 && l.made_samples.at("main").samples == 42 &&
                  l.made_samples.at("main").digest == d_offsets && l.made_miss.at("main").count == 2 && l.made_miss.at("main").digest == d_misses && gs[0].problem.empty(),
              "the made, samples and misses rows read (and the launch file still starts the game)");
    }
    ProfileState s = state(yes + same);
    CHECK(s.known && !s.better() && s.reason().empty(), "the packs took what the folder holds: nothing better");
    CHECK(!state(yes + same, "max").better(), "... for the max packs too (the rows hold for every pack row)");
    // JIT misses
    misses_file(prof, "main", main_sha, {{0x100, "nonlocal", 3}, {0x208, "indirect", 9}});
    CHECK(!state(yes + same).better(), "the same misses with other counts (a later session merged in): nothing better");
    misses_file(prof, "main", main_sha, {{0x100, "nonlocal", 3}, {0x208, "indirect", 9}, {0x300, "host", 1}});
    s = state(yes + same);
    CHECK(s.better() && s.reason() == "1 new JIT miss", "a new miss address: better: " + s.reason());
    CHECK(!state(yes + same.substr(0, same.find("misses\t")) + "misses\tmain\t3 " + s.misses["main"].digest + "\n").better(), "... and not once the packs took it");
    misses_file(prof, "main", main_sha, {{0x100, "nonlocal", 3}, {0x400, "call", 1}});
    s = state(yes + same);
    CHECK(s.better() && s.reason() == "JIT misses the packs did not take", "as many addresses, other ones (another digest): better: " + s.reason());
    misses_file(prof, "main", main_sha, {{0x100, "nonlocal", 3}, {0x208, "indirect", 9}, {0x300, "host", 1}});
    CHECK(!state("made\tprofile\tyes\nmade\tmisses\tunused\n" + same).better(), "made misses unused (this nxaot has no --misses): a repack would not take them either");
    misses_file(prof, "main", other_sha, {{0x100, "nonlocal", 3}, {0x208, "indirect", 9}, {0x300, "host", 1}});
    s = state(yes + same);
    CHECK(!s.better() && s.misses.empty(), "misses recorded on another image: the packer would not take them");
    misses_file(prof, "main", "", {{0x300, "host", 1}});
    s = state("");
    CHECK(s.misses.empty() && s.reason() == "2 sampled functions the packs were made without; the packs were made before the launch file recorded what they took",
          "misses that name no image: the packer would not take them (unknown rows: the profile alone)");
    misses_file(prof, "main", main_sha, {{0x100, "nonlocal", 2}, {0x208, "indirect", 1}});
    // The sample profile: nxaot puts every function holding a sampled offset at tier H, so the sampled functions are the hot set; these files are an older host's form (no function
    // lines, a row per function at its entry), whose rows count as functions
    const std::string hundred = yes + "samples\tmain\t100 10000 0000000000000000\nmisses\tmain\t2 " + d_misses + "\n";
    profile_file(prof, "main", main_sha, sampled(105, 12000));
    s = state(hundred);
    CHECK(s.samples["main"].count == 105 && s.samples["main"].samples == 12000 && !s.better(), "5 % more offsets, 20 % more samples: below the thresholds, nothing better");
    profile_file(prof, "main", main_sha, sampled(110, 11000));
    s = state(hundred);
    CHECK(s.better() && s.reason() == "10 new sampled functions", "10 % more offsets: better: " + s.reason());
    profile_file(prof, "main", main_sha, sampled(3500, 90000));
    s = state(hundred);
    CHECK(s.reason() == "3,400 new sampled functions", "the count written with thousands separators: " + s.reason());
    profile_file(prof, "main", main_sha, sampled(100, 12500));
    s = state(hundred);
    CHECK(s.better() && s.reason() == "a longer session's profile (+25 % samples)", "as many offsets, other ones, from a session with 25 % more samples: better: " + s.reason());
    CHECK(!state(yes + "samples\tmain\t100 10000 " + s.samples["main"].digest + "\nmisses\tmain\t2 " + d_misses + "\n").better(),
          "25 % more samples on the same offsets: the same hot set, the same pack: nothing better");
    profile_file(prof, "main", main_sha, sampled(100, 12400));
    CHECK(!state(hundred).better(), "other offsets with 24 % more samples: nothing better");
    profile_file(prof, "main", main_sha, sampled(95, 20000));
    CHECK(!state(hundred).better(), "fewer offsets (a shorter session's hot set) even with more samples: never better, a pack from it would put functions back at tier C");
    profile_file(prof, "main", main_sha, sampled(100, 10000));
    s = state("made\tprofile\tno\nmade\tmisses\tyes\nmisses\tmain\t2 " + d_misses + "\n");
    CHECK(s.better() && s.reason() == "100 sampled functions the packs were made without", "made profile no (--no-profile; a repack takes it): better: " + s.reason());
    CHECK(!state("made\tprofile\tunused\nmade\tmisses\tyes\nmisses\tmain\t2 " + d_misses + "\n").better(), "made profile unused (AOT_PROFILE off or an nxaot without --profile): nothing better");
    s = state(yes + "misses\tmain\t2 " + d_misses + "\n");
    CHECK(s.better() && s.reason() == "100 sampled functions the packs were made without", "made profile yes but no samples row of main (no profile of it then): better: " + s.reason());
    const std::string main_same = yes + "samples\tmain\t100 10000 " + s.samples["main"].digest + "\nmisses\tmain\t2 " + d_misses + "\n";
    // The host's function lines (aot_profile.c): the rule counts the sampled functions, nxaot's tier H, not the rows of their sampled instructions, which grow with a session's length
    profile_file(prof, "main", main_sha, sampled(10, 310));   // an older host's file: ten functions at their entries, 31 samples each
    const std::string no_row = yes + "misses\tmain\t2 " + d_misses + "\n";
    const Recorded entry_form = state(no_row).samples["main"];
    functions_file(prof, "main", main_sha, 10, 3, 10);        // the same ten functions: 3 instructions of 10 samples each and a host call's sample, each
    s = state(no_row);
    CHECK(s.samples["main"].count == 10 && s.samples["main"].samples == 310 && s.samples["main"].digest == entry_form.digest && entry_form.count == 10 && entry_form.samples == 310,
          "function lines: 10 functions (not 30 rows), the lines' 310 samples (the rows' 300 and the host calls' 10), the digest of the entries: an older host's file of those functions'");
    const std::string ten = yes + "samples\tmain\t10 310 " + entry_form.digest + "\nmisses\tmain\t2 " + d_misses + "\n";
    CHECK(!state(ten).better(), "the packs took those ten functions (a samples row made from either form): nothing better");
    functions_file(prof, "main", main_sha, 10, 12, 10);       // a session four times as long over the same functions: 120 rows, 1,210 samples
    s = state(ten);
    CHECK(s.samples["main"].count == 10 && s.samples["main"].samples == 1210 && !s.better(),
          "the same functions with four times the instruction rows and samples: nothing better (the next pack's branch weights only) " + s.reason());
    functions_file(prof, "main", main_sha, 11, 3, 10);        // an eleventh function sampled
    s = state(ten);
    CHECK(s.better() && s.reason() == "1 new sampled function", "one new sampled function among ten (10 %): better: " + s.reason());
    functions_file(prof, "main", main_sha, 10, 12, 10, false);   // the long session's rows without their function lines: each offset counts as a function
    s = state(ten);
    CHECK(s.samples["main"].count == 120 && s.samples["main"].samples == 1200 && s.reason() == "110 new sampled functions",
          "a file without function lines: its rows counted as functions (an older host's meaning): " + s.reason());
    profile_file(prof, "main", main_sha, sampled(100, 10000));   // main_same's again
    profile_file(prof, "nro0", nro_sha, {{0x10, 5}});
    s = state(main_same);
    CHECK(s.better() && s.reason() == "1 sampled function the packs were made without", "an NRO's profile (nro.tsv's image) that its pack did not take: better: " + s.reason());
    profile_file(prof, "nro0", other_sha, {{0x10, 5}});
    profile_file(prof, "other", main_sha, {{0x10, 5}});   // no module of the packs
    fake::write(prof / "main.tsv", "0x100 many\n");          // one that does not parse: the packer packs without it
    s = state(yes + same);
    CHECK(!s.better() && s.samples.empty() && s.files == 4, "a profile of another image, of no module of the packs, or one that does not parse: not taken");
    // The known targets (<module>.targets.tsv, aot_targets.c): what changes the next pack is a site whose dominant target the packs were made without
    fs::remove_all(prof);
    const std::string d_targets = "fca3e8e0f0106c26";   // test_nsp_x86.py DIGEST_TARGETS
    misses_file(prof, "main", main_sha, {{0x100, "nonlocal", 2}, {0x208, "indirect", 1}});
    targets_file(prof, "main", main_sha, kTargetRows);
    fake::write(prof / "main.weights.tsv", "0x100 0x200 1\n");   // no file the host writes: ignored, never read as a profile
    fake::write(prof / "main.bind.tsv", "0x10 0x20 memcpy\n");    // the loader's bindings, which the host writes there too: not a profile file, and not named as ignored
    s = state(main_same);
    CHECK(s.files == 2 && s.ignored == std::vector<std::string>{"main.weights.tsv"} && s.samples.empty() && s.targets.empty() && !s.better(),
          "a targets file alone is not a profile, and without the profile beside it nxaot reads none: nothing; another .tsv ignored: " + s.reason());
    profile_file(prof, "main", main_sha, sampled(100, 10000));   // main_same's profile
    const std::string targets_yes = main_same + "made\ttargets\tyes\n", took = targets_yes + "targets\tmain\t3 " + d_targets + "\n";
    s = state(main_same);
    CHECK(s.targets["main"].count == 3 && s.targets["main"].digest == d_targets && s.files == 3,
          "main.targets.tsv as the packer reads it: 3 sites with a dominant target (nxaot's 90 %), the packer's digest of the pairs");
    CHECK(s.better() && s.reason() == "3 known call targets the packs were made without", "made rows from before the targets rows, a targets file now: better: " + s.reason());
    s = state("");
    CHECK(s.reason() == "2 new JIT misses; 100 sampled functions the packs were made without; 3 known call targets the packs were made without; the packs were made before the "
                        "launch file recorded what they took", "no made rows at all: every file counts: " + s.reason());
    {
        fake::write(launch, launch0 + took);
        auto gs = find_games(fake::str(f.games));
        const LaunchFile &l = gs[0].launch;
        CHECK(l.made_targets == "yes" && l.made_target.at("main").count == 3 && l.made_target.at("main").digest == d_targets,
              "the made targets and targets rows read");
    }
    CHECK(!state(took).better(), "the packs took them: nothing better");
    CHECK(state(targets_yes).reason() == "3 known call targets the packs were made without",
          "made targets yes but no targets row of main (no targets of it then): better");
    targets_file(prof, "main", main_sha, [] { auto r = kTargetRows; r.push_back({0x1a64, 0x1820, 7}); r.push_back({0x2000, 0x3000, 9}); r.push_back({0x1984, 0x1690, 1}); return r; }());
    s = state(took);
    CHECK(s.targets["main"].digest == d_targets && !s.better(), "more binds at the same dominant targets (a later session merged in): the same pairs, nothing better: " + s.reason());
    targets_file(prof, "main", main_sha, [] { auto r = kTargetRows; r.push_back({0x2400, 0x3400, 1}); return r; }());
    s = state(took);
    CHECK(s.better() && s.reason() == "1 new known call target", "a new site with a dominant target, 1 of 3 (over 10 %): better: " + s.reason());
    targets_file(prof, "main", main_sha, [] { auto r = kTargetRows; r.push_back({0x2000, 0x3100, 16}); return r; }());
    s = state(took);
    CHECK(s.targets["main"].count == 2 && !s.better(),
          "a site whose binds became 50/50 (18 and 18): no dominant target, 2 sites; the packs' guard there still works (a mismatch takes the inline cache): nothing better");
    targets_file(prof, "main", main_sha, [] { auto r = kTargetRows; r.push_back({0x2000, 0x3100, 200}); return r; }());
    s = state(took);
    CHECK(s.targets["main"].count == 3 && s.targets["main"].digest != d_targets && s.better() && s.reason() == "1 new known call target",
          "a site whose dominant target changed (0x3000 to 0x3100): as many sites, another digest: at least 1 new: " + s.reason());
    targets_file(prof, "main", main_sha, single_targets(109));
    const std::string hundred_sites = targets_yes + "targets\tmain\t100 0000000000000000\n";
    CHECK(!state(hundred_sites).better(), "9 new sites with a dominant target among 100 (9 %): below the threshold, nothing better");
    targets_file(prof, "main", main_sha, single_targets(110));
    s = state(hundred_sites);
    CHECK(s.better() && s.reason() == "10 new known call targets", "10 new among 100 (10 %): better: " + s.reason());
    targets_file(prof, "main", main_sha, single_targets(100));
    CHECK(state(hundred_sites).reason() == "", "as many sites, another digest, 1 of 100 at least (1 %): nothing better");
    targets_file(prof, "main", main_sha, kTargetRows);
    s = state(main_same + "made\ttargets\tno\n");
    CHECK(s.better() && s.reason() == "3 known call targets the packs were made without", "made targets no (--no-profile; a repack takes them with the profile): better: " + s.reason());
    CHECK(!state(main_same + "made\ttargets\tunused\n").better(), "made targets unused (NXAOT_PROFILE_TARGETS=0, or no profile taken): nothing better");
    CHECK(state("made\tprofile\tunused\nmade\tmisses\tyes\nmisses\tmain\t2 " + d_misses + "\n").reason().empty(),
          "made profile unused and no made targets row: no targets taken by a repack either");
    targets_file(prof, "main", other_sha, kTargetRows);
    s = state(targets_yes);
    CHECK(s.targets.empty() && !s.better(), "targets recorded on another image: the packer would not take them");
    targets_file(prof, "main", "", kTargetRows);
    CHECK(state(targets_yes).targets.empty(), "targets that name no image: not taken (as the misses)");
    fake::write(prof / "main.targets.tsv", "# module main, build ID " + std::string(32, 'a') + ", image SHA-256 " + main_sha + "\n0x2000 0x3000 many\n");
    CHECK(state(targets_yes).targets.empty(), "a row that does not parse: not taken (nxaot stops on it, the packer keeps it from nxaot)");
    fake::write(launch, launch0);
    fs::remove_all(prof);
    // The repack: the packer on the NSP folder with this system, the mode and what the pack was made with
    Command r = repack_command(a, "max");
    CHECK(r.error.empty(), "the repack's command: " + r.error);
    CHECK_EQ(r.argv, (std::vector<std::string>{a.launch.host, fake::str(f.games.parent_path() / "scripts" / "build_ipa_from_nsp.py"), fake::str(f.games), "--platform", kPlatform,
                                                "--pack-mode", "max", "-k", fake::str(f.games.parent_path() / "keys" / "prod.keys"), "--nxaot", fake::str(f.games.parent_path() / "nxaot")}),
             "python, packer, the NSP folder, --platform, --pack-mode, -k, --nxaot");
    CHECK(r.cwd == fake::str(f.games) && r.log == fake::str(f.a_run / "repack.txt") && value(r.set, "PYTHONUNBUFFERED") == "1",
          "in the NSP folder, its output in repack.txt beside the game's log, unbuffered");
    CHECK(repack_command(b, "balanced").error.find("pack it once more") != std::string::npos, "a launch file without the repack's rows: said");
    r.set.push_back({"NXL_FAKE_HOST", "1"});
    Child ch; std::string err;
    CHECK(spawn(r, &ch, &err) && wait(&ch) == 0, "the packer ran: " + err);
    std::string log = fake::read(f.a_run / "repack.txt");
    CHECK(log.find("fake host: argv " + r.argv[1] + "|" + fake::str(f.games) + "|--platform|" + kPlatform + "|--pack-mode|max|-k|") != std::string::npos, "its arguments: " + log);
}

// A bundle moved or replaced (launcher_model.h, "The programs a launch file names"): the launch file's host, Python, packer and nxaot are gone, and the launcher takes the bundle's
// (its own folder's bin/nxhost-run, the one host of every game, the Python its VERSION names, scripts/build_ipa_from_nsp.py, bin/nxaot) when they are there; NXHOST wins over both
// hosts; the problem only when none is there.
// The game profile's file stays with the pack (in <id>_build), so the bundle's host gets it as the launch file's host would
static void moved_bundle(const fs::path &root) {
    const std::string id = "0100EEEE00000000";
    const fs::path games = root / "moved/games", build = games / (id + "_build"), run = games / (id + "-work/run"), old = root / "moved/old bundle", bundle = root / "moved/bundle";
    const std::string row = fake::str(old / "bin" / (std::string("nxhost-run") + kExeSuffix)), own = fake::str(bundle / "bin" / (std::string("nxhost-run") + kExeSuffix));
    fake::profile(build, id);
    const std::string row_packer = fake::str(old / "scripts/build_ipa_from_nsp.py"), row_nxaot = fake::str(old / "bin" / (std::string("nxaot") + kExeSuffix));
    const std::string own_packer = fake::str(bundle / "scripts" / "build_ipa_from_nsp.py"), own_nxaot = fake::str(bundle / "bin" / (std::string("nxaot") + kExeSuffix));
    // the bundle's Python where scripts/package_port.py --python puts it (python/bin/python3.14 on Linux, python\python.exe on Windows), named by its VERSION file
    const std::string py_rel = std::string(kPlatform) == "windows" ? "python/python.exe" : "python/bin/python3.14";
    const std::string row_python = fake::str(old / fs::u8path(py_rel).make_preferred()), own_python = fake::str(bundle / fs::u8path(py_rel).make_preferred());
    fake::write(row_python, "old python");
    fake::packs(build / "aot_v0", false);
    const std::string repack = "path\tpackage\t" + fake::str(games) + "\npath\tpacker\t" + row_packer + "\npath\tpython\t" + row_python + "\npath\tnxaot\t" + row_nxaot + "\n";
    const fs::path launch = build / ("launch-" + std::string(kPlatform) + ".tsv");
    fake::write(launch, fake::launch("Epsilon Game", id, build, run, row, false, {{"balanced", build / "aot_v0"}}, repack));
    fs::create_directories(bundle);
    MapEnv none, nxhost; nxhost.vars = {{"NXHOST", "/opt/other/nxhost-run"}};
    const std::string missing = "the host " + row + " is missing: pack the game again or set NXHOST";
    auto game = [&](const Env &env) { auto g = find_games(fake::str(games), kPlatform, fake::str(bundle), env); return g.size() == 1 ? g[0] : Game(); };

    // neither the row's host nor the bundle's: the problem, in the list and when it starts
    CHECK_EQ(game(none).problem, missing, "neither host is there: the Problem column says so");
    CHECK_EQ(compose(game(none), "", none, false, fake::str(bundle)).error, missing, "... and Play says so");
    CHECK(game(nxhost).problem.empty(), "NXHOST set: no problem, whatever the launch file names");
    Command c = compose(game(nxhost), "", nxhost, false, fake::str(bundle));
    CHECK(c.error.empty() && c.argv[0] == "/opt/other/nxhost-run" && c.notes.empty(), "NXHOST names the host when neither is there");

    // the bundle's host when the row's is gone
    fake::write(own, "bundle host");
    Game g = game(none);
    CHECK(g.problem.empty(), "the row's host is gone and the bundle's is there: no problem: " + g.problem);
    c = compose(g, "", none, false, fake::str(bundle));
    CHECK(c.error.empty() && c.argv == (std::vector<std::string>{own, "--profile", fake::str(fake::profile_file(build)), fake::str(build / "aot_v0")}),
          "the bundle's bin/nxhost-run is started, with the pack's --profile: " + c.error);
    CHECK_EQ(c.notes, (std::vector<std::string>{"host: the bundle's " + own + " (the launch file's " + row + " is missing)"}), "... and the log says which host and why");
    Found h = find_host(g.launch, none, fake::str(bundle));
    CHECK(h.path == own && h.problem.empty(), "find_host: the bundle's");
    c = compose(g, "", nxhost, false, fake::str(bundle));
    CHECK(c.argv[0] == "/opt/other/nxhost-run" && c.notes.empty(), "NXHOST wins over the bundle's host");
    CHECK(compose(g, "", none, false, fake::str(root / "moved/elsewhere")).error == missing, "another launcher's folder without the host: the problem");

    // the row's host, there again, wins over the bundle's
    fake::write(row, "old host");
    c = compose(game(none), "", none, false, fake::str(bundle));
    CHECK(c.argv[0] == row && c.notes.empty(), "the launch file's host when it is there, nothing logged");
    CHECK(compose(game(nxhost), "", nxhost, false, fake::str(bundle)).argv[0] == "/opt/other/nxhost-run", "NXHOST wins over the launch file's host too");
    fs::remove(row);

    // the repack: the packer row's script, else the bundle's scripts/build_ipa_from_nsp.py; the nxaot row's, else the bundle's bin/nxaot
    g = game(none);
    Command r = repack_command(g, "balanced", fake::str(bundle));
    CHECK_EQ(r.error, "the packer " + row_packer + " is missing: pack the game once more by hand with the bundle's " + (std::string(kPlatform) == "windows" ? "pack.cmd" : "pack.sh"),
             "neither packer is there: said");
    fake::write(own_packer, "# the bundle's packer\n");
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK_EQ(r.error, "the recompiler " + row_nxaot + " is missing: pack the game once more by hand with --nxaot <the recompiler>", "neither nxaot is there: said");
    fake::write(own_nxaot, "nxaot");
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK(r.error.empty() && r.argv == (std::vector<std::string>{row_python, own_packer, fake::str(games), "--platform", kPlatform, "--pack-mode", "balanced", "--nxaot", own_nxaot}),
          "the bundle's packer and nxaot: " + r.error);
    CHECK_EQ(r.notes, (std::vector<std::string>{"packer: the bundle's " + own_packer + " (the launch file's " + row_packer + " is missing)",
                                                "recompiler: the bundle's " + own_nxaot + " (the launch file's " + row_nxaot + " is missing)"}), "... both logged");
    fake::write(row_packer, "# the old packer\n"); fake::write(row_nxaot, "old nxaot");
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK(r.argv[1] == row_packer && r.argv.back() == row_nxaot && r.notes.empty(), "the rows' packer and nxaot when they are there");
    g.launch.nxaot.clear();
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK(r.error.empty() && std::find(r.argv.begin(), r.argv.end(), "--nxaot") == r.argv.end(), "no nxaot row: no --nxaot (the packer finds its own), the bundle's not taken");

    // the python row's Python, else the bundle's own (its VERSION's python line); a bundle without its own Python ("python no") or without VERSION: the problem
    fs::remove(row_python);
    const std::string py_missing = "the Python " + row_python + " is missing: pack the game once more by hand with the bundle's " +
                                   (std::string(kPlatform) == "windows" ? "pack.cmd" : "pack.sh");
    CHECK_EQ(repack_command(g, "balanced", fake::str(bundle)).error, py_missing, "the row's Python is gone and the bundle has no VERSION: said");
    fake::write(bundle / "VERSION", "nxnative 0123456\ndate 2026-11-15\nplatform linux-x86_64\npython no\nspirv yes\n");
    CHECK_EQ(repack_command(g, "balanced", fake::str(bundle)).error, py_missing, "... a bundle without its own Python (python no): said");
    fake::write(bundle / "VERSION", "nxnative 0123456\r\ndate 2026-11-15\r\npython 3.14.10 " + py_rel + "\r\nspirv yes\r\n");   // a Windows bundle's CRLF lines
    CHECK_EQ(repack_command(g, "balanced", fake::str(bundle)).error, py_missing, "... VERSION names a Python the bundle does not hold: said");
    fake::write(own_python, "bundle python");
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK(r.error.empty() && r.argv.size() > 2 && r.argv[0] == own_python && r.argv[1] == row_packer, "the bundle's own Python, the one its VERSION names: " + r.error);
    CHECK_EQ(r.notes, (std::vector<std::string>{"Python: the bundle's " + own_python + " (the launch file's " + row_python + " is missing)"}), "... logged");
    fake::write(row_python, "old python");
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK(r.argv[0] == row_python && r.notes.empty(), "the row's Python when it is there, nothing logged");

    // A newer bundle unpacked beside the earlier one, which is still there: the rows name the earlier bundle's programs (a folder above them holds VERSION and bin/nxhost-run), and
    // the launcher takes its own bundle's instead, for the host and for the repack; NXHOST still wins; the earlier bundle's own launcher keeps its programs
    fake::write(row, "old host"); fake::write(old / "VERSION", "nxnative 0000001\npython 3.14.10 " + py_rel + "\n");
    g = game(none);
    g.launch.nxaot = row_nxaot;
    c = compose(g, "", none, false, fake::str(bundle));
    CHECK(c.error.empty() && c.argv[0] == own, "another bundle's host in the row: this bundle's bin/nxhost-run is started: " + c.error);
    const std::string theirs = " is another bundle's, " + fake::str(old) + ")";
    CHECK_EQ(c.notes, (std::vector<std::string>{"host: the bundle's " + own + " (the launch file's " + row + theirs}), "... and the log says which host and why");
    CHECK(compose(g, "", nxhost, false, fake::str(bundle)).argv[0] == "/opt/other/nxhost-run", "NXHOST wins over both bundles' hosts");
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK(r.error.empty() && r.argv.size() > 2 && r.argv[0] == own_python && r.argv[1] == own_packer && r.argv.back() == own_nxaot,
          "the repack runs this bundle's Python, packer and nxaot, not the earlier bundle's: " + r.error);
    CHECK_EQ(r.notes, (std::vector<std::string>{"Python: the bundle's " + own_python + " (the launch file's " + row_python + theirs,
                                                "packer: the bundle's " + own_packer + " (the launch file's " + row_packer + theirs,
                                                "recompiler: the bundle's " + own_nxaot + " (the launch file's " + row_nxaot + theirs}), "... each logged");
    c = compose(g, "", none, false, fake::str(old));
    CHECK(c.argv[0] == row && c.notes.empty(), "the earlier bundle's own launcher starts its own host, nothing logged");
    r = repack_command(g, "balanced", fake::str(old));
    CHECK(r.argv[0] == row_python && r.argv[1] == row_packer && r.argv.back() == row_nxaot && r.notes.empty(), "... and repacks with its own programs");
    fs::remove(own);
    c = compose(g, "", none, false, fake::str(bundle));
    CHECK(c.argv[0] == row && c.notes.empty(), "a launcher's folder without a host of its own keeps the row's, another bundle's or not");
    fake::write(own, "bundle host");

    // The recompiler of the packs (launcher_model.h): the bundle's VERSION names its nxaot's unit key format, the launch file the one that made the packs; a later one in the
    // bundle is a reason for the repack, first in the list, with its own headline; an equal or earlier one, a bundle or a folder without the row, and a game without packs are not
    const std::string version = "nxnative 0123456\nnxaot yes\npython 3.14.10 " + py_rel + "\n";
    auto state = [&](const std::string &made, const std::string &recompiler) {
        fake::write(launch, fake::launch("Epsilon Game", id, build, run, row, false, {{"balanced", build / "aot_v0"}}, repack + made));
        fake::write(bundle / "VERSION", version + recompiler);
        return profile_state(game(none), "balanced", fake::str(bundle));
    };
    CHECK(bundle_recompiler(fake::str(bundle)) == 0 && bundle_recompiler(fake::str(root / "moved/no bundle")) == 0, "no recompiler row, no VERSION: 0");
    ProfileState ps = state("", "");
    CHECK(!ps.better() && !ps.recompiler && ps.pack_files > 0, "a bundle whose VERSION has no recompiler row: nothing said");
    ps = state("", "recompiler 15\n");
    CHECK(bundle_recompiler(fake::str(bundle)) == 15, "VERSION's recompiler row");
    CHECK(ps.better() && ps.recompiler, "packs made before the launch file recorded their recompiler: a better pack is possible");
    CHECK_EQ(ps.reason(), std::string("this bundle's recompiler (15) is newer than the one that made the packs (not recorded): packing again brings its fixes"), "... the reason");
    CHECK_EQ(ps.headline(), std::string("A newer recompiler: a better pack is possible"), "... the headline");
    ps = state("made\trecompiler\t14\n", "recompiler 15\r\n");   // a Windows bundle's CRLF line
    CHECK_EQ(ps.reason(), std::string("this bundle's recompiler (15) is newer than the one that made the packs (14): packing again brings its fixes"), "packs of recompiler 14, the bundle's 15");
    ps = state("made\trecompiler\t15\n", "recompiler 15\n");
    CHECK(!ps.better() && !ps.recompiler, "the same recompiler: nothing said");
    CHECK_EQ(ps.headline(), std::string("Profile recorded: a better pack is possible"), "the headline without the recompiler's reason");
    ps = state("made\trecompiler\t16\n", "recompiler 15\n");
    CHECK(!ps.better(), "packs of a later recompiler than the bundle's (an earlier bundle opened again): nothing said");
    fake::write(launch, fake::launch("Epsilon Game", id, build, run, row, false, {{"balanced", build / "aot_v0"}}, repack + "made\trecompiler\t14\n"));
    CHECK(!profile_state(game(none), "max", fake::str(bundle)).better(), "a mode without packs: nothing to pack better");
    CHECK(!profile_state(game(none), "balanced", fake::str(root / "moved/no bundle")).better(), "a launcher outside a bundle (a build tree): nothing said");
}

static void config(const fs::path &root) {
    MapEnv env;
#if defined(_WIN32)
    env.vars = {{"APPDATA", fake::str(root / "AppData")}};
    CHECK_EQ(config_path(env), fake::str(root / "AppData" / "nxnative" / "launcher.txt"), "%APPDATA%\\nxnative\\launcher.txt");
#else
    env.vars = {{"XDG_CONFIG_HOME", fake::str(root / "xdg")}, {"HOME", "/home/someone"}};
    CHECK_EQ(config_path(env), fake::str(root / "xdg" / "nxnative" / "launcher.txt"), "$XDG_CONFIG_HOME/nxnative/launcher.txt");
    env.vars = {{"XDG_CONFIG_HOME", "relative"}, {"HOME", "/home/someone"}};
    CHECK_EQ(config_path(env), std::string("/home/someone/.config/nxnative/launcher.txt"), "a relative XDG_CONFIG_HOME is ignored: ~/.config");
#endif
    Config c; c.path = fake::str(root / "cfg" / "launcher.txt");
    CHECK(!c.load() && c.get("games", "default") == "default", "no file yet: the defaults");
    c.set("games", "/media/games with spaces"); c.set("pack.0100AAAA00000000", "max"); c.set("show_window", "0"); c.set("games", "/media/games");
    CHECK(c.save(), "saved");
    Config d; d.path = c.path;
    CHECK(d.load() && d.get("games") == "/media/games" && d.get("pack.0100AAAA00000000") == "max" && d.get("show_window") == "0" && d.kv.size() == 3, "read back");
    CHECK(!default_games_dir().empty() && fs::path(fs::u8path(self_path())).parent_path() == fs::u8path(default_games_dir()), "the default games folder is the program's own");
    CHECK_EQ(games_dir(Config()), default_games_dir(), "... unless one was chosen");
}

static void start(const fake::Folder &f) {
    auto g = find_games(fake::str(f.games));
    MapEnv none, off; off.vars = {{"DM_LAUNCHER", "0"}};
    Config c;
    CHECK(decide_start(c, none, g, false).window, "the window by default");
    CHECK(decide_start(c, off, g, false).window, "DM_LAUNCHER=0 but no game played yet: the window");
    c.set("last", "0100AAAA00000000");
    Start s = decide_start(c, off, g, false);
    CHECK(!s.window && s.game == "0100AAAA00000000" && s.why == "DM_LAUNCHER=0", "DM_LAUNCHER=0: the last game at once");
    CHECK(decide_start(c, off, g, true).window, "--window shows it anyway");
    c.set("show_window", "0");
    s = decide_start(c, none, g, false);
    CHECK(!s.window && s.why.find("Show this window at launch") != std::string::npos, "\"Show this window at launch\" off: the last game at once");
    c.set("show_window", "1");
    fake::write(f.a_run / "env.txt", "DM_LAUNCHER=0\n");
    s = decide_start(c, none, g, false);
    CHECK(!s.window && s.why.find("env.txt") != std::string::npos, "DM_LAUNCHER=0 in the game's env.txt");
    fake::write(f.a_run / "env.txt", fake::kChoices);
    c.set("last", "0100CCCC00000000"); c.set("show_window", "0");
    CHECK(decide_start(c, none, g, false).window, "the last game has no complete pack: the window");
}

static void child(const fs::path &root) {
    Command c;
    c.argv = {self_path(), "first arg", "with \"quotes\""};
    c.set = {{"NXL_FAKE_HOST", "1"}, {"NXL_FAKE_STATUS", "7"}, {"DM_X", "a b \"c\" $HOME %PATH%"}, {"DM_SAVE", fake::str(root / "child/run/save")}};
    c.cwd = fake::str(root / "child/run"); c.log = fake::str(root / "child/run/log.txt");
    Child ch; std::string err;
    CHECK(spawn(c, &ch, &err), "started: " + err);
    CHECK_EQ(wait(&ch), 7, "its exit status");
    std::string log = fake::read(root / "child/run/log.txt");
    CHECK(log.find("fake host: argv first arg|with \"quotes\"\n") != std::string::npos, "its arguments as given: " + log);
    CHECK(log.find("fake host: env DM_X=a b \"c\" $HOME %PATH%\n") != std::string::npos, "its environment, nothing expanded");
    CHECK(log.find("fake host: cwd " + fake::str(fs::canonical(root / "child/run"))) != std::string::npos, "its working folder");
    CHECK(fs::is_directory(root / "child/run/save"), "the save folder made first");
    c.set[1].value = "0";
    CHECK(spawn(c, &ch, &err) && wait(&ch) == 0, "a second run");
    CHECK(fake::read(root / "child/run/log.prev.txt") == log, "the previous run's log kept as log.prev.txt");
    auto tail = tail_lines(c.log, 2);
    CHECK(tail.size() == 2 && tail[1].rfind("fake host: env ", 0) == 0, "the log's last lines");
    c.argv[0] = fake::str(root / "no such program");
    CHECK(!spawn(c, &ch, &err) && err.find("cannot start") != std::string::npos, "a program that is not there: " + err);
}

static void set_env(const char *k, const char *v) {   // for the children this process starts (they inherit it)
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

// A pack the host refuses: nxhost-run's status 8 after Play, and before it a pack whose bindings name a take-over of the host
static void refused_packs(const fake::Folder &f, const fs::path &root) {
    CHECK(exit_problem(kRepackStatus).find("Repack") != std::string::npos && exit_problem(kRepackStatus).find("status 8") != std::string::npos,
          "status 8: the packs were refused, Repack makes them again: " + exit_problem(kRepackStatus));
    CHECK(exit_problem(0).empty() && exit_problem(3).empty() && exit_problem(134).empty(), "other statuses: nothing the launcher explains");
    // nxhost-run's other start refusals (aot.h DM_AOT_NO_VULKAN, DM_AOT_NO_FONT): what to install, and no repack offered for them
    CHECK(exit_problem(kNoVulkanStatus).find("Vulkan loader") != std::string::npos && exit_problem(kNoVulkanStatus).find("status 9") != std::string::npos &&
          exit_problem(kNoVulkanStatus).find("Repack") == std::string::npos, "status 9: no Vulkan device, the driver and the loader to install: " + exit_problem(kNoVulkanStatus));
    CHECK(exit_problem(kNoFontStatus).find("DejaVu Sans") != std::string::npos && exit_problem(kNoFontStatus).find("status 10") != std::string::npos,
          "status 10: no font of the system for the shared font: " + exit_problem(kNoFontStatus));
    // before a start: the Vulkan loader that is not there (a name no system has) is a problem the list shows; the real one is what this machine has
    CHECK(vulkan_loader_problem("libnxnative-no-such-vulkan.so.1").find("no Vulkan loader") != std::string::npos, "a loader that does not load: the list's problem");
    const fs::path aot = f.a / "aot_v131072";
    // bindings_problem over a pack folder and a list of take-overs
    fake::write(aot / "main.bind.tsv", "# nxaot bindings: x\n# module main, build ID none, image SHA-256 " + std::string(64, '1') + "\n0x10 0x200 nn_keep\n0x18 0x240 nn_taken\n");
    fake::write(aot / "gone.bind.tsv", "0x10 0x200 nn_taken\n");   // no pack beside it: not read
    CHECK(has_bindings(fake::str(aot)), "a bindings file beside its pack");
    CHECK(bindings_problem(fake::str(aot), {"nn_other"}).empty(), "no bound name taken over: no problem");
    const std::string why = bindings_problem(fake::str(aot), {"nn_taken", "nn_other"});
    CHECK(why.find("the main pack assumed 1 PLT binding") != std::string::npos && why.find("nn_taken") != std::string::npos && why.find("Repack") != std::string::npos,
          "a bound name the host takes over: the module, the name and the repack: " + why);
    CHECK(!has_bindings(fake::str(f.b / "aot_v0")) && bindings_problem(fake::str(f.b / "aot_v0"), {"nn_taken"}).empty(), "a folder without bindings files: none");
    // The host asked (the stand-in, NXL_FAKE_TAKEOVERS): the Problem column and Play's error
    set_env("NXL_FAKE_HOST", "1"); set_env("NXL_FAKE_TAKEOVERS", "nn_x,nn_taken");
    std::string err;
    auto names = host_takeovers(self_path(), &err);
    CHECK(names.size() == 2 && names.count("nn_taken") && err.empty(), "nxhost-run --takeovers read: " + err);
    auto g = find_games(fake::str(f.games));
    CHECK(g[0].problem.rfind("the balanced packs: the main pack assumed 1 PLT binding", 0) == 0 && g[0].repack_cures, "the Problem column names the pack and offers the repack: " + g[0].problem);
    CHECK(compose(g[0], "balanced", MapEnv()).error == g[0].problem, "Play's error says the same");
    CHECK(g[1].problem.empty() && !g[1].repack_cures, "a game without bindings: no problem, the host not asked");
    set_env("NXL_FAKE_HOST", nullptr); set_env("NXL_FAKE_TAKEOVERS", nullptr);
    fake::write(root / "not_a_host.txt", "text\n");
    names = host_takeovers(fake::str(root / "not_a_host.txt"), &err);
    CHECK(names.empty() && !err.empty(), "a program that does not answer --takeovers (an older host): no list, so no problem told before the start: " + err);
    fs::remove(aot / "main.bind.tsv"); fs::remove(aot / "gone.bind.tsv");
    CHECK(find_games(fake::str(f.games))[0].problem.empty(), "the bindings file gone: no problem");
}

int main(int argc, char **argv) {
    if (std::getenv("NXL_FAKE_HOST")) return fake_host(argc, argv);
    fs::path root = fake::tmp_root("t_launcher_model");
    fake::Folder f = fake::make(root, self_path());
    fake::write(root / "scripts" / "build_ipa_from_nsp.py", "# the packer the launch files name\n");   // their packer and nxaot rows name files that are there
    fake::write(root / "nxaot", "nxaot");
    env_file_rules(root);
    games(f);
    settings(f);
    command(f);
    game_profile_rules(f, root);
    round_trip(f);
    second_pack(f);
    moved_bundle(root);
    config(root);
    start(f);
    child(root);
    refused_packs(f, root);
    std::printf("t_launcher_model: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
