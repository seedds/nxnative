// Stop and Cancel (launcher_model.h, "Stop" and interrupt) against stand-ins started from this test, which acts as them by NXL_STUB in its environment:
//   host     installs the host's own way in, plat_on_terminate (dm/host/plat: SIGTERM on Linux, the named event Local\nxnative-stop-<pid> on Windows), whose callback prints the
//            host's graceful-exit lines (hle_lifecycle.c dm_host_shutdown) and exits with status 0. request_stop ends it, and ended_how says it stopped gracefully
//   late     the same, its handler installed only after a second: on Windows request_stop fails until the event exists (the window asks again on each frame); on Linux the
//            signal's default action ends it first (status 143), which ended_how says
//   deaf     takes no stop request (SIGTERM ignored; no event): request_stop leaves it running, force_stop ends it, ended_how says Force stop did
//   tree     a tree (Command::tree) whose own child (writer) appends a line to a file every 50 ms: force_stop and interrupt end both, so the file stops growing
// The window's Stop and Add game's Cancel call these same functions (t_launcher_window drives them). Under Wine the Windows side runs as far as Wine implements it: the summary
// line says when the Ctrl+C of interrupt was not checked
#include "launcher_model.h"
#include "fake_games.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

extern "C" void plat_on_terminate(void (*fn)(const char *why));   // dm/host/plat/plat.h (C)

using namespace nxl;
namespace fs = std::filesystem;

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); } else std::printf("ok    %s\n", std::string(msg).c_str()); } while (0)

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---- The stand-ins ----
static void on_term(const char *why) {   // the host's lines, then its exit (dm_host_shutdown)
    std::printf("[dm] received %s: graceful exit\n", why);
    std::printf("[dm] graceful exit done (0.001 s)\n");
    std::fflush(stdout);
    std::_Exit(0);
}

static int writer(const char *file) {   // a line every 50 ms for at most 60 s
    for (int i = 0; i < 1200; i++) {
        if (FILE *f = std::fopen(file, "ab")) { std::fprintf(f, "%d\n", i); std::fclose(f); }
        sleep_ms(50);
    }
    return 0;
}

