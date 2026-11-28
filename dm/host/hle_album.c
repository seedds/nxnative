// Local screenshot album service (nn::album): saves RGBA8888 screenshot buffers submitted by the game to PNG files in DM_ALBUM_DIR.
// Destination: DM_ALBUM_DIR (Mac apps default to ~/Pictures/nxnative/<title ID> in dm/mac/launcher.c; iOS apps default to Documents/album in dm/ios/Sources/main.m),
// else <DM_ROOT>/album, else <dirname(DM_SAVE)>/album. The directory is created lazily on the first save, and files are written via a temporary file + hard link so no
// half-written PNG is ever visible and rapid saves never overwrite one another.
// Verified on Pokemon Pokopia (0400A3A01EE4C000, SDK 20.5.25: main+0x10fedc0 decodes its gallery JPEG with nn::image::JpegDecoder's default PixelFormat_Rgba32 into 0x7e9000 bytes
// and calls the 4-arg SaveScreenshot with ImageSize = 1 = 1920x1080), Pikmin 4 (Switch 1: BGRA->RGBA8888 loop at main+0x1e1b2bc + 4-arg SaveScreenshot with ImageSize = 0 = 1280x720),
// and Resident Evil 4 / PRAGMATA (Switch 2: 5-arg SaveScreenshot with ImageOrientation).
// Offline/system-only album features (SaveCurrentScreenshot compositor capture, SaveAndShareScreenshot social upload, ShowAlbumFiles system applet) still return failure.
#include "hle.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// nn::album Result values (module 214 = 0xd6; Pokopia SDK 20.5.25 nn::album::ConvertCapsrvResult, sdk+0x1a08a0)
#define ALBUM_RES_UNKNOWN 0xcad6u   // module 214, desc 101: general I/O or encode failure
#define ALBUM_RES_INVALID 0xccd6u   // module 214, desc 102: unsupported/invalid arguments or unconfigured album folder
#define ALBUM_RES_FULL    0xced6u   // module 214, desc 103: storage full (ENOSPC / EDQUOT; RE4 main+0x9cb2790, PRAGMATA main+0x7134c08)

static pthread_mutex_t g_album_mu = PTHREAD_MUTEX_INITIALIZER;

static int album_image_dims(int image_size, size_t *w, size_t *h) {
    if (image_size == 0) { *w = 1280; *h = 720; return 1; }
    if (image_size == 1) { *w = 1920; *h = 1080; return 1; }
    return 0;
}

static size_t album_trim_slashes(char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') path[--len] = 0;
    return len;
}

static int album_resolve_dir(char *out, size_t cap) {
    const char *e = getenv("DM_ALBUM_DIR");
    if (e && *e) {
        if ((size_t)snprintf(out, cap, "%s", e) >= cap) return 0;
        album_trim_slashes(out);
        return out[0] != 0;
    }
    e = getenv("DM_ROOT");
    if (e && *e) {
        char base[PATH_MAX];
        if ((size_t)snprintf(base, sizeof base, "%s", e) >= sizeof base) return 0;
        album_trim_slashes(base);
        return (size_t)snprintf(out, cap, "%s/album", base) < cap;
    }
    e = getenv("DM_SAVE");
    if (e && *e) {
        char base[PATH_MAX];
        if ((size_t)snprintf(base, sizeof base, "%s", e) >= sizeof base) return 0;
        album_trim_slashes(base);
        char *sl = strrchr(base, '/');
        if (sl == base) return (size_t)snprintf(out, cap, "/album") < cap;
        if (sl) { *sl = 0; return (size_t)snprintf(out, cap, "%s/album", base) < cap; }
        return (size_t)snprintf(out, cap, "album") < cap;
    }
    return 0;
}

