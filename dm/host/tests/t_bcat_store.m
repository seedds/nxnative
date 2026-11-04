// Original synthetic JSON, keys, BYAML and FRES data. No provider/game assets.
#import "../hle_bcat_apple.m"
#include <stdio.h>
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"BCAT assertion failed at %d: %s\n",__LINE__,#c);return 1;}}while(0)
static NSData *envelope(NSData *payload,EVP_PKEY *key){
    uint8_t signature[64];size_t length=sizeof signature;EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    if(!ctx||EVP_DigestSignInit(ctx,NULL,NULL,NULL,key)!=1||EVP_DigestSign(ctx,signature,&length,payload.bytes,payload.length)!=1)abort();EVP_MD_CTX_free(ctx);
    return jsonData(@{@"key_id":@"catalog-key-1",@"payload":[payload base64EncodedStringWithOptions:0],@"signature":[[NSData dataWithBytes:signature length:length] base64EncodedStringWithOptions:0]});
}
int main(int argc,const char **argv){@autoreleasepool{
    if(argc==3){NSData *zip=boundedRead([NSString stringWithUTF8String:argv[1]],zipLimit+1);BOOL expected=atoi(argv[2])!=0;NSDictionary *files=unzip(zip,NULL);if((files!=nil)!=expected)return 1;return 0;}
    CHECK(dm_bcat_supported("0100F8F0000A2000"));CHECK(!dm_bcat_supported("0100152000022000"));
    CHECK(!dm_bcat_supported("\xff"));
    CHECK(validETag(@"\"fixture\""));CHECK(!validETag(@[]));CHECK(!validETag(NSNull.null));CHECK(!validETag(@"bad\nheader"));CHECK(!validETag(@""));
    CHECK(safePath(@"vsdata/VSSetting_0.byaml"));CHECK(!safePath(@"../data"));CHECK(!safePath(@"/absolute"));CHECK(!safePath(@"a//b"));CHECK(!safePath(@"a/../b"));CHECK(!safePath(@"a\\b"));CHECK(!safePath(@"c:/b"));
    CHECK(!json([@"{\"a\":1,\"a\":2}" dataUsingEncoding:NSUTF8StringEncoding]));
    CHECK(!json([@"{\"a\":1,\"\\u0061\":2}" dataUsingEncoding:NSUTF8StringEncoding]));
    CHECK(!json([@"{\"p\":[{\"x\":1,\"x\":2}]}" dataUsingEncoding:NSUTF8StringEncoding]));
    CHECK(json([@"{\"p\":[{\"x\":1},{\"x\":2}]}" dataUsingEncoding:NSUTF8StringEncoding]));
    CHECK(!json([NSData dataWithBytes:"{\"a\":1}\0" length:8]));
    NSMutableString *nested=[NSMutableString stringWithString:@"{\"a\":"];
    for(int i=0;i<40;i++)[nested appendString:@"["];[nested appendString:@"0"];for(int i=0;i<40;i++)[nested appendString:@"]"];[nested appendString:@"}"];
    CHECK(!json([nested dataUsingEncoding:NSUTF8StringEncoding]));
    uint8_t secret[32]={7},public[32];size_t publicLength=32;EVP_PKEY *key=EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,NULL,secret,32);CHECK(key&&EVP_PKEY_get_raw_public_key(key,public,&publicLength)==1);NSData *pub=[NSData dataWithBytes:public length:publicLength];
    NSISO8601DateFormatter *formatter=[NSISO8601DateFormatter new];NSTimeInterval now=NSDate.date.timeIntervalSince1970;
    NSMutableDictionary *game=[@{@"title_ids":@[@"0100f8f0000a2000"],@"name":@"Original fixture",@"enabled":@YES,@"required_capabilities":@[@"versions.exact.v1"],@"versions":@{@"policy":@"exact",@"accepted":@[@"5.5.2"]}} mutableCopy];
    NSMutableDictionary *document=[@{@"document_type":@"game_catalog",@"schema_version":@1,@"revision":@4,@"issued_at":[formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:now-100]],@"expires_at":[formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:now+100]],@"games":@[game]} mutableCopy];
    NSData *signedData=envelope(jsonData(document),key);
    CHECK(catalogWithKey(signedData,@"0100f8f0000a2000",@"5.5.2",YES,4,pub,NULL));
    CHECK(!catalogWithKey(signedData,@"0100f8f0000a2000",@"5.5.1",YES,4,pub,NULL));
    CHECK(!catalogWithKey(signedData,@"01003bc0000a0000",@"5.5.2",YES,4,pub,NULL));
    CHECK(!catalogWithKey(signedData,@"0100f8f0000a2000",@"5.5.2",YES,5,pub,NULL));
    CHECK(!catalog(signedData,@"0100f8f0000a2000",@"5.5.2",YES,0,NULL)); // original key is not the provider
    NSMutableData *bad=[signedData mutableCopy];((uint8_t *)bad.mutableBytes)[bad.length/2]^=1;CHECK(!catalogWithKey(bad,@"0100f8f0000a2000",@"5.5.2",YES,0,pub,NULL));
    game[@"enabled"]=@NO;CHECK(!catalogWithKey(envelope(jsonData(document),key),@"0100f8f0000a2000",@"5.5.2",YES,0,pub,NULL));game[@"enabled"]=@YES;
    game[@"required_capabilities"]=@[@"future.unknown.v2"];CHECK(!catalogWithKey(envelope(jsonData(document),key),@"0100f8f0000a2000",@"5.5.2",YES,0,pub,NULL));game[@"required_capabilities"]=@[@"versions.exact.v1"];
    document[@"expires_at"]=[formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:now-1]];
    CHECK(!catalogWithKey(envelope(jsonData(document),key),@"0100f8f0000a2000",@"5.5.2",YES,0,pub,NULL));
    CHECK(catalogWithKey(envelope(jsonData(document),key),@"0100f8f0000a2000",@"5.5.2",NO,0,pub,NULL));
    game[@"title_ids"]=@[@"0100f8f0000a2000",@"0100F8F0000A2000"];CHECK(!catalogWithKey(envelope(jsonData(document),key),@"0100f8f0000a2000",@"5.5.2",NO,0,pub,NULL));
    EVP_PKEY_free(key);
    char canonical[4096];CHECK(realpath(NSTemporaryDirectory().fileSystemRepresentation,canonical));
    NSString *dir=[[NSString stringWithUTF8String:canonical] stringByAppendingPathComponent:NSUUID.UUID.UUIDString];CHECK(safeDirectory(dir,YES));
    NSString *file=[dir stringByAppendingPathComponent:@"current.json"];NSData *original=[@"original" dataUsingEncoding:NSUTF8StringEncoding];CHECK(atomicWrite(original,file));
    CHECK([boundedRead(file,8) isEqual:original]);CHECK(!boundedRead(file,3));
    NSString *link=[dir stringByAppendingPathComponent:@"link"];CHECK(!symlink(dir.fileSystemRepresentation,link.fileSystemRepresentation));CHECK(!safeDirectory([link stringByAppendingPathComponent:@"nested"],YES));
    CHECK(!snapshot(dir,@"0100f8f0000a2000",@"5.5.2"));CHECK([boundedRead(file,8) isEqual:original]);
    CHECK(atomicWrite(jsonData(@{@"formatVersion":@1,@"titleID":@"0100f8f0000a2000",@"version":@"5.5.2"}),[dir stringByAppendingPathComponent:@"setup.json"]));
    // A descriptor for another region must not select even a valid local cache.
    startModules=dir;startTitle=@"01003bc0000a0000";startOnline=NO;setenv("DM_NEXTENDO_BCAT_DIR",dir.UTF8String,1);start();unsetenv("DM_NEXTENDO_BCAT_DIR");
    CHECK(!activeVersion&&!activeRefresh&&!refreshRunning);
    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    // Retained observers belong to one job even after a later refresh starts.
    startOnline=YES;activeVersion=@"5.5.2";lastRefresh=NSDate.date.timeIntervalSince1970;
    DmBcatRefresh *first=[DmBcatRefresh new];first.status=2;first.result=1;activeRefresh=first;
    void *observer=dm_bcat_sync_observe();CHECK(observer);unsigned outcome=123;uint64_t downloaded=123;
    CHECK(dm_bcat_sync_observer_status(observer,&outcome,&downloaded)==2);
    activeRefresh=[DmBcatRefresh new];activeRefresh.status=9;activeRefresh.result=0;activeRefresh.bytes=99;
    CHECK(dm_bcat_sync_observer_status(observer,&outcome,&downloaded)==2&&downloaded==0);
    first.status=9;first.result=1;
    CHECK(dm_bcat_sync_observer_status(observer,&outcome,&downloaded)==9&&outcome==1);
    dm_bcat_sync_release(observer);activeVersion=nil;activeRefresh=nil;startOnline=NO;
    setenv("DM_OODLE_SERVE","1",1);dm_bcat_start("/absent", "0100f8f0000a2000", 1);CHECK(!startupAccepted&&!activeRefresh);unsetenv("DM_OODLE_SERVE");
    setenv("DM_OODLE_JOB","fixture-job",1);dm_bcat_start("/absent", "0100f8f0000a2000", 1);CHECK(!startupAccepted&&!activeRefresh);unsetenv("DM_OODLE_JOB");
    dm_bcat_start("/absent", "0100f8f0000a2000", 0);CHECK(!dm_bcat_overlay());CHECK(!dm_bcat_delivery());CHECK(!dm_bcat_request_sync());
    dm_bcat_start("/different", "01003bc0000a0000", 1);CHECK([startTitle isEqual:@"0100f8f0000a2000"]&&!startOnline);
    printf("BCAT signed policy/path/atomic store: %u checks passed\n",checks);return 0;
}}
