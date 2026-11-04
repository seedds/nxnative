// Original synthetic HTTPS responses, signing key and resources. The Python
// runner compiles a temporary runtime copy with ONLY the fixture trust anchor
// and original XML digest substituted; production never accepts a runtime pin.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
static NSString *fixtureRoot,*fixtureMode;
static unsigned requests,catalogRequests,archiveRequests,conditionals;
@interface DmBcatFixtureProtocol : NSURLProtocol @end
@implementation DmBcatFixtureProtocol
+(BOOL)canInitWithRequest:(NSURLRequest *)r{(void)r;return YES;}
+(NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)r{return r;}
-(void)startLoading{
    @synchronized(DmBcatFixtureProtocol.class){requests++;}
    NSString *path=self.request.URL.path,*file=nil;NSInteger status=200;NSDictionary *headers=@{};
    if([path isEqual:@"/service-catalog/v1/game-catalog"]){
        @synchronized(DmBcatFixtureProtocol.class){catalogRequests++;}
        if([fixtureMode isEqual:@"interrupt"]){[@"ready" writeToFile:[fixtureRoot stringByAppendingPathComponent:@"interrupt-ready"] atomically:YES encoding:NSUTF8StringEncoding error:nil];return;}
        // The catalog uses part of the launch budget, then the archive stalls.
        // A fresh per-request deadline would incorrectly add another 12 seconds.
        if([fixtureMode isEqual:@"timeout"])[NSThread sleepForTimeInterval:4];
        if([fixtureMode isEqual:@"error"]){[self.client URLProtocol:self didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotConnectToHost userInfo:nil]];return;}
        file=[fixtureMode isEqual:@"invalid"]?@"invalid.json":@"catalog.json";
    }else if([path isEqual:@"/api/bcat/0100f8f0000a2000"]){
        @synchronized(DmBcatFixtureProtocol.class){archiveRequests++;if([self.request valueForHTTPHeaderField:@"If-None-Match"])conditionals++;}
        if([fixtureMode isEqual:@"timeout"])return;
        if([fixtureMode isEqual:@"304"]){status=304;headers=@{@"ETag":@"\"fixture-v1\""};}
        else{file=[fixtureMode isEqual:@"corrupt"]?@"corrupt.zip":[fixtureMode isEqual:@"v2"]?@"v2.zip":@"v1.zip";if(![fixtureMode isEqual:@"noetag"])headers=@{@"ETag":[fixtureMode isEqual:@"v2"]?@"\"fixture-v2\"":@"\"fixture-v1\""};}
    }else if([path hasSuffix:@"/System/GameConfigSetting.xml"])file=@"static.xml";
    else if([path hasSuffix:@"/dummy/dummy.txt"])file=@"dummy.txt";
    else{[self.client URLProtocol:self didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorUnsupportedURL userInfo:nil]];return;}
    NSData *data=file?[NSData dataWithContentsOfFile:[fixtureRoot stringByAppendingPathComponent:file]]:NSData.data;
    if(!data)abort();NSHTTPURLResponse *r=[[NSHTTPURLResponse alloc]initWithURL:self.request.URL statusCode:status HTTPVersion:@"HTTP/1.1" headerFields:headers];
    [self.client URLProtocol:self didReceiveResponse:r cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    [self.client URLProtocol:self didLoadData:data];[self.client URLProtocolDidFinishLoading:self];
}
-(void)stopLoading{}
@end
@interface NSURLSessionConfiguration(DmBcatFixture)
+(instancetype)dm_bcat_fixture_configuration;
@end
@implementation NSURLSessionConfiguration(DmBcatFixture)
+(instancetype)dm_bcat_fixture_configuration{
    NSURLSessionConfiguration *c=[self dm_bcat_fixture_configuration];c.protocolClasses=@[DmBcatFixtureProtocol.class];return c;
}
@end
#ifndef DM_BCAT_RUNTIME_INCLUDE
#define DM_BCAT_RUNTIME_INCLUDE "../hle_bcat_apple.m"
#endif
// Exercise public startup without reading/writing the developer's real cache.
// Only this fixture substitutes the OS-supplied private home directory.
static NSString *fixtureHome(void){return [fixtureRoot stringByAppendingPathComponent:[@"home-" stringByAppendingString:fixtureMode]];}
#define NSHomeDirectory fixtureHome
#import DM_BCAT_RUNTIME_INCLUDE
#undef NSHomeDirectory

