// The Mac's own preferred languages, for the game language default.
// The host hands the game its language as DM_LANG (hle_oe.c): the iOS app sets that from the IPA (main.m), the Mac has nothing that sets it, so the host asks macOS instead —
// the list the user ordered in System Settings > General > Language & Region. Everything else about the language (which languages the game has, the Switch codes, the log line)
// is platform-independent and lives in hle_oe.c; this file only reads the list.
// Mac only: dm/ios/tools/gen_build.py leaves sources named *_mac out of the iOS app, and only hle_oe.c's non-iOS path calls dm_mac_preferred_languages.
#import <Foundation/Foundation.h>
#include <string.h>

// Write the user's preferred language identifiers into out as space-separated tags ("it-IT en-US"), in preference order, and return how many there are.
// [NSLocale preferredLanguages] is the same list UIKit hands an iOS app, so the Mac and the phone read one notion of "the user's languages"; its entries are CLDR-ish identifiers
// such as it-IT, en-GB or zh-Hans-CN, which hle_oe.c maps to Switch codes. Returns 0 (empty out) when macOS offers no list; a tag that does not fit is left out whole rather
// than truncated, so the caller never sees half an identifier.
int dm_mac_preferred_languages(char *out, size_t n) {
    if (!out || !n) return 0;   // no buffer: nothing to write (and no memcpy below can see a NULL out)
    out[0] = 0;
    int count = 0;
    size_t used = 0;
    @autoreleasepool {   // called from the game's first language query, on a thread no AppKit run loop drains: release the list here
        for (NSString *tag in [NSLocale preferredLanguages]) {
            const char *s = tag.UTF8String;
            if (!s || !*s) continue;
            size_t len = strlen(s);
            if (len > 15) continue;                                    // no language identifier is longer; anything else is not one
            if (used + len + 1 + (count ? 1 : 0) > n) break;           // the tag, its terminating NUL and the separating space before it
            if (count) out[used++] = ' ';
            memcpy(out + used, s, len + 1);
            used += len;
            count++;
        }
    }
    return count;
}
