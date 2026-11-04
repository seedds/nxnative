// The built-in launchers and guest services use the same launch-time roster.
#import <Foundation/Foundation.h>
#include "local_users.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static dm_user_roster roster;
static const char *problem;
static char edit_path[PATH_MAX];
static int selected, locked;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t selection_lock = PTHREAD_MUTEX_INITIALIZER;

int dm_users_prepare_file(const char *base_directory, const char *default_file) {
    const char *path = getenv("DM_USERS_FILE");
    if (!path) {
        if (strlcpy(edit_path, default_file, sizeof edit_path) >= sizeof edit_path) return 0;
        struct stat st;
        if (lstat(default_file, &st) && errno == ENOENT) return 1;
    } else if (*path == '/') {
        if (strlcpy(edit_path, path, sizeof edit_path) >= sizeof edit_path) return 0;
    } else if (*path) {
        int n = snprintf(edit_path, sizeof edit_path, "%s/%s", base_directory, path);
        if (n < 0 || (size_t)n >= sizeof edit_path) return 0;
    } else return 0;
    return setenv("DM_USERS_FILE", edit_path, 1) == 0;
}

// Exact hex, with no sign, prefix or separator. Text is the high word followed by the low word.
static int parse_id(const char *s, dm_user_uid *uid) {
    if (!s || strlen(s) != 32) return 0;
    *uid = (dm_user_uid){0};
    for (int i = 0; i < 32; ++i) {
        unsigned c = (unsigned char)s[i], digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return 0;
        int word = i < 16 ? 1 : 0;
        uid->v[word] = (uid->v[word] << 4) | digit;
    }
    return uid->v[0] || uid->v[1];
}

static int index_of(const dm_user_roster *list, const dm_user_uid *uid) {
    if (uid) for (int i = 0; i < list->count; ++i) {
        if (uid->v[0] == list->users[i].uid.v[0] && uid->v[1] == list->users[i].uid.v[1]) return i;
    }
    return -1;
}

static int valid_nickname(NSString *name) {
    return [name isKindOfClass:NSString.class] && name.length &&
           [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= 32 &&
           [name rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location == NSNotFound;
}

static const char *decode_roster(id doc, dm_user_roster *list) {
    if (![doc isKindOfClass:[NSDictionary class]]) return "DM_USERS_FILE must contain a JSON object";
    id version = doc[@"version"], users = doc[@"users"];
    if (![version isKindOfClass:[NSNumber class]] || CFGetTypeID((__bridge CFTypeRef)version) == CFBooleanGetTypeID() ||
        ![version isEqual:@1]) return "Unsupported local user roster version (expected 1)";
    if (![users isKindOfClass:[NSArray class]] || [users count] < 1 || [users count] > DM_MAX_LOCAL_USERS)
        return "The local user roster must contain 1 to 8 users";
    for (id row in users) {
        if (![row isKindOfClass:[NSDictionary class]]) return "Each local user must be a JSON object";
        NSString *key = row[@"id"], *name = row[@"nickname"];
        dm_local_user *user = &list->users[list->count];
        if (![key isKindOfClass:[NSString class]] || key.length != 32 || !parse_id(key.UTF8String, &user->uid))
            return "A local user ID must be 32 nonzero-valued hexadecimal digits";
        if (index_of(list, &user->uid) >= 0) return "The local user roster contains a duplicate ID";
        snprintf(user->id, sizeof user->id, "%016llx%016llx", (unsigned long long)user->uid.v[1], (unsigned long long)user->uid.v[0]);
        if (!valid_nickname(name))
            return "A local nickname must be 1 to 32 UTF-8 bytes and contain no control characters";
        if (![name getCString:user->nickname maxLength:sizeof user->nickname encoding:NSUTF8StringEncoding])
            return "Invalid UTF-8 nickname";
        ++list->count;
    }
    list->configured = 1;
    return NULL;
}

static const char *read_file(const char *path, dm_user_roster *list, NSMutableDictionary **document, int *missing) {
    // Open nonblocking before checking the file type; an env override may name a FIFO.
    int fd = open(path, O_NONBLOCK | O_CLOEXEC | (document ? O_RDWR | O_NOFOLLOW : O_RDONLY));
    if (fd < 0) {
        if (missing) *missing = errno == ENOENT;
        return document ? "Cannot open the local user list for editing." : "Cannot open DM_USERS_FILE.";
    }
    struct stat st;
    char data[16385];
    size_t size = 0;
    int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size < (off_t)sizeof data;
    while (ok && size < sizeof data) {
        ssize_t n = read(fd, data + size, sizeof data - size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = n == 0; break; }
        size += (size_t)n;
    }
    close(fd);
    if (!ok || size == sizeof data) return "DM_USERS_FILE must be a readable regular file of at most 16 KiB";
    id doc = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:data length:size]
                                             options:document ? NSJSONReadingMutableContainers : 0 error:nil];
    const char *error = decode_roster(doc, list);
    if (!error && document) *document = doc;
    return error;
}

