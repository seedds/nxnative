// nxlauncher's model (launcher_model.h says what it reads, writes and starts). Plain C++17 over std::filesystem, cJSON for the settings list, and the OS's process calls.
#include "launcher_model.h"
#include "cJSON.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

namespace nxl {

static fs::path P(const std::string &s) { return fs::u8path(s); }
static std::string S(const fs::path &p) { return p.u8string(); }

static bool read_file(const std::string &path, std::string *out) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    *out = ss.str();
    return true;
}

static bool write_file(const std::string &path, const std::string &text, std::string *err) {   // a new file beside it, then renamed over it
    std::error_code ec;
    fs::create_directories(P(path).parent_path(), ec);
    std::string tmp = path + ".partial";
    {
        std::ofstream f(P(tmp), std::ios::binary | std::ios::trunc);
        if (!(f << text) || !f.flush()) { if (err) *err = "cannot write " + tmp; return false; }
    }
    fs::rename(P(tmp), P(path), ec);
    if (ec) { fs::remove(P(tmp), ec); if (err) *err = "cannot replace " + path; return false; }
    return true;
}

static std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out; std::string cur;
    for (char ch : text) { if (ch == '\n') { out.push_back(cur); cur.clear(); } else cur += ch; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string lower(std::string s) { for (auto &ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }
static std::string name_key(const std::string &k) {   // a variable's name as the OS compares it: Windows ignores case
#if defined(_WIN32)
    std::string u = k; for (auto &ch : u) ch = (char)std::toupper((unsigned char)ch); return u;
#else
    return k;
#endif
}

// ---- The environment ----
bool ProcessEnv::get(const std::string &key, std::string *value) const {
    const char *v = std::getenv(key.c_str());
    if (!v) return false;
    if (value) *value = v;
    return true;
}
bool MapEnv::get(const std::string &key, std::string *value) const {
    auto it = vars.find(key);
    if (it == vars.end()) return false;
    if (value) *value = it->second;
    return true;
}

bool valid_name(const std::string &n) {
    if (n.empty() || !(std::isalpha((unsigned char)n[0]) || n[0] == '_')) return false;
    for (char ch : n) if (!(std::isalnum((unsigned char)ch) || ch == '_')) return false;
    return true;
}

static bool parse_kv_line(const std::string &line, std::string *k, std::string *v) {   // dm/mac/launcher.c env_file: one line, without name checks
    size_t a = 0, b = line.size();
    while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
    while (b > a && (line[b - 1] == '\n' || line[b - 1] == '\r' || line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
    std::string s = line.substr(a, b - a);
    size_t eq = s.find('=');
    if (s.empty() || s[0] == '#' || eq == std::string::npos || eq == 0) return false;
    std::string key = s.substr(0, eq);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    if (key.empty()) return false;
    *k = key; *v = s.substr(eq + 1);
    return true;
}

std::vector<KV> read_env_file(const std::string &path) {
    std::vector<KV> out; std::string text, k, v;
    if (!read_file(path, &text)) return out;
    for (const auto &line : lines_of(text))
        if (parse_kv_line(line, &k, &v) && valid_name(k)) out.push_back({k, v});
    return out;
}

// ---- Settings ----
static bool number(const std::string &v, double *out) {   // as LauncherSettings.swift and launcher.c take a range's value: no blanks, no hex, finite
    if (v.empty() || std::isspace((unsigned char)v[0]) || v.find_first_of("xX") != std::string::npos) return false;
    char *end = nullptr; errno = 0;
    double x = std::strtod(v.c_str(), &end);
    if (*end || errno || !std::isfinite(x)) return false;
    *out = x;
    return true;
}

bool Setting::accepts(const std::string &v) const {
    if (kind == "choice") { for (const auto &o : options) if (o.value == v) return true; return false; }
    if (kind == "range") { double x; return number(v, &x) && x >= min && x <= max; }
    if (kind == "toggle") return v == on || v == off;
    return true;
}

bool Setting::same(const std::string &a, const std::string &b) const {
    double x, y;
    if (kind == "range" && number(a, &x) && number(b, &y)) return std::fabs(x - y) < 1e-6;
    return a == b;
}

static std::string jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

bool load_settings(const std::string &path, std::vector<Setting> *out, std::string *err) {
    std::string text;
    if (!read_file(path, &text)) { *err = "cannot read " + path; return false; }
    cJSON *root = cJSON_Parse(text.c_str());
    if (!root) { *err = path + " is not JSON"; return false; }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema"), *list = cJSON_GetObjectItemCaseSensitive(root, "settings");
    bool ok = cJSON_IsArray(list) && (!schema || (cJSON_IsNumber(schema) && schema->valueint == 1));   // docs/launcher.schema.json: schema 1 grows only by optional fields
    if (!ok) { *err = path + ": not a settings list of schema 1 (docs/launcher.schema.json)"; cJSON_Delete(root); return false; }
    out->clear();
    const cJSON *row;
    cJSON_ArrayForEach(row, list) {
        Setting s;
        s.env = jstr(row, "env"); s.kind = jstr(row, "kind"); s.section = jstr(row, "section"); s.label = jstr(row, "label"); s.note = jstr(row, "note");
        s.def = jstr(row, "default"); s.level = jstr(row, "level"); s.needs = jstr(row, "needs");
        if (s.section.empty()) s.section = "Game";
        if (s.env.empty() || (s.kind != "choice" && s.kind != "range" && s.kind != "toggle")) continue;   // a kind this reader does not know: not drawn
        const cJSON *opts = cJSON_GetObjectItemCaseSensitive(row, "options"), *o;
        cJSON_ArrayForEach(o, opts) {
            if (cJSON_GetArraySize(o) == 2 && cJSON_IsString(cJSON_GetArrayItem(o, 0)) && cJSON_IsString(cJSON_GetArrayItem(o, 1)))
                s.options.push_back({cJSON_GetArrayItem(o, 0)->valuestring, cJSON_GetArrayItem(o, 1)->valuestring});
        }
        for (auto [key, dst] : {std::pair<const char *, double *>{"min", &s.min}, {"max", &s.max}, {"step", &s.step}}) {
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(row, key);
            if (cJSON_IsNumber(n)) *dst = n->valuedouble;
        }
        if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(row, "on"))) s.on = jstr(row, "on");
        if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(row, "off"))) s.off = jstr(row, "off");
        const cJSON *base = cJSON_GetObjectItemCaseSensitive(row, "base");
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(base, "docked"), *h = cJSON_GetObjectItemCaseSensitive(base, "handheld");
        if (cJSON_GetArraySize(d) == 2 && cJSON_GetArraySize(h) == 2) {
            s.has_base = true;
            for (int i = 0; i < 2; i++) { s.docked[i] = cJSON_GetArrayItem(d, i)->valueint; s.handheld[i] = cJSON_GetArrayItem(h, i)->valueint; }
        }
        if (s.kind == "choice" && s.options.empty()) continue;
        out->push_back(std::move(s));
    }
    cJSON_Delete(root);
    return true;
}

