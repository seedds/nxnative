// Original bounded canonical-provider HTTPS transport for social/account data.
// This uses normal system TLS, with no cookies, redirects or alternate origins.
#import "nextendo_api_apple.h"
#include "nextendo_account.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>

void dm_guest_host_wait_begin(void);
void dm_guest_host_wait_end(void);
enum { API_MAX = 1024 * 1024 };

bool dm_nextendo_api_baas_namespace(const char *kind) { @autoreleasepool {
    if (!kind || (strcmp(kind,"account") && strcmp(kind,"friends"))) return false;
    const char *routes = getenv("DM_NEXTENDO_ROUTES");
    if (!routes || routes[0]!='/' || strnlen(routes,PATH_MAX)>=PATH_MAX) return false;
    NSString *path = [[[NSString stringWithUTF8String:routes] stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"manifest.json"];
    int fd=open(path.fileSystemRepresentation,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK); if(fd<0)return false;
    struct stat info; if (fstat(fd,&info) || !S_ISREG(info.st_mode) ||
        (info.st_uid!=geteuid() && info.st_uid!=0) || (info.st_mode&022) || info.st_size<=0 || info.st_size>65536) { close(fd); return false; }
    NSMutableData *bytes=[NSMutableData dataWithLength:(NSUInteger)info.st_size]; size_t done=0;
    while(done<bytes.length) { ssize_t n=read(fd,(uint8_t*)bytes.mutableBytes+done,bytes.length-done); if(n<0&&errno==EINTR)continue; if(n<=0){close(fd);return false;}done+=(size_t)n; }
    close(fd);
    id object = [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
    if (![object isKindOfClass:NSDictionary.class] || ![object[@"identity"] isKindOfClass:NSDictionary.class]) return false;
    NSString *namespace = [NSString stringWithUTF8String:kind];
    return namespace && [object[@"identity"][namespace] isEqual:@"baas"];
} }

uint32_t dm_nextendo_api_baas_id(uint64_t *output) { @autoreleasepool {
    if (!output) return DM_ACCOUNT_FAIL; *output = 0;
    NSData *identity = dm_nextendo_api_identity(); if (!identity) return DM_ACCOUNT_FAIL;
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; static NSData *cached_identity; static uint64_t cached_id;
    pthread_mutex_lock(&mutex); uint64_t existing = [cached_identity isEqual:identity] ? cached_id : 0; pthread_mutex_unlock(&mutex);
    if (existing) { *output=existing; return 0; }
    uint8_t *buffer=malloc(API_MAX); if (!buffer) return DM_ACCOUNT_FAIL; size_t size=0;
    uint32_t result=dm_nextendo_api_request("me",NULL,0,buffer,API_MAX,&size);
    NSData *bytes=result?nil:[NSData dataWithBytes:buffer length:size]; free(buffer);
    id document=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil]:nil;
    id field=[document isKindOfClass:NSDictionary.class]?document[@"nsa"]:nil;
    if (![field isKindOfClass:NSString.class] || ![field length] || [field length]>16 || ![identity isEqual:dm_nextendo_api_identity()]) return DM_ACCOUNT_FAIL;
    uint64_t value=0;
    for (NSUInteger i=0;i<[field length];i++) { unichar c=[field characterAtIndex:i]; unsigned digit;
        if (c>='0' && c<='9') digit=c-'0'; else if(c>='a' && c<='f') digit=c-'a'+10; else if(c>='A' && c<='F') digit=c-'A'+10; else return DM_ACCOUNT_FAIL;
        value=(value<<4)|digit;
    }
    if (!value) return DM_ACCOUNT_FAIL;
    pthread_mutex_lock(&mutex); cached_identity=identity; cached_id=value; pthread_mutex_unlock(&mutex);
    *output=value; return 0;
} }

// The field names /api/me gives, logged once (names only, never a value; a name that is not plain letters, digits and _ is left out): what else a
// device log shows the account has, to answer more of the game's account questions from it
static bool plain_name(id key) {
    if (![key isKindOfClass:NSString.class] || ![key length] || [key length] > 40) return false;
    for (NSUInteger i = 0; i < [key length]; i++) { unichar c = [key characterAtIndex:i]; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false; }
    return true;
}
static void me_fields(id document) {
    if (![document isKindOfClass:NSDictionary.class]) return;
    static dispatch_once_t once; dispatch_once(&once, ^{
        NSMutableArray *names = [NSMutableArray new];
        for (id key in [document allKeys]) { if (!plain_name(key)) continue; id value = document[key];
            if ([value isKindOfClass:NSDictionary.class]) { for (id inner in [value allKeys]) if (plain_name(inner)) [names addObject:[NSString stringWithFormat:@"%@.%@", key, inner]]; }
            else [names addObject:key]; }
        [names sortUsingSelector:@selector(compare:)]; NSString *line = [names componentsJoinedByString:@" "]; if (line.length > 800) line = [line substringToIndex:800];
        fprintf(stderr, "nextendo: account fields: %s\n", line.UTF8String);
    });
}
// The Nextendo account's country, two capital letters from /api/me's account.country (the player picks it at sign-up), else "": never a guessed one.
// Ryujinx-Nextendo (NextendoCountry.cs) answers CachedNintendoAccountInfo::GetCountry from the same field. Fetched once per signed-in account
uint32_t dm_nextendo_account_country(char output[3]) { @autoreleasepool {
    if (!output) return DM_ACCOUNT_FAIL; output[0]=0;
    NSData *identity = dm_nextendo_api_identity();
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; static NSData *cached_identity; static char cached[3];
    pthread_mutex_lock(&mutex); bool known = identity && [cached_identity isEqual:identity]; if (known) memcpy(output,cached,3); pthread_mutex_unlock(&mutex);
    if (known) return 0;
    uint8_t *buffer=malloc(API_MAX); if (!buffer) return DM_ACCOUNT_FAIL; size_t size=0;
    uint32_t result=dm_nextendo_api_request("me",NULL,0,buffer,API_MAX,&size);
    NSData *bytes=result?nil:[NSData dataWithBytes:buffer length:size]; free(buffer); if (result) return result;
    id document=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil]; me_fields(document);
    id account=[document isKindOfClass:NSDictionary.class]?document[@"account"]:nil;
    id field=[account isKindOfClass:NSDictionary.class]?account[@"country"]:nil;
    NSString *country=[field isKindOfClass:NSString.class]?[field stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].uppercaseString:@"";
    char code[3]={0};
    if (country.length==2) { unichar a=[country characterAtIndex:0], b=[country characterAtIndex:1]; if (a>='A' && a<='Z' && b>='A' && b<='Z') { code[0]=(char)a; code[1]=(char)b; } }
    if (identity && [identity isEqual:dm_nextendo_api_identity()]) { pthread_mutex_lock(&mutex); cached_identity=identity; memcpy(cached,code,3); pthread_mutex_unlock(&mutex); }
    memcpy(output,code,3); return 0;
} }

