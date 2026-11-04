// Original Nextendo calendar importer. Provider files are data in private caches,
// never source assets. HTTPS authenticates downloads; the signed game catalog
// authenticates eligibility, NOT the (currently unsigned) calendar ZIP itself.
#import <Foundation/Foundation.h>
#include "nextendo_bcat.h"
#include <CommonCrypto/CommonDigest.h>
#ifdef DM_OPENSSL_X509
#include <openssl/evp.h>
#endif
#include <zlib.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <time.h>

// Only the pre-guest launch check shares this total network budget across its
// catalog, archive and optional static-resource requests. Tool/runtime refreshes
// retain their existing per-request limits. No wall-clock or cache-age shortcut.
static _Thread_local uint64_t launchDeadline;
static uint64_t monotonicTime(void){return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);}

static NSString *const provider = @"https://nextendo.network";
static NSString *const catalogURL = @"https://nextendo.network/service-catalog/v1/game-catalog";
static NSString *const recipeCommit = @"482c182917228d69ebbbabfea44fffa4a44051be";
static const NSUInteger zipLimit = 8 * 1024 * 1024, expandedLimit = 32 * 1024 * 1024;
static NSString *const xmlHash = @"8d8d21a93dce2f8eab1659a3bf5cdca10f61df83d50bc0a34784b1629dd215fa";
static NSString *const dummyHash = @"5feceb66ffc86f38d952786c6d696c79c2dbc239dd4e91b46729d73a27fb57e9";
static NSArray<NSString *> *titles(void) { return @[@"0100f8f0000a2000", @"01003bc0000a0000", @"01003c700009c800"]; }
int dm_bcat_supported(const char *title) { if(!title)return 0;NSString *value=[NSString stringWithUTF8String:title];return value&&[titles() containsObject:value.lowercaseString]; }
static NSString *digest(NSData *data) { uint8_t h[32]; CC_SHA256(data.bytes, (CC_LONG)data.length, h); NSMutableString *s = [NSMutableString new]; for (int i=0;i<32;i++) [s appendFormat:@"%02x",h[i]]; return s; }
static BOOL fail(NSString **error, NSString *message) { if (error) *error = message; return NO; }
static BOOL integer(id n) { if(![n isKindOfClass:NSNumber.class]||CFGetTypeID((__bridge CFTypeRef)n)==CFBooleanGetTypeID())return NO;int64_t value;return CFNumberGetValue((__bridge CFNumberRef)n,kCFNumberSInt64Type,&value)&&[n compare:@(value)]==NSOrderedSame; }
static BOOL validETag(id tag){return [tag isKindOfClass:NSString.class]&&[tag length]&&[tag length]<=256&&[tag rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location==NSNotFound;}
static BOOL safePath(NSString *path) {
    if (![path isKindOfClass:NSString.class] || !path.length || path.length > 240 || [path hasPrefix:@"/"] || [path containsString:@"\\"] || [path containsString:@":"]) return NO;
    for (NSString *p in [path componentsSeparatedByString:@"/"]) {
        if (!p.length || [p isEqual:@"."] || [p isEqual:@".."] || p.length > 128) return NO;
        for (NSUInteger i=0;i<p.length;i++) { unichar c=[p characterAtIndex:i]; if (c<32 || c>126) return NO; }
    }
    return YES;
}
static BOOL safeDirectory(NSString *path, BOOL create) {
    if (![path hasPrefix:@"/"] || path.length > 800) return NO;
    NSString *walk = @"/";
    for (NSString *part in path.pathComponents) {
        if ([part isEqual:@"/"]) continue;
        if ([part isEqual:@"."] || [part isEqual:@".."] || !part.length) return NO;
        walk = [walk stringByAppendingPathComponent:part]; struct stat st;
        if (lstat(walk.fileSystemRepresentation,&st)) {
            if (!create || errno != ENOENT || mkdir(walk.fileSystemRepresentation,0700)) return NO;
        } else if (!S_ISDIR(st.st_mode)) return NO;
    }
    return YES;
}
static NSData *boundedRead(NSString *path, NSUInteger max) {
    int fd=open(path.fileSystemRepresentation,O_RDONLY|O_CLOEXEC|O_NOFOLLOW); if(fd<0)return nil;
    struct stat s; if(fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_size<0||(uint64_t)s.st_size>max){close(fd);return nil;}
    NSMutableData *d=[NSMutableData dataWithLength:(NSUInteger)s.st_size]; size_t done=0;
    while(done<d.length){ssize_t n=read(fd,(char *)d.mutableBytes+done,d.length-done);if(n<=0){close(fd);return nil;}done+=(size_t)n;}close(fd);return d;
}
static BOOL atomicWrite(NSData *data, NSString *path) {
    if (!data || !safeDirectory(path.stringByDeletingLastPathComponent,YES)) return NO;
    NSString *tmp=[path.stringByDeletingLastPathComponent stringByAppendingPathComponent:[@".incoming-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    int fd=open(tmp.fileSystemRepresentation,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600); if(fd<0)return NO;
    size_t done=0; BOOL ok=YES; while(done<data.length){ssize_t n=write(fd,(const char *)data.bytes+done,data.length-done);if(n<=0){ok=NO;break;}done+=(size_t)n;}
    if(fsync(fd))ok=NO; if(close(fd))ok=NO;
    if(ok && rename(tmp.fileSystemRepresentation,path.fileSystemRepresentation))ok=NO;
    if(!ok)unlink(tmp.fileSystemRepresentation);return ok;
}
static NSData *jsonData(id object) { return [NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingSortedKeys error:nil]; }

// Foundation checks JSON syntax. This separate bounded pass rejects duplicate
// fields, including escaped spellings, before interpreting any signed payload.
typedef struct { const uint8_t *b; NSUInteger n,p; } JSONScan;
static int whitespace(uint8_t c){return c==9||c==10||c==13||c==32;}
static void ws(JSONScan *s){while(s->p<s->n&&whitespace(s->b[s->p]))s->p++;}
static NSString *scanString(JSONScan *s){NSUInteger a=s->p;if(s->p>=s->n||s->b[s->p++]!='"')return nil;while(s->p<s->n){uint8_t c=s->b[s->p++];if(c=='"'){NSData *d=[NSData dataWithBytes:s->b+a length:s->p-a];return [NSJSONSerialization JSONObjectWithData:d options:NSJSONReadingFragmentsAllowed error:nil];}if(c=='\\'&&s->p<s->n)s->p++;}return nil;}
static BOOL scanValue(JSONScan *s,unsigned depth){
    if(depth>32)return NO;ws(s);if(s->p>=s->n)return NO;uint8_t c=s->b[s->p];
    if(c=='"')return scanString(s)!=nil;
    if(c=='{'||c=='['){s->p++;ws(s);uint8_t end=c=='{'?'}':']';NSMutableSet *keys=[NSMutableSet new];if(s->p<s->n&&s->b[s->p]==end){s->p++;return YES;}
        for(;;){if(c=='{'){ws(s);NSString *k=scanString(s);if(!k||[keys containsObject:k])return NO;[keys addObject:k];ws(s);if(s->p>=s->n||s->b[s->p++]!=':')return NO;}
            if(!scanValue(s,depth+1))return NO;ws(s);if(s->p>=s->n)return NO;uint8_t next=s->b[s->p++];if(next==end)return YES;if(next!=',')return NO;}}
    NSUInteger a=s->p;while(s->p<s->n&&s->b[s->p]!=','&&s->b[s->p]!=']'&&s->b[s->p]!='}'&&!whitespace(s->b[s->p]))s->p++;return s->p>a;
}
static NSDictionary *json(NSData *data){if(!data.length||data.length>1048576)return nil;JSONScan s={data.bytes,data.length,0};if(!scanValue(&s,0))return nil;ws(&s);if(s.p!=s.n)return nil;id v=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];return [v isKindOfClass:NSDictionary.class]?v:nil;}
static NSDate *date(id value){if(![value isKindOfClass:NSString.class]||![value hasSuffix:@"Z"]||[value length]>40)return nil;NSISO8601DateFormatter *f=[NSISO8601DateFormatter new];f.formatOptions=NSISO8601DateFormatWithInternetDateTime|NSISO8601DateFormatWithFractionalSeconds;NSDate *d=[f dateFromString:value];if(d)return d;f.formatOptions=NSISO8601DateFormatWithInternetDateTime;return [f dateFromString:value];}
static BOOL verifyCatalogSignature(NSData *key,NSData *sig,NSData *payload){
#ifdef DM_OPENSSL_X509
    EVP_PKEY *pk=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,NULL,key.bytes,key.length);EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    BOOL verified=pk&&ctx&&EVP_DigestVerifyInit(ctx,NULL,NULL,NULL,pk)==1&&EVP_DigestVerify(ctx,sig.bytes,sig.length,payload.bytes,payload.length)==1;EVP_MD_CTX_free(ctx);EVP_PKEY_free(pk);
    return verified;
#else
    // No optional crypto provider: BCAT remains unavailable. Never treat an
    // unchecked local manifest or HTTPS response as verified calendar data.
    (void)key;(void)sig;(void)payload;return NO;
#endif
}
static NSDictionary *catalogWithKey(NSData *envelope, NSString *title, NSString *version, BOOL fresh, NSInteger minimum, NSData *key, NSString **error){
    NSDictionary *outer=json(envelope);NSString *encoded=outer[@"payload"],*signature=outer[@"signature"];
    if(![outer[@"key_id"] isEqual:@"catalog-key-1"]||![encoded isKindOfClass:NSString.class]||![signature isKindOfClass:NSString.class]){fail(error,@"Invalid signed calendar eligibility document");return nil;}
    NSData *payload=[[NSData alloc]initWithBase64EncodedString:encoded options:0],*sig=[[NSData alloc]initWithBase64EncodedString:signature options:0];
    if(!payload||sig.length!=64||![[payload base64EncodedStringWithOptions:0] isEqual:encoded]||![[sig base64EncodedStringWithOptions:0] isEqual:signature]){fail(error,@"Invalid catalog encoding");return nil;}
    if(!verifyCatalogSignature(key,sig,payload)){fail(error,@"Calendar eligibility signature failed or verifier unavailable");return nil;}
    NSDictionary *p=json(payload);NSDate *issued=date(p[@"issued_at"]),*expires=date(p[@"expires_at"]);NSTimeInterval now=NSDate.date.timeIntervalSince1970;
    if(![p[@"document_type"] isEqual:@"game_catalog"]||!integer(p[@"schema_version"])||[p[@"schema_version"] integerValue]!=1||!integer(p[@"revision"])||[p[@"revision"] integerValue]<MAX(1,minimum)||!issued||!expires||[expires timeIntervalSinceDate:issued]<=0||[expires timeIntervalSinceDate:issued]>86400||issued.timeIntervalSince1970>now+300||(fresh&&expires.timeIntervalSince1970<=now)) {fail(error,@"Calendar eligibility expired, rolled back or malformed");return nil;}
    NSArray *games=p[@"games"];if(![games isKindOfClass:NSArray.class]||games.count>10000){fail(error,@"Invalid catalog games");return nil;}
    NSMutableSet *seen=[NSMutableSet new];NSDictionary *match=nil;
    for(id game in games){if(![game isKindOfClass:NSDictionary.class])return nil;NSArray *ids=game[@"title_ids"];if(![ids isKindOfClass:NSArray.class]||!ids.count||ids.count>32)return nil;
        for(id idValue in ids){if(![idValue isKindOfClass:NSString.class]||[idValue length]!=16||[idValue rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"] invertedSet]].location!=NSNotFound)return nil;NSString *tid=[idValue lowercaseString];if(!strtoull(tid.UTF8String,NULL,16)||[seen containsObject:tid])return nil;[seen addObject:tid];if([tid isEqual:title])match=game;}}
    if(!match||![match[@"enabled"] isKindOfClass:NSNumber.class]||CFGetTypeID((__bridge CFTypeRef)match[@"enabled"])!=CFBooleanGetTypeID()||![match[@"enabled"] boolValue]){fail(error,@"No enabled BCAT game in the verified catalog");return nil;}
    NSArray *caps=match[@"required_capabilities"];if(![caps isKindOfClass:NSArray.class]||caps.count>64||[NSSet setWithArray:caps].count!=caps.count)return nil;
    for(id cap in caps)if(![@[@"versions.exact.v1",@"versions.any.v1"] containsObject:cap]){fail(error,@"Unsupported calendar eligibility capability");return nil;}
    NSDictionary *versions=match[@"versions"];BOOL accepted=NO;
    if([versions isKindOfClass:NSDictionary.class]){
        if([versions[@"policy"] isEqual:@"any"]&&[caps containsObject:@"versions.any.v1"]&&!versions[@"accepted"]&&!versions[@"recommended"])accepted=YES;
        NSArray *values=versions[@"accepted"];
        if([versions[@"policy"] isEqual:@"exact"]&&[caps containsObject:@"versions.exact.v1"]&&[values isKindOfClass:NSArray.class]&&values.count&&values.count<=64&&[NSSet setWithArray:values].count==values.count){
            BOOL valid=YES;for(id v in values)if(![v isKindOfClass:NSString.class]||![v length]||[v length]>32||![[v stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] isEqual:v]||[v rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location!=NSNotFound){valid=NO;break;}
            id preferred=versions[@"recommended"]?:values[0];accepted=valid&&[values containsObject:preferred]&&[values containsObject:version];
        }
    }
    if(!accepted){fail(error,@"The game version is not accepted for calendar preparation");return nil;}return p;
}

static NSDictionary *catalog(NSData *data, NSString *title, NSString *version, BOOL fresh, NSInteger minimum, NSString **error) {
    NSData *key=[[NSData alloc]initWithBase64EncodedString:@"xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4=" options:0];
    return catalogWithKey(data,title,version,fresh,minimum,key,error);
}

@interface DmBcatDownload : NSObject<NSURLSessionDataDelegate>
@property(nonatomic,strong) NSMutableData *bytes;
@property(nonatomic) NSUInteger limit;
@property(nonatomic) NSInteger status;
@property(nonatomic,strong) NSString *etag;
@property(nonatomic) BOOL failed;
@property(nonatomic,strong) dispatch_semaphore_t done;
@end
@implementation DmBcatDownload
-(void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t willPerformHTTPRedirection:(NSHTTPURLResponse *)r newRequest:(NSURLRequest *)q completionHandler:(void (^)(NSURLRequest *))completion {self.failed=YES;completion(nil);}
-(void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveResponse:(NSURLResponse *)r completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    if(![r isKindOfClass:NSHTTPURLResponse.class]){self.failed=YES;completion(NSURLSessionResponseCancel);return;}
    NSHTTPURLResponse *h=(id)r;self.status=h.statusCode;
    if(r.expectedContentLength>(int64_t)self.limit||![r.URL isEqual:t.originalRequest.URL]){self.failed=YES;completion(NSURLSessionResponseCancel);return;}
    id tag=h.allHeaderFields[@"Etag"]?:h.allHeaderFields[@"ETag"];if(validETag(tag))self.etag=tag;
    completion(NSURLSessionResponseAllow);
}
-(void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveData:(NSData *)d {if(d.length>self.limit-self.bytes.length){self.failed=YES;[t cancel];}else[self.bytes appendData:d];}
-(void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t didCompleteWithError:(NSError *)e {if(e)self.failed=YES;dispatch_semaphore_signal(self.done);}
@end
static NSData *download(NSString *url, NSUInteger limit, NSString *etag, NSInteger *status, NSString **newTag){
    if(status)*status=0;if(newTag)*newTag=nil;
    uint64_t now=monotonicTime();if(launchDeadline&&now>=launchDeadline)return nil;
    NSTimeInterval remaining=launchDeadline?(launchDeadline-now)/(double)NSEC_PER_SEC:30;
    NSURL *u=[NSURL URLWithString:url];if(![u.scheme isEqual:@"https"]||u.user||u.password||u.port||u.fragment)return nil;
    if(!([u.host isEqual:@"nextendo.network"]||([u.host isEqual:@"raw.githubusercontent.com"]&&[u.path hasPrefix:[@"/NextendoNetwork/Prelude-Nextendo/" stringByAppendingString:recipeCommit]])))return nil;
    DmBcatDownload *d=[DmBcatDownload new];d.bytes=[NSMutableData new];d.limit=limit;d.done=dispatch_semaphore_create(0);
    NSURLSessionConfiguration *c=NSURLSessionConfiguration.ephemeralSessionConfiguration;c.HTTPCookieStorage=nil;c.HTTPShouldSetCookies=NO;c.URLCredentialStorage=nil;c.URLCache=nil;c.requestCachePolicy=NSURLRequestReloadIgnoringLocalCacheData;c.timeoutIntervalForRequest=MIN(15,remaining);c.timeoutIntervalForResource=MIN(25,remaining);
    NSOperationQueue *queue=[NSOperationQueue new];queue.maxConcurrentOperationCount=1;NSURLSession *s=[NSURLSession sessionWithConfiguration:c delegate:d delegateQueue:queue];
    NSMutableURLRequest *r=[NSMutableURLRequest requestWithURL:u];[r setValue:@"nxnative/BCAT" forHTTPHeaderField:@"User-Agent"];
    if(validETag(etag))[r setValue:etag forHTTPHeaderField:@"If-None-Match"];
    NSURLSessionDataTask *task=[s dataTaskWithRequest:r];[task resume];
    uint64_t deadline=launchDeadline?:now+30*NSEC_PER_SEC;BOOL finished=NO;
    while((now=monotonicTime())<deadline){
        uint64_t wait=MIN(deadline-now,NSEC_PER_SEC/10);
        if(!dispatch_semaphore_wait(d.done,dispatch_time(DISPATCH_TIME_NOW,(int64_t)wait))){finished=YES;break;}
    }
    if(!finished){[task cancel];[s invalidateAndCancel];return nil;}
    [s invalidateAndCancel];if(status)*status=d.status;if(newTag)*newTag=d.etag;return d.failed?nil:d.bytes;
}

static uint16_t u16(const uint8_t *p){return p[0]|p[1]<<8;}static uint32_t u32(const uint8_t *p){return (uint32_t)u16(p)|(uint32_t)u16(p+2)<<16;}
static BOOL fileFormat(NSString *path, NSData *data){
    if([path.pathExtension isEqual:@"byaml"]){if(data.length<16)return NO;const uint8_t *b=data.bytes;BOOL le=b[0]=='Y'&&b[1]=='B',be=b[0]=='B'&&b[1]=='Y';if(!le&&!be)return NO;unsigned version=le?u16(b+2):b[2]*256+b[3];if(version<1||version>7)return NO;for(unsigned i=4;i<16;i+=4){uint32_t offset=le?u32(b+i):((uint32_t)b[i]<<24|(uint32_t)b[i+1]<<16|b[i+2]<<8|b[i+3]);if(offset&&((offset&3)||offset>=data.length))return NO;}return YES;}
    if([path.pathExtension isEqual:@"bfres"])return data.length>=32&&!memcmp(data.bytes,"FRES",4);
    if([path.pathExtension isEqual:@"szs"]){
        if(data.length>=16&&(!memcmp(data.bytes,"Yaz0",4)||!memcmp(data.bytes,"SARC",4)))return YES;
        // The pinned provider revision also publishes two exact short marker
        // resources under .szs names. They are not compressed archives. Retain
        // only those authenticated bytes; never accept arbitrary text as SZS.
        NSDictionary *markers=@{@"fesdata/Fld_Deli_Octa24.Nin_NX_NVN.szs":@"4be0e8f7f609db1fb9e96f58299d7bca6848208f055e7af394a0a9c65e1fe2ed",@"fesdata/Fld_Deli_Octa24_Vss.szs":@"431b9113111518d3b389d97f89eee213150959f2453f6a4edec61690ef8ae16a"};
        return markers[path]&&[digest(data) isEqual:markers[path]];
    }
    return NO;
}
static BOOL scheduleName(NSString *path){return safePath(path)&&[path componentsSeparatedByString:@"/"].count==2&&[@[@"coopdata",@"vsdata",@"fesdata"] containsObject:[path componentsSeparatedByString:@"/"][0]]&&[@[@"byaml",@"bfres",@"szs"] containsObject:path.pathExtension];}
/* ZIP central directory is authoritative only after matching every local header.
 * No symlinks, extras, duplicate/case-alias paths, ZIP64, encryption or overlap.
 * Deflate output and CRC are checked before any file becomes visible. */
static NSDictionary<NSString *,NSData *> *unzip(NSData *zip, NSString **error){
    const uint8_t *b=zip.bytes;NSUInteger n=zip.length;if(n<22||n>zipLimit){fail(error,@"Calendar archive exceeds its bounds");return nil;}
    NSUInteger end=NSNotFound;for(NSUInteger p=n-22;;p--){if(u32(b+p)==0x06054b50&&p+22+u16(b+p+20)==n){end=p;break;}if(!p||n-p>65557)break;}
    if(end==NSNotFound||u16(b+end+4)||u16(b+end+6)||u16(b+end+8)!=u16(b+end+10)){fail(error,@"Invalid calendar ZIP directory");return nil;}
    unsigned count=u16(b+end+10);NSUInteger central=u32(b+end+16),centralSize=u32(b+end+12);if(!count||count>64||central>end||centralSize!=end-central){fail(error,@"Invalid calendar ZIP bounds");return nil;}
    NSMutableDictionary *files=[NSMutableDictionary new];NSMutableSet *names=[NSMutableSet new];NSMutableArray *ranges=[NSMutableArray new];NSUInteger at=central,total=0;
    for(unsigned i=0;i<count;i++){
        if(at+46>end||u32(b+at)!=0x02014b50)goto invalid;unsigned flags=u16(b+at+8),method=u16(b+at+10),nameLen=u16(b+at+28),extra=u16(b+at+30),comment=u16(b+at+32);uint32_t packed=u32(b+at+20),size=u32(b+at+24),crc=u32(b+at+16);NSUInteger local=u32(b+at+42);
        if(flags&~(8|0x800)||(method!=0&&method!=8)||u16(b+at+34)||!nameLen||nameLen>240||at+46+nameLen+extra+comment>end||size>8*1024*1024||size>expandedLimit-total||!size||(((u32(b+at+38)>>16)&0170000)&&((u32(b+at+38)>>16)&0170000)!=0100000))goto invalid;
        NSString *name=[[NSString alloc]initWithBytes:b+at+46 length:nameLen encoding:NSUTF8StringEncoding];if(!scheduleName(name)||[names containsObject:name.lowercaseString])goto invalid;[names addObject:name.lowercaseString];
        if(local+30>central||u32(b+local)!=0x04034b50||u16(b+local+6)!=flags||u16(b+local+8)!=method||u16(b+local+26)!=nameLen)goto invalid;
        NSUInteger start=local+30+nameLen+u16(b+local+28);if(start>central||packed>central-start||memcmp(b+local+30,b+at+46,nameLen))goto invalid;
        if(!(flags&8)&&(u32(b+local+14)!=crc||u32(b+local+18)!=packed||u32(b+local+22)!=size))goto invalid;
        NSUInteger stop=start+packed;
        if(flags&8){BOOL signedDD=stop+4<=central&&u32(b+stop)==0x08074b50;NSUInteger dd=stop+(signedDD?4:0);if(dd+12>central||u32(b+dd)!=crc||u32(b+dd+4)!=packed||u32(b+dd+8)!=size)goto invalid;stop=dd+12;}
        for(NSValue *v in ranges){NSRange r=v.rangeValue;if(local<NSMaxRange(r)&&r.location<stop)goto invalid;}[ranges addObject:[NSValue valueWithRange:NSMakeRange(local,stop-local)]];
        NSMutableData *data=[NSMutableData dataWithLength:size];
        if(method==0){if(packed!=size)goto invalid;memcpy(data.mutableBytes,b+start,size);}else{z_stream z={0};z.next_in=(Bytef *)b+start;z.avail_in=packed;z.next_out=data.mutableBytes;z.avail_out=size;if(inflateInit2(&z,-MAX_WBITS)!=Z_OK)goto invalid;int result=inflate(&z,Z_FINISH);BOOL ok=result==Z_STREAM_END&&z.total_out==size&&z.total_in==packed;inflateEnd(&z);if(!ok)goto invalid;}
        if(crc32(0,data.bytes,(uInt)data.length)!=crc||!fileFormat(name,data))goto invalid;files[name]=data;total+=size;at+=46+nameLen+extra+comment;
    }
    if(at!=end)goto invalid;
    for(NSString *required in @[@"coopdata/CoopSetting.byaml",@"vsdata/VSSetting_0.byaml",@"vsdata/VSSetting_1.byaml",@"fesdata/Festival.byaml",@"fesdata/HapTexture.bfres",@"fesdata/IconTexture.bfres",@"fesdata/PanelTexture.bfres"])if(!files[required])goto invalid;
    return files;
invalid:fail(error,@"Calendar ZIP failed path, bounds, format or integrity validation");return nil;
}

static NSDictionary *snapshot(NSString *root,NSString *title,NSString *version){
    if(!safeDirectory(root,NO))return nil;NSDictionary *m=json(boundedRead([root stringByAppendingPathComponent:@"manifest.json"],1048576));
    if(m[@"etag"]&&!validETag(m[@"etag"]))return nil;
    if(!integer(m[@"formatVersion"])||[m[@"formatVersion"] integerValue]!=1||![m[@"catalogSHA256"] isKindOfClass:NSString.class]||[m[@"catalogSHA256"] length]!=64||![m[@"source"] isEqual:[provider stringByAppendingFormat:@"/api/bcat/%@",title]]||![m[@"titleID"] isEqual:title]||![m[@"version"] isEqual:version]||![m[@"recipe"] isEqual:recipeCommit]||![m[@"files"] isKindOfClass:NSDictionary.class]||![m[@"files"] count]||[m[@"files"] count]>68)return nil;
    NSData *signedData=boundedRead([root stringByAppendingPathComponent:@"catalog.json"],1048576);
    NSDictionary *verified=catalog(signedData,title,version,NO,0,NULL);
    if(!verified||![digest(signedData) isEqual:m[@"catalogSHA256"]]||![verified[@"revision"] isEqual:m[@"catalogRevision"]])return nil;
    NSDictionary *files=m[@"files"];NSMutableSet *found=[NSMutableSet new];NSString *base=[root stringByAppendingPathComponent:@"files"];
    NSDirectoryEnumerator *it=[NSFileManager.defaultManager enumeratorAtPath:base];for(NSString *rel in it){NSString *path=[base stringByAppendingPathComponent:rel];struct stat st;if(!safePath(rel)||lstat(path.fileSystemRepresentation,&st)||S_ISLNK(st.st_mode))return nil;if(S_ISDIR(st.st_mode))continue;if(!S_ISREG(st.st_mode)||!files[rel])return nil;NSData *data=boundedRead(path,8*1024*1024);if(!data||![digest(data) isEqual:files[rel]])return nil;[found addObject:rel];}
    if(found.count!=files.count)return nil;
    for(NSString *name in @[@"coopdata/CoopSetting.byaml",@"vsdata/VSSetting_0.byaml",@"vsdata/VSSetting_1.byaml",@"fesdata/Festival.byaml",@"fesdata/HapTexture.bfres",@"fesdata/IconTexture.bfres",@"fesdata/PanelTexture.bfres"]){NSString *rel=[@"DebugUnderPilot/bcat" stringByAppendingPathComponent:name];NSData *data=boundedRead([base stringByAppendingPathComponent:rel],8*1024*1024);if(!fileFormat(name,data))return nil;}
    if(![title isEqual:@"01003c700009c800"]){if(![files[@"System/GameConfigSetting.xml"] isEqual:xmlHash]||![files[@"DebugUnderPilot/bcat/dummy/dummy.txt"] isEqual:dummyHash])return nil;}
    return m;
}
static NSString *snapshotIdentity(NSDictionary *m){return digest(jsonData(@{@"files":m[@"files"],@"title":m[@"titleID"],@"version":m[@"version"],@"recipe":m[@"recipe"],@"revision":m[@"catalogRevision"],@"catalog":m[@"catalogSHA256"]}));}
static NSString *current(NSString *cache,NSString *title,NSString *version){NSDictionary *p=json(boundedRead([cache stringByAppendingPathComponent:@"current.json"],4096));NSString *name=p[@"snapshot"];if(![name isKindOfClass:NSString.class]||name.length!=64||[name rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location!=NSNotFound)return nil;NSString *root=[[cache stringByAppendingPathComponent:@"snapshots"] stringByAppendingPathComponent:name];NSDictionary *m=snapshot(root,title,version);return m&&[snapshotIdentity(m) isEqual:name]?root:nil;}
int dm_bcat_validate(const char *title,const char *version,const char *directory){@autoreleasepool{
    if(!dm_bcat_supported(title)||!version||strcmp(version,"5.5.2")||!directory)return 0;
    return current([NSString stringWithUTF8String:directory],[[NSString stringWithUTF8String:title] lowercaseString],[NSString stringWithUTF8String:version])!=nil;
}}
static BOOL publishLocked(NSString *cache,NSString *title,NSString *version,NSData *envelope,NSString *old,NSString **error){
    NSDictionary *oldManifest=old?snapshot(old,title,version):nil;NSInteger minimum=[oldManifest[@"catalogRevision"] integerValue];NSDictionary *policy=catalog(envelope,title,version,YES,minimum,error);if(!policy)return NO;
    if(oldManifest){NSDictionary *previous=catalog(boundedRead([old stringByAppendingPathComponent:@"catalog.json"],1048576),title,version,NO,0,NULL);
        NSDate *previousIssued=date(previous[@"issued_at"]),*issued=date(policy[@"issued_at"]);
        if(!previous||!previousIssued||!issued)return fail(error,@"Calendar eligibility changed during validation; previous calendar retained");
        if([previous[@"revision"] isEqual:policy[@"revision"]]&&[issued compare:previousIssued]==NSOrderedAscending)return fail(error,@"Catalog issue time rolled back; previous calendar retained");}
    NSInteger status=0;NSString *etag=nil;NSData *zip=download([provider stringByAppendingFormat:@"/api/bcat/%@",title],zipLimit,oldManifest[@"etag"],&status,&etag);
    BOOL unchanged=zip&&status==304&&oldManifest&&[oldManifest[@"etag"] isKindOfClass:NSString.class]&&[oldManifest[@"etag"] length]&&(!etag||[etag isEqual:oldManifest[@"etag"]]);NSMutableDictionary<NSString *,NSData *> *contents=[NSMutableDictionary new];
    if(unchanged){
        for(NSString *path in oldManifest[@"files"]){NSData *data=boundedRead([[old stringByAppendingPathComponent:@"files"] stringByAppendingPathComponent:path],8*1024*1024);if(!data)return fail(error,@"Calendar cache changed during validation");contents[path]=data;}
        etag=etag?:oldManifest[@"etag"];
    }else{
        if(!zip||status!=200)return fail(error,status==204?@"No calendar has been published for this title":@"Calendar download unavailable; previous validated snapshot retained");
        NSDictionary *entries=unzip(zip,error);if(!entries)return NO;
        for(NSString *name in entries)contents[[@"DebugUnderPilot/bcat" stringByAppendingPathComponent:name]]=entries[name];
        if(![title isEqual:@"01003c700009c800"]){
            NSDictionary *staticFiles=@{@"System/GameConfigSetting.xml":xmlHash,@"DebugUnderPilot/bcat/dummy/dummy.txt":dummyHash};
            for(NSString *path in staticFiles){NSData *data=nil;NSString *oldPath=[[old stringByAppendingPathComponent:@"files"] stringByAppendingPathComponent:path];if(old)data=boundedRead(oldPath,131072);
                if(!data||![digest(data) isEqual:staticFiles[path]]){NSString *u=[NSString stringWithFormat:@"https://raw.githubusercontent.com/NextendoNetwork/Prelude-Nextendo/%@/romfs/bcatdata/%@/romfs/%@",recipeCommit,title.uppercaseString,path];data=download(u,131072,nil,&status,NULL);if(status!=200)data=nil;}
                if(!data||![digest(data) isEqual:staticFiles[path]])return fail(error,@"Static calendar resource integrity check failed");contents[path]=data;}
        }
    }
    NSMutableDictionary *hashes=[NSMutableDictionary new];for(NSString *path in contents)hashes[path]=digest(contents[path]);
    NSMutableDictionary *manifest=[@{@"formatVersion":@1,@"titleID":title,@"version":version,@"recipe":recipeCommit,@"source":[provider stringByAppendingFormat:@"/api/bcat/%@",title],@"archiveSHA256":unchanged?oldManifest[@"archiveSHA256"]:digest(zip),@"catalogRevision":policy[@"revision"],@"catalogSHA256":digest(envelope),@"fetchedAt":@(NSDate.date.timeIntervalSince1970),@"files":hashes} mutableCopy];if(etag)manifest[@"etag"]=etag;
    NSData *m=jsonData(manifest);NSString *name=snapshotIdentity(manifest),*snapshots=[cache stringByAppendingPathComponent:@"snapshots"],*destination=[snapshots stringByAppendingPathComponent:name];
    if(!safeDirectory(snapshots,YES))return fail(error,@"Unsafe calendar cache directory");
    if(snapshot(destination,title,version))return atomicWrite(jsonData(@{@"snapshot":name}),[cache stringByAppendingPathComponent:@"current.json"]);
    NSString *temporary=[snapshots stringByAppendingPathComponent:[@".stage-" stringByAppendingString:NSUUID.UUID.UUIDString]];if(!safeDirectory(temporary,YES))return fail(error,@"Cannot stage calendar snapshot");
    BOOL ok=YES;for(NSString *path in contents)if(!atomicWrite(contents[path],[[temporary stringByAppendingPathComponent:@"files"] stringByAppendingPathComponent:path])){ok=NO;break;}
    if(ok)ok=atomicWrite(envelope,[temporary stringByAppendingPathComponent:@"catalog.json"])&&atomicWrite(m,[temporary stringByAppendingPathComponent:@"manifest.json"])&&snapshot(temporary,title,version)!=nil;
    if(ok&&rename(temporary.fileSystemRepresentation,destination.fileSystemRepresentation))ok=NO;
    if(!ok){[NSFileManager.defaultManager removeItemAtPath:temporary error:nil];return fail(error,@"Calendar snapshot publication failed; previous snapshot retained");}
    if(!atomicWrite(jsonData(@{@"snapshot":name}),[cache stringByAppendingPathComponent:@"current.json"]))return fail(error,@"Cannot publish calendar snapshot pointer");return YES;
}
static BOOL publish(NSString *cache,NSString *title,NSString *version,NSData *envelope,NSString *old,NSString **error){
    if(!safeDirectory(cache,YES))return fail(error,@"Unsafe calendar cache directory");
    NSString *lockPath=[cache stringByAppendingPathComponent:@".update.lock"];
    int fd=open(lockPath.fileSystemRepresentation,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return fail(error,@"Calendar cache lock unavailable");
    struct stat st;BOOL acquired=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!flock(fd,LOCK_EX|LOCK_NB);
    if(!acquired){close(fd);return fail(error,@"Another calendar refresh is in progress");}
    // Only the exclusive updater removes its own abandoned staging directories.
    // Published snapshots and the currently running game's resources stay intact.
    NSString *snapshots=[cache stringByAppendingPathComponent:@"snapshots"];
    for(NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:snapshots error:nil]){
        if(![name hasPrefix:@".stage-"]||![[NSUUID alloc]initWithUUIDString:[name substringFromIndex:7]])continue;
        NSString *path=[snapshots stringByAppendingPathComponent:name];if(!lstat(path.fileSystemRepresentation,&st)&&S_ISDIR(st.st_mode))[NSFileManager.defaultManager removeItemAtPath:path error:nil];
    }
    NSString *newer=current(cache,title,version);
    if(newer){NSDictionary *have=snapshot(newer,title,version),*before=old?snapshot(old,title,version):nil;
        if(!before||[have[@"catalogRevision"] longLongValue]>=[before[@"catalogRevision"] longLongValue])old=newer;}
    BOOL ok=publishLocked(cache,title,version,envelope,old,error);flock(fd,LOCK_UN);close(fd);return ok;
}
int dm_bcat_prepare(const char *titleC,const char *versionC,const char *catalogC,const char *destinationC,char *error,size_t errorSize){@autoreleasepool{
    NSString *title=titleC?[[NSString stringWithUTF8String:titleC] lowercaseString]:nil,*version=versionC?[NSString stringWithUTF8String:versionC]:nil,*destination=destinationC?[NSString stringWithUTF8String:destinationC]:nil,*why=nil;
    BOOL ok=dm_bcat_supported(titleC)&&[version isEqual:@"5.5.2"]&&safeDirectory(destination,YES);
    if(ok){NSString *old=current(destination,title,version);ok=publish(destination,title,version,catalogC?boundedRead([NSString stringWithUTF8String:catalogC],1048576):nil,old,&why);}
    if(!ok&&error&&errorSize)snprintf(error,errorSize,"%s",(why?:@"Unsupported calendar recipe or unsafe destination").UTF8String);return ok;
}}
@interface DmBcatRefresh : NSObject
@property(nonatomic) unsigned status;
@property(nonatomic) unsigned result;
@property(nonatomic) uint64_t bytes;
@end
@implementation DmBcatRefresh
@end
static char overlayPath[1024],deliveryPath[1024];static pthread_once_t startOnce=PTHREAD_ONCE_INIT;
static pthread_mutex_t refreshLock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t startupLock=PTHREAD_MUTEX_INITIALIZER;static BOOL startupAccepted;
static NSString *startModules,*startTitle,*activeVersion,*activeCache,*activeRoot,*latestRoot;
static BOOL startOnline,refreshRunning;static DmBcatRefresh *activeRefresh;static NSTimeInterval lastRefresh;
static NSString *refreshSnapshot(NSString **error){
    if(!safeDirectory(activeCache,YES)){fail(error,@"Unsafe calendar cache directory");return nil;}
    NSInteger status=0;NSData *envelope=download(catalogURL,1048576,nil,&status,NULL);
    if(!envelope||status!=200){fail(error,@"Calendar eligibility check unavailable");return nil;}
    NSString *previous=current(activeCache,startTitle,activeVersion)?:activeRoot;
    if(!publish(activeCache,startTitle,activeVersion,envelope,previous,error))return nil;
    return current(activeCache,startTitle,activeVersion);
}
static uint64_t snapshotBytes(NSString *root){
    NSDictionary *m=root?snapshot(root,startTitle,activeVersion):nil;uint64_t bytes=0;
    for(NSString *path in m[@"files"]){struct stat st;if(!stat([[[root stringByAppendingPathComponent:@"files"] stringByAppendingPathComponent:path] fileSystemRepresentation],&st))bytes+=(uint64_t)st.st_size;}
    return bytes;
}
static void refresh(void) {
    pthread_mutex_lock(&refreshLock);
    if(!startOnline||!activeVersion||refreshRunning||NSDate.date.timeIntervalSince1970-lastRefresh<300){pthread_mutex_unlock(&refreshLock);return;}
    DmBcatRefresh *job=[DmBcatRefresh new];job.status=2;job.result=1;activeRefresh=job;
    refreshRunning=YES;lastRefresh=NSDate.date.timeIntervalSince1970;
    pthread_mutex_unlock(&refreshLock);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool{
        NSString *why=nil,*selected=refreshSnapshot(&why);uint64_t bytes=snapshotBytes(selected);
        BOOL delivered=selected!=nil;
        pthread_mutex_lock(&refreshLock);if(delivered)latestRoot=selected;refreshRunning=NO;job.status=9;job.result=delivered?0:1;job.bytes=delivered?bytes:0;pthread_mutex_unlock(&refreshLock);
        fprintf(stderr,"BCAT: %s\n",delivered?"calendar refresh validated; delivery opens use the new snapshot, RomFS overlay remains fixed for this session":"refresh unavailable or invalid; previous validated calendar retained");
    }});
}
// Runs on the pre-guest boot thread, never on a UI thread or during rendering.
// Every new process checks again, even when
// a previous launch has just populated the cache. Only the completed, validated
// selection becomes the immutable RomFS overlay seen by the game's first open.
static void selectAtLaunch(NSString *seed){
    activeRoot=current(activeCache,startTitle,activeVersion)?:current(seed,startTitle,activeVersion);latestRoot=activeRoot;
    if(startOnline){
        launchDeadline=monotonicTime()+12*NSEC_PER_SEC;
        fprintf(stderr,"BCAT: checking calendar for this launch before game startup\n");
        NSDictionary *before=activeRoot?snapshot(activeRoot,startTitle,activeVersion):nil;
        NSString *why=nil,*selected=refreshSnapshot(&why);BOOL timedOut=monotonicTime()>=launchDeadline;
        launchDeadline=0;
        DmBcatRefresh *job=[DmBcatRefresh new];job.status=9;job.result=selected?0:1;job.bytes=snapshotBytes(selected);
        pthread_mutex_lock(&refreshLock);activeRefresh=job;lastRefresh=NSDate.date.timeIntervalSince1970;pthread_mutex_unlock(&refreshLock);
        if(selected){
            NSDictionary *after=snapshot(selected,startTitle,activeVersion);BOOL same=[before[@"archiveSHA256"] isEqual:after[@"archiveSHA256"]];
            activeRoot=selected;latestRoot=selected;
            fprintf(stderr,"BCAT: calendar check validated (%s); selected before game startup\n",same?"unchanged":"updated");
        }else fprintf(stderr,"BCAT: calendar check %s; %s (freshness not confirmed)\n",timedOut?"timed out":"unavailable or invalid",activeRoot?"last validated snapshot retained":"no validated calendar available");
    }else fprintf(stderr,"BCAT: offline mode; %s (freshness not checked)\n",activeRoot?"validated local calendar selected":"no validated calendar available");
    if(activeRoot){strlcpy(overlayPath,[activeRoot stringByAppendingPathComponent:@"files"].fileSystemRepresentation,sizeof overlayPath);strlcpy(deliveryPath,[[activeRoot stringByAppendingPathComponent:@"files"] stringByAppendingPathComponent:@"DebugUnderPilot/bcat"].fileSystemRepresentation,sizeof deliveryPath);}
}
static void start(void){@autoreleasepool{
    // Valid local data remains useful without online services. Only refresh and
    // synchronization requests depend on the network preference.
    if(!dm_bcat_supported(startTitle.UTF8String))return;
#ifndef DM_OPENSSL_X509
    return; // no signature verifier => unavailable, without any HTTP request
#endif
    const char *override=getenv("DM_NEXTENDO_BCAT_DIR");
    NSString *seed=override?[NSString stringWithUTF8String:override]:[startModules stringByAppendingPathComponent:@"bcat"];
    if(!safeDirectory(seed,NO))return; // absent/unsafe recipe => no network request
    NSDictionary *setup=json(boundedRead([seed stringByAppendingPathComponent:@"setup.json"],4096));NSString *version=setup[@"version"];
    if(!integer(setup[@"formatVersion"])||[setup[@"formatVersion"] integerValue]!=1||![setup[@"titleID"] isEqual:startTitle]||![version isKindOfClass:NSString.class]||![version isEqual:@"5.5.2"])return;
    NSString *bundleVersion=[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    if(bundleVersion&&![bundleVersion isEqual:version])return;
    // iOS returns container paths through /var, a system symlink. Canonicalize
    // only the OS-provided home root, then reject symlinks in our own cache tree.
    // This is ordinary private-container access, not an alternate permission path.
    char *home=realpath(NSHomeDirectory().fileSystemRepresentation,NULL);if(!home)return;
    NSString *support=[[NSString stringWithUTF8String:home] stringByAppendingPathComponent:@"Library/Application Support"];free(home);
    activeVersion=version;activeCache=[[[support stringByAppendingPathComponent:@"nxnative/BCAT"] stringByAppendingPathComponent:startTitle] stringByAppendingPathComponent:version];
    selectAtLaunch(seed);
}}
void dm_bcat_start(const char *modules,const char *title,int online){
    // Oodle's helper modes can enter guest_main only to expose a decoder; they
    // are not game launches and must not initiate calendar/network preparation.
    const char *job=getenv("DM_OODLE_JOB");if(getenv("DM_OODLE_SERVE")||(job&&*job))return;
    pthread_mutex_lock(&startupLock);
    if(startupAccepted){pthread_mutex_unlock(&startupLock);return;}
    startupAccepted=YES;
    char *resolved=modules?realpath(modules,NULL):NULL;startModules=resolved?[NSString stringWithUTF8String:resolved]:nil;free(resolved);
    startTitle=title?[[NSString stringWithUTF8String:title] lowercaseString]:nil;startOnline=online;
    pthread_once(&startOnce,start);pthread_mutex_unlock(&startupLock);
}
// This backend can be reviewed/used independently of the online-service series.
// An absent provider is offline; merely linking BCAT never enables networking.
__attribute__((weak)) int dm_bcat_online_enabled(void){return 0;}
void dm_bcat_pre_guest_start(const char *modules,const char *title){
    dm_bcat_start(modules,title,dm_bcat_online_enabled());
}
const char *dm_bcat_overlay(void){return overlayPath[0]?overlayPath:NULL;}
const char *dm_bcat_delivery(void){return deliveryPath[0]?deliveryPath:NULL;}
int dm_bcat_copy_delivery(char *out,size_t size){@autoreleasepool{
    pthread_mutex_lock(&refreshLock);NSString *root=latestRoot;pthread_mutex_unlock(&refreshLock);
    if(!root||!out||!size)return 0;NSString *path=[[root stringByAppendingPathComponent:@"files"] stringByAppendingPathComponent:@"DebugUnderPilot/bcat"];
    return strlcpy(out,path.fileSystemRepresentation,size)<size;
}}
int dm_bcat_request_sync(void){if(!startOnline||!activeVersion)return 0;refresh();return 1;}
static int refreshStatus(DmBcatRefresh *job,unsigned *result,uint64_t *downloaded){int status=job?(int)job.status:0;if(result)*result=job?job.result:1;if(downloaded)*downloaded=job?job.bytes:0;return status;}
int dm_bcat_sync_status(unsigned *result,uint64_t *downloaded){pthread_mutex_lock(&refreshLock);int status=refreshStatus(activeRefresh,result,downloaded);pthread_mutex_unlock(&refreshLock);return status;}
void *dm_bcat_sync_observe(void){@autoreleasepool{
    if(!dm_bcat_request_sync())return NULL;
    pthread_mutex_lock(&refreshLock);void *observer=activeRefresh?(void *)CFBridgingRetain(activeRefresh):NULL;pthread_mutex_unlock(&refreshLock);return observer;
}}
int dm_bcat_sync_observer_status(const void *observer,unsigned *result,uint64_t *downloaded){
    pthread_mutex_lock(&refreshLock);int status=refreshStatus((__bridge DmBcatRefresh *)observer,result,downloaded);pthread_mutex_unlock(&refreshLock);return status;
}
void dm_bcat_sync_release(void *observer){if(observer)CFRelease(observer);}

void dm_bcat_digest(const void *bytes,size_t length,unsigned char out[16]){CC_MD5(bytes,(CC_LONG)length,out);}
