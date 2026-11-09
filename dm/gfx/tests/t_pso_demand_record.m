// A pipeline a hybrid draw stands in for is listed in pso_user.tsv when first asked for (demand_record), once per key; DM_PSO_DEMAND_RECORD=0 lists none of them.
// Run twice: with no argument and with --off (the Makefile's check target). The hybrid call site in the draw path is not run here: it needs translated shader libraries.
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int g_failed;
#define CHECK(name, cond) do { int ok_ = !!(cond); printf("%s %s\n", name, ok_ ? "OK" : "FAIL"); g_failed += !ok_; } while (0)

static int rows(const char *path) {
    NSString *t = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil]; int n = 0;
    for (NSString *l in [t componentsSeparatedByString:@"\n"]) n += [l hasPrefix:@"R\t"];
    return n;
}

int main(int argc, char **argv) {
    int off = argc > 1 && !strcmp(argv[1], "--off");
    char path[] = "/tmp/t_pso_demand_record.XXXXXX"; int fd = mkstemp(path); if (fd < 0) return 2; close(fd);
    setenv("DM_PSO_USER", path, 1); unsetenv("DM_SAVE");
    if (off) setenv("DM_PSO_DEMAND_RECORD", "0", 1); else unsetenv("DM_PSO_DEMAND_RECORD");
    NvShader *vs = [NvShader new], *fs = [NvShader new];
    snprintf(vs->key, sizeof vs->key, "demand-vs"); snprintf(fs->key, sizeof fs->key, "demand-fs");
    pso_key a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); a.samples = 1; b.samples = 4;
    demand_record(&a, vs, fs); demand_record(&a, vs, fs);
    CHECK(off ? "off: nothing listed" : "listed once for one key", rows(path) == (off ? 0 : 1));
    demand_record(&b, vs, fs);
    CHECK(off ? "off: still nothing" : "a second key has its own row", rows(path) == (off ? 0 : 2));
    unlink(path);
    printf("%s\n", g_failed ? "FAIL" : "PASS");
    return g_failed != 0;
}