// ---- The launch file ----
bool load_launch(const std::string &path, LaunchFile *out, std::string *err) {
    std::string text;
    if (!read_file(path, &text)) { *err = "cannot read " + path; return false; }
    auto lines = lines_of(text);
    if (lines.empty() || lines[0] != "# nxnative launch 1") { *err = path + " is not a launch file of version 1 (# nxnative launch 1): pack the game again"; return false; }
    *out = LaunchFile();
    for (const auto &line : lines) {
        if (line.empty() || line[0] == '#') continue;
        size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::string kind = line.substr(0, t1), name = line.substr(t1 + 1, t2 - t1 - 1), value = line.substr(t2 + 1);
        if (!value.empty() && value.back() == '\r') value.pop_back();
        if (kind == "game") {
            if (name == "title") out->title = value; else if (name == "title_id") out->title_id = value; else if (name == "version") out->version = value;
            else if (name == "package") out->package = value; else if (name == "platform") out->platform = value; else if (name == "graphics") out->graphics = value;
        } else if (kind == "path") {
            if (name == "host") out->host = value; else if (name == "cwd") out->cwd = value; else if (name == "choices") out->choices = value;
            else if (name == "log") out->log = value; else if (name == "settings") out->settings = value;
            else if (name == "profile") out->profile = value; else if (name == "package") out->package_dir = value; else if (name == "packer") out->packer = value;
            else if (name == "python") out->python = value; else if (name == "keys") out->keys = value; else if (name == "nxaot") out->nxaot = value;
        } else if (kind == "pack") out->packs.push_back({name, value, false});
        else if (kind == "env") out->env.push_back({name, value});
    }
    if (out->title_id.empty() || out->host.empty() || out->cwd.empty()) { *err = path + " names no title ID, host or working folder: pack the game again"; return false; }
    if (out->title.empty()) out->title = out->title_id;
    if (out->log.empty()) out->log = S(P(out->cwd) / "log.txt");
    return true;
}

