// Mac app launcher (CFBundleExecutable of the .app that dm/mac/pack_app.py packs): sets the host's environment from the bundle, then runs the Mac host (Contents/MacOS/dmhost) as a child.
// The host is not exec'ed: macOS privacy checks (TCC) attribute a child's file access to the app that launched it, while a launcher that hands its process over (a shell script exec'ing
// dmhost) gets EPERM on every open on a removable volume, with no prompt, signed or not. The launcher forwards SIGTERM / SIGINT / SIGHUP to the host and returns its exit code.
// Bundle layout: Resources/romfs.img (the game's RomFS image; legacy romfs/ also supported), Resources/msl/<b-chain | vk | gl> (the Mac shader libraries), Resources/aoc/<index> (DLC), Resources/pso (the pipeline archive made
// offline, handed to the host as DM_PSO_BUNDLE as the iOS app's main.m does), Resources/dm (the rewritten modules, the shader tables and pairs, font/, env.txt). Saves, the pipeline list and temporary files go to ~/Library/Application Support/nxnative/<title ID>/, the host's output to ~/Library/Logs/nxnative/<title ID>.log
// (the previous run's kept as <title ID>.prev.log). Environment switches: one KEY=VALUE per line in <Application Support folder>/env.txt, then Resources/dm/env.txt (the first one wins; neither
// replaces a variable already set), as the iOS app reads Documents/env.txt and dm/env.txt.
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "romfs_image.h"

extern char **environ;
static pid_t g_child;

static void forward(int sig) { if (g_child > 0) kill(g_child, sig); }

static int info_string(const char *key, char *out, size_t n) {   // a string key of the bundle's Info.plist
    CFBundleRef b = CFBundleGetMainBundle(); if (!b) return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(b, k); CFRelease(k);
    return v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetCString(v, out, (CFIndex)n, kCFStringEncodingUTF8) && *out;
}

static void alert(const char *title, const char *msg) {   // a dialog (the app has no window of its own), and the same line on stderr
    fprintf(stderr, "[RED] %s\n", msg);
    char *script = "on run argv\ndisplay alert (item 1 of argv) message (item 2 of argv) as critical\nend run";
    char *argv[] = {"osascript", "-e", script, (char *)title, (char *)msg, NULL}; pid_t pid; int st;
    if (!posix_spawnp(&pid, "osascript", NULL, NULL, argv, environ)) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}

static int modules_present(const char *dm) {   // the packer's inventory detects even a missing binary/table pair
    CFBundleRef b = CFBundleGetMainBundle(); if (!b) return 0;
    CFTypeRef files = CFBundleGetValueForInfoDictionaryKey(b, CFSTR("DMModuleFiles"));
    if (!files || CFGetTypeID(files) != CFArrayGetTypeID() || !CFArrayGetCount(files)) return 0;
    for (CFIndex i = 0; i < CFArrayGetCount(files); i++) {
        CFTypeRef v = CFArrayGetValueAtIndex(files, i); char name[PATH_MAX], path[PATH_MAX]; struct stat st;
        if (CFGetTypeID(v) != CFStringGetTypeID() || !CFStringGetCString(v, name, sizeof name, kCFStringEncodingUTF8) || !*name || strchr(name, '/')) return 0;
        int n = snprintf(path, sizeof path, "%s/%s", dm, name);
        if (n < 0 || (size_t)n >= sizeof path || stat(path, &st) || !S_ISREG(st.st_mode) || access(path, R_OK)) return 0;
    }
    return 1;
}

static void mkdirs(const char *path) {   // mkdir -p
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

static void env_file(const char *path) {   // KEY=VALUE lines, # starts a comment; a variable already set stays
    FILE *f = fopen(path, "r"); if (!f) return;
    char ln[4096];
    while (fgets(ln, sizeof ln, f)) {
        char *s = ln; while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s); while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        char *eq = strchr(s, '=');
        if (*s == '#' || !eq || eq == s) continue;
        *eq = 0; char *k = s; for (char *t = eq - 1; t > k && (*t == ' ' || *t == '\t'); t--) *t = 0;
        if (!getenv(k)) { setenv(k, eq + 1, 0); fprintf(stderr, "launcher: env %s=%s (%s)\n", k, eq + 1, path); }
    }
    fclose(f);
}

