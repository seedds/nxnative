// Local album service unit test (hle_album.c, GitHub issue #3211): verifies all registered nn::album::SaveScreenshot overloads,
// 720p and 1080p RGBA8888 PNG output and pixel fidelity (ignoring source alpha), ImageOrientation rotations 0..3, collision-free
// atomic publishing, DM_ALBUM_DIR / DM_ROOT / DM_SAVE fallback order, and invalid-input / unwritable-directory error handling.
#include "../hle_album.c"
#include <dirent.h>
#include <mach-o/getsect.h>
#include <sys/stat.h>

extern const struct mach_header_64 _mh_execute_header;
static int checks, fails;
#define CHECK(c, ...) do { checks++; int _ok = !!(c); fails += !_ok; printf("%s ", _ok ? "OK" : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)

static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *entries = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; entries && i < bytes / sizeof *entries; i++) {
        if (strcmp(entries[i].name, name)) continue;
        count++; fn = entries[i].fn;
    }
    CHECK(count == 1 && fn, "hook registered once: %s", name);
    return count == 1 ? fn : NULL;
}

static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[PATH_MAX];
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", path, de->d_name) < sizeof sub) rm_tree(sub);
    }
    closedir(d);
    rmdir(path);
}

static int list_pngs(const char *dir, char out[][PATH_MAX], int cap, int *tmp_count) {
    *tmp_count = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        size_t len = strlen(de->d_name);
        if (len >= 4 && !strcmp(de->d_name + len - 4, ".tmp")) { (*tmp_count)++; continue; }
        if (len >= 4 && !strcmp(de->d_name + len - 4, ".png") && n < cap)
            snprintf(out[n++], PATH_MAX, "%s/%s", dir, de->d_name);
    }
    closedir(d);
    return n;
}

// Decode a PNG back into RGBA8888 and return its dimensions; caller frees *out_rgba.
static int read_png_rgba(const char *path, size_t *out_w, size_t *out_h, uint8_t **out_rgba) {
    *out_rgba = NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageSourceRef src = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    int ok = 0;
    if (img) {
        size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
        uint8_t *buf = calloc(w * h, 4);
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = buf && cs ? CGBitmapContextCreate(buf, w, h, 8, w * 4, cs,
                                                             (CGBitmapInfo)kCGImageAlphaPremultipliedLast | (CGBitmapInfo)kCGBitmapByteOrderDefault) : NULL;
        if (ctx) {
            CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
            *out_w = w; *out_h = h; *out_rgba = buf; buf = NULL; ok = 1;
        }
        if (ctx) CGContextRelease(ctx);
        if (cs) CGColorSpaceRelease(cs);
        free(buf);
    }
    if (img) CGImageRelease(img);
    if (src) CFRelease(src);
    if (url) CFRelease(url);
    return ok;
}

