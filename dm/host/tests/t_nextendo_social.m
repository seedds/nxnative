// Actual HTTP/schema/cache and SDK adapters with synthetic users/instructions.
#import <Foundation/Foundation.h>
#include "../nextendo_api_apple.h"
#include "../nextendo_social.h"
#include "../net_socket.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int checks,failures,requests; static unsigned generation=1,response_status=200;
static BOOL linked=YES; static NSString *reply,*posted_detail;
static dm_net_mode_t mode=DM_NET_AUTO;
static uint8_t uid[16]={1};
static uint32_t constructor[]={0x52804002,0x2a1f03e1,0x14000000};
static uint32_t presence_constructor[]={0x52801c02,0x2a1f03e1,0x14000000};
static uint32_t nick[]={0x91006000,0xd65f03c0},presence[]={0x91010000,0xd65f03c0},account[]={0xf9400800,0xd65f03c0},valid[]={0x3944a000,0xd65f03c0},favorite[]={0x39448000,0xd65f03c0};
#define CHECK(x) do{checks++;if(!(x)){failures++;fprintf(stderr,"social check failed line %d\n",__LINE__);}}while(0)
NSString *dm_nextendo_api_bearer(void){return linked?@"synthetic-social-proof":nil;}
NSData *dm_nextendo_api_identity(void){return linked?[NSData dataWithBytes:&generation length:sizeof generation]:nil;}
void dm_guest_host_wait_begin(void){} void dm_guest_host_wait_end(void){}
dm_net_mode_t dm_net_mode(void){return mode;}
int dm_account_uid_exists(const void *value){return value&&!memcmp(value,uid,16);}
int dm_account_copy_uid(void *value){if(!value)return 0;memcpy(value,uid,16);return 1;}
uint32_t dm_nextendo_account_ensure(void){return linked?0:0x2a2;}
void *guest_sym(const char *name){
    if(strstr(name,"UserPresenceC1"))return presence_constructor;
    if(strstr(name,"FriendC1"))return constructor;
    if(strstr(name,"GetNickname"))return nick;
    if(strstr(name,"GetPresence"))return presence;
    if(strstr(name,"GetAccountId"))return account;
    if(strstr(name,"IsValid"))return valid;
    if(strstr(name,"IsFavorite"))return favorite;
    return NULL;
}
static bool transport(const char *url,const char *method,const char *auth,const uint8_t *body,size_t body_size,uint8_t *output,size_t capacity,size_t *size,unsigned *status){
    requests++;CHECK(!strcmp(auth,"Bearer synthetic-social-proof"));
    if(strstr(url,"/api/presence")){
        CHECK(!strcmp(method,"POST")&&body&&body_size);
        NSDictionary *posted=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:body length:body_size] options:0 error:nil];
        CHECK([posted[@"app_id"] isEqual:@"0100152000022000"]&&[posted[@"status"] unsignedIntValue]<=2);
        posted_detail=posted[@"app_detail"];
        memcpy(output,"{}",2);*size=2;*status=response_status;return true;
    }
    CHECK(!strcmp(method,"GET"));
    NSData *data=[(strstr(url,"/api/me")?@"{\"nsa\":\"abcdef0000001234\"}":reply) dataUsingEncoding:NSUTF8StringEncoding];
    if(data.length>capacity)return false;memcpy(output,data.bytes,data.length);*size=data.length;*status=response_status;return true;
}
int main(void){@autoreleasepool{
    setenv("DM_NEXTENDO_ROUTES","/synthetic/public-policy.tsv",1);
    reply=@"{\"friends\":[{\"pid\":501,\"nsa\":\"abcdef0000000501\",\"name\":\"Alpha\",\"favorite\":true,\"presence\":{\"status\":2,\"app_id\":\"0100152000022000\",\"app_field\":\"AQID\"}},{\"pid\":502,\"name\":\"Beta\",\"presence\":{\"status\":1,\"app_id\":\"0100000000001000\"}},{\"pid\":503,\"name\":\"Gamma\",\"presence\":null}]}";
    dm_nextendo_api_test_transport(transport);
    uint8_t available=9;
    CHECK(!dm_nextendo_friends_available(&available,uid)&&available);
    mode=DM_NET_LAN;available=9;CHECK(!dm_nextendo_friends_available(&available,uid)&&!available);mode=DM_NET_AUTO;
    linked=NO;available=9;CHECK(dm_nextendo_friends_available(&available,uid)!=0&&!available);linked=YES;
    uint8_t unavailable_uid[16]={9};CHECK(dm_nextendo_friends_ensure(unavailable_uid)!=0);
    uint8_t filter[16]={0},objects[0x600+16];memset(objects,0xa5,sizeof objects);int count=-1;
    CHECK(!dm_nextendo_friends_list(&count,objects,uid,0,3,filter,false)&&count==3);
    uint64_t id=0;memcpy(&id,objects+0x10,8);CHECK(id==501&&!strcmp((char*)objects+0x18,"Alpha"));
    CHECK(objects[0x58]==2&&objects[0x5c]==1&&objects[0x60]==1&&objects[0x61]==2&&objects[0x62]==3&&objects[0x120]==1&&objects[0x128]==1);
    CHECK(!memcmp(objects,uid,16)&&objects[0x600]==0xa5);
    int before=requests;uint64_t ids[4]={0,0,0,0xface};
    CHECK(!dm_nextendo_friends_list(&count,ids,NULL,1,1,filter,true)&&count==1&&ids[0]==502&&ids[1]==0&&ids[3]==0xface&&requests==before);
    char profile[]="/tmp/nx-social-profile-XXXXXX";CHECK(mkdtemp(profile)!=NULL);
    NSString *profilePath=[NSString stringWithUTF8String:profile]; if (!profilePath) return 1;
    NSString *manifest=[profilePath stringByAppendingPathComponent:@"manifest.json"];
    CHECK([@"{\"identity\":{\"friends\":\"baas\",\"account\":\"baas\"}}" writeToFile:manifest atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    chmod(manifest.fileSystemRepresentation,0600);
    NSString *routes=[[NSString stringWithUTF8String:profile] stringByAppendingPathComponent:@"routes.tsv"];
    setenv("DM_NEXTENDO_ROUTES",routes.fileSystemRepresentation,1);
    CHECK(dm_nextendo_api_baas_namespace("friends")&&dm_nextendo_api_baas_namespace("account"));
    CHECK(!dm_nextendo_friends_list(&count,ids,uid,0,1,filter,true)&&count==1&&ids[0]==0xabcdef0000000501ull);
    chmod(manifest.fileSystemRepresentation,0666);CHECK(!dm_nextendo_api_baas_namespace("friends"));
    unlink(manifest.fileSystemRepresentation);rmdir(profile);unsetenv("DM_NEXTENDO_ROUTES");
    available=9;before=requests;CHECK(!dm_nextendo_friends_available(&available,uid)&&!available&&requests==before);
    setenv("DM_NEXTENDO_ROUTES","/synthetic/public-policy.tsv",1);
    filter[4]=1;CHECK(!dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)&&count==1&&ids[0]==501);filter[4]=0;
    filter[0]=3;CHECK(!dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)&&count==2);filter[0]=0;
    filter[5]=1;CHECK(!dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)&&count==1);filter[5]=0;
    uint64_t selected_ids[]={502,501};before=requests;
    CHECK(!dm_nextendo_friends_update(objects,uid,selected_ids,2)&&requests==before+1&&!strcmp((char*)objects+0x18,"Beta")&&!strcmp((char*)objects+0x218,"Alpha"));
    uint64_t absent=999;memset(objects,0xa5,sizeof objects);CHECK(dm_nextendo_friends_update(objects,uid,&absent,1)!=0&&objects[0]==0xa5);
    filter[6]=1;CHECK(dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)!=0&&!count);filter[6]=0;
    valid[0]=0;CHECK(dm_nextendo_friends_list(&count,objects,uid,0,1,filter,false)!=0&&!count);valid[0]=0x3944a000;
    uint8_t bad_uid[16]={2};CHECK(dm_nextendo_friends_list(&count,ids,bad_uid,0,3,filter,true)!=0&&!count);
    mode=DM_NET_LAN;CHECK(dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)!=0&&!count);mode=DM_NET_AUTO;
    linked=NO;CHECK(dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)!=0&&!count);linked=YES;
    generation++;response_status=503;CHECK(dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)!=0&&!count);response_status=200;
    generation++;response_status=503;available=9;CHECK(dm_nextendo_friends_available(&available,uid)!=0&&!available);response_status=200;
    for(NSString *bad in @[@"{}",@"{\"friends\":null}",@"{\"friends\":[{\"pid\":true,\"name\":\"wrong\"}]}",@"{\"friends\":[{\"pid\":501,\"name\":\"wrong\",\"presence\":{\"status\":99}}]}"]){
        generation++;reply=bad;CHECK(dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)!=0&&!count);
    }
    generation++;reply=@"{\"friends\":[]}";CHECK(!dm_nextendo_friends_list(&count,ids,uid,0,3,filter,true)&&!count);
    uint64_t nsa=0;CHECK(!dm_nextendo_api_baas_id(&nsa)&&nsa==0xabcdef0000001234ull);
    uint8_t own[0xe0+16];memset(own,0,sizeof own);own[0xe0]=0xa5;
    CHECK(!dm_nextendo_presence_initialize(own,uid)&&!memcmp(own,uid,16)&&own[0xe0]==0xa5);
    own[0x20]=1;own[0x21]=2;CHECK(!dm_nextendo_presence_commit(own)&&[posted_detail isEqual:@""]);
    // the game's description: key and value strings in turn, the description under sys_description
    static const char described[]="mode\0race\0sys_description\0Grand Prix";memset(own+0x20,0,192);memcpy(own+0x20,described,sizeof described);
    CHECK(!dm_nextendo_presence_commit(own)&&[posted_detail isEqual:@"Grand Prix"]);
    memset(own+0x20,'x',192);CHECK(!dm_nextendo_presence_commit(own)&&[posted_detail isEqual:@""]);   // a field that never ends its strings
    memset(own+0x20,0,192);
    response_status=503;CHECK(dm_nextendo_presence_commit(own)!=0);response_status=200;
    presence_constructor[0]=0;CHECK(dm_nextendo_presence_initialize(own,uid)!=0);presence_constructor[0]=0x52801c02;
    mode=DM_NET_OFFLINE;CHECK(dm_nextendo_presence_commit(own)!=0);mode=DM_NET_AUTO;
    linked=NO;CHECK(dm_nextendo_api_baas_id(&nsa)!=0&&!nsa);CHECK(dm_nextendo_presence_initialize(own,uid)!=0);
    printf("Nextendo social: %d checks, %d failed; real parsers and SDK adapters, synthetic HTTP only\n",checks,failures);return failures?1:0;
}}