static void load_once(void) {
    @autoreleasepool {
        const char *path = getenv("DM_USERS_FILE"), *requested = getenv("DM_USER_ID");
        roster.configured = path != NULL;
        if (path) {
            problem = read_file(path, &roster, NULL, NULL);
            if (problem) { roster.count = 0; return; }
        } else {
            dm_local_user *user = &roster.users[0];
            parse_id(DM_LEGACY_USER_ID, &user->uid);
            strlcpy(user->id, DM_LEGACY_USER_ID, sizeof user->id);
            const char *nick = getenv("DM_NICK");
            if (!nick || ![NSString stringWithUTF8String:nick]) nick = "Player";
            size_t bytes = strnlen(nick, 32);
            while (bytes && ((unsigned char)nick[bytes] & 0xc0) == 0x80) --bytes;
            memcpy(user->nickname, nick, bytes);
            roster.count = 1;
        }
        if (requested) {
            dm_user_uid uid;
            selected = parse_id(requested, &uid) ? index_of(&roster, &uid) : -1;
            if (selected < 0) {
                problem = "DM_USER_ID does not identify a user in this roster";
                roster.count = 0;
            }
            locked = 1;
        }
    }
}

const dm_user_roster *dm_users(void) { pthread_once(&once, load_once); return &roster; }
int dm_users_count(void) { return dm_users()->count; }
const char *dm_users_error(void) { dm_users(); return problem; }

static const dm_local_user *at(int index) {
    return index >= 0 && index < dm_users_count() ? &roster.users[index] : NULL;
}

const dm_local_user *dm_user_find(const dm_user_uid *uid) {
    dm_users();
    return at(index_of(&roster, uid));
}

const char *dm_users_id_at(int index) { const dm_local_user *u = at(index); return u ? u->id : NULL; }
const char *dm_users_nickname_at(int index) { const dm_local_user *u = at(index); return u ? u->nickname : NULL; }

int dm_users_selected_index(void) {
    if (!dm_users_count()) return -1;
    pthread_mutex_lock(&selection_lock);
    int i = selected;
    pthread_mutex_unlock(&selection_lock);
    return i;
}

const dm_local_user *dm_selected_user(void) { return at(dm_users_selected_index()); }

int dm_users_selection_locked(void) {
    dm_users();
    pthread_mutex_lock(&selection_lock);
    int result = locked;
    pthread_mutex_unlock(&selection_lock);
    return result;
}

int dm_users_select_index(int index) {
    const dm_local_user *u = at(index);
    if (!u) return 0;
    pthread_mutex_lock(&selection_lock);
    int ok = !locked;
    if (ok && roster.configured && *edit_path) ok = !setenv("DM_USERS_FILE", edit_path, 1);
    if (ok) ok = !setenv("DM_USER_ID", u->id, 1);
    if (ok) selected = index;
    pthread_mutex_unlock(&selection_lock);
    return ok;
}

int dm_users_begin_session(void) {
    if (!dm_users_count()) return 0;
    pthread_mutex_lock(&selection_lock);
    locked = 1;
    pthread_mutex_unlock(&selection_lock);
    return 1;
}

int dm_users_write_json(FILE *out) {
    @autoreleasepool {
        const dm_local_user *active = dm_selected_user();
        if (!active) return 0;
        NSMutableArray *users = [NSMutableArray array];
        for (int i = 0; i < roster.count; ++i) {
            const dm_local_user *u = &roster.users[i];
            [users addObject:@{@"id": @(u->id), @"nickname": @(u->nickname)}];
        }
        NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"version": @1, @"users": users, @"selected": @(active->id)}
                                                       options:0 error:nil];
        return json && fwrite(json.bytes, 1, json.length, out) == json.length && fputc('\n', out) != EOF && !fflush(out);
    }
}