// The Nextendo account's birthday as YYYY-MM-DD, else "". Which /api/me field carries it is not confirmed yet (the account fields line above will show it): the
// usual names are tried, in account and at the top level, and only a value that starts with a plain date is taken (a time after it is dropped). Fetched once per account
uint32_t dm_nextendo_account_birthday(char output[11]) { @autoreleasepool {
    if (!output) return DM_ACCOUNT_FAIL; output[0]=0;
    NSData *identity = dm_nextendo_api_identity();
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; static NSData *cached_identity; static char cached[11];
    pthread_mutex_lock(&mutex); bool known = identity && [cached_identity isEqual:identity]; if (known) memcpy(output,cached,11); pthread_mutex_unlock(&mutex);
    if (known) return 0;
    uint8_t *buffer=malloc(API_MAX); if (!buffer) return DM_ACCOUNT_FAIL; size_t size=0;
    uint32_t result=dm_nextendo_api_request("me",NULL,0,buffer,API_MAX,&size);
    NSData *bytes=result?nil:[NSData dataWithBytes:buffer length:size]; free(buffer); if (result) return result;
    id document=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil]; me_fields(document);
    id account=[document isKindOfClass:NSDictionary.class]?document[@"account"]:nil;
    char date[11]={0};
    for (id place in @[account ?: NSNull.null, document ?: NSNull.null]) {
        if (![place isKindOfClass:NSDictionary.class] || date[0]) continue;
        for (NSString *name in @[@"birthday", @"birth_date", @"birthdate", @"date_of_birth"]) {
            id field=place[name]; if (![field isKindOfClass:NSString.class] || [field length]<10) continue;
            const char *text=[[field substringToIndex:10] UTF8String]; bool plain=text && strlen(text)==10 && text[4]=='-' && text[7]=='-';
            for (int i=0;plain && i<10;i++) if (i!=4 && i!=7 && (text[i]<'0' || text[i]>'9')) plain=false;
            if (plain && ([field length]==10 || [field characterAtIndex:10]=='T' || [field characterAtIndex:10]==' ')) { memcpy(date,text,11); break; }
        }
    }
    if (identity && [identity isEqual:dm_nextendo_api_identity()]) { pthread_mutex_lock(&mutex); cached_identity=identity; memcpy(cached,date,11); pthread_mutex_unlock(&mutex); }
    memcpy(output,date,11); return 0;
} }

@interface DmNextendoApiTransfer : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
@property(nonatomic) NSLock *lock;
@property(nonatomic) NSMutableData *data;
@property(nonatomic) dispatch_semaphore_t completed;
@property(nonatomic) BOOL failed;
@property(nonatomic) NSInteger status;
@end
@implementation DmNextendoApiTransfer
- (instancetype)init {
    if ((self = [super init])) { _lock = [NSLock new]; _data = [NSMutableData new]; _completed = dispatch_semaphore_create(0); }
    return self;
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response
        newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))completion {
    (void)session; (void)task; (void)response; (void)request;
    [self.lock lock]; self.failed = YES; [self.lock unlock]; completion(nil);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response
        completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    (void)session; (void)task;
    [self.lock lock];
    self.failed = self.failed || ![response isKindOfClass:NSHTTPURLResponse.class] || response.expectedContentLength > API_MAX;
    self.status = [response isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)response).statusCode : 0;
    BOOL failed = self.failed; [self.lock unlock]; completion(failed ? NSURLSessionResponseCancel : NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)session; [self.lock lock];
    if (data.length > API_MAX - self.data.length) self.failed = YES;
    else if (!self.failed) [self.data appendData:data];
    BOOL failed = self.failed; [self.lock unlock]; if (failed) [task cancel];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void)session; (void)task; [self.lock lock]; self.failed = self.failed || error != nil; [self.lock unlock];
    dispatch_semaphore_signal(self.completed);
}
@end

