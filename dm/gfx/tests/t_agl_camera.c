// Decode an observed agl layout and verify transforms geometrically, including rejects.
// Optional arguments validate captured uniform files locally; no game data belongs in this test.
#include "../nvn/nvn_agl_camera.h"
#include <stdio.h>
#include <stdlib.h>
static int fail;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);fail++;}}while(0)
static void fixture(float f[128]) {
    memset(f,0,128*sizeof(float));for(int i=0;i<3;i++) {f[4*i+i]=1;f[0x150/4+4*i+i]=1;}
    f[3]=2; f[0x150/4+3]=3; // previous view moved +1 in camera x
    float n=.1f,z=100.f,fov=.872664626f, aspect=16.f/9;
    float p[16]={1/tanf(fov/2)/aspect,0,0,0,0,1/tanf(fov/2),0,0,0,0,-(z+n)/(z-n),-2*z*n/(z-n),0,0,-1,0};
    memcpy(f+0x70/4,p,sizeof p);memcpy(f+0x1c0/4,p,sizeof p);
    f[0xe0/4]=n;f[0xe4/4]=z;f[0x118/4]=fov;f[0x120/4]=1536;f[0x124/4]=864;
}
int main(int argc,char **argv) {
    if(argc>1){for(int i=1;i<argc;i++){FILE*f=fopen(argv[i],"rb");unsigned char b[512];if(!f)return 2;size_t n=fread(b,1,sizeof b,f);fclose(f);nvn_camera_input c;
        if(!nvn_agl_camera_decode(b,n,&c)){fprintf(stderr,"REJECT %s\n",argv[i]);return 3;}
        printf("decoded %ux%u near %.6g far %.6g FOV %.6g\n",c.width,c.height,c.near_plane,c.far_plane,c.fov*180/3.141592653589793); }return 0;}
    float f[128];fixture(f);nvn_camera_input c;
    CHECK(nvn_agl_camera_decode(f,sizeof f,&c));CHECK(c.width==1536&&c.height==864);
    CHECK(c.has_render_matrices);
    CHECK(c.world_to_view[3]==2 && c.world_to_view[15]==1);
    float product[16];nvn_cam_mul(c.view_to_clip,c.world_to_view,product);
    for(int i=0;i<16;i++)CHECK(fabsf(product[i]-c.world_to_clip[i])<1e-6f);
    float near_z=c.view_to_clip[10]*(-c.near_plane)+c.view_to_clip[11];
    float far_z=c.view_to_clip[10]*(-c.far_plane)+c.view_to_clip[11];
    CHECK(fabsf(near_z/c.near_plane)<1e-5f);CHECK(fabsf(far_z/c.far_plane-1)<1e-5f);
    // A current view point (0,0,-2) is world(-2,0,-2); its previous x is +1.
    float q[4]={0,0,(-.5f*((100.f+.1f)/(100.f-.1f)+1)*(-2)-100.f*.1f/(100.f-.1f))/2,1};
    float world[4]={0},previous[4]={0};
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)world[i]+=c.clip_to_world[i*4+j]*q[j];
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)previous[i]+=c.previous_world_to_clip[i*4+j]*world[j];
    CHECK(fabsf(world[0]/world[3]+2)<.001f);CHECK(fabsf(world[2]/world[3]+2)<.001f);
    CHECK(fabsf(previous[0]/previous[3]-f[0x70/4]/2)<.001f);
    CHECK(!nvn_agl_camera_decode(f,511,&c));CHECK(!nvn_agl_camera_decode(NULL,512,&c));
    fixture(f);f[0]=2;CHECK(!nvn_agl_camera_decode(f,sizeof f,&c));
    fixture(f);f[0x120/4]=0;CHECK(!nvn_agl_camera_decode(f,sizeof f,&c));
    fixture(f);f[0x118/4]=NAN;CHECK(!nvn_agl_camera_decode(f,sizeof f,&c));
    fixture(f);f[0xe0/4]=-1;CHECK(!nvn_agl_camera_decode(f,sizeof f,&c));
    fixture(f);f[0x1c0/4+14]=0;CHECK(!nvn_agl_camera_decode(f,sizeof f,&c));
    printf("agl camera decoder: %s\n",fail?"FAIL":"PASS");return fail?1:0;
}