bool packs_complete(const std::string &folder) {
    std::error_code ec; int manifests = 0;
    for (const auto &e : fs::directory_iterator(P(folder), ec)) {
        std::string name = S(e.path().filename());
        const std::string m = ".aot.tsv";
        if (name.size() <= m.size() || name.compare(name.size() - m.size(), m.size(), m)) continue;
        manifests++;
        if (!fs::is_regular_file(P(folder) / P(name.substr(0, name.size() - m.size()) + kPackSuffix), ec)) return false;
    }
    std::string nros;
    if (read_file(S(P(folder) / "nro.tsv"), &nros)) {
        for (const auto &line : lines_of(nros)) {
            std::istringstream ss(line); std::string name;
            if (line.empty() || line[0] == '#' || !(ss >> name) || name == "refused") continue;
            if (!fs::is_regular_file(P(folder) / P(name + kPackSuffix), ec)) return false;
        }
    }
    return manifests > 0;
}

std::vector<std::string> Game::modes() const {
    std::vector<std::string> out;
    for (const auto &p : launch.packs) if (p.complete) out.push_back(p.mode);
    return out;
}

static void add_game(const fs::path &build, const std::string &platform, std::vector<Game> *out) {
    std::error_code ec;
    fs::path launch = build / P("launch-" + platform + ".tsv");
    if (!fs::is_regular_file(launch, ec)) return;
    Game g; g.dir = S(build); g.launch_path = S(launch);
    std::string err;
    if (!load_launch(g.launch_path, &g.launch, &err)) {
        g.launch.title = S(build.filename()); g.problem = err;
        out->push_back(std::move(g)); return;
    }
    for (auto &p : g.launch.packs) p.complete = packs_complete(p.folder);
    if (g.launch.packs.empty()) g.problem = "no pack in " + g.launch_path + ": pack the game with scripts/build_ipa_from_nsp.py --platform " + platform;
    else if (g.modes().empty()) g.problem = "the packs in " + g.launch.packs[0].folder + " are incomplete: pack the game again";
    if (!g.launch.settings.empty() && !load_settings(g.launch.settings, &g.settings, &err)) g.settings_error = err;
    out->push_back(std::move(g));
}

static bool is_build_dir(const fs::directory_entry &e) {
    std::error_code ec; std::string n = S(e.path().filename());
    return e.is_directory(ec) && n.size() > 6 && n.compare(n.size() - 6, 6, "_build") == 0;
}

std::vector<Game> find_games(const std::string &dir, const std::string &platform) {
    std::vector<Game> out; std::error_code ec;
    for (const auto &e : fs::directory_iterator(P(dir), ec)) {
        if (is_build_dir(e)) { add_game(e.path(), platform, &out); continue; }
        if (!e.is_directory(ec)) continue;
        std::error_code ec2;
        for (const auto &f : fs::directory_iterator(e.path(), ec2)) if (is_build_dir(f)) add_game(f.path(), platform, &out);
    }
    std::sort(out.begin(), out.end(), [](const Game &a, const Game &b) {
        std::string x = lower(a.launch.title), y = lower(b.launch.title);
        return x != y ? x < y : a.dir < b.dir;
    });
    return out;
}

const Game *find_game(const std::vector<Game> &games, const std::string &id, std::string *err) {
    const Game *hit = nullptr; int n = 0;
    for (const auto &g : games) {
        std::error_code ec;
        bool same_dir = fs::equivalent(P(g.dir), P(id), ec) && !ec;
        if (lower(g.launch.title_id) == lower(id) || same_dir) { hit = &g; n++; }
    }
    if (n == 1) return hit;
    if (err) *err = n ? "several games have the title ID " + id + ": name the folder (<id>_build) instead" : "no game " + id + " in the games folder";
    return nullptr;
}

