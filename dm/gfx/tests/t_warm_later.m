// warm_parse with a list's #@after_first_frame line (issue #1747): the lines before it, and every line of the lists after it (this device's pso_user.tsv), come first and are
// the prepare step's; the lines after it come last (built after the first frame when warm_start splits), and a line the first part already has stays there. A pair's render pipelines stay one task, among
// the first ones when its first row is. Without the mark, everything comes first as before. CPU only: the parse needs the shader map, not Metal.
#include "../nvn/nvn_mtl.m"
#include <stdlib.h>
#include <unistd.h>

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int g_checks, g_failed;
#define EXPECT_EQ(name, want, got) do { \
    long long w_ = (long long)(want), g_ = (long long)(got); g_checks++; \
    printf("%s expected %lld -> got %lld %s\n", name, w_, g_, w_ == g_ ? "OK" : "FAIL"); \
    g_failed += w_ != g_; \
} while (0)

static NSString *put(NSString *dir, NSString *name, NSString *text) {
    NSString *p = [dir stringByAppendingPathComponent:name];
    if (![text writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:nil]) { printf("cannot write %s\n", name.UTF8String); exit(2); }
    return p;
}
static NSString *rrow(const char *vs, const char *fs, int variant) {   // a render pipeline row of the pair vs / fs; variant (fz) makes rows of one pair differ
    pso_skey sk; memset(&sk, 0, sizeof sk);
    snprintf(sk.vs, sizeof sk.vs, "%s", vs); snprintf(sk.fs, sizeof sk.fs, "%s", fs); sk.vs_stage = 0; sk.fs_stage = 4; sk.fz = (uint8_t)variant;
    char hex[2 * sizeof sk + 1]; pso_skey_hex(&sk, hex);
    return [NSString stringWithFormat:@"R\t%s", hex];
}
static NSString *keys(NSArray<WarmTask *> *t, long from, long to) {
    NSMutableArray *k = [NSMutableArray array];
    for (long i = from; i < to; i++) [k addObject:t[(NSUInteger)i]->kind == 1 ? [NSString stringWithFormat:@"%@|%@", t[(NSUInteger)i]->vkey, t[(NSUInteger)i]->fkey] : t[(NSUInteger)i]->key];
    return [k componentsJoinedByString:@" "];
}

