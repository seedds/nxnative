// nxlauncher's model (launcher_model.h says what it reads, writes and starts). Plain C++17 over std::filesystem, cJSON for the settings list, and the OS's process calls.
#include "launcher_model.h"
#include "cJSON.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <dlfcn.h>
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

// What a player installs for the Vulkan loader (exit_problem, vulkan_loader_problem)
#if defined(_WIN32)
static const char *const kVulkanLoaderHint = "vulkan-1.dll, which the GPU driver installs";
#else
static const char *const kVulkanLoaderHint = "libvulkan.so.1: the package libvulkan1 on Ubuntu and Debian";
#endif

// ---- The environment ----
bool ProcessEnv::get(const std::string &key, std::string *value) const {
    const char *v = std::getenv(key.c_str());
    if (!v || !*v) return false;   // an empty value is unset (Env)
    if (value) *value = v;
    return true;
}
bool MapEnv::get(const std::string &key, std::string *value) const {
    auto it = vars.find(key);
    if (it == vars.end() || it->second.empty()) return false;   // an empty value is unset (Env)
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
            if (name == "host") out->host = value; else if (name == "game_profile") out->game_profile = value;
            else if (name == "cwd") out->cwd = value; else if (name == "choices") out->choices = value;
            else if (name == "log") out->log = value; else if (name == "settings") out->settings = value;
            else if (name == "profile") out->profile = value; else if (name == "package") out->package_dir = value; else if (name == "packer") out->packer = value;
            else if (name == "python") out->python = value; else if (name == "keys") out->keys = value; else if (name == "nxaot") out->nxaot = value;
        } else if (kind == "made") {
            if (name == "profile") out->made_profile = value; else if (name == "misses") out->made_misses = value; else if (name == "targets") out->made_targets = value;
            else if (name == "recompiler") out->made_recompiler = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (kind == "samples" || kind == "misses" || kind == "targets") {   // '<functions> <samples> <digest>', '<addresses> <digest>', '<sites> <digest>' (made_rows)
            std::istringstream ss(value); Recorded r;
            if (kind == "samples" ? !(ss >> r.count >> r.samples >> r.digest) : !(ss >> r.count >> r.digest)) continue;
            (kind == "samples" ? out->made_samples : kind == "misses" ? out->made_miss : out->made_target)[name] = r;
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

// ---- The programs a launch file names, found again when they are gone (launcher_model.h) ----
static bool is_file(const std::string &path) { std::error_code ec; return !path.empty() && fs::is_regular_file(P(path), ec); }

// The bundle a program sits in: the nearest folder above it that holds VERSION and bin/nxhost-run, a bundle as scripts/package_port.py lays it out (its programs are at most
// three folders deep: python/bin/python3.14); '' when there is none
static std::string bundle_of(const std::string &path) {
    std::error_code ec;
    fs::path d = P(path).parent_path();
    for (int i = 0; i < 5 && !d.empty(); i++) {
        if (fs::is_regular_file(d / "VERSION", ec) && fs::is_regular_file(d / P(std::string("bin/nxhost-run") + kExeSuffix).make_preferred(), ec)) return S(d);
        if (d == d.parent_path()) break;
        d = d.parent_path();
    }
    return "";
}

// The row's program when it is there and not another bundle's while this bundle has its own, else the bundle's (rel: its place in a bundle), else what to do
static Found find_in_bundle(const std::string &what, const std::string &row, const std::string &rel, const std::string &bundle, const std::string &fix) {
    Found f;
    const std::string own = bundle.empty() ? std::string() : S(P(bundle) / P(rel).make_preferred());
    if (is_file(row)) {
        const std::string theirs = bundle_of(row);
        std::error_code ec;
        if (theirs.empty() || !is_file(own) || fs::equivalent(P(theirs), P(bundle), ec)) { f.path = row; return f; }
        f.path = own; f.note = what + ": the bundle's " + own + " (the launch file's " + row + " is another bundle's, " + theirs + ")";
        return f;
    }
    if (is_file(own)) { f.path = own; f.note = what + ": the bundle's " + own + " (the launch file's " + row + " is missing)"; return f; }
    f.problem = "the " + what + " " + row + " is missing: " + fix;
    return f;
}

Found find_host(const LaunchFile &l, const Env &env, const std::string &bundle) {
    std::string v;
    if (env.get("NXHOST", &v) && !v.empty()) { Found f; f.path = v; return f; }
    return find_in_bundle("host", l.host, std::string("bin/nxhost-run") + kExeSuffix, bundle, "pack the game again or set NXHOST");
}

std::string game_profile_problem(const LaunchFile &l) {
    if (l.game_profile.empty()) return "the launch file names no game profile (a pack made before one nxhost-run served every game): pack the game again";
    if (!is_file(l.game_profile)) return "the game profile " + l.game_profile + " is missing: pack the game again";
    return "";
}

Found find_nxaot(const LaunchFile &l, const std::string &bundle) {
    if (l.nxaot.empty()) return Found();   // a launch file without the row: no --nxaot, the packer finds its own
    return find_in_bundle("recompiler", l.nxaot, std::string("bin/nxaot") + kExeSuffix, bundle, "pack the game once more by hand with --nxaot <the recompiler>");
}

static std::string pack_by_hand() { return std::string("pack the game once more by hand with the bundle's ") + (std::string(kPlatform) == "windows" ? "pack.cmd" : "pack.sh"); }

Found find_packer(const LaunchFile &l, const std::string &bundle) {
    return find_in_bundle("packer", l.packer, "scripts/build_ipa_from_nsp.py", bundle, pack_by_hand());
}

// The words after `key` on its line of the bundle's VERSION file (scripts/package_port.py writes it: "<key> <value> ..." lines); empty without the file or the line
static std::vector<std::string> bundle_row(const std::string &bundle, const std::string &key) {
    std::string text;
    std::vector<std::string> words;
    if (bundle.empty() || !read_file(S(P(bundle) / "VERSION"), &text)) return words;
    for (auto line : lines_of(text)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // a Windows bundle's VERSION has CRLF lines
        std::istringstream ss(line); std::string k, w;
        if (!(ss >> k) || k != key) continue;
        while (ss >> w) words.push_back(w);
        break;
    }
    return words;
}

// The bundle's own Python: VERSION's "python <version> <program>" line ("python no" in a bundle without one), the program's path relative to the bundle
static std::string bundle_python(const std::string &bundle) {
    const std::vector<std::string> w = bundle_row(bundle, "python");
    return w.size() >= 2 ? w[1] : "";
}

unsigned bundle_recompiler(const std::string &bundle) {
    const std::vector<std::string> w = bundle_row(bundle, "recompiler");
    return w.empty() ? 0 : static_cast<unsigned>(std::strtoul(w[0].c_str(), nullptr, 10));
}

Found find_python(const LaunchFile &l, const std::string &bundle) {
    const std::string rel = bundle_python(bundle);   // '' (no VERSION line): the bundle's folder itself, never a file, so the row or the problem
    return find_in_bundle("Python", l.python, rel, bundle, pack_by_hand());
}

static void add_game(const fs::path &build, const std::string &platform, const std::string &bundle, const Env &env, std::vector<Game> *out) {
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
    else {
        const Found host = find_host(g.launch, env, bundle);
        g.problem = host.problem;   // shown only when neither the row's host nor the bundle's is there
        if (g.problem.empty()) g.problem = game_profile_problem(g.launch);
        for (const auto &p : g.launch.packs) {   // a complete pack mode the host would refuse for its bindings (asked only when a pack took bindings)
            if (!g.problem.empty() || !p.complete || !has_bindings(p.folder)) continue;
            std::string why = bindings_problem(p.folder, host_takeovers(host.path));
            if (!why.empty()) { g.problem = "the " + p.mode + " packs: " + why; g.repack_cures = true; }
        }
    }
    if (!g.launch.settings.empty() && !load_settings(g.launch.settings, &g.settings, &err)) g.settings_error = err;
    out->push_back(std::move(g));
}

// ---- A pack the host refuses ----
std::string exit_problem(int status) {
    if (status == kRepackStatus)
        return "nxhost-run refused the game's packs (status 8): they were made for another host or another bundle (the reason is in its log's [RED] line); Repack makes them again";
    if (status == kNoVulkanStatus)
        return "no Vulkan device for the game (status 9): it needs a GPU driver with Vulkan 1.3 and the Vulkan loader (" + std::string(kVulkanLoaderHint) + "); the reason is in its log's [RED] line";
    if (status == kNoFontStatus)
        return "no font of the system stands in for the game's shared font (status 10): install DejaVu Sans, Liberation Sans, Noto Sans or FreeSans (on Windows: Arial, Segoe UI or "
               "Tahoma); its log's [RED] line names the folders looked in";
    return "";
}


std::set<std::string> host_takeovers(const std::string &host, std::string *err) {
    static std::map<std::string, std::set<std::string>> cache;
    auto hit = cache.find(host);
    if (hit != cache.end()) return hit->second;
    std::set<std::string> out;
    std::error_code ec;
#if defined(_WIN32)
    const std::string tag = std::to_string(GetCurrentProcessId());
#else
    const std::string tag = std::to_string(getpid());
#endif
    fs::path dir = fs::temp_directory_path(ec) / P("nxlauncher-takeovers-" + tag);
    Command c; c.argv = {host, "--takeovers"}; c.cwd = S(dir); c.log = S(dir / "out.txt");
    Child ch; std::string e, text;
    // the program's own log lines (its constructors', on the same output: "[dm] ...") may come before the list's '#' line; the names are the lines after it
    bool ok = spawn(c, &ch, &e) && wait(&ch) == 0 && read_file(c.log, &text);
    size_t at = ok ? text.find("# nxhost-run take-overs") : std::string::npos;
    ok = ok && at != std::string::npos && (at == 0 || text[at - 1] == '\n');
    fs::remove_all(dir, ec);
    if (!ok) { if (err) *err = host + " --takeovers gave no list" + (e.empty() ? "" : ": " + e); return out; }
    for (const auto &line : lines_of(text.substr(at))) if (!line.empty() && line[0] != '#' && line[0] != '[') out.insert(line);
    cache[host] = out;
    return out;
}

static std::vector<fs::path> binding_files(const std::string &folder) {   // <module>.bind.tsv with its pack beside it, sorted
    std::vector<fs::path> out; std::error_code ec;
    for (const auto &e : fs::directory_iterator(P(folder), ec)) {
        const std::string n = S(e.path().filename());
        if (n.size() <= 9 || n.compare(n.size() - 9, 9, ".bind.tsv")) continue;
        if (fs::is_regular_file(P(folder) / P(n.substr(0, n.size() - 9) + kPackSuffix), ec)) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool has_bindings(const std::string &folder) { return !binding_files(folder).empty(); }

std::string bindings_problem(const std::string &folder, const std::set<std::string> &takeovers) {
    for (const auto &f : binding_files(folder)) {
        std::string text, name = S(f.filename());
        name.resize(name.size() - 9);
        if (!read_file(S(f), &text)) continue;
        std::vector<std::string> hit;
        for (const auto &line : lines_of(text)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream in(line); std::string slot, target, sym;
            if (in >> slot >> target >> sym && takeovers.count(sym)) hit.push_back(sym);
        }
        if (hit.empty()) continue;
        std::string names;
        for (size_t i = 0; i < hit.size() && i < 3; i++) names += (i ? ", " : "") + hit[i];
        if (hit.size() > 3) names += ", ...";
        return "the " + name + " pack assumed " + std::to_string(hit.size()) + " PLT binding" + (hit.size() == 1 ? "" : "s") + " to its own functions that this host takes over (" +
               names + "): the host would refuse it (made for another host or bundle); Repack makes the packs again";
    }
    return "";
}

static bool is_build_dir(const fs::directory_entry &e) {
    std::error_code ec; std::string n = S(e.path().filename());
    return e.is_directory(ec) && n.size() > 6 && n.compare(n.size() - 6, 6, "_build") == 0;
}

static void sort_games(std::vector<Game> *games) {   // by title, then folder
    std::sort(games->begin(), games->end(), [](const Game &a, const Game &b) {
        std::string x = lower(a.launch.title), y = lower(b.launch.title);
        return x != y ? x < y : a.dir < b.dir;
    });
}

std::vector<Game> find_games(const std::string &dir, const std::string &platform, const std::string &bundle, const Env &env) {
    std::vector<Game> out; std::error_code ec;
    for (const auto &e : fs::directory_iterator(P(dir), ec)) {
        if (is_build_dir(e)) { add_game(e.path(), platform, bundle, env, &out); continue; }
        if (!e.is_directory(ec)) continue;
        std::error_code ec2;
        for (const auto &f : fs::directory_iterator(e.path(), ec2)) if (is_build_dir(f)) add_game(f.path(), platform, bundle, env, &out);
    }
    sort_games(&out);
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
static bool list_key(const std::string &k) { return k == "nsp_folder"; }   // a key with a line per value
std::vector<std::string> Config::all(const std::string &key) const {
    std::vector<std::string> out;
    for (const auto &e : kv) if (e.key == key) out.push_back(e.value);
    return out;
}
void Config::add(const std::string &key, const std::string &value) {
    for (const auto &e : kv) if (e.key == key && e.value == value) return;
    kv.push_back({key, value});
}
void Config::remove(const std::string &key, const std::string &value) {
    kv.erase(std::remove_if(kv.begin(), kv.end(), [&](const KV &e) { return e.key == key && e.value == value; }), kv.end());
}
bool Config::load() {
    kv.clear(); std::string text, k, v;
    if (!read_file(path, &text)) return false;
    for (const auto &line : lines_of(text)) if (parse_kv_line(line, &k, &v)) { if (list_key(k)) add(k, v); else set(k, v); }
    return true;
}
bool Config::save() const {
    std::string text = "# nxlauncher's own settings (dm/launcher): the games folder, the window's switches, each game's pack mode and last start (played.<title ID>, seconds since 1970),\n"
                       "# the NSP folders of games added outside the games folder (nsp_folder, a line each), the prod.keys file Add game passes to the packer (its path only)\n";
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

std::string bundle_dir() { return default_games_dir(); }   // a bundle's nxlauncher sits at its root

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

Command compose(const Game &g, const std::string &mode, const Env &env, bool record, const std::string &bundle) {
    Command c; c.cwd = g.launch.cwd; c.log = g.launch.log;
    const Pack *pack = nullptr;
    for (const auto &p : g.launch.packs) if (mode.empty() || p.mode == mode) { pack = &p; break; }   // as run.sh: the first pack row, or the one NXPACK names
    if (!pack) { c.error = g.launch.packs.empty() ? "no pack in " + g.launch_path + ": pack the game with scripts/build_ipa_from_nsp.py --platform " + std::string(kPlatform)
                                                  : "no " + mode + " pack in " + g.launch_path + ": pack the game with --pack-mode " + mode; return c; }
    c.mode = pack->mode;
    if (!pack->complete) { c.error = "the " + pack->mode + " packs in " + pack->folder + " are incomplete: pack the game again (--pack-mode " + pack->mode + ")"; return c; }
    const Found host = find_host(g.launch, env, bundle);   // NXHOST, the host row, the bundle's
    if (!host.problem.empty()) { c.error = host.problem; return c; }
    const std::string profile = game_profile_problem(g.launch);   // the game's constants: without the file the host would run another profile
    if (!profile.empty()) { c.error = profile; return c; }
    if (has_bindings(pack->folder)) {   // a pack the host would refuse for its PLT bindings: said before the start, with the repack (bindings_problem)
        const std::string why = bindings_problem(pack->folder, host_takeovers(host.path));
        if (!why.empty()) { c.error = "the " + pack->mode + " packs: " + why; return c; }
    }
    if (!host.note.empty()) c.notes.push_back(host.note);
    c.argv = {host.path, "--profile", g.launch.game_profile, pack->folder};
    std::string v;
    std::set<std::string> have;
    auto add = [&](const KV &e) {   // an env.txt line with an empty value is as if absent (an empty value is unset: Env), so a row of the same name still applies
        if (e.value.empty() || env.get(e.key) || have.count(name_key(e.key))) return;
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
    std::vector<std::wstring> vars;   // the environment block: this process's with the launcher's variables, sorted without case as Windows wants it; no empty value (Env)
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
    // no empty value (Env): a variable set to nothing is unset; the hidden "=C:=C:\\..." entries (a drive's current folder) start with '=' and stay
    vars.erase(std::remove_if(vars.begin(), vars.end(), [](const std::wstring &e) { return !e.empty() && e[0] != L'=' && e.back() == L'='; }), vars.end());
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
    // A tree starts suspended and runs once it is in its job, so no process it starts escapes the job (force_stop ends the job). Its own hidden console, as every child's
    // (CREATE_NO_WINDOW): interrupt sends CTRL_C_EVENT there
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | (cmd.tree ? CREATE_SUSPENDED : 0);
    BOOL ok = CreateProcessW(app.c_str(), &line[0], nullptr, nullptr, TRUE, flags, &block[0], cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(log);
    if (!ok) { char msg[64]; std::snprintf(msg, sizeof msg, "Windows error %lu", (unsigned long)e); *err = "cannot start " + cmd.argv[0] + ": " + msg; return false; }
    child->job = nullptr;
    if (cmd.tree) {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job && AssignProcessToJobObject(job, pi.hProcess)) child->job = job;
        else if (job) CloseHandle(job);   // no job (a job the launcher's own does not allow nested in, before Windows 8): force_stop ends the process alone
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    child->pid = pi.dwProcessId; child->handle = pi.hProcess; child->tree = cmd.tree; child->running = true; child->status = -1;
    return true;
}

bool poll(Child *child) {
    if (!child->running) return true;
    if (WaitForSingleObject((HANDLE)child->handle, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0; GetExitCodeProcess((HANDLE)child->handle, &code);
    CloseHandle((HANDLE)child->handle); child->handle = nullptr;
    if (child->job) { CloseHandle((HANDLE)child->job); child->job = nullptr; }   // the job ends with its last process
    child->status = (int)code; child->running = false;
    return true;
}

int wait(Child *child) {
    if (child->running) WaitForSingleObject((HANDLE)child->handle, INFINITE);
    poll(child);
    return child->status;
}

bool wait_for(Child *child, int ms) {
    if (child->running) WaitForSingleObject((HANDLE)child->handle, ms < 0 ? 0 : (DWORD)ms);
    return poll(child);
}

bool request_stop(Child *child, std::string *err) {
    if (!child->running) return true;
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, W(stop_event_name(child->pid)).c_str());
    if (!ev) {
        if (err) *err = "the game takes no stop request yet (no event " + stop_event_name(child->pid) + ": a host older than Stop, or one still starting)";
        return false;
    }
    BOOL ok = SetEvent(ev);
    CloseHandle(ev);
    if (!ok && err) *err = "cannot signal " + stop_event_name(child->pid);
    return ok != 0;
}

bool force_stop(Child *child, std::string *err) {
    if (!child->running) return true;
    BOOL ok = child->job ? TerminateJobObject((HANDLE)child->job, 1) : TerminateProcess((HANDLE)child->handle, 1);
    if (!ok && err) { char msg[64]; std::snprintf(msg, sizeof msg, "Windows error %lu", (unsigned long)GetLastError()); *err = std::string("cannot end the process: ") + msg; }
    return ok != 0;
}

// The launcher's own answer to the Ctrl+C it sends (interrupt): taken, so the default handler does not end the launcher. A handler, not SetConsoleCtrlHandler(NULL, TRUE),
// whose "ignore Ctrl+C" every process started later would inherit (the next packer could not be cancelled)
static BOOL WINAPI swallow_ctrl_c(DWORD ev) { return ev == CTRL_C_EVENT; }

bool interrupt(Child *child, std::string *err) {
    if (!child->running) return true;
    // A process attaches to one console at a time: the launcher leaves its own (a GUI program has none; --repack in a terminal has its caller's) for the child's, takes
    // the event itself, sends it to every process of that console (the packer and the tools it started), and goes back
    static bool handler = SetConsoleCtrlHandler(swallow_ctrl_c, TRUE) != 0; (void)handler;
    const bool had = GetConsoleWindow() != nullptr;
    FreeConsole();
    if (!AttachConsole((DWORD)child->pid)) {
        char msg[64]; std::snprintf(msg, sizeof msg, "Windows error %lu", (unsigned long)GetLastError());
        if (err) *err = std::string("cannot reach the packer's console: ") + msg;
        if (had) AttachConsole(ATTACH_PARENT_PROCESS);
        return false;
    }
    BOOL ok = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    Sleep(50);   // the event reaches each process of the console on a thread of its own, the launcher's too: its handler answers it while still attached
    FreeConsole();
    if (had) AttachConsole(ATTACH_PARENT_PROCESS);
    if (!ok && err) *err = "cannot send Ctrl+C to the packer's console";
    return ok != 0;
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
    vars.erase(std::remove_if(vars.begin(), vars.end(), [](const std::string &e) { return !e.empty() && e.back() == '='; }), vars.end());   // no empty value (Env): unset
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
        if (cmd.tree) setpgid(0, 0);   // a process group of its own: interrupt and force_stop reach the tools it starts (the parent sets it too, whichever runs first)
        if (!cmd.cwd.empty() && chdir(cmd.cwd.c_str())) e = errno;
        if (!e && (dup2(log, 1) < 0 || dup2(log, 2) < 0)) e = errno;
        if (!e) { execve(argv[0], argv.data(), envp.data()); e = errno; }
        ssize_t w = write(pipefd[1], &e, sizeof e); (void)w;
        _exit(127);
    }
    if (cmd.tree) setpgid(pid, pid);   // EACCES once the child has run execve: it set the group itself then
    close(log); close(pipefd[1]);
    int e = 0; ssize_t n;
    while ((n = read(pipefd[0], &e, sizeof e)) < 0 && errno == EINTR) {}
    close(pipefd[0]);
    if (n == (ssize_t)sizeof e) {
        int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        *err = "cannot start " + cmd.argv[0] + " in " + cmd.cwd + ": " + std::strerror(e);
        return false;
    }
    child->pid = pid; child->tree = cmd.tree; child->running = true; child->status = -1;
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

bool wait_for(Child *child, int ms) {
    for (int waited = 0; !poll(child); waited += 10) {
        if (waited >= ms) return false;
        usleep(10000);
    }
    return true;
}

static bool send_signal(Child *child, int sig, const char *what, std::string *err) {
    if (!child->running) return true;
    pid_t to = child->tree ? -(pid_t)child->pid : (pid_t)child->pid;
    if (kill(to, sig) == 0) return true;
    if (errno == ESRCH) return true;   // it has just ended: poll reaps it
    if (err) *err = std::string("cannot send ") + what + ": " + std::strerror(errno);
    return false;
}
bool request_stop(Child *child, std::string *err) { return send_signal(child, SIGTERM, "SIGTERM", err); }
bool force_stop(Child *child, std::string *err) { return send_signal(child, SIGKILL, "SIGKILL", err); }
bool interrupt(Child *child, std::string *err) { return send_signal(child, SIGINT, "SIGINT", err); }
#endif

std::string stop_event_name(int64_t pid) { return "Local\\nxnative-stop-" + std::to_string(pid); }

std::string ended_how(int status, bool stopped, bool forced, double seconds, const std::vector<std::string> &tail) {
    bool received = false, done = false;
    for (const auto &l : tail) {
        if (l.find("graceful exit done") != std::string::npos) done = true;
        else if (l.find("received ") != std::string::npos && l.find(": graceful exit") != std::string::npos) received = true;
    }
    char head[96]; std::snprintf(head, sizeof head, " (status %d, %.1f s after Stop)", status, seconds);
    if (forced) return std::string("Force stop ended it") + head + ": this session's profile was not written; the saves the game committed are kept";
    if (!stopped) return "";
    if (done) return std::string("It stopped gracefully") + head + ": the host wrote this session's files and ended; the saves the game committed are kept";
    if (received) return std::string("Stop reached the host, which ended before its graceful exit was done") + head + ": its log's last lines say why";
#if !defined(_WIN32)
    if (status == 128 + SIGTERM) return std::string("Stop ended it before the host had its exit handler") + head + ": SIGTERM's default action, nothing of the session written";
#endif
    return std::string("It ended after Stop") + head + ", without the host's graceful-exit lines in its log";
}

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

// FNV-1a, 64 bits, as 16 hex digits: the digests of the made rows (build_ipa_from_nsp.py fnv64)
static std::string fnv64(const std::string &text) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char ch : text) { h ^= ch; h *= 0x100000001b3ULL; }
    char buf[17]; std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

static bool hex_word(const std::string &w, uint64_t *v) {   // hexadecimal, 0x optional (Python's int(w, 16))
    size_t i = w.size() > 2 && w[0] == '0' && (w[1] == 'x' || w[1] == 'X') ? 2 : 0;
    if (i == w.size() || w.size() - i > 16) return false;
    for (size_t j = i; j < w.size(); j++) if (!std::isxdigit((unsigned char)w[j])) return false;
    *v = std::strtoull(w.c_str() + i, nullptr, 16);
    return true;
}

static bool dec_word(const std::string &w, uint64_t *v) {
    if (w.empty() || w.size() > 19) return false;
    for (char ch : w) if (!std::isdigit((unsigned char)ch)) return false;
    *v = std::strtoull(w.c_str(), nullptr, 10);
    return true;
}

static std::vector<std::string> words(const std::string &s) { std::istringstream ss(s); std::vector<std::string> out; std::string w; while (ss >> w) out.push_back(w); return out; }

// The image a profile or misses file was recorded on: its '# module <name>, build ID <id>, image SHA-256 <64 hex>' line (dm/host/aot_profile.c; the packer's PROFILE_HEAD), '' when none
static std::string image_of(const std::string &line) {
    static const std::string key = ", image SHA-256 ";
    if (line.rfind("# module ", 0) != 0) return "";
    size_t at = line.find(key);
    if (at == std::string::npos) return "";
    std::string sha = line.substr(at + key.size());
    if (!sha.empty() && sha.back() == '\r') sha.pop_back();
    if (sha.size() != 64) return "";
    for (char ch : sha) if (!std::isdigit((unsigned char)ch) && (ch < 'a' || ch > 'f')) return "";
    return sha;
}

// The images of the modules a pack folder holds packs for, as the packer checks a profile against them (manifest_sha, nro_pack_rows): per manifest <name>.aot.tsv the SHA-256 of
// its first line (# nxaot-manifest 1 <name> <build ID> <SHA-256>), per NRO row of nro.tsv (not the refused ones) its SHA-256; '' where none is written
static std::map<std::string, std::string> pack_images(const std::string &folder) {
    std::map<std::string, std::string> out; std::error_code ec;
    for (const auto &e : fs::directory_iterator(P(folder), ec)) {
        std::string n = S(e.path().filename());
        const std::string m = ".aot.tsv";
        if (n.size() <= m.size() || n.compare(n.size() - m.size(), m.size(), m)) continue;
        std::string text, head, sha;
        if (read_file(S(e.path()), &text)) head = text.substr(0, text.find('\n'));
        std::istringstream ss(head);
        for (int i = 0; i < 4 && std::getline(ss, sha, '\t'); i++) {}
        out[n.substr(0, n.size() - m.size())] = sha.size() == 64 ? lower(sha) : "";
    }
    std::string nros;
    if (read_file(S(P(folder) / "nro.tsv"), &nros)) {
        for (const auto &line : lines_of(nros)) {
            auto w = words(line);
            if (line.empty() || line[0] == '#' || w.empty() || w[0] == "refused") continue;
            out[w[0]] = w.size() > 1 ? lower(w[1]) : "";
        }
    }
    return out;
}

// <module>.tsv as nxaot and the packer read it (aot_profile): '<offset> <count>' lines, '#' to the end of a line a comment; an offset may repeat (the counts add up), a count of 0
// makes no function hot. Taken when it parses and its image line names sha or no image (the packer's rule). r, as build_ipa_from_nsp.py profile_summary computes it: the host's
// writer puts a line '# function 0x<entry> <samples> <rows>' before each sampled function's rows (dm/host/aot_profile.c): the functions are those lines' entries with samples, the
// samples their sum, the digest over the entries. A file without such lines (an older host's, its rows at functions' entries, or one written by hand) counts its rows: each offset
// with samples a function, the samples the rows' sum. A row per sampled instruction grows with a session's length; the functions, nxaot's tier H, do not
static bool function_line(const std::string &line, uint64_t *entry, uint64_t *samples) {   // '# function 0x<entry> <samples> <rows>'
    const size_t hash = line.find_first_not_of(" \t");
    if (hash == std::string::npos || line[hash] != '#') return false;
    auto w = words(line.substr(hash + 1));
    uint64_t rows;
    return w.size() == 4 && w[0] == "function" && hex_word(w[1], entry) && dec_word(w[2], samples) && dec_word(w[3], &rows);
}

static bool read_profile(const std::string &path, const std::string &sha, Recorded *r) {
    std::string text, image;
    if (!read_file(path, &text)) return false;
    std::map<uint64_t, uint64_t> rows, fns;
    for (const auto &line : lines_of(text)) {
        if (image.empty()) image = image_of(line);
        uint64_t off, n;
        if (function_line(line, &off, &n)) { fns[off] += n; continue; }
        auto w = words(line.substr(0, line.find('#')));
        if (w.empty()) continue;
        if (w.size() != 2 || !hex_word(w[0], &off) || !dec_word(w[1], &n)) return false;   // the packer packs without a file that does not parse
        rows[off] += n;
    }
    if (!image.empty() && !sha.empty() && image != sha) return false;   // recorded on another image
    const std::map<uint64_t, uint64_t> &counts = fns.empty() ? rows : fns;
    std::string list;
    *r = Recorded();
    for (const auto &c : counts) {
        r->samples += c.second;
        if (!c.second) continue;
        char buf[24]; std::snprintf(buf, sizeof buf, "%llx\n", (unsigned long long)c.first);
        list += buf; r->count++;
    }
    r->digest = fnv64(list);
    return true;
}

// <module>.misses.tsv as the packer reads it (aot_misses): '<module offset> <reason> <entries>' lines, '#' lines; taken when it parses and its image line names sha (nxaot refuses a
// file that names none or another). r: the addresses, the digest of the (address, reason) pairs
static bool read_misses(const std::string &path, const std::string &sha, Recorded *r) {
    std::string text, image;
    if (!read_file(path, &text)) return false;
    std::set<std::pair<uint64_t, std::string>> pairs;
    std::set<uint64_t> addrs;
    for (const auto &line : lines_of(text)) {
        if (image.empty()) image = image_of(line);
        auto w = words(line);
        if (w.empty() || w[0][0] == '#') continue;
        uint64_t off, n;
        if (w.size() != 3 || !hex_word(w[0], &off) || !dec_word(w[2], &n)) return false;
        pairs.insert({off, w[1]}); addrs.insert(off);
    }
    if (image.empty() || (!sha.empty() && image != sha)) return false;
    std::string list;
    for (const auto &p : pairs) {
        char buf[24]; std::snprintf(buf, sizeof buf, "%llx ", (unsigned long long)p.first);
        list += buf + p.second + "\n";
    }
    *r = Recorded{(uint64_t)addrs.size(), 0, fnv64(list)};
    return true;
}

// <module>.targets.tsv (dm/host/aot_targets.c) as nxaot reads it beside the profile (dm/tools/aot/src/build/pack.cpp TargetsFromProfile) and the packer takes it (aot_targets):
// '0x<site> 0x<target> <binds>' lines, '#' to the end of a line a comment; taken when it parses and its image line names sha (as the misses: the file is merged over the sessions
// of one image, and the packer refuses one that names none or another). r: the sites with a dominant target and the digest of the (site, dominant target) pairs. The rule is
// nxaot's, stated here, in the packer's targets_summary and in TargetsFromProfile (one rule in three languages; t_launcher_model and test_nsp_x86.py check the same fixture):
// a site's binds summed per target (a row of 0 binds counts for nothing), the target with the most (the lowest offset of equal ones) dominates when it has at least 90 % of the
// site's binds (10 x best >= 9 x total). nxaot then guards the site only at tier H and only at a blr or br to a function of the module; the launcher cannot tell those without the
// module's code, so it counts every site with a dominant target
static bool read_targets(const std::string &path, const std::string &sha, Recorded *r) {
    std::string text, image;
    if (!read_file(path, &text)) return false;
    std::map<uint64_t, std::map<uint64_t, uint64_t>> sites;   // site -> target -> binds
    for (const auto &line : lines_of(text)) {
        if (image.empty()) image = image_of(line);
        auto w = words(line.substr(0, line.find('#')));
        if (w.empty()) continue;
        uint64_t site, target, n;
        if (w.size() != 3 || !hex_word(w[0], &site) || !hex_word(w[1], &target) || !dec_word(w[2], &n)) return false;   // nxaot stops on such a row; the packer takes none
        if (n) sites[site][target] += n;
    }
    if (image.empty() || (!sha.empty() && image != sha)) return false;
    std::string list;
    *r = Recorded();
    for (const auto &s : sites) {
        uint64_t total = 0, best = 0, to = 0;
        for (const auto &t : s.second) { total += t.second; if (t.second > best) { best = t.second; to = t.first; } }
        const bool dominant = total <= UINT64_MAX / 10 ? best * 10 >= total * 9 : best >= total - total / 10;   // the second form for counts no session reaches
        if (!dominant) continue;   // no dominant target: nxaot does not guard it
        char buf[48]; std::snprintf(buf, sizeof buf, "%llx:%llx\n", (unsigned long long)s.first, (unsigned long long)to);
        list += buf; r->count++;
    }
    r->digest = fnv64(list);
    return true;
}

static std::string count_text(uint64_t n) {   // 3400 -> "3,400"
    std::string d = std::to_string(n), out;
    for (size_t i = 0; i < d.size(); i++) { if (i && (d.size() - i) % 3 == 0) out += ','; out += d[i]; }
    return out;
}

static std::string counted(uint64_t n, const char *one, const char *many) { return count_text(n) + " " + (n == 1 ? one : many); }

std::string ProfileState::headline() const {
    if (!recompiler) return "Profile recorded: a better pack is possible";
    return reasons.size() == 1 ? "A newer recompiler: a better pack is possible" : "A newer recompiler and a recorded profile: a better pack is possible";
}

std::string ProfileState::reason() const {
    std::string s;
    for (const auto &r : reasons) { if (!s.empty()) s += "; "; s += r; }
    return s;
}

ProfileState profile_state(const Game &g, const std::string &mode, const std::string &bundle) {
    ProfileState s; s.folder = profile_folder(g);
    const LaunchFile &l = g.launch;
    std::error_code ec;
    // <module>.tsv, <module>.misses.tsv (dm/host/aot_profile.c) and <module>.targets.tsv (aot_targets.c), not the writers' .tmp files; another .tsv is named once in the log
    static std::set<std::string> said;
    for (const auto &e : fs::directory_iterator(P(s.folder), ec)) {
        std::string n = S(e.path().filename());
        if (!e.is_regular_file(ec) || n.size() < 5 || n.compare(n.size() - 4, 4, ".tsv")) continue;
        std::string stem = n.substr(0, n.size() - 4);
        for (const char *kind : {".misses", ".targets"}) {
            const size_t k = std::strlen(kind);
            if (stem.size() > k && !stem.compare(stem.size() - k, k, kind)) { stem.resize(stem.size() - k); break; }
        }
        if (!stem.empty() && stem.find('.') == std::string::npos) { s.files++; continue; }
        // <module>.bind.tsv, the loader's bindings (dm/host/aot_profile.c): the host's too, the packer's to read (a first pack's --bind), nothing a pack is compared by
        if (stem.size() > 5 && !stem.compare(stem.size() - 5, 5, ".bind") && stem.find('.') == stem.size() - 5) continue;
        s.ignored.push_back(n);
        if (said.insert(S(e.path())).second)
            std::fprintf(stderr, "nxlauncher: %s: ignored: the profile folder holds the host's <module>.tsv, <module>.misses.tsv, <module>.targets.tsv and <module>.bind.tsv, "
                                 "and this is none of them\n", S(e.path()).c_str());
    }
    std::sort(s.ignored.begin(), s.ignored.end());
    const std::string suffix = kPackSuffix;
    const Pack *pack = nullptr;
    for (const auto &p : l.packs) {
        if (!mode.empty() && p.mode != mode) continue;
        if (!pack) pack = &p;
        for (const auto &e : fs::directory_iterator(P(p.folder), ec)) {
            std::string n = S(e.path().filename());
            if (n.size() > suffix.size() && !n.compare(n.size() - suffix.size(), suffix.size(), suffix)) s.pack_files++;
        }
        if (!mode.empty()) break;
    }
    // The recompiler of the packs (launcher_model.h): this bundle's is a later one, so a pack made now compiles the same code to something else (its fixes). The first reason,
    // and one a profile folder without files has too
    const auto recompiler = [&] {
        const unsigned own = bundle_recompiler(bundle), made = l.made_recompiler;
        if (!own || made >= own || !s.pack_files) return;
        s.recompiler = true;
        s.reasons.insert(s.reasons.begin(), "this bundle's recompiler (" + std::to_string(own) + ") is newer than the one that made the packs (" +
                                                (made ? std::to_string(made) : std::string("not recorded")) + "): packing again brings its fixes");
    };
    if (!pack || !s.files) { recompiler(); return s; }
    // What the packer would take now: per module of the packs, its files of its image
    for (const auto &m : pack_images(pack->folder)) {
        Recorded r;
        const bool profile = read_profile(S(P(s.folder) / P(m.first + ".tsv")), m.second, &r);
        if (profile && r.count) s.samples[m.first] = r;
        if (read_misses(S(P(s.folder) / P(m.first + ".misses.tsv")), m.second, &r) && r.count) s.misses[m.first] = r;
        if (profile && read_targets(S(P(s.folder) / P(m.first + ".targets.tsv")), m.second, &r) && r.count) s.targets[m.first] = r;   // nxaot reads it beside the profile only
    }
    // Against what the packs took (the made rows); without them, as packs made without any
    s.known = !l.made_profile.empty() || !l.made_misses.empty();
    uint64_t new_misses = 0, without = 0, new_functions = 0, longer = 0, new_targets = 0, targets_without = 0;
    bool other_misses = false;
    if (!s.known || l.made_misses == "yes") {
        for (const auto &m : s.misses) {
            auto it = l.made_miss.find(m.first);
            const Recorded had = it == l.made_miss.end() ? Recorded() : it->second;
            if (m.second.count > had.count) new_misses += m.second.count - had.count;
            else if (m.second.digest != had.digest) other_misses = true;
        }
    }
    if (!s.known || l.made_profile == "yes" || l.made_profile == "no") {
        for (const auto &m : s.samples) {
            auto it = l.made_samples.find(m.first);
            if (l.made_profile != "yes" || it == l.made_samples.end()) { without += m.second.count; continue; }   // no profile of the module in its packs
            const Recorded &had = it->second, &now = m.second;
            if (now.count > had.count && now.count * 100 >= had.count * (100 + kMoreFunctionsPercent)) new_functions += now.count - had.count;
            else if (now.digest != had.digest && now.count >= had.count && now.samples > had.samples && now.samples * 100 >= had.samples * (100 + kMoreSamplesPercent))
                longer = std::max(longer, had.samples ? (now.samples - had.samples) * 100 / had.samples : 100);
        }
    }
    // The known targets (launcher_model.h, "The second pack"): made targets as written, or for packs made before that row as made profile says, unknown (no made rows) or not
    const std::string made_targets = !l.made_targets.empty() ? l.made_targets : l.made_profile == "unused" ? "unused" : "";
    if (made_targets != "unused") {
        for (const auto &m : s.targets) {
            auto it = l.made_target.find(m.first);
            if (made_targets != "yes" || it == l.made_target.end()) { targets_without += m.second.count; continue; }   // the packs took no targets of the module
            const Recorded &had = it->second, &now = m.second;
            const uint64_t more = now.count > had.count ? now.count - had.count : now.count == had.count && now.digest != had.digest ? 1 : 0;   // at least that many new sites
            if (more && more * 100 >= had.count * kMoreTargetsPercent) new_targets += more;
        }
    }
    if (new_misses) s.reasons.push_back(counted(new_misses, "new JIT miss", "new JIT misses"));
    else if (other_misses) s.reasons.push_back("JIT misses the packs did not take");
    if (new_functions) s.reasons.push_back(counted(new_functions, "new sampled function", "new sampled functions"));
    if (without) s.reasons.push_back(counted(without, "sampled function", "sampled functions") + " the packs were made without");
    if (longer) s.reasons.push_back("a longer session's profile (+" + std::to_string(longer) + " % samples)");
    if (new_targets) s.reasons.push_back(counted(new_targets, "new known call target", "new known call targets"));
    if (targets_without) s.reasons.push_back(counted(targets_without, "known call target", "known call targets") + " the packs were made without");
    if (!s.known && !s.reasons.empty()) s.reasons.push_back("the packs were made before the launch file recorded what they took");
    recompiler();
    return s;
}

Command repack_command(const Game &g, const std::string &mode, const std::string &bundle) {
    Command c; c.mode = mode.empty() ? "balanced" : mode;
    const LaunchFile &l = g.launch;
    if (l.package_dir.empty() || l.packer.empty() || l.python.empty()) {
        c.error = g.launch_path + " names no package folder, packer or Python to pack " + l.title + " again with (a pack made before those rows): pack it once more with "
                  "scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform " + std::string(kPlatform) + ", which writes them";
        return c;
    }
    const Found python = find_python(l, bundle), packer = find_packer(l, bundle), nxaot = find_nxaot(l, bundle);   // the rows', else the bundle's
    for (const Found *f : {&python, &packer, &nxaot}) {
        if (!f->problem.empty()) { c.error = f->problem; return c; }
        if (!f->note.empty()) c.notes.push_back(f->note);
    }
    c.argv = {python.path, packer.path, l.package_dir, "--platform", kPlatform, "--pack-mode", c.mode};
    if (!l.keys.empty()) { c.argv.push_back("-k"); c.argv.push_back(l.keys); }
    if (!nxaot.path.empty()) { c.argv.push_back("--nxaot"); c.argv.push_back(nxaot.path); }
    c.cwd = l.package_dir;
    c.log = S(P(l.log.empty() ? S(P(l.cwd) / "log.txt") : l.log).parent_path() / "repack.txt");
    // PYTHONUNBUFFERED: its lines reach repack.txt as they are said, for the log pane. PYTHONUTF8: repack.txt is a file, and a Python on Windows outside its UTF-8 mode
    // writes a file in the ANSI code page, which has no check mark: the packer's first finished stage ended the repack with UnicodeEncodeError (pack.cmd sets it too)
    c.set = {{"PYTHONUNBUFFERED", "1"}, {"PYTHONUTF8", "1"}};
    return c;
}

// ---- The Vulkan loader, before a start (kNoVulkanStatus) ----
std::string vulkan_loader_problem(const char *lib) {
#if defined(_WIN32)
    std::wstring name = lib ? W(lib) : L"vulkan-1.dll";
    HMODULE h = LoadLibraryW(name.c_str());
    if (h) { FreeLibrary(h); return ""; }
#else
    void *h = dlopen(lib ? lib : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (h) { dlclose(h); return ""; }
#endif
    return "no Vulkan loader (" + std::string(kVulkanLoaderHint) + "): a game would start and stop at once; install your GPU's Vulkan driver and the loader";
}

// ---- Add game ----
static bool is_dir(const std::string &path) { std::error_code ec; return !path.empty() && fs::is_directory(P(path), ec); }
static bool same_path(const std::string &a, const std::string &b) { std::error_code ec; return fs::equivalent(P(a), P(b), ec) && !ec; }

bool is_game_file(const std::string &path) {
    const std::string ext = lower(S(P(path).extension()));
    for (const char *e : kGameFileExts) if (ext == e) return true;
    return false;
}

std::string nsp_folder_of(const std::string &file) { return S(P(file).parent_path()); }

std::string which(const std::string &program, const Env &env) {
    if (program.empty()) return "";
    if (P(program).has_parent_path()) return is_file(program) ? program : "";
#if defined(_WIN32)
    const char sep = ';';
    const std::vector<std::string> names = P(program).has_extension() ? std::vector<std::string>{program} : std::vector<std::string>{program + ".exe", program};
#else
    const char sep = ':';
    const std::vector<std::string> names = {program};
#endif
    std::string path;
    if (!env.get("PATH", &path)) return "";
    size_t a = 0;
    while (a <= path.size()) {
        size_t b = path.find(sep, a);
        if (b == std::string::npos) b = path.size();
        const std::string dir = path.substr(a, b - a);
        for (const auto &n : names) if (!dir.empty() && is_file(S(P(dir) / P(n)))) return S(P(dir) / P(n));
        a = b + 1;
    }
    return "";
}

PackTools find_pack_tools(const std::string &bundle, const Env &env) {
    PackTools t;
    std::string v, py;
    const bool own_python = env.get("PYTHON", &py);   // pack.sh / pack.cmd take PYTHON over the bundle's own
    if (env.get("NXPACKER", &v)) { t.packer = v; t.from = "NXPACKER"; }
    else if (!bundle.empty() && is_file(S(P(bundle) / "VERSION")) && is_file(S(P(bundle) / "scripts" / "build_ipa_from_nsp.py"))) {
        t.packer = S(P(bundle) / "scripts" / "build_ipa_from_nsp.py"); t.from = "the bundle";
        const std::string rel = bundle_python(bundle);
        if (!own_python && !rel.empty()) {   // as pack.sh runs it: the bundle's Python with its own modules only
            t.python = S(P(bundle) / P(rel).make_preferred());
            t.set = {{"PYTHONHOME", ""}, {"PYTHONPATH", ""}, {"PYTHONNOUSERSITE", "1"}};
            if (!is_file(t.python)) { t.problem = "the bundle's Python " + t.python + " is missing: unpack the bundle again"; return t; }
        }
    } else {   // a development tree: the checkout above the launcher's build folder
        fs::path d = P(bundle.empty() ? std::string(".") : bundle);
        std::error_code ec;
        for (int i = 0; i < 6 && !d.empty(); i++) {
            if (fs::is_regular_file(d / "scripts" / "build_ipa_from_nsp.py", ec) && fs::is_directory(d / "dm" / "launcher", ec)) {
                t.packer = S(d / "scripts" / "build_ipa_from_nsp.py"); t.from = "the development tree"; break;
            }
            if (d == d.parent_path()) break;
            d = d.parent_path();
        }
    }
    if (t.packer.empty()) { t.problem = "no packer: " + bundle + " is neither a bundle (VERSION, scripts/build_ipa_from_nsp.py) nor in a checkout; set NXPACKER to build_ipa_from_nsp.py"; return t; }
    if (!is_file(t.packer)) { t.problem = "the packer " + t.packer + " is missing"; return t; }
    if (t.python.empty()) {
        if (own_python) t.python = is_file(py) ? py : which(py, env);
#if defined(_WIN32)
        else if (!(t.python = which("python", env)).empty()) {}
        else if (!(t.python = which("py", env)).empty()) t.python_args = {"-3"};
#else
        else t.python = which("python3", env);
#endif
        if (t.python.empty()) { t.problem = own_python ? "PYTHON names " + py + ", which is not there" : "no Python 3.12 or later on PATH: the packer needs one (or set PYTHON)"; return t; }
    }
    t.set.push_back({"PYTHONUNBUFFERED", "1"});   // its lines reach the log as they are said
    t.set.push_back({"PYTHONUTF8", "1"});         // its output is a file: UTF-8 on Windows too (repack_command)
    return t;
}

KeysFound find_keys(const Config &c, const std::vector<Game> &games, const std::string &bundle, const Env &env) {
    KeysFound k;
    auto look = [&](const std::string &p) {   // by its path only: the file is never opened here
        if (p.empty()) return false;
        if (std::find(k.looked.begin(), k.looked.end(), p) == k.looked.end()) k.looked.push_back(p);
        if (is_file(p)) { k.path = p; return true; }
        return false;
    };
    if (look(c.get("keys"))) return k;
    for (const auto &g : games) if (look(g.launch.keys)) return k;
    if (!bundle.empty() && look(S(P(bundle) / "keys" / "prod.keys"))) return k;
    std::string home;
#if defined(_WIN32)
    if (env.get("USERPROFILE", &home) && look(S(P(home) / ".switch" / "prod.keys"))) return k;
#else
    if (env.get("HOME", &home) && look(S(P(home) / ".switch" / "prod.keys"))) return k;
#endif
    return k;
}

Command add_command(const std::string &file, const std::string &keys, const PackTools &tools) {
    Command c;
    if (!tools.problem.empty()) { c.error = tools.problem; return c; }
    if (!is_file(file)) { c.error = "the game file " + file + " is not there"; return c; }
    if (keys.empty()) { c.error = "no prod.keys: the packer needs it (Choose prod.keys...)"; return c; }
    const std::string dir = nsp_folder_of(file);
    c.argv.push_back(tools.python);
    c.argv.insert(c.argv.end(), tools.python_args.begin(), tools.python_args.end());
    for (const std::string &a : {tools.packer, dir, std::string("--platform"), std::string(kPlatform), std::string("--progress"), std::string("json"), std::string("-k"), keys}) c.argv.push_back(a);
    c.cwd = dir; c.log = S(P(dir) / "nxlauncher-pack.txt"); c.set = tools.set; c.tree = true;
    return c;
}

static double num(const cJSON *o, const char *k, double def = -1) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsNumber(v) ? v->valuedouble : def; }

static std::string grouped(double v) {   // 19592 -> "19,592"
    std::string s = std::to_string((long long)std::llround(v)), out;
    for (size_t i = 0; i < s.size(); i++) { if (i && (s.size() - i) % 3 == 0 && s[i - 1] != '-') out += ','; out += s[i]; }
    return out;
}

static std::string clock_text(double s) {   // m:ss, or h:mm:ss (the packer's hms)
    long t = (long)s, h = t / 3600, m = t / 60 % 60;
    char buf[32];
    if (h) std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", h, m, t % 60); else std::snprintf(buf, sizeof buf, "%ld:%02ld", m, t % 60);
    return buf;
}

PackView read_pack_log(const std::string &text) {
    PackView v;
    for (auto line : lines_of(text)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        cJSON *o = !line.empty() && line[0] == '{' ? cJSON_Parse(line.c_str()) : nullptr;
        const std::string ev = o ? jstr(o, "ev") : "";
        if (ev.empty()) {   // not an event: a line of its own (a traceback, a tool's output)
            if (o) cJSON_Delete(o);
            if (line.rfind("[RED] ", 0) == 0) v.red = line.substr(6);
            v.lines.push_back(line);
            continue;
        }
        v.events++;
        if (ev == "stage") {
            v.k = (int)num(o, "k", 0); v.n = (int)num(o, "n", 0); v.stage = jstr(o, "name"); v.step = jstr(o, "step");
            v.count.clear(); v.note.clear(); v.frac = v.eta = -1; v.in_stage = true;
            v.lines.push_back("[" + std::to_string(v.k) + "/" + std::to_string(v.n) + "] " + v.stage + (v.step.empty() ? "" : " \xC2\xB7 " + v.step));
        } else if (ev == "step") {
            v.step = jstr(o, "step"); v.count.clear(); v.note.clear(); v.frac = v.eta = -1;
        } else if (ev == "progress") {
            v.step = jstr(o, "step"); v.note = jstr(o, "note"); v.frac = num(o, "frac"); v.eta = num(o, "eta");
            const double done = num(o, "done"), total = num(o, "total");
            const std::string unit = jstr(o, "unit");
            const cJSON *est = cJSON_GetObjectItemCaseSensitive(o, "est");
            const std::string about = cJSON_IsTrue(est) ? "~" : "";
            v.count.clear();
            if (done >= 0 && unit == "B") {
                const double d = (total > 0 ? total : done) >= 1073741824.0 ? 1073741824.0 : (total > 0 ? total : done) >= 1048576.0 ? 1048576.0 : 1024.0;
                const char *u = d == 1073741824.0 ? "GB" : d == 1048576.0 ? "MB" : "KB";
                char buf[64];
                if (total > 0) std::snprintf(buf, sizeof buf, "%.1f/%s%.1f %s", done / d, about.c_str(), total / d, u); else std::snprintf(buf, sizeof buf, "%.1f %s", done / d, u);
                v.count = buf;
            } else if (done >= 0) {
                v.count = grouped(done) + (total > 0 ? "/" + about + grouped(total) : "") + (unit.empty() ? "" : " " + unit);
            }
        } else if (ev == "stage_end") {
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(o, "ok");
            const std::string summary = jstr(o, "summary");
            v.lines.push_back("[" + std::to_string((int)num(o, "k", v.k)) + "/" + std::to_string(v.n) + "] " + jstr(o, "name") + ": " + (cJSON_IsFalse(ok) ? "failed" : "done") +
                              " in " + clock_text(num(o, "elapsed", 0)) + (summary.empty() ? "" : " (" + summary + ")"));
            v.in_stage = false; v.frac = v.eta = -1; v.count.clear(); v.note.clear(); v.step.clear();
        } else if (ev == "say") {
            const std::string msg = jstr(o, "msg");
            if (msg.rfind("[RED] ", 0) == 0) v.red = msg.substr(6);
            for (const auto &l : lines_of(msg)) v.lines.push_back(l);
        }
        cJSON_Delete(o);
    }
    return v;
}

std::string pack_failure(const PackView &v, int status, const std::string &nsp_folder, const std::string &keys) {
    if (status == 0) return "";
    if (status == 130 || status == 128 + 2)   // KeyboardInterrupt, or SIGINT's default action
        return "Packing was cancelled. What the packer had finished stays in " + nsp_folder + ", and adding the game again goes on from there.";
    const std::string red = v.red;
    if (!keys.empty() && red == "no such file: " + keys)
        return "The packer found no prod.keys at " + keys + ". Choose the file with Choose prod.keys...: the packer reads keys only from the file it is given.";
    if (red.find("more than one game in the folder") != std::string::npos) {
        std::string ids = red.substr(red.rfind(':') + 1);
        while (!ids.empty() && ids[0] == ' ') ids.erase(0, 1);
        return "The folder " + nsp_folder + " holds the files of more than one game (" + ids + "), and the packer packs one game per folder. Move each game's files into a "
               "folder of its own, then add the game from there.";
    }
    if (red.rfind("no NSP with a readable title ID", 0) == 0)
        return "No file in " + nsp_folder + " says which game it is (the packer: " + red + ").";
    if (red.rfind("less than ", 0) == 0 && red.find(" GB free on the volume of ") != std::string::npos) {   // the packer's check before it extracts
        const size_t of = red.find(" GB free on the volume of ");
        return "Not enough free disk space: the packer needs at least " + red.substr(10, of - 10) + " GB free on the drive of " + red.substr(of + 26) +
               " before it extracts the game. Free some space there, or move the game's files to a folder on another drive and add the game from there.";
    }
    if (red.rfind("no base game NSP found", 0) == 0)
        return "The folder " + nsp_folder + " holds an update or DLC but not the base game: put the base game's file beside them.";
    char buf[32]; std::snprintf(buf, sizeof buf, "%d", status);
    if (red.empty()) return std::string("The packer stopped with status ") + buf + " and no message: its whole output is in " + S(P(nsp_folder) / "nxlauncher-pack.txt") + ".";
    return std::string("The packer stopped (status ") + buf + "): " + red;
}

static int64_t unix_time(const fs::file_time_type &t) {   // a file's time in seconds since 1970 (C++17 has no clock_cast)
    const auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
    return (int64_t)std::chrono::system_clock::to_time_t(sys);
}

std::string packed_build(const std::string &nsp_folder, int64_t since, const std::string &platform) {
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(P(nsp_folder), ec)) {
        if (!is_build_dir(e)) continue;
        const fs::path launch = e.path() / P("launch-" + platform + ".tsv");
        std::error_code ec2;
        if (fs::is_regular_file(launch, ec2) && unix_time(fs::last_write_time(launch, ec2)) >= since - 1) return S(e.path());
    }
    return "";
}

std::vector<std::string> nsp_folders(const Config &c) { return c.all("nsp_folder"); }

bool remember_folder(Config &c, const std::string &games_dir, const std::string &nsp_folder) {
    if (same_path(nsp_folder, games_dir) || same_path(S(P(nsp_folder).parent_path()), games_dir)) return false;   // find_games of the games folder reaches it
    for (const auto &f : nsp_folders(c)) if (f == nsp_folder || same_path(f, nsp_folder)) return false;
    c.add("nsp_folder", nsp_folder);
    return true;
}

std::vector<Game> find_all_games(const std::string &dir, const Config &c, const std::string &platform, const std::string &bundle, const Env &env) {
    std::vector<Game> out = find_games(dir, platform, bundle, env);
    for (const auto &f : nsp_folders(c)) {
        if (!is_dir(f)) continue;   // a folder moved or on a drive not there now: kept in launcher.txt, listed again once it is back
        for (auto &g : find_games(f, platform, bundle, env)) {
            bool have = false;
            for (const auto &h : out) if (same_path(h.dir, g.dir)) { have = true; break; }
            if (!have) out.push_back(std::move(g));
        }
    }
    sort_games(&out);
    return out;
}

// ---- The saves ----
Saves save_place(const Game &g, const Env &env) {
    Saves s; std::string v;
    if (env.get("DM_SAVE", &v)) s.folder = v;
    if (s.folder.empty() && !(env.get("DM_SAVED_CHOICES", &v) && v == "0"))
        for (const auto &e : read_env_file(g.launch.choices)) if (name_key(e.key) == name_key("DM_SAVE") && !e.value.empty()) { s.folder = e.value; break; }
    if (s.folder.empty()) for (const auto &e : g.launch.env) if (name_key(e.key) == name_key("DM_SAVE") && !e.value.empty()) { s.folder = e.value; break; }
    if (s.folder.empty()) { s.problem = "the launch file names no save folder (no DM_SAVE row): pack the game again"; return s; }
    while (s.folder.size() > 1 && (s.folder.back() == '/' || s.folder.back() == '\\')) s.folder.pop_back();   // as the host takes it (h_fs_MountSave)
    s.backups = s.folder + "-backups";
    for (const std::string &d : {s.folder, s.folder + "_device"}) if (is_dir(d)) s.present.push_back(d);
    if (s.present.empty()) s.problem = "no save yet: the game makes " + s.folder + " when it first saves";
    return s;
}

static void tree_count(const fs::path &dir, uint64_t *files, uint64_t *bytes) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code ec2;
        if (it->is_regular_file(ec2)) { (*files)++; *bytes += (uint64_t)it->file_size(ec2); }
    }
}

static Backup backup_at(const fs::path &dir) {
    Backup b; b.name = S(dir.filename()); b.path = S(dir);
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (!e.is_directory(ec2)) continue;
        b.folders.push_back(S(e.path().filename()));
        tree_count(e.path(), &b.files, &b.bytes);
    }
    std::sort(b.folders.begin(), b.folders.end());
    return b;
}

std::vector<Backup> list_backups(const Saves &s) {
    std::vector<Backup> out;
    std::error_code ec;
    if (s.backups.empty()) return out;
    for (const auto &e : fs::directory_iterator(P(s.backups), ec)) {
        std::error_code ec2;
        const std::string n = S(e.path().filename());
        if (!e.is_directory(ec2) || n.size() < 8 || n.compare(n.size() - 8, 8, ".partial") == 0) continue;   // a backup being made, or one a crash cut short
        out.push_back(backup_at(e.path()));
    }
    std::sort(out.begin(), out.end(), [](const Backup &a, const Backup &b) { return a.name > b.name; });   // the dated names sort by time
    return out;
}

static std::string stamp(int64_t t) {   // a folder's name on every system: no ':'
    std::time_t tt = (std::time_t)t; std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32]; std::strftime(buf, sizeof buf, "%Y-%m-%d_%H-%M-%S", &tm);
    return buf;
}

bool backup_saves(const Saves &s, int64_t now, Backup *out, std::string *err) {
    if (s.present.empty()) { *err = s.problem.empty() ? "no save to back up" : s.problem; return false; }
    std::error_code ec;
    std::string name = stamp(now);
    for (int i = 2; fs::exists(P(s.backups) / P(name), ec) || fs::exists(P(s.backups) / P(name + ".partial"), ec); i++) name = stamp(now) + "-" + std::to_string(i);
    const fs::path dst = P(s.backups) / P(name), tmp = P(s.backups) / P(name + ".partial");   // made under another name, renamed when whole: a listed backup is complete
    fs::create_directories(tmp, ec);
    if (ec) { *err = "cannot make " + S(tmp) + ": " + ec.message(); return false; }
    for (const auto &d : s.present) {
        fs::copy(P(d), tmp / P(d).filename(), fs::copy_options::recursive, ec);
        if (ec) { *err = "cannot copy " + d + ": " + ec.message(); fs::remove_all(tmp, ec); return false; }
    }
    fs::rename(tmp, dst, ec);
    if (ec) { *err = "cannot name the backup " + S(dst) + ": " + ec.message(); fs::remove_all(tmp, ec); return false; }
    if (out) *out = backup_at(dst);
    return true;
}

static std::string size_text(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1048576) std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu bytes", (unsigned long long)bytes);
    return buf;
}

std::string restore_prompt(const Saves &s, const Backup &b) {
    std::string replaced, removed;
    for (const std::string &d : {s.folder, s.folder + "_device"}) {
        const bool in_backup = std::find(b.folders.begin(), b.folders.end(), S(P(d).filename())) != b.folders.end();
        if (!is_dir(d)) continue;
        (in_backup ? replaced : removed) += (in_backup ? replaced : removed).empty() ? d : " and " + d;
    }
    std::string t = "Put back the saves of " + b.name + " (" + std::to_string(b.files) + " file" + (b.files == 1 ? "" : "s") + ", " + size_text(b.bytes) + ")?";
    if (!replaced.empty()) t += " This replaces the current save folder " + replaced + ".";
    if (!removed.empty()) t += " " + removed + ", which the backup does not have, is removed.";
    if (replaced.empty() && removed.empty()) t += " There is no current save to replace.";
    else t += " The current saves are backed up first, so this can be undone.";
    return t;
}

bool restore_saves(const Saves &s, const Backup &b, bool running, int64_t now, Backup *safety, std::string *err) {
    if (running) { *err = "the game is running: stop it first (it may be writing its save)"; return false; }
    if (s.folder.empty()) { *err = s.problem; return false; }
    if (!is_dir(b.path)) { *err = "the backup " + b.path + " is gone"; return false; }
    if (!s.present.empty() && !backup_saves(s, now, safety, err)) { *err = "the current saves could not be backed up first, so nothing was replaced: " + *err; return false; }
    std::error_code ec;
    for (const std::string &d : {s.folder, s.folder + "_device"}) {
        const fs::path src = P(b.path) / P(d).filename(), tmp = P(d + ".restoring"), old = P(d + ".replaced");
        const bool have = fs::is_directory(src, ec);
        fs::remove_all(tmp, ec); fs::remove_all(old, ec);
        if (have) {   // the backup's copy beside the folder first, then the swap: an error leaves the current folder as it was
            fs::copy(src, tmp, fs::copy_options::recursive, ec);
            if (ec) { *err = "cannot copy " + S(src) + ": " + ec.message() + (safety && !safety->path.empty() ? " (the current saves are in " + safety->path + ")" : ""); fs::remove_all(tmp, ec); return false; }
        }
        if (fs::exists(P(d), ec)) {
            fs::rename(P(d), old, ec);
            if (ec) { *err = "cannot move " + d + " aside: " + ec.message(); fs::remove_all(tmp, ec); return false; }
        }
        if (have) {
            fs::rename(tmp, P(d), ec);
            if (ec) { *err = "cannot put " + d + " in place: " + ec.message() + (safety && !safety->path.empty() ? " (the saves it replaced are in " + safety->path + ")" : ""); return false; }
        }
        fs::remove_all(old, ec);
    }
    return true;
}

// ---- The dialogs' stand-in ----
static std::string next_answer(std::vector<std::string> &a) {
    if (a.empty()) return "";
    std::string s = a.front(); a.erase(a.begin());
    return s;
}
void AnswerDialogs::open_file(const std::string &title, const std::vector<Filter> &, const std::string &, Done done) { asked.push_back("file " + title); done(next_answer(answers)); }
void AnswerDialogs::open_folder(const std::string &title, const std::string &, Done done) { asked.push_back("folder " + title); done(next_answer(answers)); }
bool AnswerDialogs::show_folder(const std::string &folder, std::string *) { shown.push_back(folder); return true; }

std::vector<Dialogs::Filter> game_file_filters() { return {{"Switch games (NSP, XCI, NSZ, XCZ)", "nsp;xci;nsz;xcz"}, {"All files", "*"}}; }

}   // namespace nxl