// ---- The launcher's own settings ----
std::string Config::get(const std::string &key, const std::string &def) const {
    for (const auto &e : kv) if (e.key == key) return e.value;
    return def;
}
void Config::set(const std::string &key, const std::string &value) {
    for (auto &e : kv) if (e.key == key) { e.value = value; return; }
    kv.push_back({key, value});
}
bool Config::load() {
    kv.clear(); std::string text, k, v;
    if (!read_file(path, &text)) return false;
    for (const auto &line : lines_of(text)) if (parse_kv_line(line, &k, &v)) set(k, v);
    return true;
}
bool Config::save() const {
    std::string text = "# nxlauncher's own settings (dm/launcher): the games folder, the window's switches, each game's pack mode and last start (played.<title ID>, seconds since 1970)\n";
    for (const auto &e : kv) text += e.key + "=" + e.value + "\n";
    return write_file(path, text, nullptr);
}

std::string config_path(const Env &env) {
    std::string v;
#if defined(_WIN32)
    if (env.get("APPDATA", &v) && !v.empty()) return S(P(v) / "nxnative" / "launcher.txt");
    if (env.get("USERPROFILE", &v) && !v.empty()) return S(P(v) / "AppData" / "Roaming" / "nxnative" / "launcher.txt");
#else
    if (env.get("XDG_CONFIG_HOME", &v) && !v.empty() && v[0] == '/') return S(P(v) / "nxnative" / "launcher.txt");
    if (env.get("HOME", &v) && !v.empty()) return S(P(v) / ".config" / "nxnative" / "launcher.txt");
#endif
    return S(P(default_games_dir()) / "launcher.txt");
}

#if defined(_WIN32)
static std::wstring W(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

std::string self_path() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
    buf.resize(n);
    return U(buf);
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : S(p);
#endif
}

std::string default_games_dir() {
    std::string self = self_path();
    return self.empty() ? std::string(".") : S(P(self).parent_path());
}

std::string games_dir(const Config &c) { return c.get("games", default_games_dir()); }

// ---- One game's settings ----
SettingsState load_state(const Game &g, const Env &env) {
    SettingsState st; st.settings = g.settings;
    std::string v;
    st.ignore_saved = env.get("DM_SAVED_CHOICES", &v) && v == "0";
    std::map<std::string, std::string> saved, pack;
    if (!st.ignore_saved) for (const auto &e : read_env_file(g.launch.choices)) saved.emplace(name_key(e.key), e.value);   // the first line of a variable wins, as run.sh's
    for (const auto &e : g.launch.env) pack.emplace(name_key(e.key), e.value);
    for (const auto &s : st.settings) {
        std::string key = name_key(s.env), def = pack.count(key) ? pack[key] : s.def;
        st.defaults[s.env] = def;
        if (env.get(s.env, &v)) { st.fixed[s.env] = v; st.values[s.env] = v; continue; }
        auto it = saved.find(key);
        if (it == saved.end()) st.values[s.env] = def;
        else if (s.accepts(it->second)) st.values[s.env] = it->second;
        else { st.refused[s.env] = it->second; st.values[s.env] = def; }
    }
    st.cache_default = pack.count(name_key("DM_VK_PSO_CACHE")) ? pack[name_key("DM_VK_PSO_CACHE")] : "";
    if (env.get("DM_VK_PSO_CACHE", &v)) { st.cache_fixed = true; st.cache_value = v; }
    else if (saved.count(name_key("DM_VK_PSO_CACHE"))) st.cache_value = saved[name_key("DM_VK_PSO_CACHE")];
    else st.cache_value = st.cache_default;
    return st;
}

std::vector<std::string> SettingsState::sections(bool basic) const {
    std::vector<std::string> out;
    for (const auto &s : settings) if (s.basic() == basic && std::find(out.begin(), out.end(), s.section) == out.end()) out.push_back(s.section);
    return out;
}

bool SettingsState::visible(const Setting &s) const {
    size_t eq = s.needs.find('=');
    if (eq == std::string::npos) return true;
    auto it = values.find(s.needs.substr(0, eq));
    return it == values.end() || it->second == s.needs.substr(eq + 1);   // a row naming a setting this game does not have stays drawn
}

std::vector<const Setting *> SettingsState::rows(const std::string &section, bool basic) const {
    std::vector<const Setting *> out;
    for (const auto &s : settings) if (s.section == section && s.basic() == basic && visible(s)) out.push_back(&s);
    return out;
}