int main(int argc,const char **argv){@autoreleasepool{
    if(argc!=4)return 2;fixtureRoot=@(argv[1]);fixtureMode=@(argv[2]);NSString *cache=@(argv[3]);
    Method a=class_getClassMethod(NSURLSessionConfiguration.class,@selector(ephemeralSessionConfiguration));
    Method b=class_getClassMethod(NSURLSessionConfiguration.class,@selector(dm_bcat_fixture_configuration));method_exchangeImplementations(a,b);
    BOOL gate=[fixtureMode hasPrefix:@"gate-"];
    activeCache=cache;activeVersion=gate?nil:@"5.5.2";startTitle=@"0100f8f0000a2000";startOnline=![fixtureMode isEqual:@"offline"];
    NSString *seed=[fixtureRoot stringByAppendingPathComponent:@"empty-seed"];
    uint64_t before=monotonicTime();
    if(gate){
        [NSFileManager.defaultManager createDirectoryAtPath:fixtureHome() withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *profile=[fixtureRoot stringByAppendingPathComponent:[fixtureMode isEqual:@"gate-missing"]?@"missing-profile":[fixtureMode isEqual:@"gate-invalid"]?@"invalid-profile":@"valid-profile"];
        setenv("DM_NEXTENDO_BCAT_DIR",profile.fileSystemRepresentation,1);
        const char *title=[fixtureMode isEqual:@"gate-unrelated"]?"0100152000022000":"0100f8f0000a2000";
        if([fixtureMode isEqual:@"gate-no-provider"]||[fixtureMode isEqual:@"gate-provider-online"])dm_bcat_pre_guest_start(fixtureRoot.fileSystemRepresentation,title);
        else dm_bcat_start(fixtureRoot.fileSystemRepresentation,title,1);
    }else selectAtLaunch(seed);
    double elapsed=(monotonicTime()-before)/(double)NSEC_PER_SEC;
    // This is the first calendar open; it must see the completed launch choice.
    const char *overlay=dm_bcat_overlay();NSString *file=overlay?[@(overlay) stringByAppendingPathComponent:@"DebugUnderPilot/bcat/vsdata/VSSetting_0.byaml"]:nil;
    NSData *first=file?boundedRead(file,1024):nil;unsigned char marker=first.length?((const unsigned char *)first.bytes)[first.length-1]:0;
    unsigned deliveryMarker=0,overlayMarker=0,retainedMarker=0;
    if([fixtureMode isEqual:@"runtime-refresh"]){
        int original=open(file.fileSystemRepresentation,O_RDONLY|O_CLOEXEC);if(original<0)abort();
        fixtureMode=@"v2";lastRefresh=0;if(!dm_bcat_request_sync())abort();
        uint64_t limit=monotonicTime()+5*NSEC_PER_SEC;unsigned result=1;
        while(dm_bcat_sync_status(&result,NULL)!=9&&monotonicTime()<limit)usleep(1000);
        if(dm_bcat_sync_status(&result,NULL)!=9||result)abort();
        char latest[1024];if(!dm_bcat_copy_delivery(latest,sizeof latest))abort();
        NSData *next=boundedRead([@(latest) stringByAppendingPathComponent:@"vsdata/VSSetting_0.byaml"],1024);
        NSData *fixed=boundedRead(file,1024);unsigned char retained=0;
        if(!next.length||!fixed.length||pread(original,&retained,1,(off_t)first.length-1)!=1)abort();close(original);
        deliveryMarker=((const unsigned char*)next.bytes)[next.length-1];overlayMarker=((const unsigned char*)fixed.bytes)[fixed.length-1];retainedMarker=retained;
    }
    NSDictionary *summary=@{@"requests":@(requests),@"catalogRequests":@(catalogRequests),@"archiveRequests":@(archiveRequests),@"conditionalRequests":@(conditionals),@"elapsed":@(elapsed),@"firstCalendarMarker":@(marker),@"hasOverlay":@(overlay!=NULL),@"result":@(activeRefresh?activeRefresh.result:99),@"freshAttempt":@(activeRefresh!=nil),@"syncAccepted":@(dm_bcat_request_sync()),@"runtimeDeliveryMarker":@(deliveryMarker),@"runtimeOverlayMarker":@(overlayMarker),@"retainedFDMarker":@(retainedMarker)};
    fwrite(jsonData(summary).bytes,1,jsonData(summary).length,stdout);fputc('\n',stdout);return 0;
}}