static int album_mkdirs(const char *path) {
    char tmp[PATH_MAX];
    if (!path || !*path || (size_t)snprintf(tmp, sizeof tmp, "%s", path) >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    struct stat st;
    if (stat(tmp, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static uint8_t *album_rotate_rgba(const uint8_t *src, size_t w, size_t h, int orient, size_t *out_w, size_t *out_h) {
    size_t npix = w * h;
    uint8_t *dst = malloc(npix * 4);
    if (!dst) return NULL;
    if (orient == 1) {   // 90 degrees clockwise
        *out_w = h; *out_h = w;
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++)
                memcpy(dst + (x * h + (h - 1 - y)) * 4, src + (y * w + x) * 4, 4);
    } else if (orient == 2) {   // 180 degrees
        *out_w = w; *out_h = h;
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++)
                memcpy(dst + ((h - 1 - y) * w + (w - 1 - x)) * 4, src + (y * w + x) * 4, 4);
    } else {   // 270 degrees clockwise (90 degrees counter-clockwise)
        *out_w = h; *out_h = w;
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++)
                memcpy(dst + ((w - 1 - x) * h + y) * 4, src + (y * w + x) * 4, 4);
    }
    return dst;
}

static int album_write_png(const char *path, const uint8_t *rgba, size_t w, size_t h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef prov = cs ? CGDataProviderCreateWithData(NULL, rgba, w * h * 4, NULL) : NULL;
    CGBitmapInfo bi = (CGBitmapInfo)kCGImageAlphaNoneSkipLast | (CGBitmapInfo)kCGBitmapByteOrderDefault;
    CGImageRef img = prov ? CGImageCreate(w, h, 8, 32, w * 4, cs, bi, prov, NULL, false, kCGRenderingIntentDefault) : NULL;
    CFURLRef url = img ? CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false) : NULL;
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    int ok = 0;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        ok = CGImageDestinationFinalize(dst) ? 1 : 0;
    }
    if (dst) CFRelease(dst);
    if (url) CFRelease(url);
    if (img) CGImageRelease(img);
    if (prov) CGDataProviderRelease(prov);
    if (cs) CGColorSpaceRelease(cs);
    return ok;
}

static Result album_errno_result(int err) {
    return (err == ENOSPC || err == EDQUOT) ? ALBUM_RES_FULL : ALBUM_RES_UNKNOWN;
}

static int album_publish_unique(const char *tmp, const char *dir, const char *stamp, char *final_path, size_t cap) {
    pthread_mutex_lock(&g_album_mu);
    int published = 0, last_err = EEXIST;
    for (unsigned seq = 0; seq < 10000; seq++) {
        int n = seq == 0 ? snprintf(final_path, cap, "%s/%s_%s.png", dir, stamp, DM_GAME_ID)
                         : snprintf(final_path, cap, "%s/%s_%s_%02u.png", dir, stamp, DM_GAME_ID, seq);
        if (n < 0 || (size_t)n >= cap) { last_err = ENAMETOOLONG; break; }
        if (!link(tmp, final_path)) {
            if (unlink(tmp)) LOG("album: could not remove temp file %s (errno=%d)", tmp, errno);
            published = 1;
            break;
        }
        if (errno == EEXIST) continue;
        if (errno == ENOTSUP || errno == EPERM || errno == EACCES) {
            struct stat st;
            if (!lstat(final_path, &st)) continue;
            if (errno != ENOENT) { last_err = errno; break; }
            if (!rename(tmp, final_path)) { published = 1; break; }
        }
        last_err = errno;
        break;
    }
    pthread_mutex_unlock(&g_album_mu);
    if (!published) { errno = last_err; return -1; }
    return 0;
}

