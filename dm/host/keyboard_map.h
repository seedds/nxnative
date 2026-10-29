// Physical Mac keys -> controller input. Overrides retain every unmapped default.
#pragma once
#include "dm_input.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef struct { int action[128]; unsigned char held[128]; } dm_keyboard;
static inline int dm_keyboard_key(const char *name) {
    static const char letters[] = "ASDFHGZXCV BQWERYT123465=97-80]OU[IP\nLJ'K;\\,/NM.\t `";
    // Letter positions follow Apple's virtual key codes, independent of text layout.
    static const struct { const char *name; int code; } named[] = {
        {"Space",49},{"Return",36},{"Enter",36},{"Escape",53},{"Tab",48},
        {"Shift",56},{"RightShift",60},{"Control",59},{"RightControl",62},
        {"Option",58},{"RightOption",61},{"Left",123},{"Right",124},{"Down",125},{"Up",126}
    };
    if (strlen(name)==1) {
        for (int i=0;i<(int)sizeof letters-1;i++)
            if (letters[i]!=' ' && strncasecmp(name,letters+i,1)==0) return i;
    }
    for (size_t i=0;i<sizeof named/sizeof *named;i++) if (!strcasecmp(name,named[i].name)) return named[i].code;
    return -1;
}
static inline int dm_keyboard_action(const char *name) {
    static const char *names[]={"A","B","X","Y","LS","RS","L","R","ZL","ZR","Plus","Minus","Left","Up","Right","Down",
        "LLeft","LRight","LUp","LDown","RLeft","RRight","RUp","RDown","Shake","None"};
    for (int i=0;i<26;i++) if (!strcasecmp(name,names[i])) return i;
    return -1;
}
static inline void dm_keyboard_init(dm_keyboard *k) {
    memset(k,0,sizeof *k); for(int i=0;i<128;i++) k->action[i]=25;
    static const int keys[]={38,40,32,34,3,5,12,14,6,8,36,48,123,126,124,125,0,2,13,1,49};
    static const int acts[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,24};
    for(size_t i=0;i<sizeof keys/sizeof *keys;i++) k->action[keys[i]]=acts[i];
}
static inline int dm_keyboard_configure(dm_keyboard *k,const char *text) {
    if(!text || !*text) return 1;
    char *copy=strdup(text); if(!copy) return 0;
    dm_keyboard next=*k; char *save=NULL; int ok=1;
    for(char *p=strtok_r(copy,",",&save);p;p=strtok_r(NULL,",",&save)) {
        char *eq=strchr(p,'='); if(!eq){ok=0;break;} *eq++=0;
        int key=dm_keyboard_key(p),action=dm_keyboard_action(eq);
        if(key<0 || action<0){ok=0;break;} next.action[key]=action;
    }
    free(copy); if(ok) *k=next; return ok;
}
static inline void dm_keyboard_state(const dm_keyboard *k,uint64_t *buttons,int32_t *lx,int32_t *ly,int32_t *rx,int32_t *ry) {
    unsigned char on[26]={0};
    for(int i=0;i<128;i++) if(k->held[i]) on[k->action[i]]=1;
    *buttons=0; for(int i=0;i<16;i++) if(on[i]) *buttons|=1ull<<i;
    if(on[24]) *buttons|=DM_BTN_SHAKE;
    *lx=(on[17]-on[16])*DM_STICK_MAX; *ly=(on[18]-on[19])*DM_STICK_MAX;
    *rx=(on[21]-on[20])*DM_STICK_MAX; *ry=(on[22]-on[23])*DM_STICK_MAX;
}
