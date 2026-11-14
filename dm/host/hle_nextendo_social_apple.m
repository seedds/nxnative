// Real, bounded friend snapshots. HTTP/schema failures never become empty lists.
#import "nextendo_api_apple.h"
#include "nextendo_social.h"
#include "nextendo_account.h"
#include "net_socket.h"
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>
#include "game_profile.h"

static bool number(id value) { return [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value)!=CFBooleanGetTypeID(); }
static NSTimeInterval monotonic_time(void) { struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now); return now.tv_sec+now.tv_nsec/1e9; }
static bool unsigned_value(id value,uint64_t *output) {
    if (!number(value)) return false;
    NSString *text=[value stringValue]; if (!text.length || text.length>20) return false;
    uint64_t result=0;
    for (NSUInteger i=0;i<text.length;i++) { unichar c=[text characterAtIndex:i];
        if (c<'0'||c>'9'||result>(UINT64_MAX-(c-'0'))/10) return false; result=result*10+c-'0'; }
    *output=result; return true;
}
static bool hexadecimal(id value,uint64_t *output) {
    if (![value isKindOfClass:NSString.class] || ![value length] || [value length]>16) return false;
    uint64_t result=0;
    for (NSUInteger i=0;i<[value length];i++) { unichar c=[value characterAtIndex:i]; unsigned digit;
        if (c>='0'&&c<='9')digit=c-'0'; else if(c>='a'&&c<='f')digit=c-'a'+10; else if(c>='A'&&c<='F')digit=c-'A'+10; else return false;
        result=(result<<4)|digit;
    }
    *output=result; return true;
}
static bool nickname(id value,char output[33]) {
    if (![value isKindOfClass:NSString.class] || ![value length] || [value length]>128 || [value rangeOfString:[NSString stringWithFormat:@"%C",(unichar)0]].location!=NSNotFound) return false;
    NSMutableData *bytes=[NSMutableData new];
    [value enumerateSubstringsInRange:NSMakeRange(0,[value length]) options:NSStringEnumerationByComposedCharacterSequences
        usingBlock:^(NSString *substring,NSRange range,NSRange enclosing,BOOL *stop) {
            (void)range; (void)enclosing; NSData *part=[substring dataUsingEncoding:NSUTF8StringEncoding];
            if (!part || bytes.length+part.length>32) { *stop=YES; return; } [bytes appendData:part];
        }];
    if (!bytes.length) return false; memcpy(output,bytes.bytes,bytes.length); output[bytes.length]=0; return true;
}
static NSData *decode_friends(NSData *bytes,size_t *count) {
    id root=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
    id rows=[root isKindOfClass:NSDictionary.class]?root[@"friends"]:nil;
    if (![rows isKindOfClass:NSArray.class] || [rows count]>DM_SOCIAL_FRIEND_MAX) return nil;
    NSMutableData *records=[NSMutableData dataWithLength:[rows count]*sizeof(dm_social_friend)];
    dm_social_friend *output=records.mutableBytes; NSMutableSet<NSNumber*> *seen=[NSMutableSet new];
    for (NSUInteger i=0;i<[rows count];i++) {
        id row=rows[i]; dm_social_friend *item=&output[i];
        if (![row isKindOfClass:NSDictionary.class] || !unsigned_value(row[@"pid"],&item->pid) || !item->pid ||
            [seen containsObject:@(item->pid)] || !nickname(row[@"name"],item->nickname)) return nil;
        [seen addObject:@(item->pid)];
        if (row[@"nsa"] && row[@"nsa"]!=NSNull.null && ![row[@"nsa"] isEqual:@""] && !hexadecimal(row[@"nsa"],&item->nsa)) return nil;
        if (row[@"favorite"]) {
            if (![row[@"favorite"] isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)row[@"favorite"])!=CFBooleanGetTypeID()) return nil;
            item->favorite=[row[@"favorite"] boolValue];
        }
        id presence=row[@"presence"];
        if (!presence || presence==NSNull.null) continue;
        if (![presence isKindOfClass:NSDictionary.class]) return nil;
        uint64_t status=0;
        if (presence[@"status"] && (!unsigned_value(presence[@"status"],&status) || status>2)) return nil;
        item->presence=(uint32_t)status;
        if (presence[@"app_id"] && presence[@"app_id"]!=NSNull.null && ![presence[@"app_id"] isEqual:@""] && !hexadecimal(presence[@"app_id"],&item->application)) return nil;
        id encoded=presence[@"app_field"];
        if (encoded && encoded!=NSNull.null) {
            if (![encoded isKindOfClass:NSString.class] || [encoded length]>256) return nil;
            NSData *field=[[NSData alloc] initWithBase64EncodedString:encoded options:0];
            if (!field || field.length>DM_SOCIAL_APP_FIELD || ![[field base64EncodedStringWithOptions:0] isEqual:encoded]) return nil;
            memcpy(item->app_field,field.bytes,field.length);
        }
    }
    *count=[rows count]; return records;
}

