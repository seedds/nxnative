// nxlauncher: the launcher of Windows and Linux (Phase 6 of the port, docs/design/migration-status.md: Dear ImGui over SDL3's renderer, so it starts without Vulkan on any PC).
// One window: the games packed on this PC on the left (scripts/build_ipa_from_nsp.py --platform linux / windows), the chosen game's settings on the right as the Mac app's settings
// window shows them (dm/mac/Launcher.swift: the basic settings first, the others under Advanced settings, Reset to tested defaults), its pack mode and the Vulkan pipeline cache's
// switch, the log of the running game below, "Show this window at launch", Quit and Play. launcher_model.h has what it reads, writes and starts. Plain ImGui style and font; keyboard
// and controller navigation (ImGuiConfigFlags_NavEnableKeyboard / NavEnableGamepad).
// The second pack (launcher_model.h, "The second pack"): Play records a sample profile and the JIT misses unless "Record a profile while playing" is off; once they hold what a pack
// made now would take and the game's packs did not (new JIT misses, more sampled functions: the launch file's made rows say what the packs took), the list says so, with the reason in
// a tooltip, the log and --list, and "Repack with profile" runs the packer in the background, its output in the log pane, Play disabled for that game until it ends.
// What a player does besides (launcher_model.h has the rules): Add game... (the system's file dialog, then the packer in the background with its stages, a Cancel, and its result
// in plain words), Browse... beside the games folder (the system's folder dialog), Stop while a game runs (the host's graceful exit; Force stop after kStopBoundMs), and the
// chosen game's Saves tab (Back up saves, the backups with Restore..., Open saves folder). The dialogs and the file manager go through launcher_model.h's Dialogs: SDL3's here.
// Without a window (tests and scripts): --list <games folder> (TSV), --command <id> (what it would start), --play <id>, --repack <id> (the packer, waited for); --frames <n> draws n
// frames and exits (SDL_VIDEODRIVER=dummy), --shot <file.bmp> keeps the last of them as a picture. The window driven without a person (the tests, the window shots):
// --answer <path> answers the next dialog in place of the system's (AnswerDialogs), --press <step> runs the steps in order, one per frame at most: a button's or a tab's
// label (pressed once it is drawn and enabled), "shot:<file.bmp>" (that frame as a picture), "until:<text>" (waits until the status line, the log pane, the Saves tab's
// message or Add game's result shows the text),
// each with "@<frame>" to wait for that frame; --exit-idle ends the window once the steps are done and nothing it started runs.
#include "launcher_model.h"
#include "imgui.h"
#include "imgui_internal.h"   // ImGui::GetItemFlags: --press presses only an enabled button
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

using namespace nxl;

static const char *kUsage =
    "usage: nxlauncher [--games <folder>] [--window] [--frames <n> [--shot <file.bmp>]]   the window (or the last game at once: \"Show this window at launch\" off, DM_LAUNCHER=0);\n"
    "                                                                 --frames: draw n frames and exit, --shot: the last one as a BMP\n"
    "                  [--answer <path>]... [--press <step>]... [--exit-idle]   the window driven without a person: a dialog's answer; a label to press, shot:<file.bmp>,\n"
    "                                                                 until:<text>, each [@<frame>]; end once the steps are done and nothing runs\n"
    "       nxlauncher --list <games folder>                         the games packed there, as TSV (better_pack: why a repack with the profile would be better, or -)\n"
    "       nxlauncher [--games <folder>] [--pack balanced|max] --command <title ID or folder>   what it would start: KEY=value lines, a blank line, the argv\n"
    "       nxlauncher [--games <folder>] [--pack balanced|max] --play <title ID or folder>      start it without the window and wait (its output in its log.txt)\n"
    "       nxlauncher [--games <folder>] [--pack balanced|max] --repack <title ID or folder>    pack it again with what its sessions recorded (the packer's output in repack.txt)\n";

#if defined(_WIN32)
// A GUI program's standard handles are invalid unless the caller redirected them: in a console, --list and --command print there
static void attach_console() {
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *f;
        freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr);
    }
    // Binary, as on Linux: --list's TSV and --command's lines end in "\n" in a pipe or a file the caller reads, not "\r\n" (a console shows them the same)
    if (_fileno(stdout) >= 0) _setmode(_fileno(stdout), _O_BINARY);
    if (_fileno(stderr) >= 0) _setmode(_fileno(stderr), _O_BINARY);
}
#else
static void attach_console() {}
#endif

static std::string played(const Config &c, const Game &g) { return now_text(std::atoll(c.get("played." + g.launch.title_id, "0").c_str())); }

// A program taken from the bundle because the launch file's is gone (find_host, find_python, find_nxaot, find_packer): said in the log; the Problem column says only that none is there
static void say_notes(const Command &c) {
    for (const auto &n : c.notes) std::fprintf(stderr, "nxlauncher: %s\n", n.c_str());
}

static std::string join(const std::vector<std::string> &v, const char *sep) {
    std::string s;
    for (const auto &x : v) { if (!s.empty()) s += sep; s += x; }
    return s;
}

// "a better pack is possible": why, for the chosen mode's packs ('' when it is not)
static std::string better_reason(const Config &cfg, const Game &g) {
    if (!g.problem.empty()) return "";
    return profile_state(g, chosen_mode(g, cfg)).reason();
}

// No Vulkan loader on this PC: every game's problem in the list (vulkan_loader_problem), as a game started now would stop with status 9 at once
static std::vector<Game> listed(const std::string &dir, const Config *cfg = nullptr) {
    auto games = cfg ? find_all_games(dir, *cfg) : find_games(dir);   // the window: also the games added outside the games folder (launcher.txt nsp_folder)
    const std::string p = vulkan_loader_problem();
    if (!p.empty()) for (auto &g : games) if (g.problem.empty()) g.problem = p;
    return games;
}

static int list_games(const Config &cfg, const std::string &dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::u8path(dir), ec)) { std::fprintf(stderr, "[RED] nxlauncher: no games folder %s\n", dir.c_str()); return 1; }
    auto games = listed(dir);
    std::printf("# title_id\ttitle\tversion\tpacks\tlast_played\tfolder\tproblem\tbetter_pack\n");
    for (const auto &g : games) {
        auto modes = g.modes();
        const std::string why = better_reason(cfg, g);
        std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", g.launch.title_id.empty() ? "-" : g.launch.title_id.c_str(), g.launch.title.c_str(), g.launch.version.empty() ? "-" : g.launch.version.c_str(),
                    modes.empty() ? "none" : join(modes, ",").c_str(), played(cfg, g).c_str(), g.dir.c_str(), g.problem.empty() ? "-" : g.problem.c_str(), why.empty() ? "-" : why.c_str());
    }
    std::fprintf(stderr, "nxlauncher: %zu game%s in %s\n", games.size(), games.size() == 1 ? "" : "s", dir.c_str());
    return 0;
}

static int print_command(const Config &cfg, const Env &env, const Game &g, const std::string &mode_in) {
    std::string mode = mode_in.empty() ? chosen_mode(g, cfg) : mode_in;
    Command c = compose(g, mode, env, record_profile(cfg, g));
    if (!c.error.empty()) { std::fprintf(stderr, "[RED] nxlauncher: %s\n", c.error.c_str()); return 1; }
    say_notes(c);
    std::printf("# %s (%s), the %s pack\n# cwd %s\n# log %s\n", g.launch.title.c_str(), g.launch.title_id.c_str(), c.mode.c_str(), c.cwd.c_str(), c.log.c_str());
    for (const auto &e : c.set) std::printf("%s=%s\n", e.key.c_str(), e.value.c_str());
    std::printf("\n");
    for (const auto &a : c.argv) std::printf("%s\n", a.c_str());
    std::fflush(stdout);
    const ProfileState better = profile_state(g, c.mode);
    if (better.better()) std::fprintf(stderr, "nxlauncher: %s: %s: %s\n", g.launch.title.c_str(), better.headline().c_str(), better.reason().c_str());
    return 0;
}