static Result album_save_screenshot(const void *p, size_t n, int image_size, int orient, int report) {
    (void)report;
    size_t w = 0, h = 0;
    if (!p || !album_image_dims(image_size, &w, &h)) {
        LOG("[RED] album::SaveScreenshot invalid image (ptr=%p size=%zu image_size=%d)", p, n, image_size);
        return ALBUM_RES_INVALID;
    }
    size_t need = w * h * 4;
    if (n < need) {
        LOG("[RED] album::SaveScreenshot buffer too small (%zu < %zu for %zux%zu)", n, need, w, h);
        return ALBUM_RES_INVALID;
    }
    if (orient < 0 || orient > 3) {
        LOG("[RED] album::SaveScreenshot invalid orientation %d", orient);
        return ALBUM_RES_INVALID;
    }

    char dir[PATH_MAX];
    if (!album_resolve_dir(dir, sizeof dir)) {
        LOG("[RED] album::SaveScreenshot no album folder configured (set DM_ALBUM_DIR, DM_ROOT or DM_SAVE)");
        return ALBUM_RES_INVALID;
    }
    if (album_mkdirs(dir)) {
        int err = errno;
        LOG("[RED] album::SaveScreenshot cannot create %s (errno=%d: %s)", dir, err, strerror(err));
        return album_errno_result(err);
    }

    const uint8_t *pixels = (const uint8_t *)p;
    uint8_t *rot = NULL;
    size_t out_w = w, out_h = h;
    if (orient != 0) {
        rot = album_rotate_rgba(pixels, w, h, orient, &out_w, &out_h);
        if (!rot) {
            LOG("[RED] album::SaveScreenshot out of memory rotating %zux%zu", w, h);
            return ALBUM_RES_UNKNOWN;
        }
        pixels = rot;
    }

    char tmp[PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/.album-XXXXXX.tmp", dir) >= sizeof tmp) {
        free(rot);
        LOG("[RED] album::SaveScreenshot album path too long: %s", dir);
        return ALBUM_RES_UNKNOWN;
    }
    int fd = mkstemps(tmp, 4);
    if (fd < 0) {
        int err = errno;
        free(rot);
        LOG("[RED] album::SaveScreenshot cannot create temp file in %s (errno=%d: %s)", dir, err, strerror(err));
        return album_errno_result(err);
    }
    (void)fchmod(fd, 0644);
    close(fd);

    errno = 0;
    int wrote = album_write_png(tmp, pixels, out_w, out_h);
    int write_err = errno;
    free(rot);
    if (!wrote) {
        unlink(tmp);
        LOG("[RED] album::SaveScreenshot PNG encode failed for %s (errno=%d)", tmp, write_err);
        return album_errno_result(write_err);
    }

    time_t now = time(NULL);
    struct tm tm_now;
    memset(&tm_now, 0, sizeof tm_now);
    localtime_r(&now, &tm_now);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d_%02d%02d%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

    char final_path[PATH_MAX];
    if (album_publish_unique(tmp, dir, stamp, final_path, sizeof final_path)) {
        int err = errno;
        unlink(tmp);
        LOG("[RED] album::SaveScreenshot cannot publish %s (errno=%d: %s)", dir, err, strerror(err));
        return album_errno_result(err);
    }
    LOG("album::SaveScreenshot %zux%zu (orient=%d) -> %s", out_w, out_h, orient, final_path);
    return RES_OK;
}

HLE_FORCE("_ZN2nn5album10InitializeEv", "local capture album init (replaces capsrv connection)", void, h_album_Init, void) {}
HLE_FORCE("_ZN2nn5album8FinalizeEv", "local capture album finalize", void, h_album_Fini, void) {}
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionE",
          "save an RGBA8888 screenshot buffer to the local album folder as PNG", Result, h_album_Save,
          const void *p, size_t n, int size, int orient, int report) {
    return album_save_screenshot(p, n, size, orient, report);
}
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE",
          "save an RGBA8888 screenshot buffer (no orientation) to the local album folder as PNG", Result, h_album_SaveNoOrient,
          const void *p, size_t n, int size, int report) {
    return album_save_screenshot(p, n, size, 0, report);
}
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionEPKNS_7account3UidEi",
          "save an RGBA8888 screenshot buffer (with user list) to the local album folder as PNG", Result, h_album_SaveUsers,
          const void *p, size_t n, int size, int orient, int report, const void *uids, int nuid) {
    (void)uids; (void)nuid;
    return album_save_screenshot(p, n, size, orient, report);
}
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionERKNS_7account3UidEPKc",
          "save and share a picture: offline, fails", Result, h_album_Share,
          const void *p, size_t n, int size, int orient, int report, const void *uid, const char *text) {
    (void)p; (void)n; (void)size; (void)orient; (void)report; (void)uid; (void)text;
    return RES_FAIL;
}
// The SDK 20 overload that hands back a share ID, and the URL of the last post (Animal Crossing: New Horizons 3.0.3)
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc",
          "save and share a picture: offline, fails", Result, h_album_ShareId,
          void *id, const void *p, size_t n, int size, int report, const void *uid, const char *text) {
    (void)id; (void)p; (void)n; (void)size; (void)report; (void)uid; (void)text;
    return RES_FAIL;
}
HLE_FORCE("_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE",
          "URL of the last shared post: nothing was shared, fails", Result, h_album_ShareUrl, char *url, size_t n, const void *id) {
    (void)id; if (url && n) url[0] = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn5album14ShowAlbumFilesEv", "album applet: not shown", Result, h_album_Show, void) { return RES_FAIL; }
// Pokemon Legends: Z-A (SDK 19), Xenoblade Chronicles X DE, Fast Fusion: compositor frame capture (not a caller-supplied pixel buffer)
HLE_FORCE("_ZN2nn5album21SaveCurrentScreenshotENS0_17AlbumReportOptionE",
          "screenshot of the current compositor frame (capsrv): not implemented, fails", Result, h_album_SaveCur, int report) {
    (void)report; LOG("album::SaveCurrentScreenshot (not implemented)"); return RES_FAIL;
}
