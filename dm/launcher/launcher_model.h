// nxlauncher's model, with no UI (main.cpp draws it with Dear ImGui; tests/t_launcher_model.cpp checks it): the launcher of Windows and Linux, the counterpart of the Mac app's settings
// window (dm/mac/Launcher.swift, dm/mac/launcher.c) and of the iPhone's settings screen (dm/ios/Sources/Launcher.swift) for every game packed on this PC.
//
// A game is a pack of scripts/build_ipa_from_nsp.py --platform linux / windows: <NSP folder>/<id>_build/ with its launch file launch-<platform>.tsv, the one source of what starts it,
// which run.sh / run.cmd read too (build_ipa_from_nsp.py port_launch_file: game, path, pack and env rows), and launcher-<platform>.json, the game's settings (dm/games/settings.py, the
// rows of dm/games/settings.tsv listed for the platform, docs/launcher.schema.json). The games folder holds such <id>_build folders, directly or one level down (an NSP folder per game).
//
// What starts the game, as run.sh composes it: nxhost-run (the path host row; NXHOST in the environment names another) with the pack folder of the chosen mode (a pack row) as its
// argument, in the working folder (path cwd), with the environment: what is already set stays; then the player's env.txt (path choices: KEY=VALUE lines, # comments, read as
// dm/mac/launcher.c reads an env file; not read with DM_SAVED_CHOICES=0 in the environment); then the env rows; each only where nothing set it before. The window writes the player's
// choices into that env.txt before it starts the game: a setting's line only while its value differs from the pack's default (the env row's value, else the list's default), the other
// lines kept. Its output goes to the log row's file (log.txt beside the save; the previous run's kept as log.prev.txt, as the Mac keeps <title ID>.prev.log).
//
// The launcher's own settings (the games folder, "Show this window at launch", the Advanced settings disclosure, the pack mode and last start of each game, the last game played) are
// KEY=VALUE lines in launcher.txt of the OS's config folder: $XDG_CONFIG_HOME/nxnative/ (else ~/.config/nxnative/) on Linux, %APPDATA%\nxnative\ on Windows.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nxl {

#if defined(_WIN32)
inline constexpr const char *kPlatform = "windows", *kPackSuffix = ".aot.dll", *kExeSuffix = ".exe";
#else
inline constexpr const char *kPlatform = "linux", *kPackSuffix = ".aot.so", *kExeSuffix = "";
#endif

// ---- The environment: the process's, or a map (tests) ----
struct Env {
    virtual ~Env() = default;
    virtual bool get(const std::string &key, std::string *value = nullptr) const = 0;   // set (an empty value counts as set, as getenv does)
};
struct ProcessEnv : Env {
    bool get(const std::string &key, std::string *value = nullptr) const override;
};
struct MapEnv : Env {
    std::map<std::string, std::string> vars;
    bool get(const std::string &key, std::string *value = nullptr) const override;
};

struct KV { std::string key, value; };
bool valid_name(const std::string &name);   // a variable a POSIX shell can export (run.sh skips the others)
// An env file's KEY=VALUE lines as dm/mac/launcher.c's env_file reads them: blanks around the line and before the =, # comments, CR line ends; the value as it is after the =
std::vector<KV> read_env_file(const std::string &path);

// ---- The game's settings (launcher-<platform>.json) ----
struct Option { std::string value, label; };
struct Setting {
    std::string env, kind, section, label, note, def, level, needs;   // kind: choice, range or toggle
    std::vector<Option> options;                                      // choice
    double min = 0, max = 1, step = 0.05;                             // range: shown as a percentage
    std::string on = "1", off = "0";                                  // toggle
    bool has_base = false; int docked[2] = {0, 0}, handheld[2] = {0, 0};   // render scale: the 3D views' size at 1.0
    bool basic() const { return level == "basic"; }
    bool accepts(const std::string &v) const;      // a value this pack offers (LauncherSettings.swift Setting.accepts)
    bool same(const std::string &a, const std::string &b) const;   // a range compares as numbers (a slider writes 1.00 for 1.0)
};
bool load_settings(const std::string &path, std::vector<Setting> *out, std::string *err);

// ---- The launch file (launch-<platform>.tsv) ----
struct Pack { std::string mode, folder; bool complete = false; };   // complete: every pack its manifests and nro.tsv need is in the folder
struct LaunchFile {
    std::string title, title_id, version, package, platform, graphics;
    std::string host, cwd, choices, log, settings;
    std::vector<Pack> packs;   // the first is the default
    std::vector<KV> env;
};
bool load_launch(const std::string &path, LaunchFile *out, std::string *err);
bool packs_complete(const std::string &folder);   // per manifest <name>.aot.tsv its <name><kPackSuffix>, and one per NRO nro.tsv lists (not the refused ones)