#ifdef DM_NEXTENDO_API_TEST
static dm_nextendo_api_transport test_transport;
void dm_nextendo_api_test_transport(dm_nextendo_api_transport transport) { test_transport = transport; }
#endif

uint32_t dm_nextendo_api_request(const char *resource, const uint8_t *body, size_t body_size,
                               uint8_t *output, size_t capacity, size_t *output_size) { @autoreleasepool {
    if (output_size) *output_size = 0;
    if (!resource || !output || !output_size || !capacity || capacity > API_MAX || body_size > 4096 || (body_size && !body)) return DM_ACCOUNT_FAIL;
    BOOL presence = !strcmp(resource,"presence");
    if (!presence && strcmp(resource,"friends") && strcmp(resource,"me")) return DM_ACCOUNT_FAIL;
    if ((!presence && body_size) || (presence && !body_size)) return DM_ACCOUNT_FAIL;
    NSString *proof = dm_nextendo_api_bearer(); if (!proof.length) return DM_ACCOUNT_FAIL;
    NSString *authorization = [@"Bearer " stringByAppendingString:proof];
    NSString *resourceName = [NSString stringWithUTF8String:resource]; if (!resourceName) return DM_ACCOUNT_FAIL;
    NSString *url = [@"https://nextendo.network/api/" stringByAppendingString:resourceName];
    NSString *method = presence ? @"POST" : @"GET";
#ifdef DM_NEXTENDO_API_TEST
    if (test_transport) {
        size_t size = 0; unsigned status = 0;
        if (!test_transport(url.UTF8String,method.UTF8String,authorization.UTF8String,body,body_size,output,capacity,&size,&status) ||
            status < 200 || status >= 300 || size > capacity) return DM_ACCOUNT_FAIL;
        *output_size = size; return 0;
    }
#endif
    NSURL *endpoint = [NSURL URLWithString:url]; if (!endpoint) return DM_ACCOUNT_FAIL;
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:endpoint];
    request.HTTPMethod = method; request.timeoutInterval = 12;
    [request setValue:authorization forHTTPHeaderField:@"Authorization"];
    [request setValue:@"nxnative" forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"application/json" forHTTPHeaderField:@"Accept"];
    if (presence) { request.HTTPBody = [NSData dataWithBytes:body length:body_size]; [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"]; }
    NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.HTTPCookieStorage = nil; configuration.HTTPShouldSetCookies = NO; configuration.URLCredentialStorage = nil;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData; configuration.timeoutIntervalForResource = 15;
    DmNextendoApiTransfer *transfer = [DmNextendoApiTransfer new];
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration delegate:transfer delegateQueue:nil];
    NSURLSessionDataTask *task = [session dataTaskWithRequest:request]; [task resume];
    dm_guest_host_wait_begin(); long timed_out = dispatch_semaphore_wait(transfer.completed,dispatch_time(DISPATCH_TIME_NOW,15 * NSEC_PER_SEC)); dm_guest_host_wait_end();
    if (timed_out) { [task cancel]; [session invalidateAndCancel]; return DM_ACCOUNT_FAIL; }
    [transfer.lock lock];
    BOOL valid = !transfer.failed && transfer.status >= 200 && transfer.status < 300 && transfer.data.length <= capacity;
    if (valid) { memcpy(output,transfer.data.bytes,transfer.data.length); *output_size = transfer.data.length; }
    NSInteger status = transfer.status;
    [transfer.lock unlock]; [session finishTasksAndInvalidate];
    // A refused request is logged once per resource and status (no body, no token), so a device log shows whether Nextendo takes this
    // bearer for the resource: 401 or 403 here would mean it wants another kind of token
    if (!valid) {
        static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER; static NSMutableSet<NSString *> *seen;
        NSString *key = [NSString stringWithFormat:@"%@ %ld", resourceName, (long)status];
        pthread_mutex_lock(&seen_lock); if (!seen) seen = [NSMutableSet new]; BOOL first = ![seen containsObject:key]; [seen addObject:key]; pthread_mutex_unlock(&seen_lock);
        NSString *what = status ? [NSString stringWithFormat:@"HTTP %ld", (long)status] : @"nothing (no connection, or an oversized answer)";
        if (first) fprintf(stderr, "[dm] [RED] nextendo: /api/%s answered %s\n", resource, what.UTF8String);
    }
    return valid ? 0 : DM_ACCOUNT_FAIL;
} }
