#include <stdio.h>
#include "../../games/010019A01E2F2000/quality_policy.inc"
static int tests,failed;
#define CHECK(c) do {tests++;if(!(c)){failed++;fprintf(stderr,"FAIL line %d\n",__LINE__);}}while(0)
int main(void) {
    uint64_t out=0;
    CHECK(mp4_scene_policy(mp4_extent(3840,2160),mp4_extent(5120,2880),mp4_extent(5120,2880),0,&out) && out==mp4_extent(5120,2880));
    CHECK(mp4_scene_policy(mp4_extent(3840,2160),mp4_extent(5120,2880),mp4_extent(5120,2880),1,&out) && out==mp4_extent(3840,2160));
    CHECK(mp4_scene_policy(mp4_extent(1280,720),mp4_extent(1600,900),mp4_extent(1600,900),1,&out) && out==mp4_extent(1280,720));
    CHECK(mp4_scene_policy(mp4_extent(1280,720),mp4_extent(5120,2880),mp4_extent(1600,900),1,&out) && out==mp4_extent(4096,2304));
    const unsigned sizes[][2]={{1600,900},{1920,1080},{2560,1440},{3840,2160},{5120,2880}};
    for(unsigned k=0;k<5;k++) for(unsigned percent=10;percent<=100;percent++) {
        unsigned w=sizes[k][0],h=sizes[k][1],nw=(w*percent/100)&~3u,nh=(h*percent/100)&~3u;
        CHECK(mp4_scene_policy(mp4_extent(nw,nh),mp4_extent(w,h),mp4_extent(w,h),1,&out));
        CHECK(out==mp4_extent(nw,nh));
        CHECK(mp4_dynamic_extent(nw,nh,w,h,1));
        CHECK(!mp4_dynamic_extent(nw,nh,w,h,0));
    }
    CHECK(!mp4_scene_policy(0,mp4_extent(5120,2880),0,1,&out));
    CHECK(!mp4_scene_policy(mp4_extent(5124,2880),mp4_extent(5120,2880),mp4_extent(5120,2880),1,&out));
    CHECK(!mp4_scene_policy(0,mp4_extent(7680,4320),0,0,&out));
    CHECK(!mp4_dynamic_extent(5124,2880,5120,2880,1));
    CHECK(!mp4_dynamic_extent(5119,2880,5120,2880,1));
    printf("MP4_QUALITY %d checks, %d failed\n",tests,failed);return failed!=0;
}
