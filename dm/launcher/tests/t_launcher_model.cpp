// nxlauncher's model (dm/launcher/launcher_model.cpp) over a games folder this test writes (fake_games.h): the games found and their pack modes, the settings rows and their effective
// values (saved choices, a value the pack does not offer, the pack's own defaults, what the environment fixes), what starts a game (the pack, the host, the environment in the order
// run.sh gives it, DM_SAVED_CHOICES=0, NXHOST), the saved-choice round trip through the player's env.txt, the second pack (the profile folder, "Record a profile while playing",
// whether the recorded profile and JIT misses make a better pack possible against what the launch file says the packs took, the repack's command), a moved bundle (the host, packer
// and nxaot taken from the launcher's folder when the launch file's are gone; NXHOST over both; the problem when none is there), the launcher's own settings,
// DM_LAUNCHER and "Show this window at launch", and a child process with its environment,
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

static std::vector<std::pair<uint64_t, uint64_t>> sampled(uint64_t offsets, uint64_t samples) {   // offsets functions sampled, samples in all
    std::vector<std::pair<uint64_t, uint64_t>> rows;
    for (uint64_t i = 0; i < offsets; i++) rows.push_back({0x1000 + 0x40 * i, samples / offsets + (i == 0 ? samples % offsets : 0)});
    return rows;
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
    CHECK_EQ(sb.reason(), std::string("2 new JIT misses; 2 sampled offsets the packs were made without; the packs were made before the launch file recorded what they took"), "... and why");
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
    CHECK(s.misses.empty() && s.reason() == "2 sampled offsets the packs were made without; the packs were made before the launch file recorded what they took",
          "misses that name no image: the packer would not take them (unknown rows: the profile alone)");
    misses_file(prof, "main", main_sha, {{0x100, "nonlocal", 2}, {0x208, "indirect", 1}});
    // The sample profile: nxaot puts every function holding a sampled offset at tier H, so the offsets are the hot set
    const std::string hundred = yes + "samples\tmain\t100 10000 0000000000000000\nmisses\tmain\t2 " + d_misses + "\n";
    profile_file(prof, "main", main_sha, sampled(105, 12000));
    s = state(hundred);
    CHECK(s.samples["main"].count == 105 && s.samples["main"].samples == 12000 && !s.better(), "5 % more offsets, 20 % more samples: below the thresholds, nothing better");
    profile_file(prof, "main", main_sha, sampled(110, 11000));
    s = state(hundred);
    CHECK(s.better() && s.reason() == "10 new sampled offsets", "10 % more offsets: better: " + s.reason());
    profile_file(prof, "main", main_sha, sampled(3500, 90000));
    s = state(hundred);
    CHECK(s.reason() == "3,400 new sampled offsets", "the count written with thousands separators: " + s.reason());
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
    CHECK(s.better() && s.reason() == "100 sampled offsets the packs were made without", "made profile no (--no-profile; a repack takes it): better: " + s.reason());
    CHECK(!state("made\tprofile\tunused\nmade\tmisses\tyes\nmisses\tmain\t2 " + d_misses + "\n").better(), "made profile unused (AOT_PROFILE off or an nxaot without --profile): nothing better");
    s = state(yes + "misses\tmain\t2 " + d_misses + "\n");
    CHECK(s.better() && s.reason() == "100 sampled offsets the packs were made without", "made profile yes but no samples row of main (no profile of it then): better: " + s.reason());
    const std::string main_same = yes + "samples\tmain\t100 10000 " + s.samples["main"].digest + "\nmisses\tmain\t2 " + d_misses + "\n";
    profile_file(prof, "nro0", nro_sha, {{0x10, 5}});
    s = state(main_same);
    CHECK(s.better() && s.reason() == "1 sampled offset the packs were made without", "an NRO's profile (nro.tsv's image) that its pack did not take: better: " + s.reason());
    profile_file(prof, "nro0", other_sha, {{0x10, 5}});
    profile_file(prof, "other", main_sha, {{0x10, 5}});   // no module of the packs
    fake::write(prof / "main.tsv", "0x100 many\n");          // one that does not parse: the packer packs without it
    s = state(yes + same);
    CHECK(!s.better() && s.samples.empty() && s.files == 4, "a profile of another image, of no module of the packs, or one that does not parse: not taken");
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

// A bundle moved or replaced (launcher_model.h, "The programs a launch file names"): the launch file's host, packer and nxaot are gone, and the launcher takes the bundle's (its own
// folder's hosts/<title ID>/nxhost-run, scripts/build_ipa_from_nsp.py, bin/nxaot) when they are there; NXHOST wins over both; the problem only when none is there
static void moved_bundle(const fs::path &root) {
    const std::string id = "0100EEEE00000000";
    const fs::path games = root / "moved/games", build = games / (id + "_build"), run = games / (id + "-work/run"), old = root / "moved/old bundle", bundle = root / "moved/bundle";
    const std::string row = fake::str(old / "hosts" / id / (std::string("nxhost-run") + kExeSuffix)), own = fake::str(bundle / "hosts" / id / (std::string("nxhost-run") + kExeSuffix));
    const std::string row_packer = fake::str(old / "scripts/build_ipa_from_nsp.py"), row_nxaot = fake::str(old / "bin" / (std::string("nxaot") + kExeSuffix));
    const std::string own_packer = fake::str(bundle / "scripts/build_ipa_from_nsp.py"), own_nxaot = fake::str(bundle / "bin" / (std::string("nxaot") + kExeSuffix));
    fake::packs(build / "aot_v0", false);
    const std::string repack = "path\tpackage\t" + fake::str(games) + "\npath\tpacker\t" + row_packer + "\npath\tpython\t/usr/bin/python3\npath\tnxaot\t" + row_nxaot + "\n";
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
    CHECK(c.error.empty() && c.argv == (std::vector<std::string>{own, fake::str(build / "aot_v0")}), "the bundle's hosts/<title ID>/nxhost-run is started: " + c.error);
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
    CHECK(r.error.empty() && r.argv == (std::vector<std::string>{"/usr/bin/python3", own_packer, fake::str(games), "--platform", kPlatform, "--pack-mode", "balanced", "--nxaot", own_nxaot}),
          "the bundle's packer and nxaot: " + r.error);
    CHECK_EQ(r.notes, (std::vector<std::string>{"packer: the bundle's " + own_packer + " (the launch file's " + row_packer + " is missing)",
                                                "recompiler: the bundle's " + own_nxaot + " (the launch file's " + row_nxaot + " is missing)"}), "... both logged");
    fake::write(row_packer, "# the old packer\n"); fake::write(row_nxaot, "old nxaot");
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK(r.argv[1] == row_packer && r.argv.back() == row_nxaot && r.notes.empty(), "the rows' packer and nxaot when they are there");
    g.launch.nxaot.clear();
    r = repack_command(g, "balanced", fake::str(bundle));
    CHECK(r.error.empty() && std::find(r.argv.begin(), r.argv.end(), "--nxaot") == r.argv.end(), "no nxaot row: no --nxaot (the packer finds its own), the bundle's not taken");
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
    fake::write(root / "scripts" / "build_ipa_from_nsp.py", "# the packer the launch files name\n");   // their packer and nxaot rows name files that are there
    fake::write(root / "nxaot", "nxaot");
    env_file_rules(root);
    games(f);
    settings(f);
    command(f);
    round_trip(f);
    second_pack(f);
    moved_bundle(root);
    config(root);
    start(f);
    child(root);
    std::printf("t_launcher_model: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