int main(void) { @autoreleasepool {
    char tmpl[] = "/tmp/t_warm_later.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    NSString *dir = @(tmpl);
    g_shmap = [NSMutableDictionary dictionary];
    for (NSString *k in @[@"a1", @"a2", @"b1", @"b2", @"u1", @"v0", @"f0", @"c1", @"c2", @"v1", @"f1", @"v2", @"f2", @"v3", @"f3"]) g_shmap[k] = @"x";
    NSString *pair = [rrow("v0_b", "f0_b", 0) substringFromIndex:2];
    pso_skey probe; int parse_ok = pso_skey_parse(pair.UTF8String, &probe) == 0;
    EXPECT_EQ("a written render row parses back", 1, parse_ok);

    // the seed list: a1, the pair v0/f0, C c1 | mark | b1, a1 again (stays first), another row of the pair (joins its first task), C c2, b2
    NSString *seed = put(dir, @"pso_manifest.tsv", [@[@"# seed", @"S\ta1_b\t0", rrow("v0_b", "f0_b", 0), @"C\tc1_b",
        @"#@after_first_frame", @"S\tb1_b\t4", @"S\ta1_b\t0", rrow("v0_b", "f0_b", 1), @"C\tc2_b", @"S\tb2_b\t4", @""] componentsJoinedByString:@"\n"]);
    NSString *user = put(dir, @"pso_user.tsv", [@[@"S\tu1_b\t0", @"S\tb2_b\t4", @""] componentsJoinedByString:@"\n"]);
    long lines = 0, first = -2;
    NSArray<WarmTask *> *t = warm_parse(@[seed, user], &lines, &first);
    EXPECT_EQ("tasks", 7, t.count);
    EXPECT_EQ("first tasks: the seed list's before the mark, then the user list's", 5, first);
    EXPECT_EQ("lines (a line the first part has is not counted again)", 8, lines);
    EXPECT_EQ("lines of the first tasks (the pair's later row among them)", 6, warm_lines(t, 0, first));
    EXPECT_EQ("the pair's task holds both rows", 2, t[1]->rs.count);
    NSString *fk = keys(t, 0, first), *lk = keys(t, first, (long)t.count);
    printf("first: %s | later: %s\n", fk.UTF8String, lk.UTF8String);
    EXPECT_EQ("first part: a1, the pair, c1, u1, b2 (the user list's b2 before the seed list's later b2)", 1, [fk isEqualToString:@"a1_b v0_b|f0_b c1_b u1_b b2_b"]);
    EXPECT_EQ("later part: b1, c2", 1, [lk isEqualToString:@"b1_b c2_b"]);

    // without the mark: everything first, as before
    NSString *plain = put(dir, @"plain.tsv", [@[@"S\ta1_b\t0", @"S\tb1_b\t4", @"C\tc2_b", @""] componentsJoinedByString:@"\n"]);
    t = warm_parse(@[plain], &lines, &first);
    EXPECT_EQ("no mark: every task first", (long)t.count, first);
    EXPECT_EQ("no mark: lines", 3, lines);

    // a mark in the user list too: its later lines join the seed list's later lines, in list order
    NSString *user2 = put(dir, @"user2.tsv", [@[@"S\tu1_b\t0", @"#@after_first_frame", @"S\ta2_b\t0", @""] componentsJoinedByString:@"\n"]);
    t = warm_parse(@[seed, user2], &lines, &first);
    EXPECT_EQ("two marks: first tasks", 4, first);
    EXPECT_EQ("two marks: later in list order", 1, [keys(t, first, (long)t.count) isEqualToString:@"b1_b c2_b b2_b a2_b"]);

    // the mark with CRLF line endings or a trailing blank still splits
    NSString *crlf = put(dir, @"crlf.tsv", [@[@"S\ta1_b\t0", @"#@after_first_frame ", @"S\tb1_b\t4", @""] componentsJoinedByString:@"\r\n"]);
    t = warm_parse(@[crlf], &lines, &first);
    EXPECT_EQ("CRLF mark: first tasks", 1, first);
    EXPECT_EQ("CRLF mark: tasks", 2, t.count);

    // an unknown shader key (another game version) is dropped in either part
    NSString *stale = put(dir, @"stale.tsv", [@[@"S\tzz_b\t0", @"#@after_first_frame", @"S\tyy_b\t0", @"S\tb1_b\t4", @""] componentsJoinedByString:@"\n"]);
    t = warm_parse(@[stale], &lines, &first);
    EXPECT_EQ("stale keys dropped: tasks", 1, t.count);
    EXPECT_EQ("stale keys dropped: first", 0, first);

    // a start that compacts the pipeline archive, or has no hybrid draws, prepares everything first; otherwise the first part only
    g_compact_enabled = 1;
    EXPECT_EQ("compacting start: everything first", 7, warm_first_part(5, 7, 1));
    g_compact_enabled = 0;
    EXPECT_EQ("no hybrid draws: everything first", 7, warm_first_part(5, 7, 0));
    EXPECT_EQ("hybrid, no compaction: the first part", 5, warm_first_part(5, 7, 1));
    EXPECT_EQ("no mark: unchanged", 7, warm_first_part(7, 7, 1));

    // the background order (location-aware priority): a pair a draw bumped builds before the earlier queued ones, once; a bump of a taken or unknown pair does nothing
    NSString *pri = put(dir, @"pri.tsv", [@[@"S\ta1_b\t0", @"#@after_first_frame", rrow("v1_b", "f1_b", 0), rrow("v2_b", "f2_b", 0), @"S\tb1_b\t4", rrow("v3_b", "f3_b", 0), @""] componentsJoinedByString:@"\n"]);
    t = warm_parse(@[pri], &lines, &first);
    NSArray<WarmTask *> *rest = [t subarrayWithRange:NSMakeRange((NSUInteger)first, t.count - (NSUInteger)first)];
    EXPECT_EQ("order: later tasks", 4, rest.count);
    wl_init(rest);
    EXPECT_EQ("bump of a queued pair", 1, wl_bump("v3_b", "f3_b"));
    EXPECT_EQ("bump of the same pair again does nothing", 0, wl_bump("v3_b", "f3_b"));
    EXPECT_EQ("bump of a pair that is not in the list does nothing", 0, wl_bump("v0_b", "f0_b"));
    EXPECT_EQ("the bumped pair builds before the earlier unbumped ones", 1, wl_next() == rest[3]);
    EXPECT_EQ("then the list in order", 1, wl_next() == rest[0]);
    EXPECT_EQ("bump of a pair a worker has taken does nothing", 0, wl_bump("v1_b", "f1_b"));
    EXPECT_EQ("a later bump goes before the list's next", 1, wl_bump("v2_b", "f2_b") && wl_next() == rest[1]);   // rest[1] is the pair v2/f2: the listed next
    EXPECT_EQ("the bumped pair is not built twice", 1, wl_next() == rest[2]);
    EXPECT_EQ("the list ends", 1, wl_next() == nil);

    // bumps are served in the order they were asked; a task is returned once even when workers take the list while a thread bumps (2000 pairs, 4 workers, a bumping thread)
    wl_init(rest);
    EXPECT_EQ("two bumps", 2, wl_bump("v3_b", "f3_b") + wl_bump("v2_b", "f2_b"));
    EXPECT_EQ("the first ask first", 1, wl_next() == rest[3]);
    EXPECT_EQ("then the second ask", 1, wl_next() == rest[1]);
    { enum { N = 2000 }; NSMutableArray<WarmTask *> *many = [NSMutableArray array]; NSMutableArray<NSString *> *vk = [NSMutableArray array];
      for (int i = 0; i < N; i++) { WarmTask *w = [WarmTask new]; w->kind = 1; w->vkey = [NSString stringWithFormat:@"v%d", i]; w->fkey = [NSString stringWithFormat:@"f%d", i % 7]; [many addObject:w]; [vk addObject:w->vkey]; }
      wl_init(many); static _Atomic int seen[N]; __block _Atomic int total = 0; dispatch_group_t g = dispatch_group_create();
      for (int w = 0; w < 4; w++) dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
          for (WarmTask *t; (t = wl_next()); ) { atomic_fetch_add(&seen[[many indexOfObjectIdenticalTo:t]], 1); atomic_fetch_add(&total, 1); } });
      for (int i = N - 1; i >= 0; i -= 3) wl_bump(vk[(NSUInteger)i].UTF8String, [NSString stringWithFormat:@"f%d", i % 7].UTF8String);
      dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
      int twice = 0, none = 0; for (int i = 0; i < N; i++) { int c = atomic_load(&seen[i]); if (c > 1) twice++; if (!c) none++; }
      EXPECT_EQ("stress: every task returned", 0, none); EXPECT_EQ("stress: no task returned twice", 0, twice); EXPECT_EQ("stress: total", N, atomic_load(&total)); }

    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    printf("t_warm_later: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
} }