static uint32_t friends_snapshot(dm_social_friend **output,size_t *count,bool refresh) { @autoreleasepool {
    if (count) *count=0; if (output) *output=NULL; if (!output || !count) return DM_ACCOUNT_FAIL;
    NSData *identity=dm_nextendo_api_identity(); if (!identity) return DM_ACCOUNT_FAIL;
    static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    static NSData *cached_identity,*cached_records; static size_t cached_count; static NSTimeInterval fetched;
    pthread_mutex_lock(&mutex); NSData *records=nil; size_t n=0;
    if (!refresh && [identity isEqual:cached_identity] && monotonic_time()-fetched<30) { records=cached_records; n=cached_count; }
    pthread_mutex_unlock(&mutex);
    if (!records) {
        uint8_t *buffer=malloc(1024*1024); if (!buffer) return DM_ACCOUNT_FAIL; size_t length=0;
        uint32_t result=dm_nextendo_api_request("friends",NULL,0,buffer,1024*1024,&length);
        NSData *data=result||!length?nil:[NSData dataWithBytes:buffer length:length]; free(buffer);
        if (!data || !(records=decode_friends(data,&n)) || ![identity isEqual:dm_nextendo_api_identity()]) return DM_ACCOUNT_FAIL;
        pthread_mutex_lock(&mutex); cached_identity=identity; cached_records=records; cached_count=n; fetched=monotonic_time(); pthread_mutex_unlock(&mutex);
    }
    if (![identity isEqual:dm_nextendo_api_identity()]) return DM_ACCOUNT_FAIL;
    // Every caller owns its snapshot; sign-out/account changes cannot mutate it.
    if (n) { *output=malloc(records.length); if (!*output) return DM_ACCOUNT_FAIL; memcpy(*output,records.bytes,records.length); }
    *count=n; return 0;
} }
uint32_t dm_nextendo_social_friends(dm_social_friend **output,size_t *count) { return friends_snapshot(output,count,false); }
uint32_t dm_nextendo_social_friends_refresh(dm_social_friend **output,size_t *count) { return friends_snapshot(output,count,true); }