static void remember_start(Config &cfg, const Game &g, const std::string &mode) {
    cfg.set("last", g.launch.title_id);
    cfg.set("played." + g.launch.title_id, std::to_string(unix_now()));
    if (!mode.empty()) cfg.set("pack." + g.launch.title_id, mode);
    if (!cfg.save()) std::fprintf(stderr, "[RED] nxlauncher: cannot write its settings %s\n", cfg.path.c_str());
}

static int play_now(Config &cfg, const Env &env, const Game &g, const std::string &mode_in) {
    std::string mode = mode_in.empty() ? chosen_mode(g, cfg) : mode_in, err;
    Command c = compose(g, mode, env, record_profile(cfg, g));
    if (!c.error.empty()) { std::fprintf(stderr, "[RED] nxlauncher: %s\n", c.error.c_str()); return 1; }
    say_notes(c);
    remember_start(cfg, g, c.mode);
    Child ch;
    if (!spawn(c, &ch, &err)) { std::fprintf(stderr, "[RED] nxlauncher: %s\n", err.c_str()); return 1; }
    std::fprintf(stderr, "nxlauncher: %s (%s), the %s pack: nxhost-run started, its output in %s\n", g.launch.title.c_str(), g.launch.title_id.c_str(), c.mode.c_str(), c.log.c_str());
    int st = wait(&ch);
    std::fprintf(stderr, "nxlauncher: nxhost-run exited with status %d\n", st);
    const std::string why = exit_problem(st);
    if (!why.empty()) std::fprintf(stderr, "[RED] nxlauncher: %s: %s%s%s%s\n", g.launch.title.c_str(), why.c_str(), st == kRepackStatus ? " (nxlauncher --repack " : "",
                                   st == kRepackStatus ? g.launch.title_id.c_str() : "", st == kRepackStatus ? ")" : "");   // a repack cures only a refused pack
    return st;
}

// "a better pack is possible": a line for the log, '' when there is none to say
static std::string profile_line(const Game &g, const ProfileState &p) {
    if (!p.better()) return "";
    return g.launch.title + ": " + p.headline() + " (" + std::to_string(p.files) + " files in " + p.folder + "; Repack with profile): " + p.reason();
}

// --repack: the packer for the game's mode, waited for (its output in repack.txt)
static int repack_now(const Config &cfg, const Game &g, const std::string &mode_in) {
    std::string mode = mode_in.empty() ? chosen_mode(g, cfg) : mode_in, err;
    Command c = repack_command(g, mode);
    if (!c.error.empty()) { std::fprintf(stderr, "[RED] nxlauncher: %s\n", c.error.c_str()); return 1; }
    say_notes(c);
    Child ch;
    if (!spawn(c, &ch, &err)) { std::fprintf(stderr, "[RED] nxlauncher: %s\n", err.c_str()); return 1; }
    std::fprintf(stderr, "nxlauncher: %s (%s): the packer started for the %s pack, its output in %s\n", g.launch.title.c_str(), g.launch.title_id.c_str(), c.mode.c_str(), c.log.c_str());
    int st = wait(&ch);
    std::fprintf(stderr, "nxlauncher: the packer exited with status %d\n", st);
    return st;
}