struct Game {
    std::string dir, launch_path;   // <id>_build and its launch file
    LaunchFile launch;
    std::vector<Setting> settings;
    std::string problem;            // why it cannot start ('' when it can)
    std::string settings_error;     // why its settings list cannot be shown ('' when it can, or when the pack has none)
    std::vector<std::string> modes() const;   // the pack modes with complete packs, the launch file's order
};
// The games under dir: <dir>/<x>_build/ and <dir>/<y>/<x>_build/ with a launch file of this platform, sorted by title then folder
std::vector<Game> find_games(const std::string &dir, const std::string &platform = kPlatform);
const Game *find_game(const std::vector<Game> &games, const std::string &id, std::string *err);   // by title ID (any case) or folder; err when none or several

// ---- The launcher's own settings (launcher.txt) ----
struct Config {
    std::string path;
    std::vector<KV> kv;   // in file order
    std::string get(const std::string &key, const std::string &def = "") const;
    void set(const std::string &key, const std::string &value);
    bool load();          // a missing file is an empty config
    bool save() const;
};
std::string config_path(const Env &env);       // $XDG_CONFIG_HOME/nxnative/launcher.txt, ~/.config/...; %APPDATA%\nxnative\launcher.txt
std::string self_path();                       // this program's file
std::string default_games_dir();               // this program's folder
std::string games_dir(const Config &c);        // the folder chosen in the window, else the default

// ---- One game's settings as the window shows them ----
struct SettingsState {
    std::vector<Setting> settings;
    std::map<std::string, std::string> values, defaults;   // defaults: the pack's (an env row's value, else the list's default)
    std::map<std::string, std::string> fixed;              // set by the environment the launcher was started with: shown, not changed (the value it has)
    std::map<std::string, std::string> refused;            // a saved value this pack does not offer: the default shows, and Play replaces it
    bool ignore_saved = false;                             // DM_SAVED_CHOICES=0: env.txt is not read (and kept)
    std::string cache_value, cache_default;                // DM_VK_PSO_CACHE as it applies and as the pack sets it: '' (the default folder), off, or a folder
    bool cache_fixed = false;
    bool cache_on() const { return cache_value != "off"; }
    std::vector<std::string> sections(bool basic) const;
    std::vector<const Setting *> rows(const std::string &section, bool basic) const;
    bool visible(const Setting &s) const;                  // needs=<VAR>=<value>: drawn only while that setting has that value
    bool changed() const;
    void reset();                                          // every setting back to the pack's default (the fixed ones stay)
    std::string range_label(const Setting &s, double v) const;   // "75 %", or "1200 × 675 (75 %)" for the render scale in the chosen console mode
};
SettingsState load_state(const Game &g, const Env &env);
std::string cache_folder(const Game &g, const SettingsState &st, const Env &env);   // where the Vulkan backend keeps its pipeline cache (vk_cache.c folder())
// The window's choices into the player's env.txt: a setting's line only while it differs from the pack's default, the cache switch's off, every other line kept
bool save_choices(const Game &g, const SettingsState &st, std::string *err);

// ---- What starts the game ----
struct Command {
    std::vector<std::string> argv;
    std::vector<KV> set;   // the variables the launcher sets, in the order it sets them
    std::string cwd, log, mode;
    std::string error;     // why it cannot start ('' when it can)
};
std::string chosen_mode(const Game &g, const Config &c);   // the player's pack mode when it has complete packs, else the first that has
Command compose(const Game &g, const std::string &mode, const Env &env);

// ---- The child process ----
struct Child {
    int64_t pid = 0;
    void *handle = nullptr;   // Windows: the process handle
    bool running = false;
    int status = -1;          // its exit status once it ended (128 + the signal for a signal, as dm/mac/launcher.c reports it)
};
bool spawn(const Command &cmd, Child *child, std::string *err);   // its output to cmd.log (the previous run's kept as log.prev.txt); the folders made first
bool poll(Child *child);    // true once it has ended (child->status)
int wait(Child *child);
std::vector<std::string> tail_lines(const std::string &path, size_t n);

// ---- Whether the window shows ----
struct Start { bool window = true; std::string game, why; };
// The window shows unless "Show this window at launch" is off or DM_LAUNCHER=0 (the environment, or the last game's env.txt) and a game was played before: then that game starts at once
// with the player's choices. force: --window, the counterpart of holding Option when a Mac app opens
Start decide_start(const Config &c, const Env &env, const std::vector<Game> &games, bool force);

std::string now_text(int64_t unix_seconds);   // "2026-11-01 14:05" in local time, "never" for 0
int64_t unix_now();

}   // namespace nxl
