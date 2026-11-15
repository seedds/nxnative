// Pure menu camera policy: no GPU, game or proprietary assets.
#include "../../games/010019A01E2F2000/pause_ui.inc"
#include <stdio.h>
#include <stdlib.h>
static int checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void) {
    uint8_t descriptor[36],before[36],frame[0x140],old[0x140],component[0xe0]={0},owner[0x40]={0},props[0x60]={0};
    uint32_t mode=2;float bounds[]={8.f/.9f,-8.f/.9f,-5,5};
    memset(descriptor,0xa5,sizeof descriptor);memcpy(descriptor,&mode,4);memcpy(descriptor+12,bounds,16);memcpy(before,descriptor,sizeof before);
    CHECK(mp4_pause_bounds(descriptor));float out[4];memcpy(out,descriptor+12,sizeof out);
    CHECK(fabsf(out[0]-8)<.000001 && fabsf(out[1]+8)<.000001 && out[2]==-4.5f && out[3]==4.5f);
    CHECK(!memcmp(descriptor,before,12));CHECK(!memcmp(descriptor+28,before+28,8)); // aspect/near/far bit-exact
    CHECK(!mp4_pause_bounds(descriptor)); // repeated borrowed packet cannot accumulate
    for(int bad=0;bad<5;bad++) {
        memcpy(descriptor,before,sizeof before);
        if(bad<3)memcpy(descriptor,&bad,4); // only mode 2 with known bounds succeeds
        if(bad==2){float nan=NAN;memcpy(descriptor+12,&nan,4);}
        if(bad==3){float x=9;memcpy(descriptor+12,&x,4);}
        if(bad==4){float x=INFINITY;memcpy(descriptor+24,&x,4);}
        uint8_t copy[36];memcpy(copy,descriptor,36);CHECK(!mp4_pause_bounds(descriptor));CHECK(!memcmp(copy,descriptor,36));
    }
    const double pi=acos(-1.);
    for(int i=0;i<3;i++) {
        float f=55.f+(i-1)*.000004f;memset(frame,0xa5,sizeof frame);memcpy(frame+8,&f,4);memcpy(old,frame,sizeof old);
        CHECK(mp4_pause_fov(frame));float fitted;memcpy(&fitted,frame+8,4);
        CHECK(fabs(tan(f*pi/360.)/tan(fitted*pi/360.)-.9)<.0000001);
        // All projected points shrink uniformly: native edge now inside the
        // 16:10 crop; aspect/depth and the reference camera's view stay intact.
        for(int n=0;n<21;n++) {
            double point=(n-10)*.1, ratio=tan(f*pi/360.)/tan(fitted*pi/360.);
            CHECK(fabs(point*ratio-point*.9)<.0000001);
        }
        CHECK(!memcmp(old,frame,8));CHECK(!memcmp(old+12,frame+12,sizeof frame-12));
        memcpy(old,frame,sizeof old);CHECK(!mp4_pause_fov(frame));CHECK(!memcmp(old,frame,sizeof old));
    }
    const float bad_fov[]={0,45,90,NAN,INFINITY,-55};
    for(unsigned i=0;i<sizeof bad_fov/sizeof *bad_fov;i++) {
        memcpy(frame+8,bad_fov+i,4);memcpy(old,frame,sizeof old);CHECK(!mp4_pause_fov(frame));CHECK(!memcmp(old,frame,sizeof old));
    }
    const uint8_t id[]={0x68,0xd8,0x47,0xf0,0x68,0x20,0x88,0x4a,0x8f,0x84,0x6c,0x55,0xb0,0x2f,0x73,0x9d};
    memcpy(props+12,id,16);const void *p=props;memcpy(owner+0x30,&p,8);p=owner;memcpy(component+0x20,&p,8);component[0x1c]=0xd8;
    CHECK(mp4_pause_template(props));CHECK(!mp4_pause_template(NULL));
    memcpy(descriptor,before,36);CHECK(mp4_pause_apply(component,descriptor,0));
    float f=55;memcpy(frame+8,&f,4);CHECK(mp4_pause_apply(component,frame,1));
    for(int b=0;b<16;b++) {
        props[12+b]^=1;memcpy(descriptor,before,36);memcpy(frame+8,&f,4);
        CHECK(!mp4_pause_apply(component,descriptor,0));CHECK(!mp4_pause_apply(component,frame,1));
        CHECK(!memcmp(descriptor,before,36));props[12+b]^=1;
    }
    // The accepted gameplay UICamera has a different original template.
    const uint8_t hud_id[]={0xc3,0x86,0x03,0x2c,0x01,0x83,0x72,0x4c,0xb3,0x03,0x44,0x29,0xfa,0x41,0xb5,0x04};
    memcpy(props+12,hud_id,16);memcpy(descriptor,before,36);memcpy(frame+8,&f,4);
    CHECK(!mp4_pause_template(props));CHECK(!mp4_pause_apply(component,descriptor,0));CHECK(!mp4_pause_apply(component,frame,1));
    CHECK(!memcmp(descriptor,before,36));float retained;memcpy(&retained,frame+8,4);CHECK(retained==55);
    memcpy(props+12,id,16);
    component[0x1c]=0;memcpy(frame+8,&f,4);CHECK(mp4_pause_apply(component,frame,1));CHECK(!mp4_pause_apply(component,frame,2));
    CHECK(!mp4_pause_apply(NULL,frame,1));CHECK(!mp4_pause_apply(component,NULL,0));
    p=NULL;memcpy(component+0x20,&p,8);CHECK(!mp4_pause_apply(component,frame,1));
    printf("PASS: %d pause UI policy checks; template scope, 2D coverage, 3D square-pixel fit, depth, invalid/no-op\n",checks);
}
