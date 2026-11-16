// The graceful exit (hle_lifecycle.c dm_host_shutdown, what SIGTERM runs): the session's files are written in full before the process ends, however long they take, and
// the suspend after them keeps its 1.5 s bound. A child runs the real dm_host_shutdown with stand-ins: its profile writer takes 2.5 s (a long sample profile, misses,
// targets and bindings under load) and writes its file as aot_profile.c does, a temporary name then a rename; the suspend returns at once. Before the fix the 1.5 s bound,
// armed before the files, ended the process in the middle of the write and left the temporary file (a Linux player's <module>.bind.tsv.tmp after a timeout's SIGTERM).
#include "../hle_lifecycle.c"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_dir[600];
uint64_t now_ns(void) { return plat_now_ns(); }
void hlecount_flush(void) {}
void dm_audio_suspend(int on) { (void)on; }
void dm_gfx_suspend(int on) { (void)on; }
int dm_aot_targets_write(void) { return 0; }
int dm_aot_profile_write(void) {   // as aot_profile.c writes a file: the temporary name first, renamed over the file at the end
    char tmp[700], path[700];
    snprintf(path, sizeof path, "%s/main.bind.tsv", g_dir); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return 1;
    fputs("# the first line\n", f); fflush(f);
    plat_sleep_ns(2500000000ull);   // longer than the suspend's 1.5 s bound
    fputs("0x10 0x20 sym\n", f); fclose(f);
    return rename(tmp, path);
}

int main(void) {
    int fails = 0;
    char cwd[400];
    if (!getcwd(cwd, sizeof cwd)) return 2;
    snprintf(g_dir, sizeof g_dir, "%s/t_lifecycle.tmp", cwd);
    mkdir(g_dir, 0755);
    char file[700], tmp[700];
    snprintf(file, sizeof file, "%s/main.bind.tsv", g_dir); snprintf(tmp, sizeof tmp, "%s.tmp", file);
    remove(file); remove(tmp);
    pid_t c = fork();
    if (c < 0) return 2;
    if (!c) { dm_host_shutdown("the test"); _exit(3); }   // dm_host_shutdown ends the process itself (status 0)
    int st = 0; waitpid(c, &st, 0);
    struct stat sb;
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0; fails += !ok;
    printf("%-5s the graceful exit ended the process with status 0\n", ok ? "ok" : "FAIL");
    ok = !stat(file, &sb) && sb.st_size > 20; fails += !ok;
    printf("%-5s the session's file written in full though it took 2.5 s (%s)\n", ok ? "ok" : "FAIL", file);
    ok = stat(tmp, &sb) != 0; fails += !ok;
    printf("%-5s no temporary file left (%s)\n", ok ? "ok" : "FAIL", tmp);
    printf("t_lifecycle: 3 checks, %d failed\n", fails);
    return fails != 0;
}
