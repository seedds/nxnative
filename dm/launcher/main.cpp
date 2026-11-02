// nxlauncher: the launcher of Windows and Linux (Phase 6 of the port, docs/design/migration-status.md: Dear ImGui over SDL3's renderer, so it starts without Vulkan on any PC).
// One window: the games packed on this PC on the left (scripts/build_ipa_from_nsp.py --platform linux / windows), the chosen game's settings on the right as the Mac app's settings
// window shows them (dm/mac/Launcher.swift: the basic settings first, the others under Advanced settings, Reset to tested defaults), its pack mode and the Vulkan pipeline cache's
// switch, the log of the running game below, "Show this window at launch", Quit and Play. launcher_model.h has what it reads, writes and starts. Plain ImGui style and font; keyboard
// and controller navigation (ImGuiConfigFlags_NavEnableKeyboard / NavEnableGamepad).
// Without a window (tests and scripts): --list <games folder> (TSV), --command <id> (what it would start), --play <id>; --frames <n> draws n frames and exits (SDL_VIDEODRIVER=dummy),
// --shot <file.bmp> keeps the last of them as a picture.
#include "launcher_model.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
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
    "       nxlauncher --list <games folder>                         the games packed there, as TSV\n"
    "       nxlauncher [--games <folder>] [--pack balanced|max] --command <title ID or folder>   what it would start: KEY=value lines, a blank line, the argv\n"
    "       nxlauncher [--games <folder>] [--pack balanced|max] --play <title ID or folder>      start it without the window and wait (its output in its log.txt)\n";

#if defined(_WIN32)
// A GUI program's standard handles are invalid unless the caller redirected them: in a console, --list and --command print there
static void attach_console() {
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *f;
        freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr);
    }
}
#else
static void attach_console() {}
#endif

static std::string played(const Config &c, const Game &g) { return now_text(std::atoll(c.get("played." + g.launch.title_id, "0").c_str())); }

static std::string join(const std::vector<std::string> &v, const char *sep) {
    std::string s;
    for (const auto &x : v) { if (!s.empty()) s += sep; s += x; }
    return s;
}

static int list_games(const Config &cfg, const std::string &dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::u8path(dir), ec)) { std::fprintf(stderr, "[RED] nxlauncher: no games folder %s\n", dir.c_str()); return 1; }
    auto games = find_games(dir);
    std::printf("# title_id\ttitle\tversion\tpacks\tlast_played\tfolder\tproblem\n");
    for (const auto &g : games) {
        auto modes = g.modes();
        std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n", g.launch.title_id.empty() ? "-" : g.launch.title_id.c_str(), g.launch.title.c_str(), g.launch.version.empty() ? "-" : g.launch.version.c_str(),
                    modes.empty() ? "none" : join(modes, ",").c_str(), played(cfg, g).c_str(), g.dir.c_str(), g.problem.empty() ? "-" : g.problem.c_str());
    }
    std::fprintf(stderr, "nxlauncher: %zu game%s in %s\n", games.size(), games.size() == 1 ? "" : "s", dir.c_str());
    return 0;
}

static int print_command(const Config &cfg, const Env &env, const Game &g, const std::string &mode_in) {
    std::string mode = mode_in.empty() ? chosen_mode(g, cfg) : mode_in;
    Command c = compose(g, mode, env);
    if (!c.error.empty()) { std::fprintf(stderr, "[RED] nxlauncher: %s\n", c.error.c_str()); return 1; }
    std::printf("# %s (%s), the %s pack\n# cwd %s\n# log %s\n", g.launch.title.c_str(), g.launch.title_id.c_str(), c.mode.c_str(), c.cwd.c_str(), c.log.c_str());
    for (const auto &e : c.set) std::printf("%s=%s\n", e.key.c_str(), e.value.c_str());
    std::printf("\n");
    for (const auto &a : c.argv) std::printf("%s\n", a.c_str());
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
    Command c = compose(g, mode, env);
    if (!c.error.empty()) { std::fprintf(stderr, "[RED] nxlauncher: %s\n", c.error.c_str()); return 1; }
    remember_start(cfg, g, c.mode);
    Child ch;
    if (!spawn(c, &ch, &err)) { std::fprintf(stderr, "[RED] nxlauncher: %s\n", err.c_str()); return 1; }
    std::fprintf(stderr, "nxlauncher: %s (%s), the %s pack: nxhost-run started, its output in %s\n", g.launch.title.c_str(), g.launch.title_id.c_str(), c.mode.c_str(), c.log.c_str());
    int st = wait(&ch);
    std::fprintf(stderr, "nxlauncher: nxhost-run exited with status %d\n", st);
    return st;
}

// ---- The window ----
struct App {
    ProcessEnv env;
    Config cfg;
    std::string dir;
    char dir_buf[2048] = {};
    std::vector<Game> games;
    std::string selected;                          // the selected game's title ID
    std::map<std::string, SettingsState> states;   // each game's settings as the window shows them (kept while another game is selected)
    Child child; std::string running, status, log;
    std::vector<std::string> lines; Uint64 tail_at = 0; size_t seen = 0;
    bool quit = false;
};