// ---- The window ----
// SDL3's dialogs and file manager (launcher_model.h Dialogs): the Windows dialogs, the XDG desktop portal or zenity on Linux. SDL calls back on a thread of its own or on the
// main thread's next event loop; the request (its filters, its properties) lives until then
struct SdlDialogs : Dialogs {
    SDL_Window *win = nullptr;
    struct Request {
        Done done;
        std::vector<Filter> filters;            // the strings SDL's filters point into
        std::vector<SDL_DialogFileFilter> sdl;
        SDL_PropertiesID props = 0;
    };
    static void SDLCALL answered(void *ud, const char *const *files, int) {
        std::unique_ptr<Request> r(static_cast<Request *>(ud));
        if (!files) std::fprintf(stderr, "[RED] nxlauncher: the system's dialog failed: %s\n", SDL_GetError());
        r->done(files && files[0] ? files[0] : "");   // no file: cancelled (or failed)
        if (r->props) SDL_DestroyProperties(r->props);
    }
    void show(SDL_FileDialogType type, const std::string &title, const std::vector<Filter> &filters, const std::string &start, Done done) {
        auto *r = new Request{std::move(done), filters, {}, SDL_CreateProperties()};
        for (const auto &f : r->filters) r->sdl.push_back({f.name.c_str(), f.pattern.c_str()});
        SDL_SetStringProperty(r->props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
        if (!start.empty()) SDL_SetStringProperty(r->props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, start.c_str());
        if (win) SDL_SetPointerProperty(r->props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, win);
        if (!r->sdl.empty()) {
            SDL_SetPointerProperty(r->props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, r->sdl.data());
            SDL_SetNumberProperty(r->props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, (Sint64)r->sdl.size());
        }
        SDL_ShowFileDialogWithProperties(type, answered, r, r->props);
    }
    void open_file(const std::string &title, const std::vector<Filter> &filters, const std::string &start, Done done) override {
        show(SDL_FILEDIALOG_OPENFILE, title, filters, start, std::move(done));
    }
    void open_folder(const std::string &title, const std::string &start, Done done) override { show(SDL_FILEDIALOG_OPENFOLDER, title, {}, start, std::move(done)); }
    bool show_folder(const std::string &folder, std::string *err) override {   // SDL_OpenURL: xdg-open on Linux, ShellExecute on Windows
        std::string path = folder, url = "file://";
        for (auto &ch : path) if (ch == '\\') ch = '/';
        if (path.empty() || path[0] != '/') url += '/';   // C:/... -> file:///C:/...
        for (unsigned char ch : path) {
            if (std::isalnum(ch) || std::strchr("/-._~:", ch) || ch >= 0x80) url += (char)ch;   // UTF-8 bytes as they are: the file manager takes an IRI
            else { char hex[4]; std::snprintf(hex, sizeof hex, "%%%02X", ch); url += hex; }
        }
        if (SDL_OpenURL(url.c_str())) return true;
        if (err) *err = SDL_GetError();
        return false;
    }
};

// A step of --press (the window driven without a person)
struct Step { std::string kind, arg; long frame = 0; };   // kind: press, shot, until

struct App {
    ProcessEnv env;
    Config cfg;
    std::string dir;
    char dir_buf[2048] = {};
    std::vector<Game> games;
    std::string selected;                          // the selected game's title ID
    std::map<std::string, SettingsState> states;   // each game's settings as the window shows them (kept while another game is selected)
    Child child; std::string running, running_id, status, log;
    std::string refused;                           // the title ID of the game whose last start nxhost-run refused for its packs (kRepackStatus): the Repack action shows
    Child repack; std::string repacking, repack_title;   // "Repack with profile": the packer, and the title ID of the game it packs while it runs
    std::map<std::string, std::pair<std::string, ProfileState>> profiles;   // each game's profile against the packs of a mode (scan: at the start, the game's exit, a repack)
    std::vector<std::string> lines; Uint64 tail_at = 0; size_t seen = 0;
    bool quit = false;
    // Stop: when it was asked (SDL ticks), whether the request reached the host, whether Force stop ended it
    bool stop_asked = false, stop_sent = false, forced = false; Uint64 stop_at = 0; std::string stop_note;
    // The dialogs, and their answers for the next frame (a callback may come on another thread)
    Dialogs *dialogs = nullptr;
    struct Mailbox { std::mutex mu; std::vector<std::pair<std::string, std::string>> answers; };   // (what was asked, the path; '' cancelled)
    std::shared_ptr<Mailbox> box = std::make_shared<Mailbox>();   // shared with each callback: one that comes after the window closed posts into it harmlessly
    // Add game: the chosen file and keys, the packer while it runs, what its log says, and its result
    struct Adding {
        bool open = false, ok = false;
        std::string file, keys, result;
        std::vector<std::string> looked;      // where find_keys looked when it found none
        PackTools tools;
        Child child; std::string log; int64_t since = 0;
        PackView view; Uint64 read_at = 0;
        bool cancelled = false; Uint64 cancel_at = 0;
    } add;
    // Saves: the chosen game's, read again after an action or a second
    std::string saves_of; Saves saves; std::vector<Backup> backups; Uint64 saves_at = 0;
    std::string saves_msg, confirm;   // the last action's message; the backup Restore... asks about
    // --press steps
    std::vector<Step> steps; size_t step = 0; long frame = 0; long shot_at = -1;   // shot_at: the frame a shot step's picture is taken on
    bool exit_idle = false;
    Dialogs::Done answer(const std::string &what) {   // the callback of a dialog: its answer, for the next frame
        auto b = box;
        return [b, what](const std::string &path) { std::lock_guard<std::mutex> l(b->mu); b->answers.push_back({what, path}); };
    }
    bool pressed(const std::string &label) {   // the current step presses this label now
        if (step >= steps.size() || steps[step].kind != "press" || steps[step].arg != label || frame < steps[step].frame) return false;
        std::fprintf(stderr, "nxlauncher: --press %s (frame %ld)\n", label.c_str(), frame);
        step++;
        return true;
    }
};

static bool button(App &a, const char *label) {   // ImGui::Button, or --press's step while it is enabled
    bool clicked = ImGui::Button(label);
    if (!(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) && a.pressed(label)) clicked = true;
    return clicked;
}

static void scan(App &a) {
    a.games = listed(a.dir, &a.cfg);
    if (!a.games.empty() && !find_game(a.games, a.selected, nullptr)) {
        const Game *last = find_game(a.games, a.cfg.get("last"), nullptr);
        a.selected = last ? last->launch.title_id : a.games[0].launch.title_id;
    }
    std::snprintf(a.dir_buf, sizeof a.dir_buf, "%s", a.dir.c_str());
    a.profiles.clear();
    a.saves_at = 0;
    for (const auto &g : a.games) {
        const std::string mode = chosen_mode(g, a.cfg);
        ProfileState p = profile_state(g, mode);
        std::string line = profile_line(g, p);
        if (!line.empty()) std::fprintf(stderr, "nxlauncher: %s\n", line.c_str());
        a.profiles[g.launch.title_id] = {mode, p};
    }
}

// A game's profile against the packs of its chosen mode: the scan's, looked at again when the player picked another mode
static const ProfileState &profile_of(App &a, const Game &g) {
    const std::string mode = chosen_mode(g, a.cfg);
    auto &e = a.profiles[g.launch.title_id];
    if (e.first != mode || e.second.folder.empty()) e = {mode, profile_state(g, mode)};
    return e.second;
}

static bool repacking(const App &a, const Game &g) { return a.repack.running && a.repacking == g.launch.title_id; }
static bool busy(const App &a) { return a.child.running || a.repack.running || a.add.child.running; }   // a game, a repack or Add game runs

// "Repack with profile": the packer in the background for the game's chosen mode, its output in the log pane; Play waits for it (repacking)
static void start_repack(App &a, const Game &g) {
    if (a.repack.running || a.child.running || a.add.child.running) return;
    std::string err, mode = chosen_mode(g, a.cfg);
    Command c = repack_command(g, mode);
    if (!c.error.empty()) { a.status = "[RED] " + c.error; return; }
    say_notes(c);
    if (!spawn(c, &a.repack, &err)) { a.status = "[RED] " + err; return; }
    a.repacking = g.launch.title_id; a.repack_title = g.launch.title;
    a.log = c.log; a.lines.clear(); a.seen = 0; a.tail_at = 0;
    a.status = "Repacking " + g.launch.title + " with its profile (the " + c.mode + " pack)";
    std::fprintf(stderr, "nxlauncher: %s (%s): the packer started for the %s pack, its output in %s\n", g.launch.title.c_str(), g.launch.title_id.c_str(), c.mode.c_str(), c.log.c_str());
}

// The game's profile: the switch, what was recorded, and the repack
static void profile_section(App &a, const Game &g) {
    ImGui::SeparatorText("Profile");
    std::string v;
    const bool fixed = a.env.get("DM_AOT_PROFILE", &v);
    bool record = record_profile(a.cfg, g);
    ImGui::BeginDisabled(fixed);
    if (ImGui::Checkbox("Record a profile while playing", &record)) { a.cfg.set("record." + g.launch.title_id, record ? "1" : "0"); a.cfg.save(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The game records which of its functions run most and which code its pack lacked (DM_AOT_PROFILE); the next pack compiles those better.");
    const ProfileState &p = profile_of(a, g);
    ImGui::PushTextWrapPos(0);
    if (fixed) ImGui::TextDisabled("Set at launch (DM_AOT_PROFILE=%s)", v.c_str());
    if ((g.repack_cures || a.refused == g.launch.title_id) && !repacking(a, g)) {   // a pack this host refuses: the repack, not a bare failure
        ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", g.repack_cures ? "This host would refuse the game's packs: they were made for another host or bundle."
                                                                             : "The host refused the game's packs at the last start: they were made for another host or bundle.");
        ImGui::BeginDisabled(busy(a));
        if (button(a, "Repack")) { a.refused.clear(); start_repack(a, g); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("the %s pack, in the background", chosen_mode(g, a.cfg).c_str());
    } else if (p.better()) {
        ImGui::TextColored(ImVec4(.95f, .8f, .35f, 1.f), "%s", p.headline().c_str());
        ImGui::TextDisabled("%s.", p.reason().c_str());
        ImGui::BeginDisabled(busy(a));
        if (button(a, "Repack with profile")) start_repack(a, g);
        ImGui::EndDisabled();
        ImGui::SameLine();
        const std::string when = repacking(a, g) ? "repacking..." : a.child.running ? "after the game ends" : "the " + chosen_mode(g, a.cfg) + " pack, in the background";
        ImGui::TextDisabled("%s", when.c_str());
    } else if (repacking(a, g)) {
        ImGui::TextDisabled("Repacking with the profile...");
    } else {
        ImGui::TextDisabled(p.files ? "The packs already took what the recorded profile holds, or it adds too little to change them (%s)." : "No profile recorded yet (%s).", p.folder.c_str());
    }
    ImGui::PopTextWrapPos();
}

static Game *current(App &a) {
    for (auto &g : a.games) if (g.launch.title_id == a.selected) return &g;
    return nullptr;
}

static SettingsState &state_of(App &a, const Game &g) {
    auto it = a.states.find(g.launch.title_id);
    if (it == a.states.end()) it = a.states.emplace(g.launch.title_id, load_state(g, a.env)).first;
    return it->second;
}

static std::string escape_format(const std::string &s) {   // a label shown through ImGui's printf-style format
    std::string out;
    for (char ch : s) { out += ch; if (ch == '%') out += '%'; }
    return out;
}

static void setting_row(SettingsState &st, const Setting &s) {
    auto fixed = st.fixed.find(s.env);
    std::string &v = st.values[s.env];
    ImGui::PushID(s.env.c_str());
    if (fixed != st.fixed.end()) ImGui::BeginDisabled();
    float x = ImGui::GetCursorPosX(), w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(s.label.c_str());
    ImGui::SameLine(x + w * 0.45f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (s.kind == "choice") {
        std::string shown = v;
        for (const auto &o : s.options) if (o.value == v) shown = o.label;
        if (ImGui::BeginCombo("##value", shown.c_str())) {
            for (const auto &o : s.options) {
                bool sel = o.value == v;
                if (ImGui::Selectable(o.label.c_str(), sel)) v = o.value;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else if (s.kind == "toggle") {
        bool on = v == s.on;
        if (ImGui::Checkbox("##value", &on)) v = on ? s.on : s.off;
    } else {
        char *end = nullptr; double d = std::strtod(v.c_str(), &end);
        if (end == v.c_str() || !std::isfinite(d)) d = s.max;
        float f = (float)d;
        std::string label = escape_format(st.range_label(s, d));
        if (ImGui::SliderFloat("##value", &f, (float)s.min, (float)s.max, label.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
            double step = s.step > 0 ? s.step : 0.01, snapped = s.min + std::round((f - s.min) / step) * step;
            char buf[32]; std::snprintf(buf, sizeof buf, "%.2f", std::fmin(s.max, std::fmax(s.min, snapped)));   // as the Mac's slider writes it
            v = buf;
        }
    }
    std::string note = s.note;
    if (fixed != st.fixed.end()) note += (note.empty() ? "" : " \xC2\xB7 ") + std::string("Set at launch");
    auto refused = st.refused.find(s.env);
    if (refused != st.refused.end()) note += (note.empty() ? "" : " \xC2\xB7 ") + ("the saved value " + refused->second + " is not offered by this pack: Play replaces it");
    if (!note.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", note.c_str()); ImGui::PopTextWrapPos(); }
    if (fixed != st.fixed.end()) ImGui::EndDisabled();
    ImGui::PopID();
}

static void settings_sections(SettingsState &st, bool basic) {
    for (const auto &section : st.sections(basic)) {
        auto rows = st.rows(section, basic);
        if (rows.empty()) continue;
        ImGui::SeparatorText(section.c_str());
        for (const Setting *s : rows) setting_row(st, *s);
    }
}

static void settings_tab(App &a, Game *g) {
    if (!g->settings_error.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Its settings cannot be shown: %s", g->settings_error.c_str()); ImGui::PopTextWrapPos(); }
    SettingsState &st = state_of(a, *g);
    if (st.ignore_saved) ImGui::TextDisabled("DM_SAVED_CHOICES=0: starting with the tested defaults; the saved choices are kept.");
    settings_sections(st, true);
    bool advanced = a.cfg.get("advanced", "0") == "1";
    ImGui::SetNextItemOpen(advanced);
    bool open = ImGui::CollapsingHeader("Advanced settings");
    if (open != advanced) { a.cfg.set("advanced", open ? "1" : "0"); a.cfg.save(); }   // the window remembers whether Advanced settings is open, as the Mac's does
    if (open) {
        settings_sections(st, false);
        ImGui::Spacing();
        ImGui::BeginDisabled(!st.changed());
        if (button(a, "Reset to tested defaults")) st.reset();
        ImGui::EndDisabled();
        ImGui::TextDisabled("Brings back all settings this game was tested with.");
    }
    profile_section(a, *g);
    ImGui::SeparatorText("Pack");
    auto modes = g->modes();
    std::string mode = chosen_mode(*g, a.cfg);
    for (const char *m : {"balanced", "max"}) {
        bool have = std::find(modes.begin(), modes.end(), m) != modes.end();
        ImGui::BeginDisabled(!have);
        if (ImGui::RadioButton(std::strcmp(m, "balanced") ? "Max: a faster game" : "Balanced: a shorter pack", mode == m) && have) { a.cfg.set("pack." + g->launch.title_id, m); a.cfg.save(); }
        ImGui::EndDisabled();
        if (!have) { ImGui::SameLine(); ImGui::TextDisabled("not packed (build_ipa_from_nsp.py --platform %s --pack-mode %s)", kPlatform, m); }
    }
    ImGui::SeparatorText("Pipeline cache");
    ImGui::BeginDisabled(st.cache_fixed);
    bool on = st.cache_on();
    if (ImGui::Checkbox("Keep the Vulkan pipelines on disk", &on)) st.cache_value = on ? (st.cache_default == "off" ? "" : st.cache_default) : "off";
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    if (st.cache_fixed) ImGui::TextDisabled("Set at launch (DM_VK_PSO_CACHE=%s)", st.cache_value.c_str());
    if (on) ImGui::TextDisabled("Folder: %s. Later starts build fewer pipelines.", cache_folder(*g, st, a.env).c_str());
    else ImGui::TextDisabled("Off (DM_VK_PSO_CACHE=off): every start builds its pipelines again.");
    ImGui::TextDisabled("The host keeps one file per game, driver and shader chain. Its size limit and its cleanup of old files are settings of the pack "
                        "(Pipeline cache size limit, Pipeline cache cleanup: Advanced settings, Performance).");
    ImGui::PopTextWrapPos();
}

static std::string size_text(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1048576) std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu bytes", (unsigned long long)bytes);
    return buf;
}

// The Saves tab: the save folder the game gets (launcher_model.h, "The saves"), its backups, Back up saves, Restore... (asked first), Open saves folder
static void saves_tab(App &a, Game *g) {
    const Uint64 now = SDL_GetTicks();
    if (a.saves_of != g->launch.title_id || a.saves_at == 0 || now - a.saves_at > 1000) {
        a.saves = save_place(*g, a.env); a.backups = list_backups(a.saves); a.saves_at = now;
        if (a.saves_of != g->launch.title_id) { a.saves_msg.clear(); a.confirm.clear(); }
        a.saves_of = g->launch.title_id;
    }
    const bool running = a.child.running && a.running_id == g->launch.title_id;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("The game saves through its own menus; these are copies of what it saved. Saving the whole running game at any moment is not offered.");
    if (!a.saves.folder.empty()) ImGui::Text("Save folder: %s", a.saves.folder.c_str());
    if (!a.saves.problem.empty()) ImGui::TextDisabled("%s", a.saves.problem.c_str());
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(running || a.saves.present.empty());
    if (button(a, "Back up saves")) {
        Backup b; std::string err;
        if (backup_saves(a.saves, unix_now(), &b, &err)) a.saves_msg = "Backed up as " + b.name + " (" + std::to_string(b.files) + (b.files == 1 ? " file, " : " files, ") + size_text(b.bytes) + ").";
        else a.saves_msg = "[RED] " + err;
        std::fprintf(stderr, "nxlauncher: %s: %s\n", g->launch.title.c_str(), a.saves_msg.c_str());
        a.saves_at = 0;
    }
    ImGui::EndDisabled();
    if (running && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Not while the game runs: it may be writing its save. Stop it first.");
    ImGui::SameLine();
    ImGui::BeginDisabled(a.saves.folder.empty());
    if (button(a, "Open saves folder")) {
        std::string err, open = a.saves.present.empty() ? std::string() : a.saves.folder;
        if (open.empty()) { open = std::filesystem::u8path(a.saves.folder).parent_path().u8string(); }   // no save yet: the folder it will be in
        if (!a.dialogs->show_folder(open, &err)) a.saves_msg = "[RED] cannot open " + open + ": " + err;
    }
    ImGui::EndDisabled();
    if (!a.saves_msg.empty()) {
        ImGui::PushTextWrapPos(0);
        if (a.saves_msg.rfind("[RED] ", 0) == 0) ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", a.saves_msg.c_str() + 6);
        else ImGui::TextUnformatted(a.saves_msg.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::SeparatorText("Backups");
    if (a.backups.empty()) ImGui::TextDisabled("None yet. Back up saves keeps a dated copy in %s.", a.saves.backups.empty() ? "a folder beside the saves" : a.saves.backups.c_str());
    else ImGui::TextDisabled("In %s, newest first.", a.saves.backups.c_str());
    if (!a.backups.empty() && ImGui::BeginTable("backups", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Backup", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Files");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto &b : a.backups) {
            ImGui::PushID(b.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)b.files);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(size_text(b.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(running);
            const bool ask = ImGui::Button("Restore...") || (!running && a.pressed("Restore " + b.name));
            ImGui::EndDisabled();
            if (running && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Not while the game runs. Stop it first.");
            if (ask) { a.confirm = b.name; ImGui::OpenPopup("Restore saves"); }
            if (ImGui::BeginPopupModal("Restore saves", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {   // this row's (the ID stack holds its name)
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36);
                ImGui::TextUnformatted(restore_prompt(a.saves, b).c_str());
                ImGui::PopTextWrapPos();
                if (button(a, "Replace the saves")) {
                    Backup safety; std::string err;
                    if (restore_saves(a.saves, b, a.child.running && a.running_id == g->launch.title_id, unix_now(), &safety, &err))
                        a.saves_msg = "Put back " + b.name + "." + (safety.name.empty() ? "" : " The saves it replaced are in the backup " + safety.name + ".");
                    else a.saves_msg = "[RED] " + err;
                    std::fprintf(stderr, "nxlauncher: %s: %s\n", g->launch.title.c_str(), a.saves_msg.c_str());
                    a.confirm.clear(); a.saves_at = 0;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (button(a, "Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { a.confirm.clear(); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static void game_pane(App &a) {
    Game *g = current(a);
    if (!g) {
        ImGui::TextWrapped("No game packed for %s in this folder. Add one with Add game... (its NSP, XCI, NSZ or XCZ file), or choose the folder that holds your packed games "
                           "with Browse....", kPlatform);
        return;
    }
    ImGui::TextUnformatted(g->launch.title.c_str());
    ImGui::TextDisabled("Version %s \xC2\xB7 %s", g->launch.version.c_str(), g->launch.title_id.c_str());
    if (!g->problem.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", g->problem.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::BeginTabBar("game")) {
        for (const char *tab : {"Settings", "Saves"}) {
            if (!ImGui::BeginTabItem(tab, nullptr, a.pressed(tab) ? ImGuiTabItemFlags_SetSelected : 0)) continue;
            ImGui::BeginChild(tab, ImVec2(0, 0), ImGuiChildFlags_None);
            if (!std::strcmp(tab, "Settings")) settings_tab(a, g); else saves_tab(a, g);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---- Add game ----
static void choose_game_file(App &a) {
    a.dialogs->open_file("Add a game: its NSP, XCI, NSZ or XCZ file", game_file_filters(), a.add.file.empty() ? a.dir : nsp_folder_of(a.add.file),
                         a.answer("game"));
}

static void prepare_add(App &a, const std::string &file) {   // the file chosen: what packing it takes, shown before the player starts it
    a.add = App::Adding();
    a.add.open = true; a.add.file = file;
    const KeysFound k = find_keys(a.cfg, a.games, bundle_dir(), a.env);
    a.add.keys = k.path; a.add.looked = k.looked;
    a.add.tools = find_pack_tools(bundle_dir(), a.env);
    std::fprintf(stderr, "nxlauncher: Add game: %s (packer %s, %s; prod.keys %s)\n", file.c_str(), a.add.tools.packer.empty() ? "none" : a.add.tools.packer.c_str(),
                 a.add.tools.from.empty() ? a.add.tools.problem.c_str() : a.add.tools.from.c_str(), k.path.empty() ? "not found" : k.path.c_str());
}

static void start_add(App &a) {
    if (busy(a)) return;
    Command c = add_command(a.add.file, a.add.keys, a.add.tools);
    std::string err;
    if (!c.error.empty()) { a.add.result = "[RED] " + c.error; return; }
    if (!spawn(c, &a.add.child, &err)) { a.add.result = "[RED] " + err; return; }
    a.add.since = unix_now(); a.add.log = c.log; a.add.result.clear(); a.add.view = PackView(); a.add.read_at = 0; a.add.cancelled = false;
    a.log = c.log; a.lines.clear(); a.seen = 0; a.tail_at = 0;
    a.status = "Packing " + std::filesystem::u8path(a.add.file).filename().u8string();
    std::fprintf(stderr, "nxlauncher: Add game: the packer started on %s, its output in %s\n", nsp_folder_of(a.add.file).c_str(), c.log.c_str());
}

static void read_add_log(App &a, bool now) {
    const Uint64 t = SDL_GetTicks();
    if (a.add.log.empty() || (!now && a.add.read_at && t - a.add.read_at < 250)) return;
    std::ifstream f(std::filesystem::u8path(a.add.log), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    a.add.view = read_pack_log(text); a.add.read_at = t;
}

static void finish_add(App &a) {   // the packer ended: the game in the list, or why not in plain words
    read_add_log(a, true);
    const int st = a.add.child.status;
    const std::string nsp = nsp_folder_of(a.add.file);
    a.add.result = pack_failure(a.add.view, st, nsp, a.add.keys);
    a.add.ok = false;
    if (a.add.result.empty()) {
        const std::string build = packed_build(nsp, a.add.since);
        if (build.empty()) a.add.result = "The packer ended without a " + std::string(kPlatform) + " pack in " + nsp + ": its output is in " + a.add.log + ".";
        else {
            if (remember_folder(a.cfg, a.dir, nsp)) std::fprintf(stderr, "nxlauncher: Add game: %s is outside the games folder: remembered in %s\n", nsp.c_str(), a.cfg.path.c_str());
            a.cfg.save();
            scan(a);
            for (const auto &g : a.games) {
                std::error_code ec;
                if (std::filesystem::equivalent(std::filesystem::u8path(g.dir), std::filesystem::u8path(build), ec)) { a.selected = g.launch.title_id; a.add.result = "Added " + g.launch.title + "."; }
            }
            if (a.add.result.empty()) a.add.result = "Packed into " + build + ", which the list does not show: its launch file has a problem.";
            a.add.ok = true;
        }
    }
    a.status = a.add.ok ? a.add.result : "Add game: " + a.add.result;
    std::fprintf(stderr, "nxlauncher: Add game: the packer exited with status %d: %s\n", st, a.add.result.c_str());
}

static void add_pane(App &a) {
    const bool packing = a.add.child.running;
    ImGui::TextUnformatted("Add game");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("File: %s", a.add.file.c_str());
    ImGui::TextDisabled("The packer packs the folder it is in, %s, and writes the game's <title ID>_build and <title ID>-work folders there. A first pack takes about 20 minutes "
                        "and several GB on disk; packing the same game again takes less than a minute.", nsp_folder_of(a.add.file).c_str());
    if (!a.add.tools.problem.empty()) ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", a.add.tools.problem.c_str());
    else ImGui::TextDisabled("Packer: %s (%s)", a.add.tools.packer.c_str(), a.add.tools.from.c_str());
    if (!a.add.keys.empty()) ImGui::Text("prod.keys: %s", a.add.keys.c_str());
    else {
        std::string looked;
        for (const auto &l : a.add.looked) looked += (looked.empty() ? "" : ", ") + l;
        ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "No prod.keys found. The packer needs the keys of your own console (prod.keys) and reads them only from the file it is "
                           "given; the launcher looked in: %s.", looked.empty() ? "nowhere" : looked.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(packing);
    if (button(a, "Choose prod.keys...")) a.dialogs->open_file("Choose your prod.keys", {{"Keys", "keys"}, {"All files", "*"}}, a.add.keys, a.answer("keys"));
    ImGui::SameLine();
    if (button(a, "Choose another file...")) choose_game_file(a);
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (packing) {
        read_add_log(a, false);
        const PackView &v = a.add.view;
        std::string head = v.k ? "[" + std::to_string(v.k) + "/" + std::to_string(v.n) + "] " + v.stage + (v.step.empty() ? "" : " \xC2\xB7 " + v.step) : "Starting the packer...";
        ImGui::TextUnformatted(head.c_str());
        std::string over = v.count;
        if (!v.note.empty()) over += (over.empty() ? "" : " \xC2\xB7 ") + v.note;
        if (v.eta >= 0) { char eta[48]; std::snprintf(eta, sizeof eta, "%sabout %ld:%02ld left", over.empty() ? "" : " \xC2\xB7 ", (long)v.eta / 60, (long)v.eta % 60); over += eta; }
        ImGui::ProgressBar(v.frac >= 0 ? (float)v.frac : -1.f * (float)ImGui::GetTime(), ImVec2(-FLT_MIN, 0), over.empty() ? nullptr : over.c_str());
        if (!a.add.cancelled) { if (button(a, "Cancel")) { std::string err; if (interrupt(&a.add.child, &err)) { a.add.cancelled = true; a.add.cancel_at = SDL_GetTicks(); } else a.status = "[RED] Cancel: " + err; } }
        else {
            ImGui::TextDisabled("Cancelling: the packer ends its current step...");
            if (SDL_GetTicks() - a.add.cancel_at > 15000) { ImGui::SameLine(); if (button(a, "Force stop##pack")) { std::string err; force_stop(&a.add.child, &err); } }
        }
    } else {
        if (!a.add.result.empty()) {
            ImGui::PushTextWrapPos(0);
            if (a.add.ok) ImGui::TextColored(ImVec4(.5f, .85f, .5f, 1.f), "%s", a.add.result.c_str());
            else ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", a.add.result.rfind("[RED] ", 0) == 0 ? a.add.result.c_str() + 6 : a.add.result.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::BeginDisabled(a.add.keys.empty() || !a.add.tools.problem.empty() || busy(a));
        if (button(a, a.add.result.empty() ? "Pack" : "Pack again")) start_add(a);
        ImGui::EndDisabled();
        if (busy(a) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("After the running game or pack ends.");
        ImGui::SameLine();
        if (button(a, "Close")) a.add.open = false;
    }
}

static void games_list(App &a) {
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("games", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);   // the title takes what the others leave; they fit their text
    ImGui::TableSetupColumn("Version");
    ImGui::TableSetupColumn("Packs");
    ImGui::TableSetupColumn("Last played");
    ImGui::TableHeadersRow();
    for (auto &g : a.games) {
        ImGui::PushID(g.dir.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(g.launch.title.c_str(), g.launch.title_id == a.selected, ImGuiSelectableFlags_SpanAllColumns) || a.pressed(g.launch.title)) { a.selected = g.launch.title_id; a.add.open = false; }   // the game's pane; Add game... shows a running pack again
        if (repacking(a, g)) ImGui::TextDisabled("Repacking with the profile...");
        else if (profile_of(a, g).better()) {   // the second pack, and why
            ImGui::TextColored(ImVec4(.95f, .8f, .35f, 1.f), "%s", profile_of(a, g).headline().c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", profile_of(a, g).reason().c_str());
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(g.launch.version.c_str());
        auto modes = g.modes();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(modes.empty() ? "none" : join(modes, ", ").c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(played(a.cfg, g).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void start_game(App &a) {
    Game *g = current(a);
    if (!g || a.child.running || repacking(a, *g)) return;
    SettingsState &st = state_of(a, *g);
    std::string err, mode = chosen_mode(*g, a.cfg);
    if (!save_choices(*g, st, &err)) { a.status = "[RED] " + err; return; }
    Command c = compose(*g, mode, a.env, record_profile(a.cfg, *g));   // Play records the profile unless "Record a profile while playing" is off
    if (!c.error.empty()) { a.status = "[RED] " + c.error; return; }
    say_notes(c);
    remember_start(a.cfg, *g, c.mode);
    if (!spawn(c, &a.child, &err)) { a.status = "[RED] " + err; return; }
    a.running = g->launch.title; a.running_id = g->launch.title_id; a.log = c.log; a.lines.clear(); a.seen = 0;
    a.stop_asked = a.stop_sent = a.forced = false; a.stop_note.clear();
    a.status = g->launch.title + " is running (the " + c.mode + " pack)";
    std::fprintf(stderr, "nxlauncher: %s (%s), the %s pack: nxhost-run started, its output in %s\n", g->launch.title.c_str(), g->launch.title_id.c_str(), c.mode.c_str(), c.log.c_str());
}

// Stop: the host's graceful exit, asked again on each frame until it is taken (Windows: the host's event exists once it reached main), then Force stop after the bound
static void stop_game(App &a) {
    if (!a.child.running || a.stop_asked) return;
    a.stop_asked = true; a.stop_at = SDL_GetTicks();
    a.status = "Stopping " + a.running + "...";
    std::fprintf(stderr, "nxlauncher: Stop: asking %s (nxhost-run %lld) for its graceful exit\n", a.running.c_str(), (long long)a.child.pid);
}

static void stop_tick(App &a) {
    if (!a.child.running || !a.stop_asked || a.stop_sent) return;
    std::string err;
    if (request_stop(&a.child, &err)) { a.stop_sent = true; a.stop_note.clear(); }
    else if (a.stop_note != err) { a.stop_note = err; std::fprintf(stderr, "nxlauncher: Stop: %s\n", err.c_str()); }
}

static void log_pane(App &a) {
    stop_tick(a);
    if (a.child.running && poll(&a.child)) {
        char buf[64]; std::snprintf(buf, sizeof buf, "%d", a.child.status);
        const std::string why = exit_problem(a.child.status);
        const std::string how = ended_how(a.child.status, a.stop_asked, a.forced, a.stop_asked ? (SDL_GetTicks() - a.stop_at) / 1000.0 : 0, tail_lines(a.log, 40));
        a.status = a.running + ": " + (!how.empty() ? how : why.empty() ? std::string("nxhost-run exited with status ") + buf : "[RED] " + why);
        if (a.child.status == kRepackStatus) a.refused = a.running_id;
        std::fprintf(stderr, "nxlauncher: %s\n", a.status.c_str());
        a.stop_asked = a.stop_sent = false;
        a.tail_at = 0;
        scan(a);   // what the session recorded (the profile against the packs)
    }
    if (a.repack.running && poll(&a.repack)) {
        a.status = a.repack_title + ": the packer exited with status " + std::to_string(a.repack.status) + (a.repack.status ? " (its output below)" : ": the new packs are in place");
        std::fprintf(stderr, "nxlauncher: %s\n", a.status.c_str());
        a.tail_at = 0;
        scan(a);   // the packs, the pack modes and the profile state again
    }
    if (a.add.child.running && poll(&a.add.child)) { finish_add(a); a.tail_at = 0; }
    Uint64 now = SDL_GetTicks();
    if (!a.log.empty() && (a.tail_at == 0 || now - a.tail_at > 250)) {
        if (a.log == a.add.log) {   // the packer's JSON events, as lines
            read_add_log(a, true);
            const auto &l = a.add.view.lines;
            a.lines.assign(l.size() > 200 ? l.end() - 200 : l.begin(), l.end());
        } else a.lines = tail_lines(a.log, 200);
        a.tail_at = now;
    }
    if (a.status.rfind("[RED] ", 0) == 0) ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", a.status.c_str() + 6);
    else ImGui::TextUnformatted(a.status.empty() ? "No game running." : a.status.c_str());
    if (!a.log.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(%s)", a.log.c_str()); }
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto &l : a.lines) ImGui::TextUnformatted(l.c_str());
    if (a.lines.size() != a.seen) { ImGui::SetScrollHereY(1.f); a.seen = a.lines.size(); }
    ImGui::EndChild();
}

// The dialogs' answers, on the frame after they came
static void take_answers(App &a) {
    std::vector<std::pair<std::string, std::string>> got;
    { std::lock_guard<std::mutex> l(a.box->mu); got.swap(a.box->answers); }
    for (const auto &e : got) {
        if (e.second.empty()) continue;   // cancelled
        if (e.first == "folder") {
            a.dir = e.second; a.cfg.set("games", a.dir); a.cfg.save(); scan(a);
            if (!a.child.running && !a.repack.running && !a.add.child.running) a.status = "Games folder: " + a.dir + " (" + std::to_string(a.games.size()) + " game" + (a.games.size() == 1 ? ")" : "s)");
        }
        else if (e.first == "game") {
            if (!is_game_file(e.second)) { a.status = "[RED] " + e.second + " is not a game file the packer takes (.nsp, .xci, .nsz or .xcz)"; continue; }
            prepare_add(a, e.second);
        } else if (e.first == "keys") { a.cfg.set("keys", e.second); a.cfg.save(); a.add.keys = e.second; }   // its path only
    }
}

static void draw(App &a) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos); ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("nxnative", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const ImGuiStyle &style = ImGui::GetStyle();
    auto width = [&](const char *label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2 + style.ItemSpacing.x; };
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Games folder"); ImGui::SameLine();
    ImGui::SetNextItemWidth(-(width("Browse...") + width("Open") + width("Add game...") + style.ItemSpacing.x * 2));
    bool enter = ImGui::InputText("##games", a.dir_buf, sizeof a.dir_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (button(a, "Browse...")) a.dialogs->open_folder("Choose the games folder", a.dir, a.answer("folder"));
    ImGui::SameLine();
    if (button(a, "Open") || enter) { a.dir = a.dir_buf; a.cfg.set("games", a.dir); a.cfg.save(); scan(a); }
    ImGui::SameLine(0, style.ItemSpacing.x * 3);
    if (button(a, "Add game...")) { if (a.add.child.running) a.add.open = true; else choose_game_file(a); }   // while a pack runs: back to its progress
    if (a.add.child.running && !a.add.open && ImGui::IsItemHovered()) ImGui::SetTooltip("A game is being packed: shows its progress.");
    float bar = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y, avail = ImGui::GetContentRegionAvail().y - bar;
    float log_h = std::fmax(ImGui::GetTextLineHeightWithSpacing() * 6, avail * 0.25f), top = avail - log_h;
    ImGui::BeginChild("list", ImVec2(ImGui::GetContentRegionAvail().x * 0.42f, top), ImGuiChildFlags_Borders);
    games_list(a);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("game", ImVec2(0, top), ImGuiChildFlags_Borders);
    if (a.add.open) add_pane(a); else game_pane(a);
    ImGui::EndChild();
    ImGui::BeginChild("log", ImVec2(0, log_h - style.ItemSpacing.y), ImGuiChildFlags_None);
    log_pane(a);
    ImGui::EndChild();
    bool show = a.cfg.get("show_window", "1") != "0";
    if (ImGui::Checkbox("Show this window at launch", &show)) { a.cfg.set("show_window", show ? "1" : "0"); a.cfg.save(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: nxlauncher starts the last game played at once with your choices; nxlauncher --window shows this window again.");
    // The last button: Play, Stop while a game runs, Stopping... until the bound, then Force stop
    const bool bound = a.stop_asked && SDL_GetTicks() - a.stop_at > (Uint64)kStopBoundMs;
    const char *act = !a.child.running ? "Play" : !a.stop_asked ? "Stop" : bound ? "Force stop" : "Stopping...";
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width("Quit") - width(act) + style.ItemSpacing.x);
    if (button(a, "Quit")) a.quit = true;
    ImGui::SameLine();
    Game *g = current(a);
    if (!a.child.running) {
        ImGui::BeginDisabled(!g || g->modes().empty() || repacking(a, *g));   // not while its packs are made again
        if (button(a, "Play")) start_game(a);
        ImGui::EndDisabled();
    } else if (!a.stop_asked) {
        if (button(a, "Stop")) stop_game(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ends %s the way closing its window does: what it saved stays, and the host writes this session's profile.", a.running.c_str());
    } else if (!bound) {
        ImGui::BeginDisabled(); button(a, "Stopping..."); ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", a.stop_note.empty() ? "The host is writing this session's files. Force stop is offered if it has not ended in 25 seconds." : a.stop_note.c_str());
    } else {
        if (button(a, "Force stop")) {
            std::string err;
            if (force_stop(&a.child, &err)) { a.forced = true; std::fprintf(stderr, "nxlauncher: Force stop: %s did not end %d s after Stop\n", a.running.c_str(), kStopBoundMs / 1000); }
            else a.status = "[RED] Force stop: " + err;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ends the game at once: what it saved stays, this session's profile is lost.");
    }
    ImGui::End();
}

// --press: the steps that are not a button (a shot, a wait), done on the frame they are reached
static void script_tick(App &a) {
    while (a.step < a.steps.size() && a.frame >= a.steps[a.step].frame) {
        const Step &s = a.steps[a.step];
        if (s.kind == "shot") { if (a.shot_at < 0) a.shot_at = a.frame + 2; return; }   // two frames on: a popup opened by the step before has its size then
        if (s.kind != "until") return;   // a press: its button takes it
        bool seen = a.status.find(s.arg) != std::string::npos || a.saves_msg.find(s.arg) != std::string::npos || a.add.result.find(s.arg) != std::string::npos;
        for (const auto &l : a.lines) if (!seen && l.find(s.arg) != std::string::npos) seen = true;
        if (!seen) return;
        std::fprintf(stderr, "nxlauncher: --press until:%s (frame %ld)\n", s.arg.c_str(), a.frame);
        a.step++;
    }
}

static bool save_shot(SDL_Renderer *ren, const std::string &path) {
    SDL_Surface *s = SDL_RenderReadPixels(ren, nullptr);
    bool ok = s && SDL_SaveBMP(s, path.c_str());
    if (!ok) std::fprintf(stderr, "[RED] nxlauncher: shot %s: %s\n", path.c_str(), SDL_GetError());
    else std::fprintf(stderr, "nxlauncher: shot %s\n", path.c_str());
    SDL_DestroySurface(s);
    return ok;
}

struct WindowRun { long frames = 0; std::string shot; std::vector<std::string> answers; std::vector<Step> steps; bool exit_idle = false; };

static int run_window(Config &cfg, const std::string &dir, const WindowRun &w) {
    if (!std::getenv("SDL_VIDEO_DRIVER")) if (const char *d = std::getenv("SDL_VIDEODRIVER")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, d);   // SDL 2's name of the switch
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) { std::fprintf(stderr, "[RED] nxlauncher: SDL could not start: %s\n", SDL_GetError()); return 1; }
    int ww = 1100, wh = 720;   // the window's size, as it was left (launcher.txt window=<w>x<h>)
    if (std::sscanf(cfg.get("window").c_str(), "%dx%d", &ww, &wh) != 2 || ww < 640 || wh < 400 || ww > 16384 || wh > 16384) { ww = 1100; wh = 720; }
    SDL_Window *win = SDL_CreateWindow("nxnative", ww, wh, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;   // SDL's 2D renderer (the software one in the host's SDL build): no Vulkan needed to choose a game
    if (!ren) { std::fprintf(stderr, "[RED] nxlauncher: no window or renderer: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    bool vsync = SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nothing written beside the program: the launcher's own settings are in launcher.txt
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    float scale = SDL_GetWindowDisplayScale(win);
    if (scale > 1.f) { ImGui::GetStyle().ScaleAllSizes(scale); io.FontGlobalScale = scale; }
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    App a; a.cfg = cfg; a.dir = dir;
    SdlDialogs sdl; sdl.win = win;
    AnswerDialogs given; given.answers = w.answers;
    const bool answering = !w.answers.empty() || !w.steps.empty();
    a.dialogs = answering ? static_cast<Dialogs *>(&given) : &sdl;   // --answer or --press: no system dialog opens (a dialog left without an answer is cancelled)
    a.steps = w.steps; a.exit_idle = w.exit_idle;
    scan(a);
    std::fprintf(stderr, "nxlauncher: %zu game%s in %s (renderer %s)\n", a.games.size(), a.games.size() == 1 ? "" : "s", a.dir.c_str(), SDL_GetRendererName(ren));
    long drawn = 0;
    while (!a.quit && (w.frames <= 0 || drawn < w.frames)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win))) a.quit = true;
        }
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); poll(&a.child); continue; }
        take_answers(a);
        a.frame = drawn;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw(a);
        ImGui::Render();
        SDL_SetRenderScale(ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(ren, .1f, .1f, .11f, 1.f);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (a.shot_at >= 0 && drawn >= a.shot_at) { save_shot(ren, a.steps[a.step].arg); a.shot_at = -1; a.step++; }   // the next step waits for the picture
        if (!w.shot.empty() && drawn + 1 == w.frames) save_shot(ren, w.shot);   // --shot: the last frame as a BMP, to look at the window without a screen (SDL's dummy driver)
        SDL_RenderPresent(ren);
        drawn++;
        script_tick(a);
        if (a.exit_idle && a.step >= a.steps.size() && !busy(a)) { if (!w.shot.empty()) save_shot(ren, w.shot); break; }
        if (!vsync) SDL_Delay(16);
    }
    if (w.frames > 0 || a.exit_idle) std::printf("nxlauncher: %ld frame%s drawn, %zu game%s listed\n", drawn, drawn == 1 ? "" : "s", a.games.size(), a.games.size() == 1 ? "" : "s");
    if (a.step < a.steps.size()) std::fprintf(stderr, "[RED] nxlauncher: --press: %zu step%s not done, the first %s:%s\n", a.steps.size() - a.step, a.steps.size() - a.step == 1 ? "" : "s",
                                              a.steps[a.step].kind.c_str(), a.steps[a.step].arg.c_str());
    if (answering) for (const auto &q : given.asked) std::fprintf(stderr, "nxlauncher: --answer: asked %s\n", q.c_str());
    if (answering) for (const auto &f : given.shown) std::fprintf(stderr, "nxlauncher: --answer: shown in the file manager: %s\n", f.c_str());
    if (a.child.running) std::fprintf(stderr, "nxlauncher: %s keeps running after the launcher closes\n", a.running.c_str());
    if (a.repack.running) std::fprintf(stderr, "nxlauncher: the packer of %s keeps running after the launcher closes (its output in %s)\n", a.repack_title.c_str(), a.log.c_str());
    if (a.add.child.running) std::fprintf(stderr, "nxlauncher: the packer of %s keeps running after the launcher closes (its output in %s)\n", a.add.file.c_str(), a.add.log.c_str());
    int cw = 0, ch = 0;
    if (SDL_GetWindowSize(win, &cw, &ch) && cw >= 640 && ch >= 400 && !(SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        const std::string size = std::to_string(cw) + "x" + std::to_string(ch);
        if (a.cfg.get("window") != size && (a.cfg.get("window") != "" || size != "1100x720")) { a.cfg.set("window", size); a.cfg.save(); }
    }
    cfg = a.cfg;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return a.step < a.steps.size() ? 3 : 0;
}

static bool parse_step(const std::string &text, Step *s) {   // label[@frame], shot:<file>[@frame], until:<text>[@frame]
    std::string t = text;
    size_t at = t.rfind('@');
    if (at != std::string::npos && at + 1 < t.size() && t.find_first_not_of("0123456789", at + 1) == std::string::npos) { s->frame = std::atol(t.c_str() + at + 1); t.resize(at); }
    if (t.rfind("shot:", 0) == 0) { s->kind = "shot"; s->arg = t.substr(5); }
    else if (t.rfind("until:", 0) == 0) { s->kind = "until"; s->arg = t.substr(6); }
    else { s->kind = "press"; s->arg = t; }
    return !s->arg.empty();
}

int main(int argc, char **argv) {
    std::string list, command, play, repack, games, mode;
    bool window = false;
    WindowRun w;
    for (int i = 1; i < argc; i++) {
        std::string o = argv[i];
        auto value = [&](std::string *dst) { if (i + 1 >= argc) return false; *dst = argv[++i]; return true; };
        std::string n;
        Step s;
        if (o == "--list" && value(&list)) continue;
        if (o == "--command" && value(&command)) continue;
        if (o == "--play" && value(&play)) continue;
        if (o == "--repack" && value(&repack)) continue;
        if (o == "--games" && value(&games)) continue;
        if (o == "--pack" && value(&mode) && (mode == "balanced" || mode == "max")) continue;
        if (o == "--frames" && value(&n) && (w.frames = std::atol(n.c_str())) > 0) continue;
        if (o == "--window") { window = true; continue; }
        if (o == "--shot" && value(&w.shot)) continue;
        if (o == "--answer" && value(&n)) { w.answers.push_back(n); continue; }
        if (o == "--press" && value(&n) && parse_step(n, &s)) { w.steps.push_back(s); continue; }
        if (o == "--exit-idle") { w.exit_idle = true; continue; }
        attach_console();
        if (o == "-h" || o == "--help") { std::fputs(kUsage, stdout); return 0; }
        std::fprintf(stderr, "nxlauncher: unknown or incomplete option %s\n%s", o.c_str(), kUsage);
        return 2;
    }
    ProcessEnv env;
    Config cfg; cfg.path = config_path(env); cfg.load();
    if (!list.empty()) { attach_console(); return list_games(cfg, list); }
    std::string dir = games.empty() ? games_dir(cfg) : games;
    if (!command.empty() || !play.empty() || !repack.empty()) {
        attach_console();
        auto found = find_all_games(dir, cfg); std::string err;
        const Game *g = find_game(found, !command.empty() ? command : !play.empty() ? play : repack, &err);
        if (!g) { std::fprintf(stderr, "[RED] nxlauncher: %s (%s)\n", err.c_str(), dir.c_str()); return 1; }
        if (!repack.empty()) return repack_now(cfg, *g, mode);
        return command.empty() ? play_now(cfg, env, *g, mode) : print_command(cfg, env, *g, mode);
    }
    auto found = find_all_games(dir, cfg);
    Start s = decide_start(cfg, env, found, window || !w.steps.empty());   // a scripted window is a window
    if (!s.window) {
        std::string err; const Game *g = find_game(found, s.game, &err);
        std::fprintf(stderr, "nxlauncher: no window (%s): %s starts at once with the saved choices\n", s.why.c_str(), g ? g->launch.title.c_str() : s.game.c_str());
        return g ? play_now(cfg, env, *g, mode) : 1;
    }
    if (!s.why.empty()) std::fprintf(stderr, "nxlauncher: %s\n", s.why.c_str());
    if (!w.answers.empty() || !w.steps.empty()) attach_console();   // the window driven by a script: its lines go to the script's console
    return run_window(cfg, dir, w);
}
