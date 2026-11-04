// Real, bounded friend snapshots. HTTP/schema failures never become empty lists.
#import "nextendo_api_apple.h"
#include "nextendo_social.h"
#include "nextendo_account.h"
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
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

uint32_t dm_nextendo_social_publish(unsigned status,const uint8_t *app_field,size_t size) { @autoreleasepool {
    if(status>2||size>DM_SOCIAL_APP_FIELD||(size&&!app_field))return DM_ACCOUNT_FAIL;
    NSData *identity=dm_nextendo_api_identity();if(!identity)return DM_ACCOUNT_FAIL;
    NSString *title=[@DM_GAME_ID lowercaseString];
    id name=[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
    if(![name isKindOfClass:NSString.class]||![name length])name=title;
    NSData *field=[NSData dataWithBytes:app_field length:size];
    NSDictionary *object=@{@"status":@(status),@"app_id":title,@"jeu":name,@"app_field":[field base64EncodedStringWithOptions:0],
        @"plateforme":@"nxnative",@"app_detail":@""};
    NSData *body=[NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    if(!body)return DM_ACCOUNT_FAIL;
    uint8_t response[4096];size_t count=0;
    uint32_t result=dm_nextendo_api_request("presence",body.bytes,body.length,response,sizeof response,&count);
    return !result&&[identity isEqual:dm_nextendo_api_identity()]?0:DM_ACCOUNT_FAIL;
} }