bool SettingsState::changed() const {
    for (const auto &s : settings) {
        if (fixed.count(s.env)) continue;
        auto v = values.find(s.env), d = defaults.find(s.env);
        if (v != values.end() && d != defaults.end() && !s.same(v->second, d->second)) return true;
    }
    return !cache_fixed && cache_value != cache_default;
}

void SettingsState::reset() {
    for (const auto &s : settings) if (!fixed.count(s.env)) values[s.env] = defaults[s.env];
    if (!cache_fixed) cache_value = cache_default;
}

std::string SettingsState::range_label(const Setting &s, double v) const {
    char p[32]; std::snprintf(p, sizeof p, "%d %%", (int)std::lround(v * 100));
    if (!s.has_base) return p;
    auto mode = values.find("DM_OPMODE");
    const int *size = mode != values.end() && mode->second == "0" ? s.handheld : s.docked;
    char out[96]; std::snprintf(out, sizeof out, "%ld \xC3\x97 %ld (%s)", std::lround(size[0] * v), std::lround(size[1] * v), p);   // U+00D7, in the default font's Latin-1 range
    return out;
}

std::string cache_folder(const Game &g, const SettingsState &st, const Env &env) {   // dm/gfx/hostvk/vk_cache.c folder(): DM_VK_PSO_CACHE, else vk_pipeline_cache beside the save
    if (!st.cache_on()) return "";
    if (!st.cache_value.empty()) return st.cache_value;
    std::string save;
    if (!env.get("DM_SAVE", &save)) for (const auto &e : g.launch.env) if (name_key(e.key) == name_key("DM_SAVE")) { save = e.value; break; }
    if (save.empty()) return "vk_pipeline_cache in the platform's data folder";
    fs::path p = P(save);
    if (!p.has_filename()) p = p.parent_path();
    return S(p.parent_path() / "vk_pipeline_cache");
}

bool save_choices(const Game &g, const SettingsState &st, std::string *err) {
    if (st.ignore_saved) return true;   // DM_SAVED_CHOICES=0: the saved choices stay as they are
    if (g.launch.choices.empty()) { if (err) *err = "the launch file names no env.txt for the player's choices"; return false; }
    std::vector<KV> want; std::set<std::string> managed;
    for (const auto &s : st.settings) {
        if (st.fixed.count(s.env)) continue;   // a setting the environment fixed for this start keeps its saved line
        managed.insert(name_key(s.env));
        auto v = st.values.find(s.env), d = st.defaults.find(s.env);
        if (v != st.values.end() && d != st.defaults.end() && !s.same(v->second, d->second)) want.push_back({s.env, v->second});
    }
    if (!st.cache_fixed) {
        managed.insert(name_key("DM_VK_PSO_CACHE"));
        if (st.cache_value != st.cache_default) want.push_back({"DM_VK_PSO_CACHE", st.cache_value});
    }
    std::string text, out, k, v;
    bool had = read_file(g.launch.choices, &text);
    std::set<std::string> placed;
    for (const auto &line : lines_of(text)) {   // every other line as it was; a managed variable's first line replaced by its choice, its other lines dropped
        if (parse_kv_line(line, &k, &v) && managed.count(name_key(k))) {
            if (placed.count(name_key(k))) continue;
            placed.insert(name_key(k));
            for (const auto &w : want) if (name_key(w.key) == name_key(k)) out += w.key + "=" + w.value + "\n";
            continue;
        }
        out += line + "\n";
    }
    for (const auto &w : want) if (!placed.count(name_key(w.key))) out += w.key + "=" + w.value + "\n";
    if (!had && out.empty()) return true;
    return out == text || write_file(g.launch.choices, out, err);
}

// ---- What starts the game ----
std::string chosen_mode(const Game &g, const Config &c) {
    auto modes = g.modes();
    std::string want = c.get("pack." + g.launch.title_id);
    if (std::find(modes.begin(), modes.end(), want) != modes.end()) return want;
    return modes.empty() ? "" : modes[0];
}

