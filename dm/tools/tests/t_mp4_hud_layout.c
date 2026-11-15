// Offline projection/eligibility checks; no game code, files, GPU or gameplay.
#include "../../games/010019A01E2F2000/hud_layout.inc"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void record(float m[19], float angle, float x, float y, float z) {
    memset(m,0,19*sizeof *m);
    m[0]=cosf(angle);m[2]=sinf(angle);m[5]=1;m[8]=-sinf(angle);m[10]=cosf(angle);
    m[3]=m[15]=x;m[7]=m[16]=y;m[11]=m[17]=z;
    m[12]=.7f;m[13]=1.2f;m[14]=.9f;
}
static void project(const float m[19],const float p[3],float out[3]) {
    // Authored UI coordinates: x left/right, y up, depth toward the reference camera; 55-degree FOV.
    double v[3];
    for(int i=0;i<3;i++) v[i]=m[i*4+3]+m[i*4]*p[0]*m[12]+m[i*4+1]*p[1]*m[13]+m[i*4+2]*p[2]*m[14];
    double depth=8.64441967010498-v[2];
    out[0]=v[0]/(depth*tan(55.*acos(-1.)/360.)*(16./9.));
    out[1]=v[1]/(depth*tan(55.*acos(-1.)/360.));out[2]=depth;
}
static void rejects(const float in[19],int anchor,float scale) {
    float m[19];memcpy(m,in,sizeof m);CHECK(!mp4_hud_matrix(m,0,anchor,scale));CHECK(!memcmp(m,in,sizeof m));
}
int main(void) {
    float m[19],n[19];
    for(int side=-1;side<=1;side++) for(int a=-1;a<=1;a+=2) for(int depth=0;depth<5;depth++) for(int rotation=0;rotation<4;rotation++) {
        record(m,.17f*rotation,-8.3f+depth,2.7f-depth,3.f-depth*3.f);memcpy(n,m,sizeof n);
        CHECK(mp4_hud_matrix(n,side,a,.85f));
        CHECK(!memcmp(m+8,n+8,7*sizeof(float))); // depth row and independent object scale bit-exact
        CHECK(n[18]==m[18]);
        for(int i=0;i<3;i++) CHECK(n[15+i]==n[3+i*4]);
        for(int p=0;p<32;p++) {
            float v[]={sinf(p*.7f),cosf(p*.7f),p*.01f},old[3],now[3];project(m,v,old);project(n,v,now);
            CHECK(fabsf(now[0]-(.85f*old[0]+side*.05f))<.00001f);
            CHECK(fabsf(now[1]-(.85f*old[1]+a*.15f))<.00001f);
            CHECK(now[2]==old[2]);
        }
        // The top or bottom edge is an invariant; both image axes have the same scale.
        float slope=0.5205670505517462f,z=m[11],d=8.64441967010498f-z;
        record(m,0,0,a*d*slope,z);memcpy(n,m,sizeof n);CHECK(mp4_hud_matrix(n,side,a,.85f));
        float zero[]={0,0,0},old[3],now[3];project(m,zero,old);project(n,zero,now);CHECK(fabsf(now[1]-a)<.000001f);
        // A side's source edge maps to that same side of the cropped canvas;
        // its authored margin is scaled, not replaced with a centre inset.
        record(m,0,side*d*slope*(16.f/9.f),0,z);memcpy(n,m,sizeof n);
        CHECK(mp4_hud_matrix(n,side,a,.85f));project(n,zero,now);
        CHECK(fabsf(now[0]-side*.9f)<.000001f);
        record(m,0,(side*.98f)*d*slope*(16.f/9.f),0,z);memcpy(n,m,sizeof n);
        CHECK(mp4_hud_matrix(n,side,a,.85f));project(n,zero,now);
        CHECK(fabsf(now[0]-side*(.9f-.85f*.02f))<.000001f);
        record(m,0,0,0,z);memcpy(n,m,sizeof n);CHECK(mp4_hud_matrix(n,side,a,.85f));
        float right[]={1,0,0},up[]={0,1,0},o[3],r[3],u[3];project(n,zero,o);project(n,right,r);project(n,up,u);
        CHECK(fabsf(fabsf((r[0]-o[0])*16.f/9.f)/m[12]-fabsf(u[1]-o[1])/m[13])<.000001f);
    }
    record(m,0,1,2,-5);
    rejects(m,0,.85f);rejects(m,1,1);rejects(m,1,0);rejects(m,1,-1);rejects(m,1,NAN);rejects(m,1,INFINITY);
    for(int i=0;i<19;i++){float old=m[i];m[i]=NAN;rejects(m,1,.85f);m[i]=old;}
    m[12]=0;rejects(m,1,.85f);m[12]=1;m[15]=2;rejects(m,1,.85f);
    record(m,0,0,0,8.6f);rejects(m,1,.85f);
    record(m,0,0,0,0);memcpy(n,m,sizeof n);
    CHECK(!mp4_hud_matrix(n,2,1,.85f));CHECK(!memcmp(n,m,sizeof n));
    CHECK(!mp4_hud_matrix(n,-2,-1,.85f));CHECK(!memcmp(n,m,sizeof n));
    CHECK(!mp4_hud_matrix(NULL,0,1,.85f));CHECK(mp4_hud_group(NULL)==-1);
    unsigned char props[0x60]={0},original[0x60],unknown[16]={0};
    CHECK(mp4_hud_group(unknown)==-1);
    CHECK(!mp4_hud_apply(NULL,m));CHECK(!mp4_hud_apply(props,NULL));
    const int expected_sides[]={-1,0,-1,0,0,0,-1,1,1,1,1};
    for(unsigned i=0;i<sizeof mp4_hud_groups/sizeof *mp4_hud_groups;i++) {
        CHECK(mp4_hud_group(mp4_hud_groups[i].template_id)==(int)i);
        CHECK(mp4_hud_groups[i].side==expected_sides[i]);
        for(unsigned j=i+1;j<sizeof mp4_hud_groups/sizeof *mp4_hud_groups;j++)
            CHECK(memcmp(mp4_hud_groups[i].template_id,mp4_hud_groups[j].template_id,16));
        memcpy(props+0xc,mp4_hud_groups[i].template_id,16);
        int count=1;memcpy(props+0x50,&count,4);
        const void *id=mp4_hud_groups[i].root_component;memcpy(props+0x58,&id,sizeof id);
        record(m,0,-10,5,-6);memcpy(n,m,sizeof n);
        CHECK(!mp4_hud_apply(props,n));CHECK(!memcmp(m,n,sizeof n)); // root retained
        id=unknown;memcpy(props+0x58,&id,sizeof id);memcpy(original,props,sizeof props);
        CHECK(mp4_hud_apply(props,n)==(int)i+1);CHECK(!memcmp(props,original,sizeof props));
        CHECK(memcmp(n,m,sizeof n));
        for(int bad=-1;bad<=257;bad+=258) {
            memcpy(props+0x50,&bad,4);memcpy(n,m,sizeof n);CHECK(!mp4_hud_apply(props,n));CHECK(!memcmp(n,m,sizeof n));
        }
        count=0;memcpy(props+0x50,&count,4);CHECK(!mp4_hud_apply(props,n));
        count=1;memcpy(props+0x50,&count,4);id=NULL;memcpy(props+0x58,&id,sizeof id);CHECK(!mp4_hud_apply(props,n));
    }
    memset(props+0xc,0,16);memcpy(n,m,sizeof n);CHECK(!mp4_hud_apply(props,n));CHECK(!memcmp(m,n,sizeof n));
    printf("PASS: %u HUD layout checks; left/right crop anchors, top/bottom anchors, equal pixel scales, depth, metadata, invalid/no-op; ASan/UBSan\n",checks);
}
