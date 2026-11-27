// DM_NVN_TEXINFO's and DM_NVN_TREG_ID's registration lines name the levels, layers and target of the view a texture ID is registered with: which levels a draw through the ID
// samples. A sparse texture that draws magenta (GitHub issue #2350) is told apart by it: bound from a level that is not written yet, or written where the draw never reads
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static char g_log[16384];
static const char *line_of(const char *start) {   // the log line that starts with start, from the [nvn-mtl] prefix on
    static char line[1024]; const char *p = g_log;
    while ((p = strstr(p, start))) { const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p); if (n >= sizeof line) n = sizeof line - 1; memcpy(line, p, n); line[n] = 0; return line; }
    return "";
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_TEXINFO", "40x24", 1); setenv("DM_NVN_TREG_ID", "905", 1);
    mtl_init();
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 90501, .off = 90501ull << 12};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 40, .height = 24, .depth = 1, .levels = 6, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t);
    view_t v = {0}; v.set_levels = 1; v.base_level = 2; v.nlevels = 3;
    view_t rest = {0}; rest.set_levels = 1; rest.base_level = 4; rest.nlevels = 0; rest.set_layers = 1; rest.min_layer = 0; rest.nlayers = 1; rest.set_target = 1; rest.target = NVN_TEXTURE_TARGET_2D;
    char path[] = "/tmp/t_texinfo_view.XXXXXX"; int fd = mkstemp(path), saved = dup(2);
    CHECK(fd >= 0 && saved >= 0, "a log file to capture stderr");
    if (fd < 0 || saved < 0) return 1;
    unlink(path); fflush(stderr); dup2(fd, 2);
    mtl_reg_tex(905, &t, &v); mtl_reg_tex(906, &t, NULL); mtl_reg_tex(907, &t, &rest);
    fflush(stderr); dup2(saved, 2); close(saved);
    ssize_t got = pread(fd, g_log, sizeof g_log - 1, 0); close(fd); g_log[got > 0 ? got : 0] = 0;
    const char *a = line_of("TEXINFO register id 905 ");
    CHECK(strstr(a, " levels 6 ") && strstr(a, " view 1 ") && strstr(a, " view levels 1:2+3 layers 0:0+0 target 0:0"), "TEXINFO names the texture's 6 levels and the view's 2+3");
    const char *b = line_of("TEXINFO register id 906 ");
    CHECK(strstr(b, " view 0 ") && strstr(b, " view levels 0:0+0 "), "a registration without a view says so");
    const char *c = line_of("TEXINFO register id 907 ");
    char want[64]; snprintf(want, sizeof want, " view levels 1:4+0 layers 1:0+1 target 1:%d", NVN_TEXTURE_TARGET_2D);
    CHECK(strstr(c, want) != NULL, "a view to the last level (count 0), with layers and a target");
    const char *d = line_of("TREG id 905 ");
    CHECK(strstr(d, " levels 6 view 1 (levels 1:2+3 layers 0:0+0 target 0:0)") != NULL, "DM_NVN_TREG_ID names the view's levels too");
    CHECK(!strstr(g_log, "TREG id 906 "), "DM_NVN_TREG_ID logs only its ID");
    if (failed) fprintf(stderr, "captured log:\n%s", g_log);
    printf("texture view levels in the registration logs: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
