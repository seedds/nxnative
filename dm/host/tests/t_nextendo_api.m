// Canonical API behavior with a synthetic proof and mock transport only.
#import "../nextendo_api_apple.h"
#include <stdio.h>
#include <string.h>
static int checks, failures, requests;
static BOOL linked = YES;
static unsigned response_status = 200;
static bool oversized, transport_failed;
// Exercise the production streaming delegate directly; no URL connection starts.
@interface DmNextendoApiTransfer : NSObject <NSURLSessionDataDelegate,NSURLSessionTaskDelegate>
@property(nonatomic) NSMutableData *data;
@property(nonatomic) BOOL failed;
@property(nonatomic) NSInteger status;
@property(nonatomic) dispatch_semaphore_t completed;
@end
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"API check failed at %d\n",__LINE__); } } while (0)
NSString *dm_nextendo_api_bearer(void) { return linked ? @"synthetic-proof" : nil; }
NSData *dm_nextendo_api_identity(void) { return nil; }
void dm_guest_host_wait_begin(void) {}
void dm_guest_host_wait_end(void) {}
static bool transport(const char *url,const char *method,const char *authorization,const uint8_t *body,size_t body_size,
                      uint8_t *output,size_t capacity,size_t *size,unsigned *status) {
    requests++; CHECK(!strcmp(authorization,"Bearer synthetic-proof"));
    CHECK(!strcmp(url,"https://nextendo.network/api/friends") || !strcmp(url,"https://nextendo.network/api/me") || !strcmp(url,"https://nextendo.network/api/presence"));
    if (strstr(url,"presence")) { CHECK(!strcmp(method,"POST") && body && body_size); }
    else { CHECK(!strcmp(method,"GET") && !body_size); }
    const char json[]="{\"friends\":[]}"; CHECK(capacity >= sizeof json);
    memcpy(output,json,sizeof json-1); *size=oversized?capacity+1:sizeof json-1; *status=response_status;
    return !transport_failed;
}
int main(void) { @autoreleasepool {
    dm_nextendo_api_test_transport(transport); uint8_t output[256]; size_t size=99;
    CHECK(!dm_nextendo_api_request("friends",NULL,0,output,sizeof output,&size) && size==14);
    CHECK(!dm_nextendo_api_request("me",NULL,0,output,sizeof output,&size));
    CHECK(!dm_nextendo_api_request("presence",(const uint8_t*)"{}",2,output,sizeof output,&size));
    for (unsigned status=300;status<=500;status+=100) { response_status=status; CHECK(dm_nextendo_api_request("friends",NULL,0,output,sizeof output,&size)!=0 && !size); }
    response_status=200; oversized=true; CHECK(dm_nextendo_api_request("friends",NULL,0,output,sizeof output,&size)!=0 && !size); oversized=false;
    transport_failed=true; CHECK(dm_nextendo_api_request("friends",NULL,0,output,sizeof output,&size)!=0 && !size); transport_failed=false;
    int before=requests;
    CHECK(dm_nextendo_api_request("https://untrusted.invalid",NULL,0,output,sizeof output,&size)!=0);
    CHECK(dm_nextendo_api_request("../friends",NULL,0,output,sizeof output,&size)!=0);
    CHECK(dm_nextendo_api_request("friends",(const uint8_t*)"{}",2,output,sizeof output,&size)!=0);
    CHECK(dm_nextendo_api_request("presence",NULL,0,output,sizeof output,&size)!=0);
    linked=NO; CHECK(dm_nextendo_api_request("friends",NULL,0,output,sizeof output,&size)!=0);
    CHECK(before==requests && !size);
    NSURLSession *dummy=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    NSURL *fixtureURL=[NSURL URLWithString:@"https://nextendo.network/api/friends"]; if (!fixtureURL) return 1;
    NSURLSessionDataTask *dummy_task=[dummy dataTaskWithURL:fixtureURL]; // never resumed
    DmNextendoApiTransfer *transfer=[DmNextendoApiTransfer new];
    NSHTTPURLResponse *response=[[NSHTTPURLResponse alloc] initWithURL:fixtureURL statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:@{@"Content-Length":@"1048577"}];
    if (!response || !dummy_task || !transfer) return 1;
    __block BOOL cancelled=NO;
    [transfer URLSession:dummy dataTask:dummy_task didReceiveResponse:response completionHandler:^(NSURLSessionResponseDisposition disposition){cancelled=disposition==NSURLSessionResponseCancel;}];
    CHECK(cancelled&&transfer.failed&&!transfer.data.length);
    transfer=[DmNextendoApiTransfer new];
    NSData *largeChunk=[NSMutableData dataWithLength:1024*1024], *extraChunk=[NSData dataWithBytes:"X" length:1];
    if (!largeChunk || !extraChunk) return 1;
    [transfer URLSession:dummy dataTask:dummy_task didReceiveData:largeChunk];
    CHECK(!transfer.failed&&transfer.data.length==1024*1024);
    [transfer URLSession:dummy dataTask:dummy_task didReceiveData:extraChunk];
    CHECK(transfer.failed&&transfer.data.length==1024*1024);
    transfer=[DmNextendoApiTransfer new];__block BOOL redirect_refused=NO;
    NSURL *redirectURL=[NSURL URLWithString:@"https://untrusted.invalid/"]; if (!redirectURL) return 1;
    NSURLRequest *redirectRequest=[NSURLRequest requestWithURL:redirectURL]; if (!redirectRequest) return 1;
    [transfer URLSession:dummy task:dummy_task willPerformHTTPRedirection:response newRequest:redirectRequest completionHandler:^(NSURLRequest *request){redirect_refused=request==nil;}];
    CHECK(redirect_refused&&transfer.failed);
    [transfer URLSession:dummy task:dummy_task didCompleteWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]];
    CHECK(transfer.failed&&dispatch_semaphore_wait(transfer.completed,DISPATCH_TIME_NOW)==0);
    [dummy invalidateAndCancel];
    printf("Nextendo API: %d checks, %d failed; synthetic transport only\n",checks,failures); return failures?1:0;
} }
