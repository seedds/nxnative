// Provider social data, independent of the guest SDK's object ABI.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
enum { DM_SOCIAL_FRIEND_MAX=1024, DM_SOCIAL_APP_FIELD=192 };
typedef struct {
    uint64_t pid, nsa, application, last_online;
    char nickname[33];
    uint8_t app_field[DM_SOCIAL_APP_FIELD];
    uint32_t presence;
    bool favorite;
} dm_social_friend;
uint32_t dm_nextendo_social_friends(dm_social_friend **output, size_t *count);
uint32_t dm_nextendo_social_friends_refresh(dm_social_friend **output, size_t *count);
bool dm_nextendo_api_baas_namespace(const char *kind);
uint32_t dm_nextendo_friends_list(int *count,void *output,const void *uid,int offset,int capacity,const void *filter,bool ids_only);
uint32_t dm_nextendo_friends_update(void *output,const void *uid,const uint64_t *ids,int count);
uint32_t dm_nextendo_friends_ensure(const void *uid);
uint32_t dm_nextendo_friends_available(uint8_t *available,const void *uid);
uint32_t dm_nextendo_presence_initialize(void *object,const void *uid);
uint32_t dm_nextendo_presence_commit(const void *object);
uint32_t dm_nextendo_social_publish(unsigned status,const uint8_t *app_field,size_t size);
