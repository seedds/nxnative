// nxlauncher's model, with no UI (main.cpp draws it with Dear ImGui; tests/t_launcher_model.cpp checks it): the launcher of Windows and Linux, the counterpart of the Mac app's settings
// window (dm/mac/Launcher.swift, dm/mac/launcher.c) and of the iPhone's settings screen (dm/ios/Sources/Launcher.swift) for every game packed on this PC.
//
// A game is a pack of scripts/build_ipa_from_nsp.py --platform linux / windows: <NSP folder>/<id>_build/ with its launch file launch-<platform>.tsv, the one source of what starts it,
// which run.sh / run.cmd read too (build_ipa_from_nsp.py port_launch_file: game, path, pack, env, made, samples and misses rows), and launcher-<platform>.json, the game's settings (dm/games/settings.py, the
// rows of dm/games/settings.tsv listed for the platform, docs/launcher.schema.json). The games folder holds such <id>_build folders, directly or one level down (an NSP folder per game).
//
// What starts the game, as run.sh composes it: nxhost-run (the path host row; NXHOST in the environment names another; the bundle's when the row's is gone: find_host) with the pack folder of the chosen mode (a pack row) as its
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
// A module's sample profile or JIT misses, summed up as build_ipa_from_nsp.py made_rows writes them and profile_state reads the profile folder: count, the offsets with samples (the
// functions nxaot puts at tier H) or the miss addresses; samples, the profile's samples (0 for misses); digest, FNV-1a 64 (16 hex digits) of the offsets in hexadecimal one per line
// ascending, or of the '<address in hexadecimal> <reason>' pairs one per line sorted
struct Recorded { uint64_t count = 0, samples = 0; std::string digest; };
struct LaunchFile {
    std::string title, title_id, version, package, platform, graphics;
    std::string host, cwd, choices, log, settings;
    // What a session records and what packs the game again (path rows of build_ipa_from_nsp.py port_launch_file; empty in a launch file written before them): profile, the folder
    // DM_AOT_PROFILE writes the sample profiles and JIT misses to; package_dir, the NSP folder (the package row); packer and python, the script and its Python; keys, prod.keys's
    // path; nxaot, the recompiler the pack was made with
    std::string profile, package_dir, packer, python, keys, nxaot;
    // What the packs were made with (made, samples and misses rows of build_ipa_from_nsp.py made_rows; the same for every pack row): made_profile yes (the sample profiles taken), no
    // (--no-profile: a repack takes them) or unused (the packer's AOT_PROFILE setting or its nxaot does not take them, a repack neither); made_misses yes or unused; per module what its
    // packs took. All empty in a launch file written before those rows: then what the packs took is unknown
    std::string made_profile, made_misses;
    std::map<std::string, Recorded> made_samples, made_miss;
    std::vector<Pack> packs;   // the first is the default
    std::vector<KV> env;
};
bool load_launch(const std::string &path, LaunchFile *out, std::string *err);
bool packs_complete(const std::string &folder);   // per manifest <name>.aot.tsv its <name><kPackSuffix>, and one per NRO nro.tsv lists (not the refused ones)

// ---- The programs a launch file names, found again when they are gone ----
// The launch file's path rows are absolute (build_ipa_from_nsp.py port_launch_file), so a bundle moved or replaced by a newer one elsewhere (docs/development.md, "The port's bundles")
// leaves them naming programs that are gone. The launcher then takes the same program from its own folder, which a bundle lays out as nxlauncher beside hosts/<title ID>/nxhost-run,
// bin/nxaot and scripts/build_ipa_from_nsp.py (the script pack.sh / pack.cmd run). The host's rule is run.sh's and run.cmd's too (they look for hosts/<title ID>/ in their own
// folder and the folders above it, since they live in the NSP folder, not in the bundle):
//   host    NXHOST from the environment if set; else the host row if that file exists; else <bundle>/hosts/<title ID>/nxhost-run[.exe] if it exists; else the problem "the host
//           <row> is missing: pack the game again or set NXHOST"
//   nxaot   the nxaot row if that file exists (no row: no --nxaot, the packer finds its own); else <bundle>/bin/nxaot[.exe] if it exists; else a problem
//   packer  the packer row if that file exists; else <bundle>/scripts/build_ipa_from_nsp.py if it exists; else a problem
std::string bundle_dir();   // the launcher's own folder (this program's; '.' when it cannot be told)
struct Found {
    std::string path;      // what to run ('' with a problem, and for an nxaot row the launch file does not have)
    std::string note;      // why it is not the launch file's ('' when it is): the launcher logs it
    std::string problem;   // why nothing can run ('' when something can)
};
Found find_host(const LaunchFile &l, const Env &env, const std::string &bundle);
Found find_nxaot(const LaunchFile &l, const std::string &bundle);
Found find_packer(const LaunchFile &l, const std::string &bundle);