static void scan(App &a) {
    a.games = find_games(a.dir);
    if (!a.games.empty() && !find_game(a.games, a.selected, nullptr)) {
        const Game *last = find_game(a.games, a.cfg.get("last"), nullptr);
        a.selected = last ? last->launch.title_id : a.games[0].launch.title_id;
    }
    std::snprintf(a.dir_buf, sizeof a.dir_buf, "%s", a.dir.c_str());
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

static void game_pane(App &a) {
    Game *g = current(a);
    if (!g) {
        ImGui::TextWrapped("No game packed for %s in this folder. Pack one with scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform %s, then choose its NSP folder "
                           "(or the folder above it) as the games folder.", kPlatform, kPlatform);
        return;
    }
    ImGui::TextUnformatted(g->launch.title.c_str());
    ImGui::TextDisabled("Version %s \xC2\xB7 %s", g->launch.version.c_str(), g->launch.title_id.c_str());
    if (!g->problem.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", g->problem.c_str()); ImGui::PopTextWrapPos(); }
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
        if (ImGui::Button("Reset to tested defaults")) st.reset();
        ImGui::EndDisabled();
        ImGui::TextDisabled("Brings back all settings this game was tested with.");
    }
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
        if (ImGui::Selectable(g.launch.title.c_str(), g.launch.title_id == a.selected, ImGuiSelectableFlags_SpanAllColumns)) a.selected = g.launch.title_id;
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
    if (!g || a.child.running) return;
    SettingsState &st = state_of(a, *g);
    std::string err, mode = chosen_mode(*g, a.cfg);
    if (!save_choices(*g, st, &err)) { a.status = "[RED] " + err; return; }
    Command c = compose(*g, mode, a.env);
    if (!c.error.empty()) { a.status = "[RED] " + c.error; return; }
    remember_start(a.cfg, *g, c.mode);
    if (!spawn(c, &a.child, &err)) { a.status = "[RED] " + err; return; }
    a.running = g->launch.title; a.log = c.log; a.lines.clear(); a.seen = 0;
    a.status = g->launch.title + " is running (the " + c.mode + " pack)";
    std::fprintf(stderr, "nxlauncher: %s (%s), the %s pack: nxhost-run started, its output in %s\n", g->launch.title.c_str(), g->launch.title_id.c_str(), c.mode.c_str(), c.log.c_str());
}

static void log_pane(App &a) {
    if (a.child.running && poll(&a.child)) {
        char buf[64]; std::snprintf(buf, sizeof buf, "%d", a.child.status);
        a.status = a.running + ": nxhost-run exited with status " + buf;
        std::fprintf(stderr, "nxlauncher: %s\n", a.status.c_str());
        a.tail_at = 0;
    }
    Uint64 now = SDL_GetTicks();
    if (!a.log.empty() && (a.tail_at == 0 || now - a.tail_at > 250)) { a.lines = tail_lines(a.log, 200); a.tail_at = now; }
    ImGui::TextUnformatted(a.status.empty() ? "No game running." : a.status.c_str());
    if (!a.log.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(%s)", a.log.c_str()); }
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto &l : a.lines) ImGui::TextUnformatted(l.c_str());
    if (a.lines.size() != a.seen) { ImGui::SetScrollHereY(1.f); a.seen = a.lines.size(); }
    ImGui::EndChild();
}

static void draw(App &a) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos); ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("nxnative", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Games folder"); ImGui::SameLine();
    float bw = ImGui::CalcTextSize("Open").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(-bw);
    bool enter = ImGui::InputText("##games", a.dir_buf, sizeof a.dir_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Open") || enter) { a.dir = a.dir_buf; a.cfg.set("games", a.dir); a.cfg.save(); scan(a); }
    float bar = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y, avail = ImGui::GetContentRegionAvail().y - bar;
    float log_h = std::fmax(ImGui::GetTextLineHeightWithSpacing() * 6, avail * 0.25f), top = avail - log_h;
    ImGui::BeginChild("list", ImVec2(ImGui::GetContentRegionAvail().x * 0.42f, top), ImGuiChildFlags_Borders);
    games_list(a);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("game", ImVec2(0, top), ImGuiChildFlags_Borders);
    game_pane(a);
    ImGui::EndChild();
    ImGui::BeginChild("log", ImVec2(0, log_h - ImGui::GetStyle().ItemSpacing.y), ImGuiChildFlags_None);
    log_pane(a);
    ImGui::EndChild();
    bool show = a.cfg.get("show_window", "1") != "0";
    if (ImGui::Checkbox("Show this window at launch", &show)) { a.cfg.set("show_window", show ? "1" : "0"); a.cfg.save(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: nxlauncher starts the last game played at once with your choices; nxlauncher --window shows this window again.");
    const ImGuiStyle &style = ImGui::GetStyle();
    float qw = ImGui::CalcTextSize("Quit").x + style.FramePadding.x * 2, pw = ImGui::CalcTextSize("Play").x + style.FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - qw - pw - style.ItemSpacing.x);
    if (ImGui::Button("Quit")) a.quit = true;
    ImGui::SameLine();
    Game *g = current(a);
    ImGui::BeginDisabled(!g || g->modes().empty() || a.child.running);
    if (ImGui::Button("Play")) start_game(a);
    ImGui::EndDisabled();
    ImGui::End();
}

static int run_window(Config &cfg, const std::string &dir, long frames, const std::string &shot) {
    if (!std::getenv("SDL_VIDEO_DRIVER")) if (const char *d = std::getenv("SDL_VIDEODRIVER")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, d);   // SDL 2's name of the switch
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) { std::fprintf(stderr, "[RED] nxlauncher: SDL could not start: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("nxnative", 1100, 720, SDL_WINDOW_RESIZABLE);
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
    scan(a);
    std::fprintf(stderr, "nxlauncher: %zu game%s in %s (renderer %s)\n", a.games.size(), a.games.size() == 1 ? "" : "s", a.dir.c_str(), SDL_GetRendererName(ren));
    long drawn = 0;
    while (!a.quit && (frames <= 0 || drawn < frames)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win))) a.quit = true;
        }
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); poll(&a.child); continue; }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw(a);
        ImGui::Render();
        SDL_SetRenderScale(ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(ren, .1f, .1f, .11f, 1.f);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (!shot.empty() && drawn + 1 == frames) {   // --shot: the last frame as a BMP, to look at the window without a screen (SDL's dummy driver)
            SDL_Surface *s = SDL_RenderReadPixels(ren, nullptr);
            if (!s || !SDL_SaveBMP(s, shot.c_str())) std::fprintf(stderr, "[RED] nxlauncher: --shot %s: %s\n", shot.c_str(), SDL_GetError());
            SDL_DestroySurface(s);
        }
        SDL_RenderPresent(ren);
        drawn++;
        if (!vsync) SDL_Delay(16);
    }
    if (frames > 0) std::printf("nxlauncher: %ld frame%s drawn, %zu game%s listed\n", drawn, drawn == 1 ? "" : "s", a.games.size(), a.games.size() == 1 ? "" : "s");
    if (a.child.running) std::fprintf(stderr, "nxlauncher: %s keeps running after the launcher closes\n", a.running.c_str());
    cfg = a.cfg;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}

