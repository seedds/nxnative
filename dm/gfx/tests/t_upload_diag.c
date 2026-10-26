// Exact byte comparison, race attribution and strict bounds of upload diagnostics.
#include <stdarg.h>
#include <assert.h>
#include <stdio.h>
static char last_log[2048];
static void test_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(last_log, sizeof last_log, fmt, ap); va_end(ap);
}
#define NVN_UPDIAG_LOG test_log
#include "../nvn/nvn_upload_diag.h"
static int writes_during_capture;
const void *nvn_dirty_span(const void *addr, size_t len) { (void)len; return addr; }
uint64_t nvn_dirty_now(void) { return 7; }
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) {
    (void)span; (void)addr; (void)len; (void)stamp; return writes_during_capture;
}
static nvn_updiag_record capture(nvn_updiag_meta *m) {
    nvn_updiag_record r; nvn_updiag_begin(&r, m); nvn_updiag_capture(&r);
    return r;
}
int main(void) {
    size_t pg = (size_t)getpagesize(); void *p = NULL;
    assert(!posix_memalign(&p, pg, pg * 2)); memset(p, 0, pg * 2);
    nvn_updiag_meta m = {.frame=1, .serial=1, .mem=p, .pool=p, .bytes=pg*2, .reupload=1};
    unsetenv("DM_NVN_UPLOAD_DIAG_FRAMES");
    nvn_updiag_record r = capture(&m); assert(!r.enabled && !g_updiag.bytes);
    setenv("DM_NVN_UPLOAD_DIAG_FRAMES", "4:first", 1); m.frame++;
    r=capture(&m); assert(r.enabled && r.stable && r.baseline && !r.uncaptured);
    char digest[sizeof r.digest]; memcpy(digest, r.digest, sizeof digest);
    r=capture(&m); assert(!r.baseline && r.equal && !r.different_pages && !strcmp(digest,r.digest));
    ((char *)p)[pg+5]=3;
    r=capture(&m); assert(!r.equal && r.different_pages==1 && r.first_different_page==pg && strcmp(digest,r.digest));
    writes_during_capture=1; r=capture(&m); assert(!r.stable);
    writes_during_capture=0; r=capture(&m); assert(r.stable && !r.equal && !r.different_pages);
    nvn_updiag_end(&r,r.capture_ns+1000,1); assert(strstr(last_log,"exact=prior-racy"));
    r=capture(&m); assert(r.equal);
    m.serial=2; m.bytes=UPDIAG_MAX_TEXTURE+1;
    r=capture(&m); assert(!strcmp(r.uncaptured,"texture-size-cap"));
    // Oversized requests never read the undersized buffer used above.
    m.bytes=pg*2; m.frame=100;
    r=capture(&m); assert(!r.enabled && !g_updiag.bytes);
    setenv("DM_NVN_UPLOAD_DIAG_FRAMES", "16:record-cap", 1); m.frame++;
    for (unsigned i=0;i<UPDIAG_RECORDS;i++) { r=capture(&m); assert(r.enabled); }
    r=capture(&m); assert(!r.enabled && !g_updiag.bytes);
    setenv("DM_NVN_UPLOAD_DIAG_FRAMES", "2:frame-cap", 1); m.frame=200;
    r=capture(&m); assert(r.enabled); m.frame++;
    r=capture(&m); assert(r.enabled); m.frame++;
    r=capture(&m); assert(!r.enabled && !g_updiag.bytes);
    free(p);
    puts("Upload diagnostics: disabled default, exact equality/change, race attribution, size/record/frame limits passed");
    return 0;
}