static void set_px(uint8_t *buf, size_t w, size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t *p = buf + (y * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static int px_eq(const uint8_t *buf, size_t w, size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t *p = buf + (y * w + x) * 4;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

// Fill corners with distinct RGB values and alpha=0 (to verify alpha is stripped/opaque on save).
static uint8_t *make_test_rgba(size_t w, size_t h) {
    uint8_t *buf = calloc(w * h, 4);
    if (!buf) return NULL;
    set_px(buf, w, 0, 0, 255, 0, 0, 0);           // top-left: red
    set_px(buf, w, w - 1, 0, 0, 255, 0, 64);      // top-right: green
    set_px(buf, w, 0, h - 1, 0, 0, 255, 128);     // bottom-left: blue
    set_px(buf, w, w - 1, h - 1, 255, 255, 0, 0); // bottom-right: yellow
    return buf;
}

int main(void) {
    void (*fn_init)(void) = hook("_ZN2nn5album10InitializeEv");
    void (*fn_fini)(void) = hook("_ZN2nn5album8FinalizeEv");
    Result (*fn_save4)(const void *, size_t, int, int) =
        hook("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE");
    Result (*fn_save5)(const void *, size_t, int, int, int) =
        hook("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionE");
    Result (*fn_save7)(const void *, size_t, int, int, int, const void *, int) =
        hook("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionEPKNS_7account3UidEi");
    Result (*fn_share)(const void *, size_t, int, int, int, const void *, const char *) =
        hook("_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionERKNS_7account3UidEPKc");
    Result (*fn_share_id)(void *, const void *, size_t, int, int, const void *, const char *) =
        hook("_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc");
    Result (*fn_share_url)(char *, size_t, const void *) =
        hook("_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE");
    Result (*fn_show)(void) = hook("_ZN2nn5album14ShowAlbumFilesEv");
    Result (*fn_cur)(int) = hook("_ZN2nn5album21SaveCurrentScreenshotENS0_17AlbumReportOptionE");

    if (fn_init) fn_init();

    const char *base_tmp = getenv("TMPDIR");
    if (!base_tmp || !*base_tmp) base_tmp = "/tmp";
    char root[PATH_MAX];
    snprintf(root, sizeof root, "%s/t_album_XXXXXX", base_tmp);
    CHECK(mkdtemp(root) != NULL, "created temp test root");

    uint8_t *img1080 = make_test_rgba(1920, 1080);
    uint8_t *img720 = make_test_rgba(1280, 720);
    CHECK(img1080 && img720, "allocated 1080p and 720p RGBA test buffers");

    // 1. Pokopia path: 4-arg SaveScreenshot with ImageSize = 1 (1920x1080, 0x7e9000 bytes), nested DM_ALBUM_DIR created lazily.
    char album1080[PATH_MAX];
    snprintf(album1080, sizeof album1080, "%s/nested/album1080", root);
    setenv("DM_ALBUM_DIR", album1080, 1);
    unsetenv("DM_ROOT");
    unsetenv("DM_SAVE");
    CHECK(fn_save4 && fn_save4(img1080, 1920u * 1080u * 4u, 1, 3) == RES_OK, "1080p 4-arg SaveScreenshot succeeds");
    char files[8][PATH_MAX]; int tmps = -1;
    int nfiles = list_pngs(album1080, files, 8, &tmps);
    CHECK(nfiles == 1 && tmps == 0, "1080p save wrote 1 PNG and left 0 temp files (got %d, %d)", nfiles, tmps);
    if (nfiles == 1) {
        size_t w = 0, h = 0; uint8_t *dec = NULL;
        CHECK(read_png_rgba(files[0], &w, &h, &dec) && w == 1920 && h == 1080, "1080p PNG decodes to 1920x1080 (got %zux%zu)", w, h);
        if (dec) {
            CHECK(px_eq(dec, w, 0, 0, 255, 0, 0, 255) &&
                  px_eq(dec, w, w - 1, 0, 0, 255, 0, 255) &&
                  px_eq(dec, w, 0, h - 1, 0, 0, 255, 255) &&
                  px_eq(dec, w, w - 1, h - 1, 255, 255, 0, 255),
                  "1080p PNG corners match RGBA input with opaque alpha");
            free(dec);
        }
    }

    // 2. Rapid saves in the same second do not collide or overwrite.
    CHECK(fn_save4 && fn_save4(img1080, 1920u * 1080u * 4u, 1, 3) == RES_OK, "second rapid SaveScreenshot succeeds");
    CHECK(fn_save7 && fn_save7(img720, 1280u * 720u * 4u, 0, 0, 2, NULL, 0) == RES_OK, "7-arg SaveScreenshot succeeds");
    nfiles = list_pngs(album1080, files, 8, &tmps);
    CHECK(nfiles == 3 && tmps == 0, "rapid saves keep all 3 distinct PNGs without temp leftovers (got %d, %d)", nfiles, tmps);

    // 3. All 4 ImageOrientation values on 1280x720 via 5-arg SaveScreenshot.
    for (int orient = 0; orient < 4; orient++) {
        char odir[PATH_MAX];
        snprintf(odir, sizeof odir, "%s/orient_%d", root, orient);
        setenv("DM_ALBUM_DIR", odir, 1);
        CHECK(fn_save5 && fn_save5(img720, 1280u * 720u * 4u, 0, orient, 2) == RES_OK, "720p orient=%d succeeds", orient);
        nfiles = list_pngs(odir, files, 8, &tmps);
        CHECK(nfiles == 1 && tmps == 0, "orient=%d wrote 1 PNG", orient);
        if (nfiles == 1) {
            size_t w = 0, h = 0; uint8_t *dec = NULL;
            size_t want_w = (orient == 1 || orient == 3) ? 720 : 1280;
            size_t want_h = (orient == 1 || orient == 3) ? 1280 : 720;
            CHECK(read_png_rgba(files[0], &w, &h, &dec) && w == want_w && h == want_h,
                  "orient=%d dimensions %zux%zu == %zux%zu", orient, w, h, want_w, want_h);
            if (dec) {
                int corners_ok = 0;
                if (orient == 0)
                    corners_ok = px_eq(dec, w, 0, 0, 255, 0, 0, 255) && px_eq(dec, w, w - 1, 0, 0, 255, 0, 255) &&
                                 px_eq(dec, w, 0, h - 1, 0, 0, 255, 255) && px_eq(dec, w, w - 1, h - 1, 255, 255, 0, 255);
                else if (orient == 1)   // 90 CW: TL->TR, TR->BR, BL->TL, BR->BL
                    corners_ok = px_eq(dec, w, w - 1, 0, 255, 0, 0, 255) && px_eq(dec, w, w - 1, h - 1, 0, 255, 0, 255) &&
                                 px_eq(dec, w, 0, 0, 0, 0, 255, 255) && px_eq(dec, w, 0, h - 1, 255, 255, 0, 255);
                else if (orient == 2)   // 180: TL->BR, TR->BL, BL->TR, BR->TL
                    corners_ok = px_eq(dec, w, w - 1, h - 1, 255, 0, 0, 255) && px_eq(dec, w, 0, h - 1, 0, 255, 0, 255) &&
                                 px_eq(dec, w, w - 1, 0, 0, 0, 255, 255) && px_eq(dec, w, 0, 0, 255, 255, 0, 255);
                else                    // 270 CW (90 CCW): TL->BL, TR->TL, BL->BR, BR->TR
                    corners_ok = px_eq(dec, w, 0, h - 1, 255, 0, 0, 255) && px_eq(dec, w, 0, 0, 0, 255, 0, 255) &&
                                 px_eq(dec, w, w - 1, h - 1, 0, 0, 255, 255) && px_eq(dec, w, w - 1, 0, 255, 255, 0, 255);
                CHECK(corners_ok, "orient=%d rotated corner pixels match", orient);
                free(dec);
            }
        }
    }

    // 4. Directory fallbacks: DM_ROOT/album and dirname(DM_SAVE)/album, and error when none set.
    char dm_root[PATH_MAX], dm_root_album[PATH_MAX], dm_save[PATH_MAX], dm_save_album[PATH_MAX];
    snprintf(dm_root, sizeof dm_root, "%s/root_fb/", root);
    snprintf(dm_root_album, sizeof dm_root_album, "%s/root_fb/album", root);
    unsetenv("DM_ALBUM_DIR");
    setenv("DM_ROOT", dm_root, 1);
    unsetenv("DM_SAVE");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 0, 0) == RES_OK, "DM_ROOT fallback succeeds");
    CHECK(list_pngs(dm_root_album, files, 8, &tmps) == 1, "DM_ROOT fallback wrote into <DM_ROOT>/album");

    snprintf(dm_save, sizeof dm_save, "%s/save_fb/save/", root);
    snprintf(dm_save_album, sizeof dm_save_album, "%s/save_fb/album", root);
    unsetenv("DM_ROOT");
    setenv("DM_SAVE", dm_save, 1);
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 0, 0) == RES_OK, "DM_SAVE sibling fallback succeeds");
    CHECK(list_pngs(dm_save_album, files, 8, &tmps) == 1, "DM_SAVE fallback wrote into <dirname(DM_SAVE)>/album");

    unsetenv("DM_SAVE");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 0, 0) == ALBUM_RES_INVALID, "no album/root/save env fails cleanly");

    // 5. Validation and unwritable destination errors.
    char bad_dir[PATH_MAX];
    snprintf(bad_dir, sizeof bad_dir, "%s/empty_check", root);
    setenv("DM_ALBUM_DIR", bad_dir, 1);
    CHECK(fn_save4 && fn_save4(NULL, 1280u * 720u * 4u, 0, 0) == ALBUM_RES_INVALID, "NULL pointer rejected");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u - 1, 0, 0) == ALBUM_RES_INVALID, "truncated 720p buffer rejected");
    CHECK(fn_save4 && fn_save4(img1080, 1920u * 1080u * 4u - 4, 1, 0) == ALBUM_RES_INVALID, "truncated 1080p buffer rejected");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 2, 0) == ALBUM_RES_INVALID, "unknown ImageSize=2 rejected");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, -1, 0) == ALBUM_RES_INVALID, "negative ImageSize rejected");
    CHECK(fn_save5 && fn_save5(img720, 1280u * 720u * 4u, 0, 4, 0) == ALBUM_RES_INVALID, "invalid ImageOrientation=4 rejected");
    CHECK(fn_save5 && fn_save5(img720, 1280u * 720u * 4u, 0, -1, 0) == ALBUM_RES_INVALID, "negative ImageOrientation rejected");
    struct stat st;
    CHECK(stat(bad_dir, &st) != 0, "invalid calls do not create album directory");

    char not_a_dir[PATH_MAX];
    snprintf(not_a_dir, sizeof not_a_dir, "%s/regular_file", root);
    FILE *fp = fopen(not_a_dir, "w");
    CHECK(fp != NULL, "created regular file blocking album dir");
    if (fp) fclose(fp);
    setenv("DM_ALBUM_DIR", not_a_dir, 1);
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 0, 0) == ALBUM_RES_UNKNOWN, "file blocking DM_ALBUM_DIR returns album error");

    // 6. Offline/system-only album calls remain failing.
    uint8_t uid[16] = {0}, sid[32] = {0}; char url[32] = "keep";
    CHECK(fn_share && fn_share(img720, 1280u * 720u * 4u, 0, 0, 0, uid, "hi") == RES_FAIL, "SaveAndShareScreenshot fails");
    CHECK(fn_share_id && fn_share_id(sid, img720, 1280u * 720u * 4u, 0, 0, uid, "hi") == RES_FAIL, "SaveAndShareScreenshot (ShareId) fails");
    CHECK(fn_share_url && fn_share_url(url, sizeof url, sid) == RES_FAIL && url[0] == 0, "GetSharePostedLastUrl clears buffer and fails");
    CHECK(fn_show && fn_show() == RES_FAIL, "ShowAlbumFiles fails");
    CHECK(fn_cur && fn_cur(0) == RES_FAIL, "SaveCurrentScreenshot fails");

    if (fn_fini) fn_fini();
    free(img1080);
    free(img720);
    rm_tree(root);
    printf("t_album: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
