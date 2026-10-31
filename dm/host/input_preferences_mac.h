// Shared controls use the app's preferences domain, as dm/mac/launcher.c does.
// The host can also run outside a bundle; then its compiled game selects the domain.
#pragma once
#import <Foundation/Foundation.h>
#include "game_profile.h"
#include "input_map.h"
static NSString *input_preferences_domain(void) {
    return NSBundle.mainBundle.bundleIdentifier ?: [@"org.nxnative." DM_GAME_ID lowercaseString];
}
static BOOL input_preferences_enabled(void) {
    const char *off=getenv("DM_SAVED_CHOICES"); return !off || strcmp(off,"0");
}
static id input_preference(NSString *domain, NSString *key) {
    if(!domain || !input_preferences_enabled()) return nil;
    return CFBridgingRelease(CFPreferencesCopyAppValue((__bridge CFStringRef)key,(__bridge CFStringRef)domain));
}
static BOOL input_preference_write(NSString *domain, NSString *key, id value) {
    if(!input_preferences_enabled()) return YES; // this session only
    if(!domain) return NO;
    CFPreferencesSetAppValue((__bridge CFStringRef)key,(__bridge CFPropertyListRef)value,(__bridge CFStringRef)domain);
    return CFPreferencesAppSynchronize((__bridge CFStringRef)domain);
}
static BOOL input_env_contains(const char *list, const char *key) {
    if(!list) return NO;
    size_t n=strlen(key);
    for(const char *p=list; *p;) { while(*p==' ') p++; const char *e=strchr(p,' '); size_t len=e ? (size_t)(e-p) : strlen(p);
        if(len==n && !strncmp(p,key,n)) return YES;
        p+=len;
    }
    return NO;
}
static BOOL input_map_fixed(const char *env) {
    if(!getenv(env)) return NO;
    // A standalone host has no launcher provenance. In an app the launcher
    // distinguishes saved values from temporary environment/env.txt overrides.
    const char *launch=getenv("DM_ENV_LAUNCH_KEYS");
    return !launch || input_env_contains(launch,env) || input_env_contains(getenv("DM_ENV_FILE_KEYS"),env);
}
static NSString *input_preference_map(NSString *domain, const char *env, int pad) {
    if(input_map_fixed(env)) return @(getenv(env));
    id value=input_preference(domain,[@"launcher." stringByAppendingString:@(env)]);
    if(!value) return nil;
    if(![value isKindOfClass:NSString.class] || [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>1023 || !input_map_valid([value UTF8String],pad)) {
        fprintf(stderr,"[RED] saved %s ignored: invalid input map\n",env); return nil;
    }
    return value;
}
