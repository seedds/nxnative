// The runtime profile (hle_profile.c): a DM_PROFILE file whose values differ from the compiled game_profile.h reaches dm_profile(),
// and without DM_PROFILE the compiled values come back. One process per case (the profile loads once), the test re-execs itself.
// Build: clang -O1 -I.. -I../gen/<game> -o build/t_profile_runtime t_profile_runtime.c ../hle_profile.c
#include "../dm.h"
#include "../hle_profile.h"
#include <assert.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

struct dm_flags g_flag;

#define CHECK(c) do { if (!(c)) { printf("[RED] %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int child_file(const char *path) {
    setenv("DM_PROFILE", path, 1);
    const struct dm_profile *p = dm_profile();
    CHECK(p == dm_prof());
    CHECK(!strcmp(p->id, "0100000000001234") && !strcmp(p->name, "Runtime Test"));
    CHECK(p->program_id == 0x0100000000001234ull);
    CHECK(p->nmod == 3 && !strcmp(p->modules[0], "main") && !strcmp(p->modules[1], "subsdk0") && !strcmp(p->modules[2], "sdk"));
    CHECK(!strcmp(p->args, "-foo=1 -bar"));
    CHECK(p->stdalloc_cap == 0x180000000ull);
    CHECK(p->shader_skip_off == 0x770 && p->shader_skip_len == 0x78);
    CHECK(p->tex_bc_cpu == 1 && p->ue_texture_pool == 1000 && p->enc_thread == 1 && p->core_preempt == 1 && p->fps_target == 30);
    CHECK(p->shared_font_ttf == 1 && p->nvn_api_major == 55 && p->nvn_api_minor == 12);
    CHECK(p->nver == 1 && !strcmp(p->ver[0].build, "abcd") && !strcmp(p->ver[0].ver, "2.0.1"));
    return 0;
}
// the "none" spellings game.py writes for a game that leaves a key out: they must undo the compiled game's value
static int child_none(const char *path) {
    setenv("DM_PROFILE", path, 1);
    const struct dm_profile *p = dm_profile();
    CHECK(p->shader_skip_len == 0 && p->shared_font_ttf == -1 && p->nvn_api_major == 0 && p->stdalloc_cap == 0);
    CHECK(p->tex_bc_cpu == 0 && p->ue_texture_pool == 0 && p->enc_thread == 0);
    return 0;
}
static int child_env(void) {   // DM_PROF_<KEY> wins over the file and over the compiled profile
    unsetenv("DM_PROFILE");
    setenv("DM_PROF_STDALLOC_CAP", "0x40000000", 1); setenv("DM_PROF_SHADER_KEY_SKIP", "0x30+0x10", 1); setenv("DM_PROF_ENC_THREAD", "1", 1);
    const struct dm_profile *p = dm_profile();
    CHECK(p->stdalloc_cap == 0x40000000ull && p->shader_skip_off == 0x30 && p->shader_skip_len == 0x10 && p->enc_thread == 1);
    return 0;
}
static int child_compiled(void) {   // no DM_PROFILE: exactly the header
    unsetenv("DM_PROFILE");
    const struct dm_profile *p = dm_profile();
    static const char *m[] = DM_GAME_MODULES;
    CHECK(!strcmp(p->id, DM_GAME_ID) && !strcmp(p->name, DM_GAME_NAME) && p->program_id == DM_GAME_PROGRAM_ID);
    CHECK(p->nmod == DM_GAME_NMOD);
    for (int i = 0; i < p->nmod; i++) CHECK(!strcmp(p->modules[i], m[i]));
    CHECK(!strcmp(p->args, DM_GAME_ARGS));
    CHECK(p->stdalloc_cap == DM_GAME_STDALLOC_CAP);
    CHECK(p->shader_skip_off == DM_GAME_SHADER_KEY_SKIP_OFF && p->shader_skip_len == DM_GAME_SHADER_KEY_SKIP_LEN);
    CHECK(p->tex_bc_cpu == DM_GAME_TEX_BC_CPU && p->ue_texture_pool == DM_GAME_UE_TEXTURE_POOL && p->enc_thread == DM_GAME_ENC_THREAD);
    CHECK(p->core_preempt == DM_GAME_CORE_PREEMPT && p->fps_target == DM_GAME_FPS_TARGET);
#ifdef DM_GAME_NVN_API_MAJOR
    CHECK(p->nvn_api_major == DM_GAME_NVN_API_MAJOR && p->nvn_api_minor == DM_GAME_NVN_API_MINOR);
#else
    CHECK(p->nvn_api_major == 0);
#endif
#ifdef DM_GAME_SHARED_FONT_TTF
    CHECK(p->shared_font_ttf == DM_GAME_SHARED_FONT_TTF);
#else
    CHECK(p->shared_font_ttf == -1);
#endif
    return 0;
}

static int write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f) { printf("[RED] cannot write %s (errno %d)\n", path, errno); return 1; }
    fputs(body, f); fclose(f); return 0;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "file")) return child_file(argv[2]);
    if (argc > 2 && !strcmp(argv[1], "none")) return child_none(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "env")) return child_env();
    if (argc > 1 && !strcmp(argv[1], "compiled")) return child_compiled();
    char dir[256]; snprintf(dir, sizeof dir, "%s/t_profile_runtime.XXXXXX", getenv("TMPDIR") ?: "/tmp");
    if (!mkdtemp(dir)) { printf("[RED] mkdtemp failed\n"); return 1; }
    char full[300], none[300]; snprintf(full, sizeof full, "%s/full.tsv", dir); snprintf(none, sizeof none, "%s/none.tsv", dir);
    if (write_file(full, "# values that differ from any compiled profile\n"
            "title_id\t0100000000001234\nprogram_id\t0x0100000000001234\nname\tRuntime Test\nmodules\tmain subsdk0 sdk\ngame_args\t-foo=1 -bar\n"
            "stdalloc_cap\t0x180000000\nshader_key_skip\t0x770+0x78\ntex_bc_cpu\t1\nue_texture_pool\t1000\nenc_thread\t1\ncore_preempt\t1\nfps_target\t30\n"
            "shared_font\tttf\ndevice_api\t55.12\nbuild\tabcd\t2.0.1\tdisplay\n")) return 1;
    if (write_file(none, "shader_key_skip\t0x0+0x0\nshared_font\tauto\ndevice_api\t0.0\nstdalloc_cap\t0x0\ntex_bc_cpu\t0\nue_texture_pool\t0\nenc_thread\t0\n")) return 1;
    const char *cases[][2] = {{"file", full}, {"none", none}, {"env", NULL}, {"compiled", NULL}};
    int bad = 0;
    for (int i = 0; i < 4; i++) {
        pid_t pid = fork();
        if (!pid) { execl(argv[0], argv[0], cases[i][0], cases[i][1], (char *)NULL); _exit(3); }
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("%-8s FAILED\n", cases[i][0]); bad++; } else printf("%-8s OK\n", cases[i][0]);
    }
    unlink(full); unlink(none); rmdir(dir);
    printf(bad ? "[RED] %d profile case(s) failed\n" : "runtime profile OK\n", bad);
    return bad ? 1 : 0;
}
