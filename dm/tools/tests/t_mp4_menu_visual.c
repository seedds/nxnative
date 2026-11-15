#include "../../games/010019A01E2F2000/menu_provenance.inc"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
static void *thread_scope(void *component) {
    assert(!mp4_menu_scope_top);assert(mp4_menu_scope_push(component));
    assert(mp4_menu_scope_top->scope.room);mp4_menu_scope_pop();assert(!mp4_menu_scope_top);return NULL;
}
int main(void) {
    uint8_t camera[0x30]={0},owner[0xa0]={0},room[0xa0]={0},frame[8]={0},child[8]={0},node[0xa0]={0},packet[0x60],before[0x60],oldnode[0xa0];
    const void *p=owner;memcpy(camera+0x20,&p,8);p=room;memcpy(owner+0x90,&p,8);p=frame;memcpy(room+0x90,&p,8);memcpy(node+0x28,&p,8);memcpy(child,&p,8);p=packet;memcpy(node+0x50,&p,8);node[0x48]=3;
    float matrix[12]={.4002f,0,0,-5.6892128f,0,.4002f,0,3.0253084f,0,0,.4002f,-9.99f};memcpy(node+0x60,matrix,48);memcpy(oldnode,node,sizeof node);memset(packet,0xa5,sizeof packet);memcpy(before,packet,sizeof packet);
    mp4_menu_binding binding={0};assert(!mp4_menu_visual(&binding,node,packet));assert(!memcmp(packet,before,sizeof packet));assert(mp4_menu_bind(&binding,camera));
    for(int pass=0;pass<10;pass++) {
        assert(mp4_menu_visual(&binding,node,packet));float actual[12];memcpy(actual,packet+8,48);
        for(int i=0;i<12;i++)assert(fabsf(actual[i]-matrix[i]*(i<8?.9f:1.f))<.00001f);
        assert(!memcmp(packet,before,8));assert(!memcmp(packet+56,before+56,sizeof packet-56));assert(!memcmp(node,oldnode,sizeof node));
    }
    // Geometry extents at native x +/-8 become +/-7.2 (the 16:10 crop),
    // with the same scale in y. Depth and the native animated source survive.
    float edge[12]={1,0,0,8,0,1,0,4.5f,0,0,1,-10};memcpy(node+0x60,edge,48);assert(mp4_menu_visual(&binding,node,packet));float out[12];memcpy(out,packet+8,48);assert(fabsf(out[3]-7.2f)<.00001f);assert(fabsf(out[7]-4.05f)<.00001f);assert(out[11]==-10);
    p=child;memcpy(node+0x28,&p,8);assert(mp4_menu_visual(&binding,node,packet));
    p=NULL;memcpy(node+0x28,&p,8);assert(!mp4_menu_visual(&binding,node,packet));
    p=child;memcpy(child,&p,8);memcpy(node+0x28,&p,8);assert(!mp4_menu_visual(&binding,node,packet)); // bounded parent cycle
    p=frame;memcpy(node+0x28,&p,8);node[0x48]=2;assert(!mp4_menu_visual(&binding,node,packet));node[0x48]=3;
    assert(!mp4_menu_visual(&binding,node,before));edge[2]=NAN;memcpy(node+0x60,edge,48);assert(!mp4_menu_visual(&binding,node,packet));
    mp4_menu_unbind(&binding,owner);assert(binding.frame);mp4_menu_unbind(&binding,camera);assert(!binding.frame);assert(!mp4_menu_visual(&binding,node,packet));
    assert(!mp4_menu_bind(NULL,camera));assert(!mp4_menu_bind(&binding,NULL));assert(!mp4_menu_visual(NULL,node,packet));
    // Global GUI-layer nodes still belong to their originating pause room.
    uint8_t props[0x60]={0},ids[16]={0},room_b[0xa0]={0},owner_b[0xa0]={0},component_b[0x30]={0},global[8]={0};
    p=props;memcpy(owner+0x30,&p,8);p=ids;memcpy(props+0x58,&p,8);uint32_t count=1;memcpy(props+0x50,&count,4);
    p=room_b;memcpy(owner_b+0x90,&p,8);p=owner_b;memcpy(component_b+0x20,&p,8);p=frame;memcpy(room_b+0x90,&p,8);
    p=global;memcpy(node+0x28,&p,8);memcpy(node+0x60,matrix,48);
    assert(mp4_menu_scope_push(camera));assert(mp4_menu_node_register(node));
    const void *collision=(const void *)((uintptr_t)node+0x4000);assert(mp4_menu_node_register(collision));
    assert(*mp4_menu_node_slot(collision));
    assert(mp4_menu_scope_push(component_b));assert(mp4_menu_scope_top->scope.room==(uintptr_t)room_b);
    mp4_menu_scope_pop();assert(mp4_menu_scope_top->scope.room==(uintptr_t)room);
    pthread_t worker;assert(!pthread_create(&worker,NULL,thread_scope,component_b));assert(!pthread_join(worker,NULL));assert(mp4_menu_scope_top->scope.room==(uintptr_t)room);
    mp4_menu_scope_pop();assert(!mp4_menu_scope_top);
    // Provenance can be registered before the camera becomes active.
    assert(mp4_menu_bind(&binding,camera));mp4_menu_node_entry *entry=*mp4_menu_node_slot(node);assert(entry);
    for(int i=0;i<10;i++)assert(mp4_menu_visual_scoped(&binding,node,packet,&entry->scope));
    float fitted[12];memcpy(fitted,packet+8,48);for(int i=0;i<12;i++)assert(fabsf(fitted[i]-matrix[i]*(i<8?.9f:1.f))<.00001f);
    // A global-frame node from another room must never inherit the pause fit.
    assert(mp4_menu_scope_push(component_b));assert(mp4_menu_node_register(node));mp4_menu_scope_pop();entry=*mp4_menu_node_slot(node);assert(!mp4_menu_visual_scoped(&binding,node,packet,&entry->scope));
    // Exact background identity overrides even the matching room-frame fallback.
    const uint8_t background[]={0xc4,0x48,0x2d,0xd8,0x86,0x14,0x56,0x4e,0xb1,0x19,0x14,0x9f,0x03,0x1d,0x18,0x59};memcpy(ids,background,16);
    assert(mp4_menu_scope_push(camera));assert(mp4_menu_node_register(node));mp4_menu_scope_pop();entry=*mp4_menu_node_slot(node);assert(entry->scope.backdrop);
    p=frame;memcpy(node+0x28,&p,8);memcpy(before,packet,sizeof packet);assert(!mp4_menu_visual_scoped(&binding,node,packet,&entry->scope));assert(!memcmp(before,packet,sizeof packet));
    memset(ids,0,16);assert(mp4_menu_scope_push(camera));assert(mp4_menu_node_register(node));mp4_menu_scope_pop();entry=*mp4_menu_node_slot(node);assert(!entry->scope.backdrop);assert(mp4_menu_visual_scoped(&binding,node,packet,&entry->scope));
    const uint8_t overlay[]={0xe3,0xfe,0xb7,0xfe,0x81,0x02,0x6e,0x4a,0x84,0xb2,0x22,0x4c,0x14,0x37,0xea,0x12};
    memcpy(ids,overlay,16);assert(mp4_menu_scope_push(camera));assert(mp4_menu_node_register(node));mp4_menu_scope_pop();entry=*mp4_menu_node_slot(node);assert(entry->scope.backdrop);assert(!mp4_menu_visual_scoped(&binding,node,packet,&entry->scope));
    // Dotted artwork and the paused-scene image are independent native
    // backgrounds too. Simulate the fresh native packet copy: none of its
    // matrix, depth, or metadata may receive the foreground .9 transform.
    const uint8_t remaining_backgrounds[][16]={
        {0xd5,0xfc,0xee,0x4c,0x3e,0x64,0xba,0x4c,0x90,0x44,0xb8,0x84,0xf4,0xa0,0xe1,0x57},
        {0x9e,0x42,0xac,0x01,0x3f,0x86,0xe9,0x02,0xbe,0x06,0xc0,0xa9,0x59,0xfd,0x69,0x83}};
    const float background_matrices[][12]={
        {.84f,0,0,0,0,1.9129f,0,0,0,0,1,-16.662094f},
        {1,0,0,0,0,1,0,0,0,0,1,-20}};
    for(unsigned bg=0;bg<2;bg++) {
        memcpy(ids,remaining_backgrounds[bg],16);assert(mp4_menu_scope_push(camera));assert(mp4_menu_node_register(node));mp4_menu_scope_pop();
        entry=*mp4_menu_node_slot(node);assert(entry->scope.backdrop);
        memcpy(node+0x60,background_matrices[bg],48);memcpy(oldnode,node,sizeof node);
        memcpy(packet+8,background_matrices[bg],48);memcpy(before,packet,sizeof packet);
        for(int pass=0;pass<3;pass++)assert(!mp4_menu_visual_scoped(&binding,node,packet,&entry->scope));
        assert(!memcmp(before,packet,sizeof packet));assert(!memcmp(oldnode,node,sizeof node));
    }
    // An adjacent foreground identity still fits on the same shared layer.
    ids[15]^=1;assert(mp4_menu_scope_push(camera));assert(mp4_menu_node_register(node));mp4_menu_scope_pop();
    entry=*mp4_menu_node_slot(node);assert(!entry->scope.backdrop);assert(mp4_menu_visual_scoped(&binding,node,packet,&entry->scope));
    entry->scope.frame=(uintptr_t)global;assert(!mp4_menu_visual_scoped(&binding,node,packet,&entry->scope));
    assert(mp4_menu_node_register(node));assert(!*mp4_menu_node_slot(node)); // reused without a new native scope
    mp4_menu_node_forget(node);assert(!*mp4_menu_node_slot(node));mp4_menu_node_forget(collision);mp4_menu_node_forget(node);
    assert(mp4_menu_node_register(node));assert(!*mp4_menu_node_slot(node)); // no creation scope; no stale tag on pointer reuse
    for(unsigned i=0;i<1024;i++)assert(!mp4_menu_nodes[i]);
    uint32_t projection[16]={0x3e000000,0,0,0,0,0x3e638e39,0,0x80000000,0,0,0xb9800000,0x80000000,0,0,0,0x3f800000},original_projection[16];
    memcpy(original_projection,projection,64);mp4_menu_binding inactive={0};assert(!mp4_menu_compass_projection(&inactive,projection));
    assert(mp4_menu_compass_projection(&binding,projection));float pf[16];memcpy(pf,projection,64);assert(fabsf(pf[0]-.1125f)<1e-7f);assert(fabsf(pf[5]-.2f)<1e-7f);
    for(unsigned i=0;i<16;i++)if(i!=0 && i!=5)assert(projection[i]==original_projection[i]);
    assert(!mp4_menu_compass_projection(&binding,projection)); // no accumulated fit
    memcpy(projection,original_projection,64);assert(mp4_menu_compass_projection(&binding,projection));
    puts("PASS: pause GUI packet scope, uniform crop fit, source retention, repeated copies, disabled nodes and camera lifetime");
}