int dm_users_can_edit(void) {
    return *edit_path && dm_users_count() && !dm_users_selection_locked();
}

// Save under a separate lock file: atomic replacement changes the roster's inode.
static const char *edit_locked(const char *rename_id, NSString *name) {
    NSMutableDictionary *doc = nil;
    dm_user_roster latest = {0};
    int missing = 0;
    const char *error = read_file(edit_path, &latest, &doc, &missing);
    if (error) {
        if (!missing || roster.configured) return error;
        // First edit on an installation with the original implicit player.
        NSString *original = @(roster.users[0].nickname);
        if (!valid_nickname(original)) original = @"Player";
        doc = [@{@"version": @1, @"users": [@[
            [@{@"id": @DM_LEGACY_USER_ID, @"nickname": original} mutableCopy]
        ] mutableCopy]} mutableCopy];
        error = decode_roster(doc, &latest);
        if (error) return error;
    }
    NSMutableArray *users = doc[@"users"];
    int index;
    if (rename_id) {
        dm_user_uid uid;
        if (!parse_id(rename_id, &uid)) return "Invalid user ID.";
        int original = index_of(&roster, &uid);
        index = index_of(&latest, &uid);
        if (original < 0 || index < 0) return "This player is no longer in the roster. Reopen the launcher.";
        if (strcmp(latest.users[index].nickname, roster.users[original].nickname) &&
            ![users[index][@"nickname"] isEqual:name])
            return "This player was renamed in another launcher. Reopen the launcher to see the new name.";
        users[index][@"nickname"] = name;
    } else {
        if (latest.count >= DM_MAX_LOCAL_USERS) return "This roster already has eight players.";
        index = latest.count;
        NSString *key = [NSUUID.UUID.UUIDString.lowercaseString stringByReplacingOccurrencesOfString:@"-" withString:@""];
        [users addObject:@{@"id": key, @"nickname": name}];
    }
    dm_user_roster edited = {0};
    error = decode_roster(doc, &edited);
    if (error) return error;
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:doc options:NSJSONWritingPrettyPrinted error:nil];
    if (bytes.length > 16384) bytes = [NSJSONSerialization dataWithJSONObject:doc options:0 error:nil];
    if (!bytes || bytes.length > 16384) return "The edited roster exceeds the 16 KiB size limit.";
    NSString *path = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:edit_path length:strlen(edit_path)];
    if (![bytes writeToFile:path options:NSDataWritingAtomic error:nil])
        return "Could not save the player list. Check the folder's permissions and available space.";
    roster = edited;
    selected = index;
    return NULL;
}

const char *dm_users_edit(const char *rename_id, const char *nickname) {
    @autoreleasepool {
        NSString *name = nickname ? [NSString stringWithUTF8String:nickname] : nil;
        if (!valid_nickname(name) || ![name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length)
            return "Enter a name of 1 to 32 UTF-8 bytes, with no control characters.";
        dm_users();
        pthread_mutex_lock(&selection_lock);
        const char *error = "Player editing is unavailable for this launch.";
        if (!locked && roster.count && *edit_path) {
            char path[PATH_MAX];
            int n = snprintf(path, sizeof path, "%s.lock", edit_path);
            int fd = n >= 0 && (size_t)n < sizeof path ? open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600) : -1;
            struct stat st;
            error = "Cannot lock the user list. Check the folder's permissions.";
            if (fd >= 0) {
                if (!fstat(fd, &st) && S_ISREG(st.st_mode)) {
                    // Ordinary concurrent edits are brief. A lock held by an external
                    // editor must not leave the launcher waiting indefinitely.
                    uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 250000000;
                    do {
                        if (!flock(fd, LOCK_EX | LOCK_NB)) { error = edit_locked(rename_id, name); break; }
                        if (errno != EWOULDBLOCK && errno != EINTR) break;
                        error = "Another launcher is editing this roster. Try again.";
                        usleep(5000);
                    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
                }
                close(fd);
            }
        }
        pthread_mutex_unlock(&selection_lock);
        return error;
    }
}