struct Game {
    std::string dir, launch_path;   // <id>_build and its launch file
    LaunchFile launch;
    std::vector<Setting> settings;
    std::string problem;            // why it cannot start ('' when it can)
    std::string settings_error;     // why its settings list cannot be shown ('' when it can, or when the pack has none)
    std::vector<std::string> modes() const;   // the pack modes with complete packs, the launch file's order
};
// The games under dir: <dir>/<x>_build/ and <dir>/<y>/<x>_build/ with a launch file of this platform, sorted by title then folder. A game whose host find_host finds nowhere has that
// as its problem (bundle and env as find_host takes them)
std::vector<Game> find_games(const std::string &dir, const std::string &platform = kPlatform, const std::string &bundle = bundle_dir(), const Env &env = ProcessEnv());
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
    std::vector<std::string> notes;   // what the launcher logs: a program taken from the bundle instead of the launch file's (find_host, find_nxaot, find_packer)
    std::vector<KV> set;   // the variables the launcher sets, in the order it sets them
    std::string cwd, log, mode;
    std::string error;     // why it cannot start ('' when it can)
};
std::string chosen_mode(const Game &g, const Config &c);   // the player's pack mode when it has complete packs, else the first that has
// record: "Record a profile while playing" (record_profile): DM_AOT_PROFILE set to the game's profile folder after everything else, so the environment, env.txt or an env row
// that sets it wins (the host writes the sample profile and the JIT misses there at exit: dm/host/aot.h)
// bundle: where find_host looks when the launch file's host is gone (the launcher's own folder)
Command compose(const Game &g, const std::string &mode, const Env &env, bool record = false, const std::string &bundle = bundle_dir());

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

// ---- The second pack: what a session teaches the next pack (docs/settings.md, "On Windows and Linux"; dm/tools/aot/README.md, "Tiers by profile" and "JIT misses") ----
// Play records by default: nxhost-run gets DM_AOT_PROFILE=<the profile folder> (compose's record) unless the player turned "Record a profile while playing" off (record.<title ID>=0
// in launcher.txt) or the environment sets it. At exit the host writes <module>.tsv (the functions it sampled this session) and <module>.misses.tsv (the addresses its JIT
// fallback ran, merged over the sessions of one image) there. The list says "Profile recorded: a better pack is possible", and "Repack with profile" runs the packer in the background,
// when the folder holds what a pack made now would take and the packs did not: profile_state compares the files, as the packer would take them (of a module of the mode's packs,
// recorded on its image: the manifest's or nro.tsv's SHA-256), with the launch file's made rows. A pack made now differs when, for a module:
//   misses   the misses file has addresses the packs did not take (more of them, or another digest): each is an entry into the JIT fallback (a stutter) that the next pack compiles
//   profile  the packs took no profile of the module (made profile no, or no samples row) and the file has samples; or the offsets with samples grew by at least 10 % (each is a
//            function at tier H instead of C: nxaot's rule has no share or count threshold, any sampled function is hot, so the offsets are the hot set; the profile is this session's
//            only, and the 10 % keeps the cold tail that comes and goes between sessions of the same play from offering a repack after every session); or another hot set (digest)
//            from a session with at least 25 % more samples (CPU time measured) and no fewer offsets. A profile with fewer offsets is a shorter session's: a pack from it would
//            put functions back at tier C, so it is never "better"
// A launch file without made rows (packs made before them) is compared as packs made without a profile or misses: any file the packer would take makes the line show once; the
// repack writes the rows, and from then on the comparison is exact
std::string profile_folder(const Game &g);                  // the profile row, else <id>-work/profile beside the working folder (a launch file before that row)
bool record_profile(const Config &c, const Game &g);        // "Record a profile while playing" (on unless record.<title ID>=0)
inline constexpr int kMoreOffsetsPercent = 10, kMoreSamplesPercent = 25;   // the profile's thresholds (above)
struct ProfileState {
    std::string folder;
    int files = 0;         // profile and misses files there (<module>.tsv, <module>.misses.tsv)
    int pack_files = 0;    // packs (<name><kPackSuffix>) in the mode's folder
    bool known = false;    // the launch file says what the packs took (its made rows)
    std::map<std::string, Recorded> samples, misses;   // per module of the packs, its files there that the packer would take (they parse and were recorded on its image)
    std::vector<std::string> reasons;                  // why a pack made now would be better ("12 new JIT misses", "3,400 new sampled offsets", ...); empty when it would not be
    bool better() const { return !reasons.empty(); }   // "Profile recorded: a better pack is possible"
    std::string reason() const;                        // the reasons joined with "; "
};
ProfileState profile_state(const Game &g, const std::string &mode);
// The repack ("Repack with profile", nxlauncher --repack): <python> <packer> <package_dir> --platform <this system> --pack-mode <mode> [-k <keys>] [--nxaot <nxaot>], in the NSP
// folder, its output in repack.txt beside the game's log (PYTHONUNBUFFERED=1: the log pane follows it as it goes); error when the launch file names no package, packer or python
// (a pack made before those rows: pack the game once more by hand, which writes them). The packer and nxaot as find_packer and find_nxaot find them in bundle
Command repack_command(const Game &g, const std::string &mode, const std::string &bundle = bundle_dir());

}   // namespace nxl
