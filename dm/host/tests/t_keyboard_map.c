#include "../keyboard_map.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    dm_keyboard k; uint64_t b; int32_t x,y,rx,ry;
    dm_keyboard_init(&k);
    assert(dm_keyboard_key("W")==13 && dm_keyboard_key("J")==38 && dm_keyboard_key("P")==35);
    assert(dm_keyboard_key("Space")==49 && dm_keyboard_key("Escape")==53);
    k.held[38]=1; k.held[13]=1;
    dm_keyboard_state(&k,&b,&x,&y,&rx,&ry); assert(b==DM_BTN_A && y==32767 && x==0);
    assert(dm_keyboard_configure(&k,"W=ZR,S=ZL,A=LLeft,D=LRight,Space=Y,Shift=A,RightShift=A,Return=A,Escape=B,P=Plus,V=R"));
    memset(k.held,0,sizeof k.held); k.held[13]=1; k.held[0]=1; k.held[49]=1;
    dm_keyboard_state(&k,&b,&x,&y,&rx,&ry); assert(b==(DM_BTN_ZR|DM_BTN_Y) && x==-32767 && y==0);
    k.held[56]=1; k.held[36]=1; k.held[38]=1;
    dm_keyboard_state(&k,&b,&x,&y,&rx,&ry); assert(b&DM_BTN_A);
    k.held[56]=k.held[36]=0;
    dm_keyboard_state(&k,&b,&x,&y,&rx,&ry); assert(b&DM_BTN_A); // releasing aliases must not release the held J
    k.held[2]=1; dm_keyboard_state(&k,&b,&x,&y,&rx,&ry); assert(x==0); // opposing directions cancel
    assert(dm_keyboard_configure(&k,"Up=RUp,Down=RDown,Left=RLeft,Right=RRight,J=None"));
    memset(k.held,0,sizeof k.held); k.held[126]=1; k.held[38]=1;
    dm_keyboard_state(&k,&b,&x,&y,&rx,&ry); assert(b==0 && ry==32767);
    dm_keyboard before=k;
    assert(!dm_keyboard_configure(&k,"W=A,BadKey=ZR")); assert(!memcmp(&k,&before,sizeof k));
    assert(!dm_keyboard_configure(&k,"W=BadAction")); assert(!dm_keyboard_configure(&k,"W"));
    memset(k.held,0,sizeof k.held); dm_keyboard_state(&k,&b,&x,&y,&rx,&ry);
    assert(b==0 && x==0 && y==0 && rx==0 && ry==0);
    puts("keyboard map: default input, racing overrides, aliases, sticks, reset and invalid-map rollback passed");
}