static int stub(const std::string &kind) {
    if (kind == "writer") return writer(std::getenv("NXL_WRITER_FILE"));
    if (kind == "host") plat_on_terminate(on_term);
    if (kind == "late") {   // ready at once, its handler a second later
        std::printf("stub late: ready\n"); std::fflush(stdout);
        sleep_ms(1000); plat_on_terminate(on_term);
        for (int i = 0; i < 600; i++) sleep_ms(100);
        return 3;
    }
#if !defined(_WIN32)
    if (kind == "deaf") signal(SIGTERM, SIG_IGN);
#endif
    if (kind == "tree") {   // its writer, in its tree: Linux, a fork in the same process group; Windows, a process that inherits the console and is in the job
#if defined(_WIN32)
        SetEnvironmentVariableW(L"NXL_STUB", L"writer");
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring line = L"\"" + std::wstring(self) + L"\"";
        STARTUPINFOW si = {}; si.cb = sizeof si; PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(self, &line[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { std::printf("cannot start the writer\n"); return 2; }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
#else
        if (fork() == 0) _exit(writer(std::getenv("NXL_WRITER_FILE")));
#endif
    }
    std::printf("stub %s: ready\n", kind.c_str());
    std::fflush(stdout);
    for (int i = 0; i < 600; i++) sleep_ms(100);   // at most a minute: a test that failed leaves nothing behind for long
    return 3;
}

// ---- The test ----
static fs::path g_root;

static bool start(const std::string &kind, Child *ch, bool tree = false) {
    Command c;
    c.argv = {self_path()};
    c.set = {{"NXL_STUB", kind}, {"NXL_WRITER_FILE", fake::str(g_root / (kind + ".lines"))}};
    c.cwd = fake::str(g_root); c.log = fake::str(g_root / (kind + ".log")); c.tree = tree;
    std::string err;
    if (!spawn(c, ch, &err)) { std::fprintf(stderr, "FAIL: cannot start the %s stand-in: %s\n", kind.c_str(), err.c_str()); return false; }
    for (int i = 0; i < 200; i++) {   // up to 10 s for its ready line
        if (fake::read(g_root / (kind + ".log")).find("ready") != std::string::npos) return true;
        if (poll(ch)) break;
        sleep_ms(50);
    }
    std::fprintf(stderr, "FAIL: the %s stand-in did not get ready (status %d): %s\n", kind.c_str(), ch->status, fake::read(g_root / (kind + ".log")).c_str());
    return false;
}

static std::vector<std::string> log_of(const std::string &kind) { return tail_lines(fake::str(g_root / (kind + ".log")), 40); }

static bool still_growing(const std::string &kind) {   // the writer's file grew within 400 ms
    const uintmax_t a = fs::exists(g_root / (kind + ".lines")) ? fs::file_size(g_root / (kind + ".lines")) : 0;
    sleep_ms(400);
    const uintmax_t b = fs::exists(g_root / (kind + ".lines")) ? fs::file_size(g_root / (kind + ".lines")) : 0;
    return b > a;
}

static bool under_wine() {
#if defined(_WIN32)
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version");
#else
    return false;
#endif
}

int main() {
    if (const char *k = std::getenv("NXL_STUB")) return stub(k);
    g_root = fake::tmp_root("t_launcher_stop");
    std::string err;

    // A host that takes the request: it ends with status 0 after its graceful-exit lines
    Child host;
    if (start("host", &host)) {
        CHECK(request_stop(&host, &err), "request_stop reaches a host that installed plat_on_terminate: " + err);
        CHECK(wait_for(&host, 5000), "the host ended within 5 s of Stop");
        CHECK(host.status == 0, "with the graceful exit's status 0: " + std::to_string(host.status));
        const std::string how = ended_how(host.status, true, false, 0.2, log_of("host"));
        CHECK(how.rfind("It stopped gracefully (status 0", 0) == 0, "ended_how: it stopped gracefully: " + how);
#if defined(_WIN32)
        CHECK(fake::read(g_root / "host.log").find("received stop request: graceful exit") != std::string::npos, "the host's callback was the stop request's (the named event)");
#else
        CHECK(fake::read(g_root / "host.log").find("received SIGTERM: graceful exit") != std::string::npos, "the host's callback was SIGTERM's");
#endif
    }
    if (host.running) force_stop(&host, nullptr), wait_for(&host, 5000);

    // A host asked before it installed its handler
    Child late;
    if (start("late", &late)) {
        const bool first = request_stop(&late, &err);
#if defined(_WIN32)
        CHECK(!first && err.find("nxnative-stop-" + std::to_string(late.pid)) != std::string::npos, "Windows: no event before the host has it, and the error names it: " + err);
        bool taken = false;
        for (int i = 0; i < 100 && !taken; i++) { sleep_ms(50); taken = request_stop(&late, nullptr); }   // as the window asks on each frame
        CHECK(taken, "the request is taken once the host made its event");
        CHECK(wait_for(&late, 5000) && late.status == 0, "and the host ended gracefully: " + std::to_string(late.status));
#else
        CHECK(first, "Linux: SIGTERM is sent at once");
        CHECK(wait_for(&late, 5000) && late.status == 128 + SIGTERM, "its default action ended the host before its handler: status " + std::to_string(late.status));
        const std::string how = ended_how(late.status, true, false, 0.1, log_of("late"));
        CHECK(how.rfind("Stop ended it before the host had its exit handler", 0) == 0, "ended_how says so: " + how);
#endif
    }
    if (late.running) force_stop(&late, nullptr), wait_for(&late, 5000);

    // A process that takes no stop request: Force stop ends it
    Child deaf;
    if (start("deaf", &deaf)) {
        err.clear();
        const bool asked = request_stop(&deaf, &err);
#if defined(_WIN32)
        CHECK(!asked && !err.empty(), "Windows: no event, so no request: " + err);
#else
        CHECK(asked, "Linux: SIGTERM sent (and ignored)");
#endif
        CHECK(!wait_for(&deaf, 1000), "it is still running a second later");
        err.clear();
        CHECK(force_stop(&deaf, &err), "force_stop: " + err);
        CHECK(wait_for(&deaf, 5000), "it ended once forced");
        const std::string how = ended_how(deaf.status, true, true, 25.0, log_of("deaf"));
        CHECK(how.rfind("Force stop ended it", 0) == 0, "ended_how says Force stop did: " + how);
    }
    if (deaf.running) force_stop(&deaf, nullptr), wait_for(&deaf, 5000);
    CHECK(ended_how(0, false, false, 0, {}).empty(), "a game that ended by itself: ended_how leaves it to the caller");

    // A tree: force_stop ends the stand-in and its writer
    Child tree;
    if (start("tree", &tree, true)) {
        CHECK(still_growing("tree"), "the tree's writer runs");
        err.clear();
        CHECK(force_stop(&tree, &err), "force_stop of the tree: " + err);
        CHECK(wait_for(&tree, 5000), "the stand-in ended");
        sleep_ms(200);
        CHECK(!still_growing("tree"), "and its writer with it (the process group; the job object)");
    }
    if (tree.running) force_stop(&tree, nullptr), wait_for(&tree, 5000);

    // Cancel: interrupt, the Ctrl+C of a terminal, ends the stand-in and its writer
    bool wine_skip = false;
    fs::rename(g_root / "tree.log", g_root / "tree-force.log");
    fs::rename(g_root / "tree.lines", g_root / "tree-force.lines");
    Child cancel;
    if (start("tree", &cancel, true)) {
        CHECK(still_growing("tree"), "the tree's writer runs again");
        err.clear();
        const bool sent = interrupt(&cancel, &err);
        if (!sent && under_wine()) { wine_skip = true; std::printf("skip  interrupt under Wine: %s (not checked here)\n", err.c_str()); }
        else {
            CHECK(sent, "interrupt: " + err);
            CHECK(wait_for(&cancel, 5000), "the stand-in ended on Ctrl+C");
            sleep_ms(200);
            CHECK(!still_growing("tree"), "and its writer with it");
#if !defined(_WIN32)
            CHECK(cancel.status == 128 + SIGINT, "status 130, as the packer's KeyboardInterrupt ends: " + std::to_string(cancel.status));
#endif
        }
    }
    if (cancel.running) force_stop(&cancel, nullptr), wait_for(&cancel, 5000);

    std::printf("t_launcher_stop: %d checks, %d failed%s\n", checks, fails, wine_skip ? " (interrupt not checked under Wine)" : "");
    return fails ? 1 : 0;
}