int main(int argc, char **argv) {
    std::string list, command, play, games, mode, shot;
    bool window = false; long frames = 0;
    for (int i = 1; i < argc; i++) {
        std::string o = argv[i];
        auto value = [&](std::string *dst) { if (i + 1 >= argc) return false; *dst = argv[++i]; return true; };
        std::string n;
        if (o == "--list" && value(&list)) continue;
        if (o == "--command" && value(&command)) continue;
        if (o == "--play" && value(&play)) continue;
        if (o == "--games" && value(&games)) continue;
        if (o == "--pack" && value(&mode) && (mode == "balanced" || mode == "max")) continue;
        if (o == "--frames" && value(&n) && (frames = std::atol(n.c_str())) > 0) continue;
        if (o == "--window") { window = true; continue; }
        if (o == "--shot" && value(&shot)) continue;
        attach_console();
        if (o == "-h" || o == "--help") { std::fputs(kUsage, stdout); return 0; }
        std::fprintf(stderr, "nxlauncher: unknown or incomplete option %s\n%s", o.c_str(), kUsage);
        return 2;
    }
    ProcessEnv env;
    Config cfg; cfg.path = config_path(env); cfg.load();
    if (!list.empty()) { attach_console(); return list_games(cfg, list); }
    std::string dir = games.empty() ? games_dir(cfg) : games;
    if (!command.empty() || !play.empty()) {
        attach_console();
        auto found = find_games(dir); std::string err;
        const Game *g = find_game(found, command.empty() ? play : command, &err);
        if (!g) { std::fprintf(stderr, "[RED] nxlauncher: %s (%s)\n", err.c_str(), dir.c_str()); return 1; }
        return command.empty() ? play_now(cfg, env, *g, mode) : print_command(cfg, env, *g, mode);
    }
    auto found = find_games(dir);
    Start s = decide_start(cfg, env, found, window);
    if (!s.window) {
        std::string err; const Game *g = find_game(found, s.game, &err);
        std::fprintf(stderr, "nxlauncher: no window (%s): %s starts at once with the saved choices\n", s.why.c_str(), g ? g->launch.title.c_str() : s.game.c_str());
        return g ? play_now(cfg, env, *g, mode) : 1;
    }
    if (!s.why.empty()) std::fprintf(stderr, "nxlauncher: %s\n", s.why.c_str());
    return run_window(cfg, dir, frames, shot);
}
