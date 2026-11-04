// Original social-data/SDK adapter. Layout is verified from SDK function semantics,
// not fixed module addresses or a guessed opaque-object size.
#include "hle.h"
#include "nextendo_social.h"
#include "account_local.h"
#include "net_socket.h"
#include "nextendo_account.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static bool getter(const char *name,uint32_t instruction) {
    const uint32_t *code=guest_sym(name); uint32_t words[2]; if (!code) return false;
    memcpy(words,code,sizeof words); return words[0]==instruction && words[1]==0xd65f03c0u;
}
static bool friend_layout(void) {
    const uint32_t *code=guest_sym("_ZN2nn7friends6FriendC1Ev"); uint32_t words[3]; if (!code) return false;
    memcpy(words,code,sizeof words);
    if (words[0]!=0x52804002u && words[1]!=0x52804002u) return false; // constructor memset size 0x200
    return getter("_ZNK2nn7friends6Friend11GetNicknameEv",0x91006000u) &&
        getter("_ZNK2nn7friends6Friend11GetPresenceEv",0x91010000u) &&
        getter("_ZNK2nn7friends6Friend12GetAccountIdEv",0xf9400800u) &&
        getter("_ZNK2nn7friends6Friend7IsValidEv",0x3944a000u) &&
        getter("_ZNK2nn7friends6Friend10IsFavoriteEv",0x39448000u);
}
static bool presence_layout(void) {
    const uint32_t *code=guest_sym("_ZN2nn7friends12UserPresenceC1Ev"); if(!code)return false;
    uint32_t first;memcpy(&first,code,4);
    if(first==0x52801c02u)return true; // constructor memset size 0xe0
    // New SDKs inline the complete fourteen-pair zero initialization.
    if(first!=0xa90d7c1fu)return false;
    uint32_t words[15];memcpy(words,code,sizeof words);
    for(unsigned i=0;i<14;i++)if(words[i]!=(0xa9007c1fu|((26-2*i)<<15)))return false;
    return words[14]==0xd65f03c0u;
}
uint32_t dm_nextendo_presence_initialize(void *object,const void *uid) {
    if(!object||!presence_layout()||!dm_net_nextendo_enabled()||dm_nextendo_account_ensure())return RES_FAIL;
    uint8_t local[16];if(!uid){if(!dm_account_copy_uid(local))return RES_FAIL;uid=local;}
    if(!dm_account_uid_exists(uid))return RES_FAIL;
    // The original SDK initializer writes only the selected local UID. Its
    // constructors, Clear, SetDescription and SetAppValue keep their own format.
    memcpy(object,uid,16);return 0;
}
uint32_t dm_nextendo_presence_commit(const void *object) {
    if(!object||!presence_layout()||!dm_net_nextendo_enabled()||!dm_account_uid_exists(object))return RES_FAIL;
    return dm_nextendo_social_publish(2,(const uint8_t*)object+0x20,DM_SOCIAL_APP_FIELD);
}
static void encode_friend(uint8_t output[0x200],const dm_social_friend *item,const void *uid,bool baas) {
    memset(output,0,0x200); memcpy(output,uid,16);
    uint64_t id=baas&&item->nsa?item->nsa:item->pid;
    memcpy(output+0x10,&id,8); memcpy(output+0x18,item->nickname,33);
    memcpy(output+0x40,uid,16); memcpy(output+0x50,&item->last_online,8); memcpy(output+0x58,&item->presence,4);
    output[0x5c]=item->application==strtoull(DM_GAME_ID,NULL,16);
    memcpy(output+0x60,item->app_field,DM_SOCIAL_APP_FIELD);
    output[0x120]=item->favorite; output[0x128]=1;
}
uint32_t dm_nextendo_friends_ensure(const void *uid) {
    if (!dm_net_nextendo_enabled() || !dm_account_uid_exists(uid)) return RES_FAIL;
    dm_social_friend *friends=NULL; size_t count=0;
    uint32_t result=dm_nextendo_social_friends(&friends,&count); free(friends); return result;
}
uint32_t dm_nextendo_friends_available(uint8_t *available,const void *uid) {
    if (!available || !dm_account_uid_exists(uid)) return RES_FAIL;
    *available=0;
    if (!dm_net_nextendo_enabled()) return 0;
    uint32_t result=dm_nextendo_friends_ensure(uid); if (!result) *available=1; return result;
}
uint32_t dm_nextendo_friends_list(int *count,void *output,const void *uid,int offset,int capacity,const void *filter,bool ids_only) {
    if (count) *count=0;
    if (!count || !filter || offset<0 || capacity<0 || capacity>DM_SOCIAL_FRIEND_MAX || (capacity&&!output) || !dm_net_nextendo_enabled()) return RES_FAIL;
    uint8_t local_uid[16];
    if (!uid) { if (!dm_account_copy_uid(local_uid)) return RES_FAIL; uid=local_uid; }
    if (!dm_account_uid_exists(uid) || (!ids_only && !friend_layout())) return RES_FAIL;
    uint8_t options[16]; memcpy(options,filter,sizeof options); uint32_t presence; memcpy(&presence,options,4);
    if (presence>3 || options[4]>1 || options[5]>1 || options[6] || options[7]) return RES_FAIL;
    dm_social_friend *friends=NULL; size_t total=0;
    if (dm_nextendo_social_friends(&friends,&total)) return RES_FAIL;
    bool baas=dm_nextendo_api_baas_namespace("friends"); size_t matched=0,written=0;
    for (size_t i=0;i<total;i++) {
        const dm_social_friend *item=&friends[i];
        if ((presence==1&&item->presence!=1) || (presence==2&&item->presence!=2) ||
            (presence==3&&!item->presence) || (options[4]&&!item->favorite) ||
            (options[5]&&item->application!=strtoull(DM_GAME_ID,NULL,16))) continue;
        if (matched++<(size_t)offset || written>=(size_t)capacity) continue;
        if (ids_only) { uint64_t id=baas&&item->nsa?item->nsa:item->pid; memcpy((uint8_t*)output+written*8,&id,8); }
        else encode_friend((uint8_t*)output+written*0x200,item,uid,baas);
        written++;
    }
    free(friends); *count=(int)written; return 0;
}
uint32_t dm_nextendo_friends_update(void *output,const void *uid,const uint64_t *ids,int count) {
    if (count<0||count>DM_SOCIAL_FRIEND_MAX||(count&&(!output||!ids))||!dm_net_nextendo_enabled()||!dm_account_uid_exists(uid)||!friend_layout())return RES_FAIL;
    dm_social_friend *friends=NULL;size_t total=0;
    // UpdateFriendInfo is an explicit refresh, independent of list pagination's
    // short-lived snapshot. A failed refresh never clears the caller's records.
    if(dm_nextendo_social_friends_refresh(&friends,&total))return RES_FAIL;
    size_t selected[DM_SOCIAL_FRIEND_MAX];
    for(int i=0;i<count;i++){
        selected[i]=total;
        for(size_t j=0;j<total;j++)if(friends[j].pid==ids[i]||(friends[j].nsa&&friends[j].nsa==ids[i])){selected[i]=j;break;}
        if(selected[i]==total){free(friends);return RES_FAIL;}
    }
    bool baas=dm_nextendo_api_baas_namespace("friends");
    for(int i=0;i<count;i++)encode_friend((uint8_t*)output+(size_t)i*0x200,&friends[selected[i]],uid,baas);
    free(friends);return 0;
}
