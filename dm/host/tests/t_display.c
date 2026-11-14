// The display size games are told with Native resolution (hle_display.c): the picture that fits the screen, the settings that turn it on and off, DM_DISPLAY_SIZE
#include "../hle.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

static int test_w, test_h;   // the screen the front end reports (0: not known)
int front_display_pixels(int *w, int *h) { if (!test_w) return 0; *w = test_w; *h = test_h; return 1; }

static void fit(int dw, int dh, int ew, int eh) { int w = 0, h = 0; dm_display_fit(dw, dh, &w, &h); assert(w == ew && h == eh); }

// dm_display_target decides once per process, so each case runs in a child
static int run_case(const char *native, const char *size, int sw, int sh, int *w, int *h) {
    int pipefd[2]; if (pipe(pipefd)) return -1;
    pid_t pid = fork();
    if (!pid) {
        test_w = sw; test_h = sh;
        if (native) setenv("DM_NATIVE_RESOLUTION", native, 1); else unsetenv("DM_NATIVE_RESOLUTION");
        if (size) setenv("DM_DISPLAY_SIZE", size, 1); else unsetenv("DM_DISPLAY_SIZE");
        int a = 0, b = 0, r = dm_display_target(&a, &b); int again = dm_display_target(&a, &b); assert(r == again);
        int out[3] = {r, a, b}; (void)!write(pipefd[1], out, sizeof out); _exit(0);
    }
    close(pipefd[1]);   // an assertion failing in the child must end the read, not hang it
    int out[3] = {-1, 0, 0}; (void)!read(pipefd[0], out, sizeof out); int st; waitpid(pid, &st, 0); assert(WIFEXITED(st) && !WEXITSTATUS(st));
    close(pipefd[0]); *w = out[1]; *h = out[2]; return out[0];
}

int main(void) {
    // the 16:9 picture inside the screen, either orientation, at most 4K, at least 720p, even numbers
    fit(2940, 1652, 2936, 1652); fit(2560, 1664, 2560, 1440); fit(1664, 2560, 2560, 1440); fit(3456, 2234, 3456, 1944);
    fit(1920, 1080, 1920, 1080); fit(5120, 2880, 3840, 2160); fit(800, 600, 1280, 720); fit(2796, 1290, 2293 & ~1, 1290);
    int w, h;
    assert(run_case("1", NULL, 2560, 1664, &w, &h) == 1 && w == 2560 && h == 1440);   // on: the screen's picture
    assert(run_case("0", NULL, 2560, 1664, &w, &h) == 0);                               // off: the Switch's sizes
    assert(run_case("1", NULL, 0, 0, &w, &h) == 0);                                     // the screen is not known
    assert(run_case("1", "1920x1080", 2560, 1664, &w, &h) == 1 && w == 1920 && h == 1080);   // DM_DISPLAY_SIZE wins
    assert(run_case("1", "off", 2560, 1664, &w, &h) == 0);                              // and so does off
    assert(run_case("1", "bad", 2560, 1664, &w, &h) == 1 && w == 2560 && h == 1440);   // an unreadable size is ignored
    assert(run_case(NULL, NULL, 2560, 1664, &w, &h) == DM_GAME_NATIVE_RES);             // unset: the profile's default on the Mac
    return 0;
}