int main(void) {
    char self[PATH_MAX], exe[PATH_MAX]; uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) || !realpath(self, exe)) return 1;
    char contents[PATH_MAX], res[PATH_MAX], host[PATH_MAX], dm[PATH_MAX], romfs[PATH_MAX], p[PATH_MAX];
    snprintf(contents, sizeof contents, "%s", exe); *strrchr(contents, '/') = 0; *strrchr(contents, '/') = 0;   // .../Contents/MacOS/<launcher> → .../Contents
    snprintf(res, sizeof res, "%s/Resources", contents); snprintf(host, sizeof host, "%s/MacOS/dmhost", contents);
    snprintf(dm, sizeof dm, "%s/dm", res); snprintf(romfs, sizeof romfs, "%s/romfs", res);
    char name[256] = "nxnative", id[64] = "", marker[512] = "", lang[32] = "";
    if (!info_string("CFBundleName", name, sizeof name)) snprintf(name, sizeof name, "nxnative");
    info_string("DMRomfsMarker", marker, sizeof marker); info_string("DMLanguage", lang, sizeof lang);
    if (!info_string("DMTitleID", id, sizeof id) || strchr(id, '/')) { alert(name, "This app has no title ID in its Info.plist (DMTitleID): pack it again with build_ipa_from_nsp.py --platform mac-app."); return 1; }
    struct stat st; snprintf(p, sizeof p, "%s/romfs.img", res);
    int valid_romfs;
    if (!stat(p, &st)) {
        romfs_img *image = romfs_image_open(p); valid_romfs = image != NULL;
        romfs_image_close(image); snprintf(romfs, sizeof romfs, "%s", p);
    } else { snprintf(p, sizeof p, "%s/%s", romfs, marker); valid_romfs = !stat(p, &st); }
    if (access(host, X_OK) || !valid_romfs || !modules_present(dm)) {
        alert(name, "The game data inside this app is missing or incomplete (Contents/Resources/romfs.img, Contents/Resources/dm or Contents/MacOS/dmhost): pack it again with build_ipa_from_nsp.py --platform mac-app.");
        return 1;
    }
    const char *home = getenv("HOME"); if (!home || !*home) return 1;
    char sup[PATH_MAX], save[PATH_MAX], temp[PATH_MAX], logs[PATH_MAX], log[PATH_MAX], prev[PATH_MAX];
    snprintf(sup, sizeof sup, "%s/Library/Application Support/nxnative/%s", home, id);
    snprintf(save, sizeof save, "%s/save", sup); snprintf(temp, sizeof temp, "%s/temp", sup);
    snprintf(logs, sizeof logs, "%s/Library/Logs/nxnative", home);
    snprintf(log, sizeof log, "%s/%s.log", logs, id); snprintf(prev, sizeof prev, "%s/%s.prev.log", logs, id);
    mkdirs(save); mkdirs(temp); mkdirs(logs);
    rename(log, prev);
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); setvbuf(stdout, NULL, _IOLBF, 0); }
    fprintf(stderr, "launcher: %s (%s), host %s\n", name, id, host);
    snprintf(p, sizeof p, "%s/env.txt", sup); env_file(p);
    snprintf(p, sizeof p, "%s/env.txt", dm); env_file(p);
    setenv("DM_ROOT", sup, 0); setenv("DM_ROMFS", romfs, 0); setenv("DM_SAVE", save, 0); setenv("DM_TEMP", temp, 0);
    snprintf(p, sizeof p, "%s/msl", res); setenv("DM_MSL_DIR", p, 0);
    snprintf(p, sizeof p, "%s/font", dm); setenv("DM_FONT_DIR", p, 0);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dm); if (!stat(p, &st)) setenv("DM_NVN_SHMAP", p, 0);
    snprintf(p, sizeof p, "%s/aoc", res); if (!stat(p, &st)) setenv("DM_AOC_DIR", p, 0);
    snprintf(p, sizeof p, "%s/pso", res); if (!stat(p, &st)) setenv("DM_PSO_BUNDLE", p, 0);   // the bases of every shader pair: the first start builds none
    if (*lang) setenv("DM_LANG", lang, 0);
    setenv("DM_WINDOW", "1", 0); setenv("DM_AUDIO", "1", 0);
    if (chdir(sup)) { fprintf(stderr, "[RED] launcher: cannot enter %s\n", sup); return 1; }
    struct sigaction sa = {.sa_handler = forward};   // no SA_RESTART: waitpid below sees EINTR and waits again
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    char *argv[] = {host, dm, NULL};
    int err = posix_spawn(&g_child, host, NULL, NULL, argv, environ);
    if (err) { fprintf(stderr, "[RED] launcher: cannot start %s: %s\n", host, strerror(err)); return 1; }
    int status;
    while (waitpid(g_child, &status, 0) < 0) if (errno != EINTR) return 1;
    if (WIFSIGNALED(status)) { fprintf(stderr, "launcher: the host stopped on signal %d\n", WTERMSIG(status)); return 128 + WTERMSIG(status); }
    fprintf(stderr, "launcher: the host exited with %d\n", WEXITSTATUS(status));
    return WEXITSTATUS(status);
}
