// nxlauncher's model (dm/launcher/launcher_model.cpp) over a games folder this test writes (fake_games.h): the games found and their pack modes, the settings rows and their effective
// values (saved choices, a value the pack does not offer, the pack's own defaults, what the environment fixes), what starts a game (the pack, the host, the environment in the order
// run.sh gives it, DM_SAVED_CHOICES=0, NXHOST), the saved-choice round trip through the player's env.txt, the second pack (the profile folder, "Record a profile while playing",
// a profile newer than the packs, the repack's command), the launcher's own settings, DM_LAUNCHER and "Show this window at launch", and a child process with its environment,
// working folder, log and exit status (this program, which acts as nxhost-run and as the packer with NXL_FAKE_HOST=1).
#include "launcher_model.h"
#include "fake_games.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
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
    CHECK_EQ(cmd.argv, (std::vector<std::string>{a.launch.host, fake::str(f.a / "aot_v131072")}), "nxhost-run and the pack folder");
    CHECK_EQ(cmd.cwd + "|" + cmd.log, fake::str(f.a_run) + "|" + fake::str(f.a_run / "log.txt"), "its working folder and log");
    CHECK_EQ(keys(cmd.set), (std::vector<std::string>{"DM_OPMODE", "DM_PRESENT_UPSCALE", "DM_DEBUG_LOG", "DM_RENDER_SCALE", "DM_MOTION", "DM_ROOT", "DM_ROMFS", "DM_SAVE", "DM_TEMP",
                                                      "DM_MSL_DIR", "DM_LANG", "DM_WINDOW", "DM_AUDIO"}), "env.txt's lines, then the env rows, as run.sh sets them");
    CHECK_EQ(value(cmd.set, "DM_PRESENT_UPSCALE"), std::string("metalfx"), "env.txt's lines go as they are, as run.sh passes them");
    CHECK_EQ(compose(a, "max", none).argv[1], fake::str(f.a / "aot_v131072/max"), "the max pack");
    MapEnv set; set.vars = {{"DM_LANG", "ja"}, {"DM_OPMODE", "1"}, {"NXHOST", "/opt/other/nxhost-run"}};
    Command cs = compose(a, "", set);
    CHECK(value(cs.set, "DM_LANG") == "<unset>" && value(cs.set, "DM_OPMODE") == "<unset>", "what the environment sets stays");
    CHECK_EQ(cs.argv[0], std::string("/opt/other/nxhost-run"), "NXHOST names another nxhost-run");
    MapEnv ignore; ignore.vars = {{"DM_SAVED_CHOICES", "0"}};
    CHECK_EQ(keys(compose(a, "", ignore).set).front(), std::string("DM_MOTION"), "DM_SAVED_CHOICES=0: env.txt is not read");
    CHECK(compose(b, "max", none).error.find("no max pack") != std::string::npos, "a mode that was not packed");
    CHECK(compose(c, "", none).error.find("incomplete") != std::string::npos, "an incomplete pack");
    Config cfg; cfg.set("pack.0100AAAA00000000", "max"); cfg.set("pack.0100BBBB00000000", "max");
    CHECK_EQ(chosen_mode(a, cfg) + " " + chosen_mode(b, cfg) + " " + chosen_mode(c, cfg), std::string("max balanced "), "the player's mode where it was packed, else the first");
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

// The second pack (launcher_model.h): the profile folder, "Record a profile while playing" and DM_AOT_PROFILE in what starts the game, a profile newer than the packs, and the
// packer's command for the repack, run (this program acts as the packer: the launch file's python row names it)
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
    // A profile newer than the packs of one mode and older than the other's
    ProfileState s0 = profile_state(a, "balanced");
    CHECK(s0.folder == a_prof && s0.files == 0 && s0.pack_files == 2 && !s0.better(), "no profile yet: nothing better (the balanced folder's two packs)");
    const fs::path prof = fs::u8path(a_prof);
    fake::write(prof / "main.tsv", "# nxaot profile\n0x100 40\n");
    fake::write(prof / "main.misses.tsv", "# nxaot misses\n0x208 indirect 1\n");
    fake::write(prof / "main.tsv.tmp", "being written\n");   // the host's temporary name: not counted
    const auto t0 = fs::file_time_type::clock::now();
    for (const char *n : {"main", "nro0"}) {
        fs::last_write_time(f.a / "aot_v131072" / (std::string(n) + kPackSuffix), t0 - std::chrono::hours(1));
        fs::last_write_time(f.a / "aot_v131072" / "max" / (std::string(n) + kPackSuffix), t0 + std::chrono::hours(1));
    }
    for (const char *n : {"main.tsv", "main.misses.tsv", "main.tsv.tmp"}) fs::last_write_time(prof / n, t0);
    ProfileState sb = profile_state(a, "balanced"), sm = profile_state(a, "max");
    CHECK(sb.files == 2 && sb.better(), "a profile and misses newer than the balanced packs: \"Profile recorded: a better pack is possible\"");
    CHECK(sm.files == 2 && !sm.better(), "... and older than the max packs: nothing to say for max");
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
    for (const char *n : {"main.tsv", "main.misses.tsv", "main.tsv.tmp"}) fs::remove(prof / n);
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

int main(int argc, char **argv) {
    if (std::getenv("NXL_FAKE_HOST")) return fake_host(argc, argv);
    fs::path root = fake::tmp_root("t_launcher_model");
    fake::Folder f = fake::make(root, self_path());
    env_file_rules(root);
    games(f);
    settings(f);
    command(f);
    round_trip(f);
    second_pack(f);
    config(root);
    start(f);
    child(root);
    std::printf("t_launcher_model: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