// Nextendo shows a player online while presence keeps arriving (Mush8 sends it again every 45 seconds). The last presence a game published
// is sent again on that period until the game publishes status 0 (its online session closed) or a send fails
enum { PRESENCE_EVERY_S = 45 };
static pthread_mutex_t g_presence_send = PTHREAD_MUTEX_INITIALIZER;   // one presence request at a time, so a repeat never lands after a newer publish; taken before g_presence_lock
static pthread_mutex_t g_presence_lock = PTHREAD_MUTEX_INITIALIZER;
static NSData *g_presence_body;           // the last published body while its status is not 0
static dispatch_source_t g_presence_timer;
static uint32_t presence_post(NSData *body,NSData *identity) {
    uint8_t response[4096];size_t count=0;
    uint32_t result=dm_nextendo_api_request("presence",body.bytes,body.length,response,sizeof response,&count);
    return !result&&[identity isEqual:dm_nextendo_api_identity()]?0:DM_ACCOUNT_FAIL;
}
static void presence_keep(NSData *body) {   // nil stops the repeats
    pthread_mutex_lock(&g_presence_lock);
    g_presence_body=body;
    if (body && !g_presence_timer) {
        g_presence_timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_global_queue(QOS_CLASS_UTILITY,0));
        dispatch_source_set_timer(g_presence_timer,dispatch_time(DISPATCH_TIME_NOW,PRESENCE_EVERY_S*NSEC_PER_SEC),PRESENCE_EVERY_S*NSEC_PER_SEC,5*NSEC_PER_SEC);
        dispatch_source_set_event_handler(g_presence_timer,^{ @autoreleasepool {
            pthread_mutex_lock(&g_presence_send);
            pthread_mutex_lock(&g_presence_lock); NSData *again=g_presence_body; pthread_mutex_unlock(&g_presence_lock);
            NSData *identity=again?dm_nextendo_api_identity():nil;
            if (again && (!identity || presence_post(again,identity))) {
                pthread_mutex_lock(&g_presence_lock); if (g_presence_body==again) g_presence_body=nil; pthread_mutex_unlock(&g_presence_lock);
            }
            pthread_mutex_unlock(&g_presence_send);
        } });
        dispatch_resume(g_presence_timer);
    }
    pthread_mutex_unlock(&g_presence_lock);
}
static NSString *game_title(void) { return [@DM_GAME_ID lowercaseString]; }
static NSString *game_name(void) {   // the app's name, the game's title ID without one
    id name=[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
    return [name isKindOfClass:NSString.class]&&[name length]?name:game_title();
}
// The game's own description of what the player does (UserPresence::SetDescription), which Nextendo shows beside the game's name. The SDK keeps
// it in the application field, a run of NUL-terminated strings, key and value in turn, up to an empty key (nn::friends::detail::PresenceAccessor;
// Mario Kart 8 Deluxe's SearchKey sdk+0x11c950), under the key sys_description. "" without one or for a field that does not end its strings
static NSString *presence_description(const uint8_t *field,size_t size) {
    size_t at=0;
    while (at<size && field[at]) {
        const char *key=(const char *)field+at; size_t key_length=strnlen(key,size-at); at+=key_length+1; if (at>=size) return @"";
        const char *value=(const char *)field+at; size_t value_length=strnlen(value,size-at); if (at+value_length>=size) return @""; at+=value_length+1;
        if (key_length==15 && !memcmp(key,"sys_description",15))
            return [[NSString alloc] initWithBytes:value length:value_length encoding:NSUTF8StringEncoding] ?: @"";
    }
    return @"";
}
uint32_t dm_nextendo_social_publish(unsigned status,const uint8_t *app_field,size_t size) { @autoreleasepool {
    if(status>2||size>DM_SOCIAL_APP_FIELD||(size&&!app_field))return DM_ACCOUNT_FAIL;
    NSData *identity=dm_nextendo_api_identity();if(!identity)return DM_ACCOUNT_FAIL;
    NSData *field=[NSData dataWithBytes:app_field length:size];
    NSDictionary *object=@{@"status":@(status),@"app_id":game_title(),@"jeu":game_name(),@"app_field":[field base64EncodedStringWithOptions:0],
        @"plateforme":@"nxnative",@"app_detail":size?presence_description(app_field,size):@""};
    NSData *body=[NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    if(!body)return DM_ACCOUNT_FAIL;
    pthread_mutex_lock(&g_presence_send);
    uint32_t result=presence_post(body,identity);
    presence_keep(!result&&status?body:nil);
    pthread_mutex_unlock(&g_presence_send);
    return result;
} }

// ---- The account's Mii and play time, from the start of a Nextendo session (loader.c nextendo_main) ----
void dm_guest_host_wait_begin(void);
void dm_guest_host_wait_end(void);
enum { MII_STORE_SIZE=0x44, HISTORY_EVERY_S=60, PROFILE_WAIT_S=5 };
static dispatch_group_t g_profile_fetch;   // the account's profile, asked for once at the start
static NSData *g_account_mii;              // its Mii's StoreData, written before g_profile_fetch completes

// /api/profile's Mii: the base64 of a Switch StoreData. nil without one, or for anything else (logged)
static NSData *account_mii_fetch(void) {
    size_t capacity=1024*1024,size=0; uint8_t *buffer=malloc(capacity); if(!buffer)return nil;
    uint32_t result=dm_nextendo_api_request("profile",NULL,0,buffer,capacity,&size);
    NSDictionary *answer=result?nil:[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytesNoCopy:buffer length:size freeWhenDone:NO] options:0 error:nil];
    free(buffer);
    if(result)return nil;   // dm_nextendo_api_request logged the refusal
    id profile=[answer isKindOfClass:NSDictionary.class]?answer[@"profile"]:nil, mii=[profile isKindOfClass:NSDictionary.class]?profile[@"mii"]:nil;
    if(![mii isKindOfClass:NSString.class]||![mii length]){fprintf(stderr,"[dm] nextendo: the account has no Mii on Nextendo\n");return nil;}
    NSData *store=[[NSData alloc] initWithBase64EncodedString:mii options:0];
    if(store.length!=MII_STORE_SIZE){fprintf(stderr,"[dm] [RED] nextendo: the account's Mii is not a StoreData (%lu bytes): it is not listed\n",(unsigned long)store.length);return nil;}
    return store;
}
// hle_mii.c: the account's Mii for the game's Mii database, waiting a few seconds at most for the profile asked for at the start
int dm_nextendo_account_mii(uint8_t store[MII_STORE_SIZE]) {
    if(!store||!g_profile_fetch||!dm_net_nextendo_enabled())return 0;
    dm_guest_host_wait_begin(); long late=dispatch_group_wait(g_profile_fetch,dispatch_time(DISPATCH_TIME_NOW,PROFILE_WAIT_S*NSEC_PER_SEC)); dm_guest_host_wait_end();
    if(late){fprintf(stderr,"[dm] [RED] nextendo: the account's profile did not arrive in %d s: its Mii is not listed this run\n",PROFILE_WAIT_S);return 0;}
    if(g_account_mii.length!=MII_STORE_SIZE)return 0;
    memcpy(store,g_account_mii.bytes,MII_STORE_SIZE); return 1;
}

// Play time: Nextendo keeps each game's total seconds per account and keeps the larger of what it has and what it is sent (/api/history), so
// the host reads the account's total for this game once and then sends it plus this run's time every minute. A minute counts only while the timer
// runs: a gap longer than that (the app suspended) counts as one period at most
static struct { bool known; int told; int64_t base; double played, last; } g_history;   // told: 1 the read failure was logged, 2 the first total sent
static bool history_base(void) {   // the account's seconds for this game so far; false while they cannot be read
    size_t capacity=8*1024*1024,size=0; uint8_t *buffer=malloc(capacity); if(!buffer)return false;
    uint32_t result=dm_nextendo_api_request("history",NULL,0,buffer,capacity,&size);
    NSDictionary *answer=result?nil:[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytesNoCopy:buffer length:size freeWhenDone:NO] options:0 error:nil];
    free(buffer);
    id entries=[answer isKindOfClass:NSDictionary.class]?answer[@"history"]:nil;
    if(![entries isKindOfClass:NSArray.class])return false;
    int64_t seconds=0;
    for(id entry in entries){
        if(![entry isKindOfClass:NSDictionary.class]||![entry[@"title_id"] isKindOfClass:NSString.class])continue;
        uint64_t value=0;
        if([entry[@"title_id"] caseInsensitiveCompare:game_title()]==NSOrderedSame&&unsigned_value(entry[@"seconds"],&value)&&value<INT64_MAX/2)seconds=(int64_t)value;
    }
    g_history.base=seconds; g_history.known=true; return true;
}
static bool history_tick(void) { @autoreleasepool {   // false stops the timer
    double now=monotonic_time(); g_history.played+=fmin(now-g_history.last,HISTORY_EVERY_S); g_history.last=now;
    if(!dm_nextendo_api_bearer()){fprintf(stderr,"[dm] [RED] nextendo: play time is not sent: no Nextendo sign-in\n");return false;}
    if(!g_history.known&&!history_base()){
        if(!g_history.told)fprintf(stderr,"[dm] [RED] nextendo: the account's play time could not be read: none is sent until it can\n");
        g_history.told=1; return true;
    }
    int64_t seconds=g_history.base+(int64_t)g_history.played;
    NSISO8601DateFormatter *format=[NSISO8601DateFormatter new]; format.timeZone=[NSTimeZone timeZoneWithAbbreviation:@"UTC"];
    NSDictionary *object=@{@"history":@[@{@"title_id":game_title(),@"name":game_name(),@"seconds":@(seconds),@"last_played":[format stringFromDate:NSDate.date]}]};
    NSData *body=[NSJSONSerialization dataWithJSONObject:object options:0 error:nil]; size_t size=0;
    if(body.length&&!dm_nextendo_api_request("history",body.bytes,body.length,NULL,0,&size)&&g_history.told!=2){
        fprintf(stderr,"[dm] nextendo: play time on Nextendo: %lld min of this game in all\n",(long long)(seconds/60)); g_history.told=2;
    }
    return true;
} }
void dm_nextendo_session_start(void) {
    static atomic_bool started; if(atomic_exchange(&started,true))return;
    dispatch_queue_t queue=dispatch_get_global_queue(QOS_CLASS_UTILITY,0);
    g_profile_fetch=dispatch_group_create();
    dispatch_group_async(g_profile_fetch,queue,^{ @autoreleasepool { g_account_mii=account_mii_fetch(); } });
    g_history.last=monotonic_time();
    dispatch_queue_t serial=dispatch_queue_create("nextendo.history",DISPATCH_QUEUE_SERIAL);
    dispatch_source_t timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,serial);
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,HISTORY_EVERY_S*NSEC_PER_SEC),HISTORY_EVERY_S*NSEC_PER_SEC,5*NSEC_PER_SEC);
    __block dispatch_source_t keep=timer;
    dispatch_source_set_event_handler(timer,^{ if(!history_tick()){dispatch_source_cancel(keep);keep=nil;} });
    dispatch_resume(timer);
}
