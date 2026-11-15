#include "../../games/010019A01E2F2000/menu_trace.inc"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint8_t component[0x340]={0},owner[0x40]={0},props[0x60]={0},node[0x100]={0};float world[15];
    const uint8_t id[]={0xeb,0x1c,0x03,0x70,0xbd,0xd0,0x24,0x4f,0x8e,0x46,0x1a,0x34,0xa4,0x16,0x17,0x08};
    const void *p=owner;memcpy(component+0x20,&p,8);p=props;memcpy(owner+0x30,&p,8);p=world;memcpy(owner+0x38,&p,8);
    component[0x1c]=0xd8;memcpy(props+12,id,16);
    float gui[12],source[]={-5.6892128f,3.0253084f,-9.99f};for(int i=0;i<12;i++)gui[i]=i*.125f;for(int i=0;i<15;i++)world[i]=i*.25f;
    memcpy(node+0x60,gui,sizeof gui);memcpy(props+0x1c,source,sizeof source);
    uint8_t oldc[sizeof component],oldo[sizeof owner],oldp[sizeof props],oldn[sizeof node];float oldw[15];
    memcpy(oldc,component,sizeof oldc);memcpy(oldo,owner,sizeof oldo);memcpy(oldp,props,sizeof oldp);memcpy(oldn,node,sizeof oldn);memcpy(oldw,world,sizeof oldw);
    mp4_menu_snapshot s;assert(mp4_menu_snapshot_read(component,node,&s));assert(!memcmp(s.gui,gui,sizeof gui));assert(!memcmp(s.world,world,sizeof world));assert(!memcmp(s.source,source,sizeof source));assert(s.pane==(uintptr_t)component);
    assert(!memcmp(oldc,component,sizeof oldc)&&!memcmp(oldo,owner,sizeof oldo)&&!memcmp(oldp,props,sizeof oldp)&&!memcmp(oldn,node,sizeof oldn)&&!memcmp(oldw,world,sizeof oldw));
    for(int i=0;i<16;i++){props[12+i]^=1;assert(!mp4_menu_snapshot_read(component,node,&s));props[12+i]^=1;}
    world[0]=NAN;assert(!mp4_menu_snapshot_read(component,node,&s));world[0]=0;
    component[0x1c]=0;p=(void*)(uintptr_t)1;memcpy(component+0x20,&p,8);assert(!mp4_menu_snapshot_read(component,node,&s));
    assert(!mp4_menu_snapshot_read(NULL,node,&s));assert(!mp4_menu_snapshot_read(component,NULL,&s));assert(!mp4_menu_snapshot_read(component,node,NULL));
    puts("PASS: menu trace reads only the selected active inventory captions; all source bytes unchanged");
}