Command compose(const Game &g, const std::string &mode, const Env &env, bool record) {
    Command c; c.cwd = g.launch.cwd; c.log = g.launch.log;
    const Pack *pack = nullptr;
    for (const auto &p : g.launch.packs) if (mode.empty() || p.mode == mode) { pack = &p; break; }   // as run.sh: the first pack row, or the one NXPACK names
    if (!pack) { c.error = g.launch.packs.empty() ? "no pack in " + g.launch_path + ": pack the game with scripts/build_ipa_from_nsp.py --platform " + std::string(kPlatform)
                                                  : "no " + mode + " pack in " + g.launch_path + ": pack the game with --pack-mode " + mode; return c; }
    c.mode = pack->mode;
    if (!pack->complete) { c.error = "the " + pack->mode + " packs in " + pack->folder + " are incomplete: pack the game again (--pack-mode " + pack->mode + ")"; return c; }
    std::string host = g.launch.host, v;
    if (env.get("NXHOST", &v) && !v.empty()) host = v;
    c.argv = {host, pack->folder};
    std::set<std::string> have;
    auto add = [&](const KV &e) {
        if (env.get(e.key) || have.count(name_key(e.key))) return;
        have.insert(name_key(e.key)); c.set.push_back(e);
    };
    if (!(env.get("DM_SAVED_CHOICES", &v) && v == "0")) for (const auto &e : read_env_file(g.launch.choices)) add(e);
    for (const auto &e : g.launch.env) add(e);
    if (record) add({"DM_AOT_PROFILE", profile_folder(g)});   // the launcher's own: last, so that what the player or the pack set wins
    return c;
}

// ---- The child process ----
static std::string value_of(const Command &cmd, const char *key) {   // a variable as the child gets it
    for (const auto &e : cmd.set) if (name_key(e.key) == name_key(key)) return e.value;
    const char *v = std::getenv(key);
    return v ? v : "";
}

static bool prepare(const Command &cmd, std::string *err) {   // the working folder, the save and temporary folders and the log's, as run.sh makes them; the previous log kept
    std::error_code ec;
    for (const std::string &d : {cmd.cwd, value_of(cmd, "DM_SAVE"), value_of(cmd, "DM_TEMP"), S(P(cmd.log).parent_path())}) {
        if (d.empty()) continue;
        fs::create_directories(P(d), ec);
        if (!fs::is_directory(P(d), ec)) { *err = "cannot make the folder " + d; return false; }
    }
    fs::path log = P(cmd.log);
    if (fs::exists(log, ec)) fs::rename(log, log.parent_path() / P(S(log.stem()) + ".prev" + S(log.extension())), ec);
    return true;
}

#if defined(_WIN32)
static std::wstring quote_arg(const std::wstring &a) {   // the C runtime's command-line rules: quotes around, backslashes doubled before a quote
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : a) {
        if (ch == L'\\') { slashes++; continue; }
        if (ch == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0; out += ch;
    }
    out.append(slashes * 2, L'\\'); out += L'"';
    return out;
}

bool spawn(const Command &cmd, Child *child, std::string *err) {
    if (cmd.argv.empty()) { *err = "nothing to start"; return false; }
    if (!prepare(cmd, err)) return false;
    std::vector<std::wstring> vars;   // the environment block: this process's with the launcher's variables, sorted without case as Windows wants it
    if (wchar_t *block = GetEnvironmentStringsW()) {
        for (wchar_t *e = block; *e; e += wcslen(e) + 1) vars.push_back(e);
        FreeEnvironmentStringsW(block);
    }
    for (const auto &kv : cmd.set) {
        std::wstring k = W(kv.key), line = k + L"=" + W(kv.value);
        bool done = false;
        for (auto &e : vars) {
            size_t eq = e.find(L'=', 1);
            if (eq == k.size() && CompareStringOrdinal(e.c_str(), (int)eq, k.c_str(), (int)k.size(), TRUE) == CSTR_EQUAL) { e = line; done = true; break; }
        }
        if (!done) vars.push_back(line);
    }
    std::sort(vars.begin(), vars.end(), [](const std::wstring &a, const std::wstring &b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
    std::wstring block;
    for (const auto &e : vars) { block += e; block += L'\0'; }
    block += L'\0';
    std::wstring line;
    for (const auto &a : cmd.argv) { if (!line.empty()) line += L' '; line += quote_arg(W(a)); }
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(W(cmd.log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { *err = "cannot write the log " + cmd.log; return false; }
    STARTUPINFOW si = {}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = log; si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    std::wstring app = W(cmd.argv[0]), cwd = W(cmd.cwd);
    BOOL ok = CreateProcessW(app.c_str(), &line[0], nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, &block[0], cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(log);
    if (!ok) { char msg[64]; std::snprintf(msg, sizeof msg, "Windows error %lu", (unsigned long)e); *err = "cannot start " + cmd.argv[0] + ": " + msg; return false; }
    CloseHandle(pi.hThread);
    child->pid = pi.dwProcessId; child->handle = pi.hProcess; child->running = true; child->status = -1;
    return true;
}

bool poll(Child *child) {
    if (!child->running) return true;
    if (WaitForSingleObject((HANDLE)child->handle, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0; GetExitCodeProcess((HANDLE)child->handle, &code);
    CloseHandle((HANDLE)child->handle); child->handle = nullptr;
    child->status = (int)code; child->running = false;
    return true;
}

int wait(Child *child) {
    if (child->running) WaitForSingleObject((HANDLE)child->handle, INFINITE);
    poll(child);
    return child->status;
}
#else
bool spawn(const Command &cmd, Child *child, std::string *err) {
    if (cmd.argv.empty()) { *err = "nothing to start"; return false; }
    if (!prepare(cmd, err)) return false;
    std::vector<std::string> vars;
    for (char **e = environ; *e; e++) vars.push_back(*e);
    for (const auto &kv : cmd.set) {
        std::string line = kv.key + "=" + kv.value; bool done = false;
        for (auto &e : vars) if (e.compare(0, kv.key.size() + 1, kv.key + "=") == 0) { e = line; done = true; break; }
        if (!done) vars.push_back(line);
    }
    std::vector<char *> envp, argv;   // made before fork: the child only calls what is async-signal-safe
    for (auto &e : vars) envp.push_back(&e[0]);
    envp.push_back(nullptr);
    std::vector<std::string> args = cmd.argv;
    for (auto &a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    int log = open(cmd.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log < 0) { *err = "cannot write the log " + cmd.log + ": " + std::strerror(errno); return false; }
    int pipefd[2];   // the exec's failure, reported back through a pipe that closes on a successful exec
    if (pipe2(pipefd, O_CLOEXEC)) { close(log); *err = std::string("pipe: ") + std::strerror(errno); return false; }
    pid_t pid = fork();
    if (pid < 0) { close(log); close(pipefd[0]); close(pipefd[1]); *err = std::string("fork: ") + std::strerror(errno); return false; }
    if (pid == 0) {
        int e = 0;
        if (!cmd.cwd.empty() && chdir(cmd.cwd.c_str())) e = errno;
        if (!e && (dup2(log, 1) < 0 || dup2(log, 2) < 0)) e = errno;
        if (!e) { execve(argv[0], argv.data(), envp.data()); e = errno; }
        ssize_t w = write(pipefd[1], &e, sizeof e); (void)w;
        _exit(127);
    }
    close(log); close(pipefd[1]);
    int e = 0; ssize_t n;
    while ((n = read(pipefd[0], &e, sizeof e)) < 0 && errno == EINTR) {}
    close(pipefd[0]);
    if (n == (ssize_t)sizeof e) {
        int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        *err = "cannot start " + cmd.argv[0] + " in " + cmd.cwd + ": " + std::strerror(e);
        return false;
    }
    child->pid = pid; child->running = true; child->status = -1;
    return true;
}

static void reap(Child *child, int st) {
    child->status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
    child->running = false;
}

bool poll(Child *child) {
    if (!child->running) return true;
    int st; pid_t r = waitpid((pid_t)child->pid, &st, WNOHANG);
    if (r == 0) return false;
    if (r < 0) { child->running = false; child->status = -1; return true; }
    reap(child, st);
    return true;
}

int wait(Child *child) {
    if (!child->running) return child->status;
    int st; pid_t r;
    while ((r = waitpid((pid_t)child->pid, &st, 0)) < 0 && errno == EINTR) {}
    if (r < 0) { child->running = false; child->status = -1; }
    else reap(child, st);
    return child->status;
}
#endif

std::vector<std::string> tail_lines(const std::string &path, size_t n) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg(), start = size > 65536 ? size - 65536 : 0;
    f.seekg(start);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto lines = lines_of(text);
    if (start > 0 && !lines.empty()) lines.erase(lines.begin());   // a line cut by the start of the window
    for (auto &l : lines) if (!l.empty() && l.back() == '\r') l.pop_back();
    if (lines.size() > n) lines.erase(lines.begin(), lines.end() - (std::ptrdiff_t)n);
    return lines;
}

// ---- Whether the window shows ----
Start decide_start(const Config &c, const Env &env, const std::vector<Game> &games, bool force) {
    Start s;
    if (force) { s.why = "--window"; return s; }
    std::string last = c.get("last"), v;
    const Game *g = last.empty() ? nullptr : find_game(games, last, nullptr);
    bool off_env = env.get("DM_LAUNCHER", &v) && v == "0", off_file = false;
    if (g) for (const auto &e : read_env_file(g->launch.choices)) if (e.key == "DM_LAUNCHER") { off_file = e.value == "0"; break; }
    bool off_cfg = c.get("show_window", "1") == "0";
    if (!off_env && !off_file && !off_cfg) return s;
    s.why = off_env ? "DM_LAUNCHER=0" : off_file ? "DM_LAUNCHER=0 in " + g->launch.choices : "\"Show this window at launch\" is off";
    if (!g) { s.why += ", but no game was played yet: the window shows"; return s; }
    if (g->modes().empty()) { s.why += ", but " + g->launch.title + " has no complete pack: the window shows"; return s; }
    s.window = false; s.game = g->launch.title_id;
    return s;
}

int64_t unix_now() { return (int64_t)std::time(nullptr); }

std::string now_text(int64_t t) {
    if (t <= 0) return "never";
    std::time_t tt = (std::time_t)t; std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32]; std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// ---- The second pack ----
std::string profile_folder(const Game &g) {
    if (!g.launch.profile.empty()) return g.launch.profile;
    return S(P(g.launch.cwd).parent_path() / "profile");   // <id>-work/run -> <id>-work/profile, where the packer looks (build_ipa_from_nsp.py: wdir / 'profile')
}

bool record_profile(const Config &c, const Game &g) { return c.get("record." + g.launch.title_id, "1") != "0"; }

static int64_t ticks(const fs::path &p) {   // a file's modification time on the file clock (only compared with another's)
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? 0 : (int64_t)t.time_since_epoch().count();
}

ProfileState profile_state(const Game &g, const std::string &mode) {
    ProfileState s; s.folder = profile_folder(g);
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(P(s.folder), ec)) {   // <module>.tsv and <module>.misses.tsv (dm/host/aot_profile.c), not the writer's .tmp files
        std::string n = S(e.path().filename());
        if (!e.is_regular_file(ec) || n.size() < 5 || n.compare(n.size() - 4, 4, ".tsv")) continue;
        const int64_t t = ticks(e.path());
        s.newest = s.files++ ? std::max(s.newest, t) : t;
    }
    const std::string suffix = kPackSuffix;
    for (const auto &p : g.launch.packs) {
        if (!mode.empty() && p.mode != mode) continue;
        for (const auto &e : fs::directory_iterator(P(p.folder), ec)) {
            std::string n = S(e.path().filename());
            if (n.size() <= suffix.size() || n.compare(n.size() - suffix.size(), suffix.size(), suffix)) continue;
            const int64_t t = ticks(e.path());
            s.packs = s.pack_files++ ? std::max(s.packs, t) : t;
        }
        if (!mode.empty()) break;
    }
    return s;
}

Command repack_command(const Game &g, const std::string &mode) {
    Command c; c.mode = mode.empty() ? "balanced" : mode;
    const LaunchFile &l = g.launch;
    if (l.package_dir.empty() || l.packer.empty() || l.python.empty()) {
        c.error = g.launch_path + " names no package folder, packer or Python to pack " + l.title + " again with (a pack made before those rows): pack it once more with "
                  "scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform " + std::string(kPlatform) + ", which writes them";
        return c;
    }
    c.argv = {l.python, l.packer, l.package_dir, "--platform", kPlatform, "--pack-mode", c.mode};
    if (!l.keys.empty()) { c.argv.push_back("-k"); c.argv.push_back(l.keys); }
    if (!l.nxaot.empty()) { c.argv.push_back("--nxaot"); c.argv.push_back(l.nxaot); }
    c.cwd = l.package_dir;
    c.log = S(P(l.log.empty() ? S(P(l.cwd) / "log.txt") : l.log).parent_path() / "repack.txt");
    c.set = {{"PYTHONUNBUFFERED", "1"}};   // its lines reach repack.txt as they are said, for the log pane
    return c;
}

}   // namespace nxl
